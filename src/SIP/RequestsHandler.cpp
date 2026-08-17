// RequestsHandler.cpp: Issues #24 and #28 resolved.
#include "RequestsHandler.hpp"
#include <atomic>
#include <sstream>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include "SipMessageTypes.h"
#include "SipSdpMessage.hpp"
#include "IDGen.hpp"
#include "IPHelper.hpp"
#include "PoolConfig.hpp"
#include "CallDetailRecord.hpp"
#include "PbxConfig.hpp"
#include "PbxPersist.hpp"
#include "SipHeaderUtil.hpp"
#include "SipWireUtil.hpp"
#include "AdminAuth.hpp"
#include "SipDigest.hpp"
#include "SipSecretStore.hpp"
#include "ArpLookup.hpp"

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	// PBX config (call-forward / ring groups) and the persistent CDR ring live in
	// NVS on the device. nvs_flash/nvs are core ESP-IDF components present on every
	// transport, so they gate on ESP_PLATFORM (not POCKETDIAL_HAS_WIFI). On host the
	// in-memory maps/ring ARE the store and these calls compile out (see the
	// load*/persist* helpers at the bottom of this file).
	#include "nvs_flash.h"
	#include "nvs.h"
	#include "esp_sntp.h"
	#include "esp_system.h"
	#include "esp_idf_version.h"
#endif

std::vector<std::shared_ptr<SipMessage>> RequestsHandler::_messagePool;

// File-scope static helpers defined later in this translation unit.
static bool sameAddress(const sockaddr_in&, const sockaddr_in&);
static std::string stripHeaderName(std::string_view fullLine);

namespace
{
	// Default lease granted when a REGISTER does not request one (seconds).
	constexpr int DEFAULT_EXPIRES = 3600;
	// Upper bound we are willing to grant, regardless of what the client asks.
	constexpr int MAX_EXPIRES = 3600;
	// Minimum non-zero lease, to avoid pathologically short registrations.
	constexpr int MIN_EXPIRES = 30;
	// How often the opportunistic sweep is allowed to run.
	constexpr auto SWEEP_INTERVAL = std::chrono::seconds(1);

	// How long an unanswered leg rings before the no-answer action fires (CFNA
	// forward, or advancing to the next hunt-group member). Polled from tick().
	constexpr auto NO_ANSWER_TIMEOUT = std::chrono::seconds(20);

	// NVS namespaces / keys for the persisted PBX config and CDR ring. Kept short
	// (NVS keys are capped at 15 chars). Forward/group entries are stored one blob
	// per extension so a single mutation rewrites only its own key (low flash wear).
	constexpr auto NVS_PBX_NS  = "pbxcfg";
	constexpr auto NVS_CDR_NS  = "cdrlog";
}

RequestsHandler::RequestsHandler(std::string serverIp, int serverPort,
	OnHandledEvent onHandledEvent) :
	_onHandled(onHandledEvent),
	_serverIp(std::move(serverIp)),
	_localIp(_serverIp == "0.0.0.0" ? getPrimaryLocalIP() : _serverIp),
	_serverPort(serverPort)
{
	initHandlers();
	// Pre-allocate pools (Issue #53). Capacities are compile-time tunable via
	// PoolConfig.hpp (-DPOCKETDIAL_MAX_* overrides); defaults preserve 32/8/32.
	_clientPool.reserve(POCKETDIAL_MAX_CLIENTS);
	for (int i = 0; i < POCKETDIAL_MAX_CLIENTS; ++i)
	{
		_clientPool.push_back(std::make_shared<SipClient>());
	}
	_sessionPool.reserve(POCKETDIAL_MAX_SESSIONS);
	for (int i = 0; i < POCKETDIAL_MAX_SESSIONS; ++i)
	{
		_sessionPool.push_back(std::make_shared<Session>());
	}
	if (_messagePool.empty())
	{
		_messagePool.reserve(POCKETDIAL_MSG_POOL);
		for (int i = 0; i < POCKETDIAL_MSG_POOL; ++i)
		{
			_messagePool.push_back(std::make_shared<SipSdpMessage>("", sockaddr_in{}));
		}
	}
	_virtualPeerPool.reserve(POCKETDIAL_VIRTUAL_PEERS);
	for (int i = 0; i < POCKETDIAL_VIRTUAL_PEERS; ++i)
	{
		_virtualPeerPool.push_back(std::make_shared<SipClient>());
	}

	// Reload persisted PBX config (call-forward / ring groups) and the CDR ring from
	// NVS so they survive reboot. No-ops on host. Construction is single-threaded
	// (no handler is dispatching yet), so these run without holding _mutex.
	loadPbxConfig();
	loadCdrRing();
	// Task 2B: load the admin extension from NVS (defaults to "1001" if absent).
	loadAdminExt();
	// STAGE 2: load the registrar mode (defaults to the POCKETDIAL_OPEN_REGISTRAR
	// seed) and the adopted-device registry from NVS.
	_registrar.loadMode();
	loadAdminHttpTtl();
	_registrar.loadDevices();
	// Prewarm the per-extension HA1 cache off the REGISTER hot path so the first
	// Secure REGISTER does not pay a blocking NVS read while holding _mutex.
	SipSecretStore::warmCache();
	// Seed the dashboard snapshot with the loaded devices so the TUI sees adopted
	// devices immediately on boot (online flags start false until each re-REGISTERs).
	refreshDeviceSnapshot();
}

// ── Message pool synchronization (Issue #101(A) / #101(E)) ──────────────────
//
// Leaf lock guarding the static _messagePool. LEAF means exactly that: nothing
// inside its critical section may acquire another lock, ever. That is what makes
// it safe to take whether or not the caller already holds _mutex, with no lock
// ordering to reason about.
//
// It is needed because handing a slot out is a check-then-take (scan for
// use_count()==1, then copy the shared_ptr) over a pool reachable from two
// tasks at once:
//
//   * the UDP receive task (UdpServer.cpp:134) -> SipServer::onNewMessage ->
//     SipMessageFactory::createMessage -> getMessageFromPool, holding NO lock —
//     handle() only takes _mutex afterwards, on the already-allocated message;
//   * the tick task (esp_main.cpp:192, or SipServer::tickLoop on desktop) ->
//     tick() -> TransactionLayer::sweep -> messageFromPool, under _mutex.
//
// Unsynchronized, both could observe use_count()==1 on the same slot and both
// take it, then reset() it concurrently — two owners writing the same
// SipMessage, reallocating its strings under each other. _mutex on one side does
// not help: a lock only excludes other holders of that same lock. Found by the
// #101(E) audit, fixed here because it lives in the function #101(A) rewrites.
static std::mutex s_msgPoolMutex;

// Heap-fallback messages currently alive. Atomic because the decrement happens
// in the deleter, which runs wherever the last reference happens to drop —
// commonly on the socket-send path after the outbox is drained, outside every
// lock we hold here.
static std::atomic<std::size_t> s_msgHeapFallbacksInFlight{0};

namespace
{
	// Releases a bounded heap-fallback message and gives its budget back.
	//
	// MUST NOT take s_msgPoolMutex (or any lock): the last reference can drop
	// while that mutex is held — dropping a superseded message inside the pool
	// critical section would self-deadlock on a non-recursive mutex. An atomic
	// decrement is all this is allowed to do.
	struct HeapFallbackDeleter
	{
		void operator()(SipMessage* p) const noexcept
		{
			delete p;
			s_msgHeapFallbacksInFlight.fetch_sub(1, std::memory_order_relaxed);
		}
	};
}

// Same bounded-fallback bookkeeping for the virtual-peer pool. Separate budget:
// a virtual peer is a long-lived per-park-slot stand-in, not a per-packet object.
static std::atomic<std::size_t> s_vpeerHeapFallbacksInFlight{0};

namespace
{
	// Same no-locking rule as HeapFallbackDeleter above.
	struct VpeerFallbackDeleter
	{
		void operator()(SipClient* p) const noexcept
		{
			delete p;
			s_vpeerHeapFallbacksInFlight.fetch_sub(1, std::memory_order_relaxed);
		}
	};
}

static void logPoolExhausted(const char* poolName, std::atomic<std::size_t>& warnCount)
{
	// Rate-limited 1-in-100: a flood that drains the pool would otherwise also
	// flood the log pipe.
	if ((warnCount.fetch_add(1, std::memory_order_relaxed) % 100) == 0)
	{
		std::cerr << "[WARNING] " << poolName << " pool exhausted! Dropping.\n";
	}
}

std::shared_ptr<SipMessage> RequestsHandler::acquirePooledMessage()
{
	std::lock_guard<std::mutex> poolLock(s_msgPoolMutex);

	for (auto& msg : _messagePool)
	{
		if (msg.use_count() == 1)
		{
			// Copy INSIDE the lock: the copy is what publishes use_count()==2 and
			// so what makes this slot invisible to the other task's scan. Callers
			// initialize it (reset() / operator=) after we return, by which point
			// they own it exclusively — keeping the critical section to a scan.
			return msg;
		}
	}

	// Pool drawn down: fall back to the heap, but only up to a fixed number alive
	// at once (Issue #101(A)). Past that, refuse and let the caller drop.
	if (s_msgHeapFallbacksInFlight.load(std::memory_order_relaxed) >= POCKETDIAL_MSG_HEAP_FALLBACK_MAX)
	{
		return nullptr;
	}

	SipMessage* raw = nullptr;
	try
	{
		raw = new SipSdpMessage("", sockaddr_in{});
	}
	catch (const std::bad_alloc&)
	{
		return nullptr;   // budget untouched — nothing was handed out
	}

	s_msgHeapFallbacksInFlight.fetch_add(1, std::memory_order_relaxed);
	try
	{
		return std::shared_ptr<SipMessage>(raw, HeapFallbackDeleter{});
	}
	catch (const std::bad_alloc&)
	{
		// The shared_ptr constructor takes ownership of `raw` before it can throw,
		// and the standard requires it to run the deleter if it does. So `raw` is
		// already deleted and the counter already decremented — undoing either
		// here would be a double free / double decrement.
		return nullptr;
	}
}

std::shared_ptr<SipMessage> RequestsHandler::getMessageFromPool(std::string message, sockaddr_in src)
{
	std::shared_ptr<SipMessage> msg = acquirePooledMessage();
	if (!msg)
	{
		static std::atomic<std::size_t> warnCount{0};
		logPoolExhausted("SIP Message", warnCount);
		return nullptr;
	}
	msg->reset(std::move(message), src);
	return msg;
}

std::shared_ptr<SipMessage> RequestsHandler::getMessageFromPool(const SipMessage& source)
{
	std::shared_ptr<SipMessage> msg = acquirePooledMessage();
	if (!msg)
	{
		static std::atomic<std::size_t> warnCount{0};
		logPoolExhausted("SIP Message", warnCount);
		return nullptr;
	}
	*msg = source;
	return msg;
}

void RequestsHandler::initHandlers()
{
	_handlers.emplace(SipMessageTypes::REGISTER,          std::bind(&RequestsHandler::onRegister,       this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::OPTIONS,           std::bind(&RequestsHandler::onOptions,        this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::CANCEL,            std::bind(&RequestsHandler::onCancel,         this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::INVITE,            std::bind(&RequestsHandler::onInvite,         this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::TRYING,            std::bind(&RequestsHandler::onTrying,         this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::RINGING,           std::bind(&RequestsHandler::onRinging,        this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::BUSY,              std::bind(&RequestsHandler::onBusy,           this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::UNAVAILABLE,       std::bind(&RequestsHandler::onUnavailable,    this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::OK,                std::bind(&RequestsHandler::onOk,             this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::ACK,               std::bind(&RequestsHandler::onAck,            this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::BYE,               std::bind(&RequestsHandler::onBye,            this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::REQUEST_TERMINATED,std::bind(&RequestsHandler::onReqTerminated,  this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::REFER,             std::bind(&RequestsHandler::onRefer,          this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::UPDATE,            std::bind(&RequestsHandler::onUpdate,         this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::MESSAGE,           std::bind(&RequestsHandler::onMessage,        this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::SUBSCRIBE,         std::bind(&RequestsHandler::onSubscribe,      this, std::placeholders::_1));
}

void RequestsHandler::handle(std::shared_ptr<SipMessage> request)
{
	// Input validation: Drop null or structurally malformed packets instantly (SEC-02)
	if (!request || !request->isValidMessage())
	{
		_packetsDropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	// Per-source IP rate limiting (Issue #38 / SEC-02) runs under its OWN lock,
	// BEFORE the big handler _mutex, so a flood from blocked IPs is dropped without
	// ever serializing on _mutex against legitimate signaling. ipAllowed/allowPacket
	// touch only the rate state, which _rateMutex now guards.
	{
		std::lock_guard<std::mutex> rlock(_rateMutex);
		if (!ipAllowed(request->getSource()) || !allowPacket(request->getSource()))
		{
			_packetsDropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}

	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> localOutbox;
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		_packetsProcessed.fetch_add(1, std::memory_order_relaxed);
		_outbox.clear();

		// Issue #33: /api/pcap capture. Only messages that clear the checks above
		// (structurally valid, not rate-limited) are captured — this is a
		// signaling-research aid, not a wire-level DoS forensics tool, and
		// capturing before the rate limiter would mean pulling the ring buffer
		// out from under _mutex for every flood packet too.
		// Serialized straight into the ring slot — no per-packet temporary inside
		// the critical section (Issue #101(D)).
		request->toString(_pcapCapture.recordInto(/*outbound=*/false, request->getSource()));

		auto client = findClientByAddress(request->getSource());
		if (client.has_value())
		{
			client.value()->markActive();
		}

		maybeSweep();

		// RFC 3261 §17 transaction layer: advance the state machine for any tracked
		// InviteClient transaction before the TU handler runs. A 1xx moves Calling →
		// Proceeding (stops retransmitting); a 2xx/3xx-6xx → Accepted/Completed.
		_txLayer.matchAndAdvance(request);

		// Route responses by parsed numeric status code so dispatch is immune to
		// reason-phrase variation (e.g. "486 Busy" vs "486 Busy Here"). Requests and
		// anything without a status line fall back to the method/start-line token.
		std::string handlerKey;
		auto status = request->getStatusInfo();
		if (status.has_value())
		{
			switch (status->code)
			{
				case 100: handlerKey = SipMessageTypes::TRYING;             break;
				case 180: handlerKey = SipMessageTypes::RINGING;            break;
				case 200: handlerKey = SipMessageTypes::OK;                 break;
				case 480: handlerKey = SipMessageTypes::UNAVAILABLE;        break;
				case 486: handlerKey = SipMessageTypes::BUSY;               break;
				case 487: handlerKey = SipMessageTypes::REQUEST_TERMINATED; break;
				default:  handlerKey = std::string(request->getType());     break;
			}
		}
		else
		{
			handlerKey = std::string(request->getType());
		}

		// Surface inbound client-error (4xx) responses once. Deferred via _logQueue
		// so the write happens outside the lock (Issue #24); replaces the per-parse
		// printf the parser used to do. softFail -> warning (stdout), else error (stderr).
		if (status.has_value() && status->klass == PocketDial::SipStatusClass::ClientError)
		{
			queueLog("[SIP] " + std::string(status->softFail ? "WARN " : "ERROR ")
				+ std::string(request->getHeader()), !status->softFail);
		}

		// Task 2C: SIP INFO with DTMF relay body — handle before the handler table
		// so it is never mistakenly forwarded by a catch-all entry.
		if (handlerKey == "INFO")
		{
			// Scan headers for Content-Type: application/dtmf-relay.
			const std::string& rawMsg = request->toString();
			bool isDtmfRelay = false;
			{
				size_t pos = 0;
				while (pos < rawMsg.size())
				{
					size_t nl = rawMsg.find('\n', pos);
					size_t next = (nl == std::string::npos) ? rawMsg.size() : nl + 1;
					// Header/body boundary: blank line.
					if (pos < rawMsg.size() && (rawMsg[pos] == '\r' || rawMsg[pos] == '\n')) break;
					// Header name = text before the first ':' (RFC 3261: no WS before colon).
					size_t colon = rawMsg.find(':', pos); if (colon != std::string::npos && colon < ((nl == std::string::npos) ? rawMsg.size() : nl))
					{
						std::string nameLC = rawMsg.substr(pos, colon - pos);
						std::transform(nameLC.begin(), nameLC.end(), nameLC.begin(),
							[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
						if (nameLC == "content-type" || nameLC == "c")
						{
							size_t valEnd = (nl == std::string::npos) ? rawMsg.size() : nl;
							std::string val = rawMsg.substr(colon + 1, valEnd - (colon + 1));
							if (val.find("application/dtmf-relay") != std::string::npos)
							{
								isDtmfRelay = true;
							}
							break;
						}
					}
					pos = next;
				}
			}
			// Always 200 OK a SIP INFO (RFC 6086 §4.2.1).
			{
				auto infoOk = getMessageFromPool(*request);
				if (!infoOk) return;   // pool exhausted: drop, peer retransmits (#101A)
				infoOk->setHeader(SipMessageTypes::OK);
				std::string activeIp = _localIp;
				infoOk->setVia(std::string(request->getVia()) + ";received=" + activeIp);
				_outbox.emplace_back(request->getSource(), std::move(infoOk));
			}
			if (isDtmfRelay)
			{
				onDtmfInfo(request);
			}
		}
		else
		{
			auto it = _handlers.find(handlerKey);
			if (it != _handlers.end())
			{
				it->second(std::move(request));
			}
		}

		// Device-registry change detection: a REGISTER may have adopted a device,
		// re-synced its extension, or flipped its online flag inside the Registrar
		// machine — mirror the registry into the dashboard snapshot once per packet.
		applyDeviceChange(_registrar.consumeDevicesChange());

		// Park-orbit change detection, same contract. Polling here (rather than
		// making every mutating call site remember refreshParkSnapshot()) means a
		// new park path cannot silently leave a stale row on the dashboard.
		if (_park.consumeParkChanged())
		{
			refreshParkSnapshot();
		}

		// BLF change detection: one pass after every handled packet covers
		// registration appear/disappear, session create/transition/teardown.
		// NOTIFYs land in _outbox and ride out with this pass (after unlock).
		_blf.refresh();

		localOutbox = drainOutbox();

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	// Print deferred logs safely outside of the lock
	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}

	// Issue #24 resolved: UDP socket syscall sendto is now executed outside the locked section to prevent lock contention.
	for (auto& event : localOutbox)
	{
		_onHandled(event.first, std::move(event.second));
	}
}

std::optional<std::shared_ptr<Session>> RequestsHandler::getSession(std::string_view callID)
{
	auto sessionIt = _sessions.find(std::string(callID));
	if (sessionIt != _sessions.end())
	{
		return sessionIt->second;
	}
	return {};
}

void RequestsHandler::onRegister(std::shared_ptr<SipMessage> data)
{
	auto fromNumber = data->getFromNumber();
	int requestedExpires = parseRequestedExpires(data);
	int grantedExpires = 0;

	if (!isValidAor(fromNumber))
	{
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader("SIP/2.0 400 Bad Request");
		response->clearBody();
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	// ── Registrar-mode admission (STAGE 2) ───────────────────────────────────────
	// Runtime policy replaces the old compile-time POCKETDIAL_OPEN_REGISTRAR gate.
	//   Open   : accept every REGISTER (legacy standalone behaviour).
	//   Secure : digest-challenge + verify against the stored HA1 for this ext.
	//   Learn  : TOFU + MAC-lock — adopt unknown devices, enforce secured ones.
	// On Challenge the helper has already enqueued the 401 + WWW-Authenticate; on
	// Reject we emit the 403 here from rejectReason. Either way a non-Accept stops.
	const std::string extStr(fromNumber);
	const RegistrarMode mode = _registrar.getMode();
	if (mode != RegistrarMode::Open)
	{
		std::string rejectReason;
		Registrar::AuthDecision decision = (mode == RegistrarMode::Secure)
			? _registrar.admitSecure(data, extStr, rejectReason)
			: _registrar.admitLearn(data, extStr, rejectReason);

		if (decision == Registrar::AuthDecision::Challenge)
		{
			// admitSecure already enqueued the 401 + WWW-Authenticate.
			return;
		}
		if (decision == Registrar::AuthDecision::Reject)
		{
			_registrar.sendForbidden(data, rejectReason.empty() ? "Forbidden" : rejectReason);
			return;
		}
		// decision == Accept → fall through to the normal binding path below.
	}

	// Resolve the device MAC once (Learn/registry bookkeeping). nullopt on a
	// first-packet ARP miss or on host — the online flag just stays unchanged then.
	std::optional<std::string> deviceMac;
	{
		auto m = ArpLookup::pdLookupMac(data->getSource());
		if (m.has_value()) deviceMac = ArpLookup::toHex12(*m);
	}

	if (requestedExpires <= 0)
	{
		// expires=0 (or an explicit zero) is a de-registration request.
		unregisterClient(fromNumber);
		if (deviceMac.has_value()) _registrar.markOnline(*deviceMac, false);
	}
	else
	{
		grantedExpires = (std::max)(MIN_EXPIRES, (std::min)(requestedExpires, MAX_EXPIRES));
		// Distinguish a brand-new binding from a lease refresh BEFORE allocating: a
		// client already present in the pool under this number is a re-REGISTER, which
		// must NOT trigger a welcome MESSAGE (phones re-register every lease period).
		bool isNewBinding = !findClient(fromNumber).has_value();
		// Always update address so re-REGISTER after a NAT rebind works correctly
		auto newClient = allocateClient(std::string(data->getFromNumber()), data->getSource(), grantedExpires);
		if (newClient)
		{
			// allocateClient() has already placed the binding in the client pool;
			// the registrar keeps no separate index, so there is nothing more to do
			// here (this was previously a no-op registerClient() hook).
			// Register beep: on a brand-new binding ONLY (never a lease refresh —
			// phones re-REGISTER every lease period), send the registering phone a
			// brief intercom auto-answer INVITE so it plays its own tone, then tear
			// the call back down. Signaling-only: the server sources NO RTP. Bounded
			// and best-effort — if the beep table is full the beep is simply skipped.
			if (isNewBinding)
			{
				_beeper.sendBeep(newClient);
			}
			if (deviceMac.has_value()) _registrar.markOnline(*deviceMac, true);
		}
		else
		{
			// Server full: reply with 503 Service Unavailable
			auto response = getMessageFromPool(*data);
			if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
			response->setHeader("SIP/2.0 503 Service Unavailable");
			response->clearBody();
			std::string activeIp = _localIp;
			response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
			_outbox.emplace_back(data->getSource(), std::move(response));
			return;
		}
	}

	auto response = getMessageFromPool(*data);
	if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
	response->setHeader(SipMessageTypes::OK);
	std::string activeIp = _localIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
	// Echo the granted lease back in the Contact so the client knows when to refresh.
	response->setContact(buildContact(fromNumber) + ";expires=" + std::to_string(grantedExpires));
	endHandle(fromNumber, response);
}

void RequestsHandler::onOptions(std::shared_ptr<SipMessage> data)
{
	auto response = getMessageFromPool(*data);
	if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
	response->setHeader(SipMessageTypes::OK);
	std::string activeIp = _localIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
	response->setContact(buildContact(data->getFromNumber()));
	_outbox.emplace_back(data->getSource(), std::move(response));
}

void RequestsHandler::onCancel(std::shared_ptr<SipMessage> data)
{
	std::string destNumber(data->getToNumber());

	// Issue #46: same off-path teardown guard as onBye(). A CANCEL whose Call-ID
	// names an established two-leg dialog must come from a leg IP.
	if (auto cancelSess = getSession(data->getCallID());
		cancelSess.has_value() &&
		!isDialogSourceAuthorized(cancelSess.value(), data->getSource()))
	{
		queueLog("CANCEL for Call-ID " + std::string(data->getCallID()) +
			" rejected: source not a dialog leg (spoofed teardown)", true);
		_registrar.sendForbidden(data, "Forbidden");
		return;
	}

	if (destNumber == "777")
	{
		endCall(data->getCallID(), data->getFromNumber(), "777");
		return;
	}

	if (destNumber == "440")
	{
		// CANCEL of the media call: stop the stream (if it owns this Call-ID) and end.
		_rtpSender.stop(std::string(data->getCallID()));
		endCall(data->getCallID(), data->getFromNumber(), "440");
		return;
	}

	if (_park.orbitIndex(destNumber) >= 0)
	{
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (destNumber == "999" || isPageZoneDialog(destNumber))
	{
		auto session = getSession(data->getCallID());
		if (session.has_value())
		{
			std::string activeIp = _localIp;
			std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);
			std::string originalCSeq(data->getCSeq());
			size_t invitePos = originalCSeq.find("INVITE");
			if (invitePos != std::string::npos)
			{
				originalCSeq.replace(invitePos, 6, "CANCEL");
			}

			for (const auto& target : session.value()->getPendingTargets())
			{
				auto cancelMsg = getMessageFromPool(*data);
				if (!cancelMsg) continue;   // pool exhausted: skip this target (#101A)
				std::string targetIpPort = sipwire::addrToIpPort(target->getAddress());

				cancelMsg->setHeader("CANCEL sip:" + target->getNumber() + "@" + targetIpPort + " SIP/2.0");

				std::string newTo = "To: <sip:" + target->getNumber() + "@" + serverIpPort + ">";
				cancelMsg->setTo(newTo);
				cancelMsg->setCSeq(originalCSeq);
				_outbox.emplace_back(target->getAddress(), std::move(cancelMsg));
			}
		}
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	setCallState(data->getCallID(), Session::State::Cancel);
	endHandle(data->getToNumber(), data);
}

void RequestsHandler::onReqTerminated(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		return;
	}
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onInvite(std::shared_ptr<SipMessage> data)
{
	// Task 2A: Retransmission guard — silently drop if a session for this Call-ID
	// is already active (Invited or Connected). RFC 3261 §17.2.3: a UAS that receives
	// a retransmission of a request for which a non-2xx final response has been sent
	// should retransmit that response; for 2xx, the ACK re-drive handles it. The
	// simplest safe policy here is a silent drop so we never create a second session
	// slot for the same dialog, which could exhaust the pool and trigger spurious 503.
	if (auto existing = getSession(data->getCallID()); existing.has_value())
	{
		const auto st = existing.value()->getState();
		// Mid-dialog re-INVITE (RFC 3261 §12.2): To-tag present = established dialog.
		// This is the hold/resume path — route to onReinvite().
		if ((st == Session::State::Connected || st == Session::State::Held) &&
			std::string_view(data->getTo()).find("tag=") != std::string_view::npos)
		{
			onReinvite(data);
			return;
		}
		if (st == Session::State::Invited || st == Session::State::Connected ||
			st == Session::State::Held)
		{
			return; // silent drop per RFC 3261 §17.2.3
		}
	}

	if (!isValidAor(data->getFromNumber()) || !isValidAor(data->getToNumber()))
	{
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader("SIP/2.0 400 Bad Request");
		response->clearBody();
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	// Check if the caller is registered
	auto caller = findClient(data->getFromNumber());
	if (!caller.has_value())
	{
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader("SIP/2.0 403 Forbidden");
		response->clearBody();
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	std::string destNumber(data->getToNumber());
	if (destNumber == "777")
	{
		// SDP loopback echo test
		auto ringing = getMessageFromPool(*data);
		if (!ringing) return;   // pool exhausted: drop, peer retransmits (#101A)
		ringing->setHeader("SIP/2.0 180 Ringing");
		ringing->clearBody();
		std::string activeIp = _localIp;
		ringing->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		std::string toTag = IDGen::GenerateID(9);
		ringing->setTo(std::string(data->getTo()) + ";tag=" + toTag);
		ringing->setContact(buildContact("777"));
		_outbox.emplace_back(data->getSource(), std::move(ringing));

		auto okResponse = getMessageFromPool(*data);
		if (!okResponse) return;   // pool exhausted: drop, peer retransmits (#101A)
		okResponse->setHeader(SipMessageTypes::OK);
		okResponse->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		okResponse->setTo(std::string(data->getTo()) + ";tag=" + toTag);
		okResponse->setContact(buildContact("777"));
		okResponse->enforceG711();
		_outbox.emplace_back(data->getSource(), std::move(okResponse));

		// Per-session dummy dest (never the old shared _dummyClient): a concurrent
		// 777/440 call must not overwrite this session's destination identity. The
		// shared_ptr lives as long as the session references it (released on
		// teardown / pool reuse) — bounded by the session pool, not the packet path.
		auto dummy777 = std::make_shared<SipClient>();
		dummy777->reset("777", data->getSource(), 3600);
		auto newSession = allocateSession(std::string(data->getCallID()), caller.value());
		if (newSession)
		{
			newSession->setDest(dummy777);
			_sessions.emplace(data->getCallID(), newSession);
			newSession->setState(Session::State::Connected);
		}
		return;
	}

	if (destNumber == "999")
	{
		// All Page / Broadcast: fork to every other registered client, with the
		// intercom auto-answer headers. Targets are picked here; the fork machinery
		// is the shared startBroadcastFork() helper (also used by ring groups).
		std::vector<std::shared_ptr<SipClient>> targets;
		for (const auto& client : _clientPool)
		{
			if (!client->getNumber().empty() && client->getNumber() != caller.value()->getNumber())
			{
				targets.push_back(client);
			}
		}

		if (targets.empty())
		{
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*data);
			if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
			responseObj->setHeader(SipMessageTypes::NOT_FOUND);
			responseObj->clearBody();
			responseObj->setContact(buildContact(caller.value()->getNumber()));
			_outbox.emplace_back(data->getSource(), std::move(responseObj));
			return;
		}

		startBroadcastFork(data, caller.value(), std::move(targets), /*intercom=*/true);
		return;
	}

	// Paging zones (980–989): a configured zone is a scoped 999 — fork an intercom
	// INVITE to every registered zone member. An unconfigured 98x falls through to
	// normal routing (and 404s, since 98x is not a real extension/group target).
	if (pbx::isPageZoneExt(destNumber))
	{
		if (const pbx::PageZone* zone = findPageZone(destNumber))
		{
			std::vector<std::shared_ptr<SipClient>> targets;
			for (const auto& m : zone->members)
			{
				if (m == caller.value()->getNumber()) continue;
				auto mc = findClient(m);
				if (mc.has_value())
					targets.push_back(mc.value());
			}

			if (targets.empty())
			{
				std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*data);
				if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
				responseObj->setHeader(SipMessageTypes::UNAVAILABLE);
				responseObj->clearBody();
				responseObj->setContact(buildContact(caller.value()->getNumber()));
				_outbox.emplace_back(data->getSource(), std::move(responseObj));
				return;
			}

			startBroadcastFork(data, caller.value(), std::move(targets), /*intercom=*/true);
			return;
		}
	}

	if (destNumber == "440")
	{
		// Media beachhead: the server answers and sources a one-way RTP tone stream.
		onMediaInvite(data, caller.value());
		return;
	}

	// Call parking (park-orbit, 700..70N): an INVITE to a FREE orbit parks the
	// caller's leg there; an INVITE to an OCCUPIED orbit retrieves the parked call.
	{
		int orbitIdx = _park.orbitIndex(destNumber);
		if (orbitIdx >= 0)
		{
			_park.onInvite(data, caller.value(), orbitIdx);
			return;
		}
	}

	// Ring / hunt groups (Class A sweep): a configured group extension (e.g. 6xx)
	// maps to an ordered member list. Ring-all reuses the broadcast fork (without the
	// intercom auto-answer headers, so members ring normally); hunt rings members one
	// at a time, driven from tick(). Resolved before DND because a group ext is not a
	// real endpoint and so never carries its own DND/forward config.
	if (const pbx::RingGroup* group = findRingGroup(destNumber))
	{
		// Collect the registered members (skip the caller and any offline member).
		std::vector<std::shared_ptr<SipClient>> members;
		std::vector<std::string> huntOrder;
		for (const auto& m : group->members)
		{
			if (m == caller.value()->getNumber()) continue;
			auto mc = findClient(m);
			if (mc.has_value())
			{
				members.push_back(mc.value());
				huntOrder.push_back(m);
			}
		}

		if (members.empty())
		{
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*data);
			if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
			responseObj->setHeader(SipMessageTypes::UNAVAILABLE);
			responseObj->clearBody();
			responseObj->setContact(buildContact(caller.value()->getNumber()));
			endHandle(data->getFromNumber(), responseObj);
			return;
		}

		if (group->mode == pbx::GroupMode::RingAll)
		{
			startBroadcastFork(data, caller.value(), std::move(members), /*intercom=*/false);
			return;
		}

		// Hunt (sequential): build a broadcast-style session but ring one at a time.
		auto newSession = allocateSession(std::string(data->getCallID()), caller.value());
		if (!newSession)
		{
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*data);
			if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
			responseObj->setHeader("SIP/2.0 503 Service Unavailable");
			responseObj->clearBody();
			responseObj->setContact(buildContact(caller.value()->getNumber()));
			_outbox.emplace_back(data->getSource(), std::move(responseObj));
			return;
		}
		newSession->setBroadcast(true);
		newSession->setHunt(true);
		newSession->setGroupExt(destNumber);
		newSession->setInviteMessage(data);
		newSession->setHuntMembers(std::move(huntOrder));
		newSession->setHuntIndex(0);
		_sessions.emplace(data->getCallID(), newSession);

		// 180 Ringing back to the caller while we walk the list.
		auto ringing = getMessageFromPool(*data);
		if (!ringing) return;   // pool exhausted: drop, peer retransmits (#101A)
		ringing->setHeader("SIP/2.0 180 Ringing");
		ringing->clearBody();
		std::string activeIp = _localIp;
		ringing->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		ringing->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
		ringing->setContact(buildContact(destNumber));
		_outbox.emplace_back(data->getSource(), std::move(ringing));

		huntRingNext(newSession);   // ring the first member, arm its timeout
		return;
	}

	// Call Forward Unconditional (CFU): if the destination has an "always" forward
	// target, redirect the call to that target before ringing the original callee.
	{
		std::string cfu = getForwardTarget(destNumber, "always");
		if (!cfu.empty() && cfu != destNumber)
		{
			queueLog("CFU: forwarding " + destNumber + " -> " + cfu);
			if (redirectInvite(data, caller.value(), cfu))
			{
				return;
			}
			// Forward target offline: fall through and try the original callee.
		}
	}

	// Do Not Disturb (Phase 2): if the target extension has DND enabled, decline
	// with 480 Temporarily Unavailable instead of ringing it. This branch is reached
	// only for ordinary extensions — the virtual 777 (echo) and 999 (broadcast)
	// extensions are handled above and so are never affected by DND.
	if (isDndEnabled(destNumber))
	{
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader("SIP/2.0 480 Temporarily Unavailable");
		response->clearBody();
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		response->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), response);
		return;
	}

	// Check if the called is registered
	auto called = findClient(data->getToNumber());
	if (!called.has_value())
	{
		// Send "SIP/2.0 404 Not Found"
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*data);
		if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
		responseObj->setHeader(SipMessageTypes::NOT_FOUND);
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	SipSdpMessage* message = nullptr;
	if (data && data->hasSdp())
	{
		message = static_cast<SipSdpMessage*>(data.get());
	}
	if (!message) 
	{
		queueLog("Couldn't get SDP from " + std::string(data->getFromNumber()) + "'s INVITE request.", true);
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*data);
		if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
		responseObj->setHeader(SipMessageTypes::BAD_REQUEST);
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	auto newSession = allocateSession(std::string(data->getCallID()), caller.value());
	if (!newSession)
	{
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*data);
		if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
		responseObj->setHeader("SIP/2.0 503 Service Unavailable");
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}
	_sessions.emplace(data->getCallID(), newSession);

	// Retain the original INVITE on the session if the callee has ANY conditional
	// forward (busy or no-answer), so onBusy()/tick() can re-drive it to the forward
	// target. CFB (busy) is handled in onBusy(); CFNA arms a one-shot ring timer that
	// tick() fires after NO_ANSWER_TIMEOUT (CANCEL original leg, INVITE the target).
	std::string cfb  = getForwardTarget(destNumber, "busy");
	std::string cfna = getForwardTarget(destNumber, "noanswer");
	if (!cfb.empty() || !cfna.empty())
	{
		newSession->setInviteMessage(data);
	}
	if (!cfna.empty() && cfna != destNumber)
	{
		newSession->setNoAnswerTarget(cfna);
		newSession->armRingTimer(std::chrono::steady_clock::now() + NO_ANSWER_TIMEOUT);
	}

	auto response = getMessageFromPool(*data);
	if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
	response->setContact(buildContact(caller.value()->getNumber()));
	endHandle(data->getToNumber(), response);
}

std::string RequestsHandler::buildMediaSdp(const std::string& serverIp, int rtpPort)
{
	// The server's OWN SDP offer/answer for the 440 tone stream. PCMU (PT 0) only —
	// matches enforceG711()/the codec the rest of the PBX speaks. Sendonly: the
	// server streams to the caller and ignores any media the caller sends back.
	// CRLF line endings throughout so Content-Length (computed by the caller via
	// syncContentLength()) matches the wire bytes exactly.
	std::string s;
	s += "v=0\r\n";
	s += "o=- 0 0 IN IP4 " + serverIp + "\r\n";
	s += "s=pocketdial-media\r\n";
	s += "c=IN IP4 " + serverIp + "\r\n";
	s += "t=0 0\r\n";
	s += "m=audio " + std::to_string(rtpPort) + " RTP/AVP 0\r\n";
	s += "a=rtpmap:0 PCMU/8000\r\n";
	s += "a=sendonly\r\n";
	return s;
}

bool RequestsHandler::parseCallerRtp(const std::shared_ptr<SipMessage>& invite,
	std::string& outIp, uint16_t& outPort)
{
	// Port: the m=audio port from the caller's offered SDP (0 if absent/invalid).
	int port = 0;
	if (invite && invite->hasSdp())
	{
		auto* sdp = static_cast<SipSdpMessage*>(invite.get());
		port = sdp->getRtpPort();
	}
	if (port <= 0 || port > 65535)
	{
		return false;
	}
	outPort = static_cast<uint16_t>(port);

	// IP: prefer the SDP c= line ("c=IN IP4 <addr>"); fall back to the INVITE source
	// IP (handles phones that put 0.0.0.0 or a private/NAT addr in c=).
	outIp.clear();
	if (invite && invite->hasSdp())
	{
		auto* sdp = static_cast<SipSdpMessage*>(invite.get());
		std::string_view c = sdp->getConnectionInformation();   // "c=IN IP4 1.2.3.4"
		size_t ip4 = c.find("IP4 ");
		if (ip4 != std::string_view::npos)
		{
			size_t start = ip4 + 4;
			while (start < c.size() && std::isspace(static_cast<unsigned char>(c[start]))) ++start;
			size_t end = start;
			while (end < c.size() && (std::isdigit(static_cast<unsigned char>(c[end])) || c[end] == '.')) ++end;
			std::string candidate(c.substr(start, end - start));
			if (!candidate.empty() && candidate != "0.0.0.0")
			{
				outIp = candidate;
			}
		}
	}
	if (outIp.empty() && invite)
	{
		char ipBuf[INET_ADDRSTRLEN]{};
		sockaddr_in src = invite->getSource();
		inet_ntop(AF_INET, &src.sin_addr, ipBuf, sizeof(ipBuf));
		outIp = ipBuf;
	}
	return !outIp.empty();
}

void RequestsHandler::onMediaInvite(std::shared_ptr<SipMessage> data,
	const std::shared_ptr<SipClient>& caller)
{
	std::string activeIp = _localIp;

	// Single-stream cap: a 2nd dial of 440 while a stream is live is rejected so the
	// one media slot/socket/task is never double-booked (degrade gracefully).
	if (_rtpSender.isActive())
	{
		auto busy = getMessageFromPool(*data);
		if (!busy) return;   // pool exhausted: drop, peer retransmits (#101A)
		busy->setHeader("SIP/2.0 486 Busy Here");
		busy->clearBody();
		busy->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		busy->setContact(buildContact("440"));
		_outbox.emplace_back(data->getSource(), std::move(busy));
		queueLog("440 media: busy (one stream max), rejected " + std::string(data->getFromNumber()));
		return;
	}

	// Resolve where to stream: caller's RTP addr:port (c= line + m= port, src fallback).
	std::string destIp;
	uint16_t destPort = 0;
	if (!parseCallerRtp(data, destIp, destPort))
	{
		auto bad = getMessageFromPool(*data);
		if (!bad) return;   // pool exhausted: drop, peer retransmits (#101A)
		bad->setHeader(SipMessageTypes::BAD_REQUEST);
		bad->clearBody();
		bad->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		bad->setContact(buildContact("440"));
		_outbox.emplace_back(data->getSource(), std::move(bad));
		queueLog("440 media: no usable RTP destination in INVITE from "
			+ std::string(data->getFromNumber()), true);
		return;
	}

	// Start the RTP tone stream FIRST. Sending 200 OK before the stream is up risks
	// answering the call (caller hears nothing) when the socket/task fails to start,
	// recoverable only by the caller hanging up. On failure answer 500 instead.
	if (!_rtpSender.start(destIp, destPort, std::string(data->getCallID())))
	{
		auto err = getMessageFromPool(*data);
		if (!err) return;   // pool exhausted: drop, peer retransmits (#101A)
		err->setHeader("SIP/2.0 500 Server Internal Error");
		err->clearBody();
		err->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		err->setContact(buildContact("440"));
		_outbox.emplace_back(data->getSource(), std::move(err));
		queueLog("440 media: RTP stream failed to start to " + destIp + ":"
			+ std::to_string(destPort), true);
		return;
	}

	// Track a Session so the dashboard shows the call and CDR is recorded on teardown.
	// Per-session dummy dest (never a shared client) so a concurrent 777/440 can't
	// overwrite this call's destination identity. If the session pool is full, stop
	// the stream we just started and answer 503 rather than streaming an untracked
	// call that nothing can later tear down.
	auto dummy440 = std::make_shared<SipClient>();
	dummy440->reset("440", data->getSource(), 3600);
	auto newSession = allocateSession(std::string(data->getCallID()), caller);
	if (!newSession)
	{
		_rtpSender.stop(std::string(data->getCallID()));
		auto busy = getMessageFromPool(*data);
		if (!busy) return;   // pool exhausted: drop, peer retransmits (#101A)
		busy->setHeader("SIP/2.0 503 Service Unavailable");
		busy->clearBody();
		busy->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		busy->setContact(buildContact("440"));
		_outbox.emplace_back(data->getSource(), std::move(busy));
		queueLog("440 media: session pool full, rejected " + std::string(data->getFromNumber()), true);
		return;
	}
	newSession->setDest(dummy440);
	_sessions.emplace(data->getCallID(), newSession);
	newSession->setState(Session::State::Connected);

	// Build the 200 OK carrying the SERVER's own SDP. We rebuild the message body
	// directly (there is no generic body setter): take the INVITE clone, strip its
	// body, then append our SDP and the SDP Content-Type, and resync Content-Length
	// via enforceG711()/syncContentLength() so the answer isn't dropped on UDP (the
	// 777-bug class — see tests/SipMessage_test.cpp).
	std::string toTag = IDGen::GenerateID(9);
	std::string sdpBody = buildMediaSdp(activeIp, _rtpSender.serverRtpPort());

	// Assemble the OK from the INVITE's headers + our body. clearBody() leaves the
	// header/blank-line boundary intact; we then append Content-Type + the SDP and
	// let syncContentLength() (invoked by enforceG711) fix the length.
	auto ok = getMessageFromPool(*data);
	if (!ok) return;   // pool exhausted: drop, peer retransmits (#101A)
	ok->setHeader(SipMessageTypes::OK);
	ok->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	ok->setTo(std::string(data->getTo()) + ";tag=" + toTag);
	ok->setContact(buildContact("440"));
	ok->clearBody();
	// Append our SDP body after the header/body separator. We rebuild the raw string
	// because clearBody() emptied the body and zeroed length. The cloned INVITE
	// already carries "Content-Type: application/sdp" (which clearBody() does NOT
	// strip), so only add the header if it is somehow absent — never duplicate it.
	{
		std::string raw = ok->toString();
		size_t sep = raw.find("\r\n\r\n");
		if (sep != std::string::npos)
		{
			std::string_view headerView(raw.data(), sep);
			if (headerView.find("application/sdp") == std::string_view::npos)
			{
				// No SDP Content-Type yet: splice one in just before the blank line.
				raw.insert(sep, "\r\nContent-Type: application/sdp");
				sep = raw.find("\r\n\r\n");   // separator moved by the inserted bytes
			}
			raw.erase(sep + 4);          // drop anything stale after the separator
			raw += sdpBody;              // append our SDP body
		}
		ok->reset(std::move(raw), data->getSource());
	}
	ok->enforceG711();                   // collapse codec list (no-op here) + sync length
	ok->syncContentLength();             // belt-and-suspenders: length == body bytes
	_outbox.emplace_back(data->getSource(), std::move(ok));

	queueLog("440 media: streaming tone to " + destIp + ":" + std::to_string(destPort)
		+ " (callID=" + std::string(data->getCallID()) + ")");
}

void RequestsHandler::onTrying(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		return;
	}
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onRinging(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		return;
	}
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onBusy(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		// Hunt group: a busy member means advance to the next one (the timer is
		// disarmed inside huntRingNext). If the list is exhausted, fail to caller.
		if (session.value()->isHunt())
		{
			if (session.value()->getState() == Session::State::Invited && !huntRingNext(session.value()))
			{
				endHandle(session.value()->getSrc()->getNumber(), data);
				endCall(data->getCallID(), session.value()->getSrc()->getNumber(),
					session.value()->getGroupExt(), "hunt group exhausted (busy)");
			}
			return;
		}

		session.value()->removePendingTarget(std::string(data->getFromNumber()));
		if (session.value()->getPendingTargets().empty() && session.value()->getState() == Session::State::Invited)
		{
			endHandle(session.value()->getSrc()->getNumber(), data);
			endCall(data->getCallID(), session.value()->getSrc()->getNumber(), "999", "all targets busy");
		}
		return;
	}

	// Call Forward Busy (CFB): if the busy callee has an on-busy forward target,
	// swallow the 486 and redirect the call there instead of failing the caller.
	if (session.has_value())
	{
		std::string busyExt(data->getFromNumber());
		std::string cfb = getForwardTarget(busyExt, "busy");
		if (!cfb.empty() && cfb != busyExt)
		{
			auto inviteMsg = session.value()->getInviteMessage();
			auto src = session.value()->getSrc();
			if (inviteMsg && src)
			{
				queueLog("CFB: " + busyExt + " busy, forwarding -> " + cfb);
				std::string callID(data->getCallID());
				// Tear down the busy leg's session, then start a fresh leg to the
				// forward target reusing the retained original INVITE.
				endCall(callID, src->getNumber(), busyExt, "forwarded on busy");
				if (redirectInvite(inviteMsg, src, cfb))
				{
					return;
				}
			}
		}
	}

	setCallState(data->getCallID(), Session::State::Busy);
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onUnavailable(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		// Hunt group: treat unavailable like busy — advance to the next member.
		if (session.value()->isHunt())
		{
			if (session.value()->getState() == Session::State::Invited && !huntRingNext(session.value()))
			{
				endHandle(session.value()->getSrc()->getNumber(), data);
				endCall(data->getCallID(), session.value()->getSrc()->getNumber(),
					session.value()->getGroupExt(), "hunt group exhausted (unavailable)");
			}
			return;
		}

		session.value()->removePendingTarget(std::string(data->getFromNumber()));
		if (session.value()->getPendingTargets().empty() && session.value()->getState() == Session::State::Invited)
		{
			endHandle(session.value()->getSrc()->getNumber(), data);
			endCall(data->getCallID(), session.value()->getSrc()->getNumber(), "999", "all targets unavailable");
		}
		return;
	}
	setCallState(data->getCallID(), Session::State::Unavailable);
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onBye(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	std::string destNumber(data->getToNumber());

	// Issue #46: reject an off-path forged teardown. A BYE for an established
	// two-phone dialog must originate from one of the call's leg IPs.
	if (session.has_value() &&
		!isDialogSourceAuthorized(session.value(), data->getSource()))
	{
		queueLog("BYE for Call-ID " + std::string(data->getCallID()) +
			" rejected: source not a dialog leg (spoofed teardown)", true);
		_registrar.sendForbidden(data, "Forbidden");
		return;
	}

	if (destNumber == "777")
	{
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (destNumber == "440")
	{
		// Media beachhead teardown: stop the RTP tone stream (only if it owns this
		// Call-ID), 200 OK the BYE, and end the session. Stream stop is idempotent.
		_rtpSender.stop(std::string(data->getCallID()));
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (destNumber == "999" || isPageZoneDialog(destNumber))
	{
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));

		if (session.has_value())
		{
			auto answeringClient = session.value()->getDest();
			if (answeringClient)
			{
				auto byeFork = getMessageFromPool(*data);
				if (!byeFork) return;   // pool exhausted: drop, peer retransmits (#101A)
				std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);
				std::string targetIpPort = sipwire::addrToIpPort(answeringClient->getAddress());

				byeFork->setHeader("BYE sip:" + answeringClient->getNumber() + "@" + targetIpPort + " SIP/2.0");

				std::string originalTo(data->getTo());
				std::string newTo = "To: <sip:" + answeringClient->getNumber() + "@" + serverIpPort + ">";
				siphdr::appendTagFrom(newTo, originalTo);
				byeFork->setTo(newTo);

				_outbox.emplace_back(answeringClient->getAddress(), std::move(byeFork));
			}
		}
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	setCallState(data->getCallID(), Session::State::Bye);
	endHandle(data->getToNumber(), data);
}

void RequestsHandler::onOk(std::shared_ptr<SipMessage> data)
{
	if (data->getCSeq().find("OPTIONS") != std::string::npos)
	{
		return;
	}

	// Register-beep dialog (server-originated UAC, no Session): recognised by
	// Call-ID before the normal session lookup. handleOk drives ACK→BYE→free.
	if (_beeper.handleOk(data))
	{
		return;
	}

	// Park dialogs (server-originated re-INVITE ACKs + ring-back answers). The
	// snapshot mirror is driven by _park.consumeParkChanged() in handle(), so a
	// state-neutral ACK confirmation no longer pays a rebuild.
	if (_park.handleOk(data))
	{
		return;
	}
	auto session = getSession(data->getCallID());
	if (session.has_value())
	{
		if (session.value()->getState() == Session::State::Cancel)
		{
			endHandle(data->getFromNumber(), data);
			return;
		}

		if (data->getCSeq().find(SipMessageTypes::INVITE) != std::string::npos)
		{
			// Re-INVITE answer (hold/resume): relay 200 OK to the opposite leg and
			// preserve the session state set by onReinvite() — do NOT re-run connect.
			{
				const auto st = session.value()->getState();
				if ((st == Session::State::Connected || st == Session::State::Held) &&
					!session.value()->isBroadcast())
				{
					auto legSrc  = session.value()->getSrc();
					auto legDest = session.value()->getDest();
					if (legSrc && legDest)
					{
						std::shared_ptr<SipClient> peer;
						if (sameAddress(data->getSource(), legSrc->getAddress()))
						{
							peer = legDest;
						}
						else if (sameAddress(data->getSource(), legDest->getAddress()))
						{
							if (!data->getBody().empty())
								session.value()->setRemoteSdp(std::string(data->getBody()));
							peer = legSrc;
						}
						if (peer)
						{
							_outbox.emplace_back(peer->getAddress(), data);
							return;
						}
					}
				}
			}

			if (session.value()->isBroadcast())
			{
				// Only the first answer from a pending fork (Invited state) should run
				// the connect path. A re-INVITE 200 OK from an already-connected or held
				// broadcast call (Held / Connected state) falls through to a silent drop
				// below — the peer relay above was already skipped by !isBroadcast(), so
				// the hold/resume path for broadcast calls is currently unsupported and
				// the 200 OK is discarded (the hold stays in place). (#69)
				if (session.value()->getState() == Session::State::Invited)
				{
					auto clientOpt = findClientByAddress(data->getSource());
					if (!clientOpt.has_value())
					{
						return;
					}
					auto answeringClient = clientOpt.value();

					SipSdpMessage* sdpMessage = nullptr;
					if (data->hasSdp())
					{
						sdpMessage = static_cast<SipSdpMessage*>(data.get());
					}
					if (!sdpMessage)
					{
						queueLog("Couldn't get SDP from: " + answeringClient->getNumber() + "'s broadcast OK message.", true);
						return;
					}

					session->get()->setDest(answeringClient);
					session->get()->setState(Session::State::Connected);
					session->get()->clearRingTimer();   // hunt answered: disarm timeout

					auto inviteMsg = session.value()->getInviteMessage();

					auto response = getMessageFromPool(*data);
					if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
					response->setContact(buildContact(answeringClient->getNumber()));

					if (inviteMsg)
					{
						std::string originalTo(inviteMsg->getTo());
						std::string bTo(data->getTo());
						siphdr::appendTagFrom(originalTo, bTo);
						response->setTo(originalTo);
					}

					response->enforceG711();
					endHandle(session.value()->getSrc()->getNumber(), std::move(response));

					if (inviteMsg)
					{
						std::string originalCSeq(inviteMsg->getCSeq());
						size_t invitePos = originalCSeq.find("INVITE");
						if (invitePos != std::string::npos)
						{
							originalCSeq.replace(invitePos, 6, "CANCEL");
						}

						for (const auto& target : session.value()->getPendingTargets())
						{
							if (target->getNumber() != answeringClient->getNumber())
							{
								auto cancelMsg = getMessageFromPool(*inviteMsg);
								if (!cancelMsg) continue;   // pool exhausted: skip this target (#101A)
								std::string targetIpPort = sipwire::addrToIpPort(target->getAddress());

								cancelMsg->setHeader("CANCEL sip:" + target->getNumber() + "@" + targetIpPort + " SIP/2.0");
								cancelMsg->setCSeq(originalCSeq);
								_outbox.emplace_back(target->getAddress(), std::move(cancelMsg));
							}
						}
					}
				}
				return;
			}

			auto client = findClient(data->getToNumber());
			if (!client.has_value())
			{
				return;
			}

			SipSdpMessage* sdpMessage = nullptr;
			if (data->hasSdp())
			{
				sdpMessage = static_cast<SipSdpMessage*>(data.get());
			}
			if (!sdpMessage) 
			{
				queueLog("Couldn't get SDP from: " + client.value()->getNumber() + "'s OK message.", true);
				std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*data);
				if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
				responseObj->setHeader(SipMessageTypes::BAD_REQUEST);
				responseObj->clearBody();
				responseObj->setContact(buildContact(data->getToNumber()));
				endHandle(data->getToNumber(), responseObj);
				endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), "SDP parse error.");
				return;
			}
			session->get()->setDest(client.value());
			session->get()->setState(Session::State::Connected);
			session->get()->clearRingTimer();   // answered: disarm any CFNA timeout
			// Capture dialog From/To and callee SDP so attended transfer can
			// cross-connect two live sessions without querying stored invite messages.
			session->get()->setDialogHeaders(std::string(data->getFrom()),
			                                std::string(data->getTo()));
			session->get()->setRemoteSdp(std::string(data->getBody()));
			armSessionTimer(session->get(), data);
			auto response = getMessageFromPool(*data);
			if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
			response->setContact(buildContact(data->getToNumber()));
			endHandle(data->getFromNumber(), std::move(response));
			return;
		}

		if (session.value()->getState() == Session::State::Bye)
		{
			endHandle(data->getFromNumber(), data);
			endCall(data->getCallID(), data->getToNumber(), data->getFromNumber());
		}
	}
}

void RequestsHandler::onAck(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (!session.has_value())
	{
		return;
	}

	std::string destNumber(data->getToNumber());
	if (destNumber == "777")
	{
		return;
	}

	if (destNumber == "999")
	{
		auto answeringClient = session.value()->getDest();
		if (answeringClient)
		{
			auto ackFork = getMessageFromPool(*data);
			if (!ackFork) return;   // pool exhausted: drop, peer retransmits (#101A)
			std::string activeIp = _localIp;
			std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);
			std::string targetIpPort = sipwire::addrToIpPort(answeringClient->getAddress());

			ackFork->setHeader("ACK sip:" + answeringClient->getNumber() + "@" + targetIpPort + " SIP/2.0");

			std::string originalTo(data->getTo());
			std::string newTo = "To: <sip:" + answeringClient->getNumber() + "@" + serverIpPort + ">";
			siphdr::appendTagFrom(newTo, originalTo);
			ackFork->setTo(newTo);

			_outbox.emplace_back(answeringClient->getAddress(), std::move(ackFork));
		}
		return;
	}

	endHandle(data->getToNumber(), data);

	auto sessionState = session.value()->getState();
	std::string endReason;
	if (sessionState == Session::State::Busy)
	{
		endReason = std::string(data->getToNumber()) + " is busy.";
		endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), endReason);
		return;
	}

	if (sessionState == Session::State::Unavailable)
	{
		endReason = std::string(data->getToNumber()) + " is unavailable.";
		endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), endReason);
		return;
	}

	if (sessionState == Session::State::Cancel)
	{
		endReason = std::string(data->getFromNumber()) + " canceled the session.";
		endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), endReason);
		return;
	}
}

void RequestsHandler::onRefer(std::shared_ptr<SipMessage> data)
{
	// Blind (unattended) transfer, RFC 3515. The transferor (the party that holds
	// the call and pressed "transfer") sends REFER with a Refer-To header naming the
	// new target. We ack 202 Accepted, then drive a fresh INVITE from the transferor
	// to the target, and report progress with a NOTIFY (Event: refer + sipfrag body).
	// Attended transfer (Refer-To carrying a Replaces= dialog) is OUT OF SCOPE — see
	// the summary; such a REFER is treated as a blind transfer to the named target.
	auto transferorOpt = findClient(data->getFromNumber());
	if (!transferorOpt.has_value())
	{
		// Unknown transferor: reject (consistent with onInvite's 403 for non-registered).
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader("SIP/2.0 403 Forbidden");
		response->clearBody();
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}
	auto transferor = transferorOpt.value();

	// Pull the Refer-To header value out of the raw message and extract the target.
	std::string target;
	{
		const std::string& raw = data->toString();
		// Case-insensitive scan for a "Refer-To:" header line (no compact form in 3515).
		size_t pos = 0;
		while (pos < raw.size())
		{
			size_t lineEnd = raw.find('\n', pos);
			size_t next = (lineEnd == std::string::npos) ? raw.size() : lineEnd + 1;
			if ((raw[pos] == 'r' || raw[pos] == 'R') && next - pos >= 9)
			{
				std::string name = raw.substr(pos, 9);
				std::transform(name.begin(), name.end(), name.begin(),
					[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
				if (name == "refer-to:")
				{
					size_t valEnd = (lineEnd == std::string::npos) ? raw.size() : lineEnd;
					std::string value = raw.substr(pos + 9, valEnd - (pos + 9));
					target = pbx::parseReferToTarget(value);
					break;
				}
			}
			else if (raw[pos] == '\r' || raw[pos] == '\n')
			{
				break; // header/body boundary
			}
			pos = next;
		}
	}

	if (target.empty() || !isValidAor(target))
	{
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader(SipMessageTypes::BAD_REQUEST);
		response->clearBody();
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	// 202 Accepted to the transferor (RFC 3515 §2.4.4).
	{
		auto accepted = getMessageFromPool(*data);
		if (!accepted) return;   // pool exhausted: drop, peer retransmits (#101A)
		accepted->setHeader(SipMessageTypes::ACCEPTED);
		accepted->clearBody();
		std::string activeIp = _localIp;
		accepted->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		accepted->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
		_outbox.emplace_back(data->getSource(), std::move(accepted));
	}

	// Blind transfer drops the transferor's ORIGINAL call. Tear that leg down FIRST,
	// then drive the new INVITE — ordering is load-bearing: redirectInvite() reuses the
	// Session stored under this Call-ID, so ending the call AFTER it (the previous
	// order) erased the freshly-created transfer leg, and the target's 200 OK then
	// matched no session and the transfer silently never completed. onBusy()/tick()
	// (CFB/CFNA) end-then-redirect for exactly this reason. CDR is recorded as the
	// original leg tears down; redirectInvite() then allocates a clean session.
	std::string callID(data->getCallID());
	auto targetClient = findClient(target);

	endCall(callID, transferor->getNumber(), std::string(data->getToNumber()), "blind transfer");

	bool ok = targetClient.has_value() && redirectInvite(data, transferor, target);

	// NOTIFY the transferor with the transfer result (message/sipfrag body).
	std::string frag = ok ? "SIP/2.0 200 OK" : "SIP/2.0 404 Not Found";
	auto notify = buildReferNotify(data, transferor, frag, /*terminated=*/true);
	if (notify)
	{
		_outbox.emplace_back(transferor->getAddress(), std::move(notify));
	}

	if (!ok)
	{
		queueLog("REFER: blind transfer to " + target + " failed (target not registered)", true);
	}
	else
	{
		queueLog("REFER: blind transfer " + transferor->getNumber() + " -> " + target);
	}
}

void RequestsHandler::onMessage(std::shared_ptr<SipMessage> data)
{
	// Inbound MESSAGE hygiene (RFC 3428). Phones may send delivery receipts / IMs;
	// if we don't 200 them they retransmit. We do NOT interpret the body and the
	// server never originates a MESSAGE. Simple stateless ack, mirroring onOptions().
	auto response = getMessageFromPool(*data);
	if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
	response->setHeader(SipMessageTypes::OK);
	response->clearBody();
	std::string activeIp = _localIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
	_outbox.emplace_back(data->getSource(), std::move(response));
}

void RequestsHandler::buildInviteFork(const std::shared_ptr<SipMessage>& invite,
	const std::shared_ptr<SipClient>& caller,
	const std::shared_ptr<SipClient>& target,
	bool intercom)
{
	auto inviteFork = getMessageFromPool(*invite);
	if (!inviteFork) return;   // pool exhausted: drop, peer retransmits (#101A)
	inviteFork->setContact(buildContact(caller->getNumber()));

	std::string activeIp = _localIp;
	std::string targetIpPort = sipwire::addrToIpPort(target->getAddress());
	std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

	inviteFork->setHeader("INVITE sip:" + target->getNumber() + "@" + targetIpPort + " SIP/2.0");
	inviteFork->setTo("To: <sip:" + target->getNumber() + "@" + serverIpPort + ">");

	if (intercom)
	{
		// Auto-answer / intercom headers — used by 999 all-page only. A ring group
		// omits these so members ring normally (the caller can be picked up by hand).
		inviteFork->addHeader("Call-Info", "<sip:any>;answer-after=0");
		inviteFork->addHeader("Alert-Info", "info=alert-autoanswer");
		inviteFork->addHeader("Alert-Info", "answer-after=0");
		inviteFork->addHeader("Alert-Info", "intercom=true");
		inviteFork->addHeader("P-Auto-Answer", "normal");
	}
	inviteFork->enforceG711();
	_outbox.emplace_back(target->getAddress(), std::move(inviteFork));
}

void RequestsHandler::startBroadcastFork(std::shared_ptr<SipMessage> invite,
	std::shared_ptr<SipClient> caller,
	const std::vector<std::shared_ptr<SipClient>>& targets,
	bool intercom)
{
	// Shared fan-out core: build the broadcast Session, send 180 Ringing to the
	// caller, then one forked INVITE per target. First answer wins; onOk() cancels
	// the losers (it walks getPendingTargets()). Used by 999 (intercom=true) and
	// ring-all groups (intercom=false).
	auto newSession = allocateSession(std::string(invite->getCallID()), caller);
	if (!newSession)
	{
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*invite);
		if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
		responseObj->setHeader("SIP/2.0 503 Service Unavailable");
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller->getNumber()));
		_outbox.emplace_back(invite->getSource(), std::move(responseObj));
		return;
	}
	newSession->setBroadcast(true);
	newSession->setPendingTargets(targets);
	newSession->setInviteMessage(invite);
	_sessions.emplace(invite->getCallID(), newSession);

	std::string contactExt = intercom ? std::string("999") : std::string(invite->getToNumber());

	auto ringing = getMessageFromPool(*invite);
	if (!ringing) return;   // pool exhausted: drop, peer retransmits (#101A)
	ringing->setHeader("SIP/2.0 180 Ringing");
	ringing->clearBody();
	std::string activeIp = _localIp;
	ringing->setVia(std::string(invite->getVia()) + ";received=" + activeIp);
	ringing->setTo(std::string(invite->getTo()) + ";tag=" + IDGen::GenerateID(9));
	ringing->setContact(buildContact(contactExt));
	_outbox.emplace_back(invite->getSource(), std::move(ringing));

	for (auto& target : targets)
	{
		buildInviteFork(invite, caller, target, intercom);
	}
}

bool RequestsHandler::huntRingNext(const std::shared_ptr<Session>& session)
{
	// Ring the next not-yet-tried hunt member. Returns false when the list is
	// exhausted (caller fails the call). The single ringing member is kept in
	// getPendingTargets() so onOk()/onCancel() can address it like a broadcast.
	auto& members = session->getHuntMembers();
	auto invite = session->getInviteMessage();
	auto caller = session->getSrc();
	if (!invite || !caller)
	{
		return false;
	}

	while (session->getHuntIndex() < members.size())
	{
		std::string ext = members[session->getHuntIndex()];
		session->setHuntIndex(session->getHuntIndex() + 1);

		auto mc = findClient(ext);
		if (!mc.has_value())
		{
			continue;   // member went offline since the call started; skip it
		}

		session->setPendingTargets({ mc.value() });
		buildInviteFork(invite, caller, mc.value(), /*intercom=*/false);
		session->armRingTimer(std::chrono::steady_clock::now() + NO_ANSWER_TIMEOUT);
		return true;
	}

	session->clearRingTimer();
	return false;
}

bool RequestsHandler::redirectInvite(const std::shared_ptr<SipMessage>& invite,
	const std::shared_ptr<SipClient>& caller,
	const std::string& target)
{
	// Re-point an INVITE at `target` and send it as a fresh leg. Powers blind
	// transfer and the call-forward redirect paths. A new Session is allocated under
	// the SAME Call-ID so subsequent responses (180/200/BYE) route normally.
	auto targetClient = findClient(target);
	if (!targetClient.has_value())
	{
		return false;
	}

	// Don't double-allocate if a session for this Call-ID already exists (e.g. CFU
	// from onInvite, which hasn't created one yet) — reuse or create as needed.
	std::shared_ptr<Session> session;
	auto existing = getSession(invite->getCallID());
	if (existing.has_value())
	{
		session = existing.value();
		session->setDest(targetClient.value());
	}
	else
	{
		session = allocateSession(std::string(invite->getCallID()), caller);
		if (!session)
		{
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*invite);
			if (!responseObj) return false;   // pool exhausted: drop, peer retransmits (#101A)
			responseObj->setHeader("SIP/2.0 503 Service Unavailable");
			responseObj->clearBody();
			responseObj->setContact(buildContact(caller->getNumber()));
			_outbox.emplace_back(invite->getSource(), std::move(responseObj));
			return true;   // we DID handle it (with a 503); target lookup succeeded
		}
		_sessions.emplace(invite->getCallID(), session);
	}

	buildInviteFork(invite, caller, targetClient.value(), /*intercom=*/false);
	return true;
}

std::shared_ptr<SipMessage> RequestsHandler::buildReferNotify(const std::shared_ptr<SipMessage>& refer,
	const std::shared_ptr<SipClient>& transferor,
	const std::string& sipfrag,
	bool terminated)
{
	// RFC 3515 §2.4.5 NOTIFY: Event: refer + message/sipfrag body reporting the
	// transfer result. Sent within the REFER's dialog back to the transferor.
	std::string activeIp = _localIp;
	std::string destIpPort = sipwire::addrToIpPort(transferor->getAddress());
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);

	std::string body = sipfrag + "\r\n";
	std::string subState = terminated ? "terminated;reason=noresource" : "active;expires=60";

	std::ostringstream ss;
	ss << "NOTIFY sip:" << transferor->getNumber() << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   // getTo()/getFrom()/getCallID() hand back the FULL header line, so the value
	   // has to be unwrapped before it is re-stamped under a new name — otherwise
	   // this NOTIFY goes out as "From: To: <sip:...>" and the transferor cannot
	   // match it to the REFER dialog. Roles swap (RFC 3515 §2.4.5).
	   << "From: " << stripHeaderName(refer->getTo()) << "\r\n"
	   << "To: " << stripHeaderName(refer->getFrom()) << "\r\n"
	   << "Call-ID: " << stripHeaderName(refer->getCallID()) << "\r\n"
	   << "CSeq: 2 NOTIFY\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Event: refer\r\n"
	   << "Subscription-State: " << subState << "\r\n"
	   << "Contact: <sip:server@" << srcIpPort << ">\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: message/sipfrag;version=2.0\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	return getMessageFromPool(ss.str(), transferor->getAddress());
}

std::shared_ptr<SipMessage> RequestsHandler::buildCancel(const std::shared_ptr<SipMessage>& invite,
	const std::shared_ptr<SipClient>& target)
{
	// Build a CANCEL for an outstanding forked INVITE leg toward `target`, derived
	// from the original INVITE (same Call-ID / branch). Mirrors the inline CANCEL
	// construction used by onCancel()/onOk() for the 999 path.
	std::string activeIp = _localIp;
	std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

	auto cancelMsg = getMessageFromPool(*invite);
	if (!cancelMsg) return nullptr;   // pool exhausted: propagate, caller drops (#101A)

	std::string targetIpPort = sipwire::addrToIpPort(target->getAddress());

	cancelMsg->setHeader("CANCEL sip:" + target->getNumber() + "@" + targetIpPort + " SIP/2.0");
	cancelMsg->setTo("To: <sip:" + target->getNumber() + "@" + serverIpPort + ">");

	std::string cseq(invite->getCSeq());
	size_t invitePos = cseq.find("INVITE");
	if (invitePos != std::string::npos)
	{
		cseq.replace(invitePos, 6, "CANCEL");
		cancelMsg->setCSeq(cseq);
	}
	cancelMsg->clearBody();
	return cancelMsg;
}

bool RequestsHandler::setCallState(std::string_view callID, Session::State state)
{
	auto session = getSession(callID);
	if (session)
	{
		session->get()->setState(state);
		return true;
	}
	return false;
}

void RequestsHandler::endCall(std::string_view callID, std::string_view srcNumber, std::string_view destNumber, std::string_view reason)
{
	// DTMF accumulators are keyed by Call-ID and share the dialog lifecycle; drop
	// this dialog's entry so _dtmfState can't grow unbounded across calls (Fix #4).
	_dtmfState.erase(std::string(callID));

	// RFC 3261 §17: free any transaction slots tracking retransmits for this call.
	_txLayer.freeForCallId(callID);
	// Free any park orbit slot holding this call's parked leg.
	_park.freeForCallId(callID);

	// Capture the session (for CDR start time / final state) BEFORE we erase it.
	std::shared_ptr<Session> ending;
	auto sit = _sessions.find(std::string(callID));
	if (sit != _sessions.end())
	{
		ending = sit->second;
	}

	if (_sessions.erase(std::string(callID)) > 0)
	{
		// Record exactly once per torn-down dialog (Phase 2 CDR).
		recordCdr(ending, srcNumber, destNumber);

		std::ostringstream message;
		message << "Session has been disconnected between " << srcNumber << " and " << destNumber;
		if (!reason.empty())
		{
			message << " because " << reason;
		}
		queueLog(message.str());
	}

	for (auto& session : _sessionPool)
	{
		if (session->getCallID() == callID)
		{
			session->release();
			break;
		}
	}

	// Media beachhead safety net: if the dialog being torn down owns the live RTP
	// tone stream (BYE/CANCEL paths already call stop(); this also covers lease
	// expiry / force-disconnect / hunt cleanup that route through endCall()), stop
	// it so the socket + 20 ms task never leak past the call. Idempotent no-op when
	// the stream is idle or owned by a different Call-ID.
	_rtpSender.stop(std::string(callID));
}

uint64_t RequestsHandler::nowEpochMs() const
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

void RequestsHandler::recordCdr(const std::shared_ptr<Session>& session,
	std::string_view srcNumber, std::string_view destNumber)
{
	CallDetailRecord rec;
	rec.caller = std::string(srcNumber);
	rec.callee = std::string(destNumber);

	uint64_t startMs = nowEpochMs();
	uint32_t durationSec = 0;
	CdrResult result = CdrResult::Failed;

	if (session)
	{
		auto now = std::chrono::steady_clock::now();
		startMs = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				session->getStartTime().time_since_epoch()).count());

		switch (session->getState())
		{
			// Both Connected and Bye are "answered": a normal call ends via BYE, which
			// sets the state to Bye (NOT Connected) just before endCall() runs, while
			// the echo (777) path tears down straight from Connected. Session::setState
			// resets _startTime to the connect instant on the Connected transition and
			// the later Bye transition does NOT touch it, so getStartTime() still marks
			// the answer instant in both cases — talk time is now - startTime.
			case Session::State::Connected:
			case Session::State::Held:   // call torn down mid-hold → still Answered (#73)
			case Session::State::Bye:
				result = CdrResult::Answered;
				{
					int64_t secs = static_cast<int64_t>(
						std::chrono::duration_cast<std::chrono::seconds>(
							now - session->getStartTime()).count());
					if (secs < 0) secs = 0;
					durationSec = static_cast<uint32_t>(secs);
				}
				break;
			case Session::State::Busy:        result = CdrResult::Busy;        break;
			case Session::State::Cancel:      result = CdrResult::Cancelled;   break;
			case Session::State::Unavailable: result = CdrResult::Unavailable; break;
			default:                          result = CdrResult::Failed;      break;
		}
	}

	rec.startMs = startMs;
	rec.durationSec = durationSec;
	rec.result = result;

	// Fixed ring write: overwrite the oldest slot once full (no heap growth).
	_cdrRing[_cdrHead] = std::move(rec);
	_cdrHead = (_cdrHead + 1) % POCKETDIAL_CDR_RECORDS;
	if (_cdrCount < POCKETDIAL_CDR_RECORDS)
	{
		++_cdrCount;
	}

	// Persist the ring so records survive reboot (write-through on teardown; no-op
	// on host). Caller (endCall) holds _mutex. See persistCdrRing() for the wear note.
	persistCdrRing();
}

void RequestsHandler::unregisterClient(std::string_view number)
{
	queueLog("Unregistered client: " + std::string(number));
	for (auto& client : _clientPool)
	{
		if (client->getNumber() == number)
		{
			client->release();
			break;
		}
	}
}

int RequestsHandler::parseRequestedExpires(const std::shared_ptr<SipMessage>& data) const
{
	// Helper: read a non-negative integer starting at `from`. Returns -1 if no digits.
	auto readInt = [](const std::string& s, size_t from) -> int {
		while (from < s.size() && std::isspace(static_cast<unsigned char>(s[from]))) ++from;
		size_t end = from;
		while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
		if (end == from) return -1;
		int val = 0;
		for (size_t i = from; i < end; ++i)
		{
			if (val > 200000000) return 200000000;
			val = val * 10 + (s[i] - '0');
		}
		return val;
	};

	// 1. expires= parameter on the Contact header (most common form).
	std::string contact(data->getContact());
	auto cpos = contact.find("expires=");
	if (cpos != std::string::npos)
	{
		int v = readInt(contact, cpos + 8);
		if (v >= 0) return v;
	}

	// 2. Standalone Expires header — scan only lines beginning with 'e'/'E' to
	//    avoid copying and lowercasing the entire message (which includes the SDP body).
	const std::string& raw = data->toString();
	size_t pos = 0;
	while (pos < raw.size())
	{
		size_t lineEnd = raw.find('\n', pos);
		size_t next = (lineEnd == std::string::npos) ? raw.size() : lineEnd + 1;
		char first = raw[pos];
		if (first == 'e' || first == 'E')
		{
			// Check for "expires:" (case-insensitive, 8 chars + colon)
			if (next - pos >= 9)
			{
				std::string name = raw.substr(pos, 8);
				std::transform(name.begin(), name.end(), name.begin(),
					[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
				if (name == "expires:")
				{
					// Skip past any \r before \n in the offset calculation
					int v = readInt(raw, pos + 8);
					if (v >= 0) return v;
				}
			}
		}
		else if (first == '\r' || first == '\n')
		{
			break; // reached the header/body boundary blank line
		}
		pos = next;
	}

	// 3. No expiry specified — grant the default lease.
	return DEFAULT_EXPIRES;
}

void RequestsHandler::sweepExpired()
{
	auto now = std::chrono::steady_clock::now();
	for (auto& client : _clientPool)
	{
		if (client->getNumber().empty())
			continue;

		bool keepAliveTimedOut = (now - client->getLastActiveTime() > std::chrono::seconds(15));
		bool leaseExpired = client->isExpired(now);

		if (keepAliveTimedOut || leaseExpired)
		{
			if (keepAliveTimedOut)
			{
				queueLog("Pruning client due to missed OPTIONS keepalive pings: " + client->getNumber());
			}
			else
			{
				queueLog("Registration lease expired: " + client->getNumber());
			}

			// Clean up sessions involving this client
			std::string extension = client->getNumber();
			for (auto sit = _sessions.begin(); sit != _sessions.end(); )
			{
				bool involved = false;
				if (sit->second->getSrc() && sit->second->getSrc()->getNumber() == extension)
					involved = true;
				if (sit->second->getDest() && sit->second->getDest()->getNumber() == extension)
					involved = true;
				if (involved)
				{
					std::string callID = sit->first;
					// Media beachhead: if this dialog owned the live RTP tone stream,
					// stop it so a caller whose lease expires mid-stream doesn't leak
					// the socket/task. Idempotent no-op otherwise.
					_rtpSender.stop(callID);
					sit = _sessions.erase(sit);
					for (auto& session : _sessionPool)
					{
						if (session->getCallID() == callID)
						{
							session->release();
							break;
						}
					}
				}
				else
				{
					++sit;
				}
			}

			client->release();
		}
	}
}

void RequestsHandler::maybeSweep()
{
	auto now = std::chrono::steady_clock::now();
	if (now - _lastSweep < SWEEP_INTERVAL)
	{
		return;
	}
	_lastSweep = now;
	sweepExpired();
}

std::optional<std::shared_ptr<SipClient>> RequestsHandler::findClient(std::string_view number)
{
	for (auto& client : _clientPool)
	{
		if (client->getNumber() == number)
			return client;
	}
	return {};
}

void RequestsHandler::endHandle(std::string_view destNumber, std::shared_ptr<SipMessage> message)
{
	auto destClient = findClient(destNumber);
	if (destClient.has_value())
	{
		_outbox.emplace_back(destClient.value()->getAddress(), std::move(message));
	}
	else
	{
		// Clone the message so we don't mutate a shared object's header
		auto notFound = getMessageFromPool(*message);
		if (!notFound) return;   // pool exhausted: drop, peer retransmits (#101A)
		notFound->setHeader(SipMessageTypes::NOT_FOUND);
		notFound->clearBody();
		auto src = message->getSource();
		_outbox.emplace_back(src, std::move(notFound));
	}
}

std::string RequestsHandler::buildContact(std::string_view number) const
{
	std::string activeIp = _localIp;
	return "Contact: <sip:" + std::string(number) + "@" + activeIp + ":" + std::to_string(_serverPort) + ";transport=UDP>";
}

// ── Dashboard query API ──────────────────────────────────────────────────────

static const char* sessionStateToString(Session::State s)
{
	switch (s)
	{
		case Session::State::Invited:     return "Invited";
		case Session::State::Connected:   return "Connected";
		case Session::State::Busy:        return "Busy";
		case Session::State::Unavailable: return "Unavailable";
		case Session::State::Cancel:      return "Cancel";
		case Session::State::Bye:         return "Bye";
		default:                          return "Unknown";
	}
}

std::vector<std::pair<std::string, std::string>> RequestsHandler::getActiveClients()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.clients;
}

std::vector<std::tuple<std::string, std::string, std::string, int>> RequestsHandler::getActiveSessions()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.sessions;
}

void RequestsHandler::forceDisconnect(const std::string& extension)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		queueLog("Admin: force-disconnecting extension " + extension);
		for (auto& client : _clientPool)
		{
			if (client->getNumber() == extension)
			{
				client->release();
				break;
			}
		}
		// Also remove any sessions involving this extension
		for (auto it = _sessions.begin(); it != _sessions.end(); )
		{
			bool involved = false;
			if (it->second->getSrc() && it->second->getSrc()->getNumber() == extension)
				involved = true;
			if (it->second->getDest() && it->second->getDest()->getNumber() == extension)
				involved = true;
			if (involved)
			{
				std::string callID = it->first;
				_rtpSender.stop(callID);   // media beachhead: stop any owned RTP stream
				it = _sessions.erase(it);
				for (auto& session : _sessionPool)
				{
					if (session->getCallID() == callID)
					{
						session->release();
						break;
					}
				}
			}
			else
			{
				++it;
			}
		}
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

uint64_t RequestsHandler::getPacketsProcessed() const
{
	return _packetsProcessed.load(std::memory_order_relaxed);
}

uint64_t RequestsHandler::getPacketsDropped() const
{
	return _packetsDropped.load(std::memory_order_relaxed);
}

std::vector<CallDetailRecord> RequestsHandler::getCallDetailRecords()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.cdr;
}

std::string RequestsHandler::getPcapCapture()
{
	// Unlike the dashboard snapshot fields, the pcap ring isn't mirrored out to
	// _snapshot: it's populated directly under _mutex (handle()/drainOutbox()),
	// and serializing up to POCKETDIAL_PCAP_RING_SIZE small entries is cheap
	// enough to do inline rather than adding a second copy of the same data.
	std::lock_guard<std::mutex> lock(_mutex);
	return _pcapCapture.toPcapFile(_localIp, static_cast<uint16_t>(_serverPort));
}

std::vector<PcapCapture::TraceRecord> RequestsHandler::getTraceRecords()
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _pcapCapture.traceRecords();
}

std::optional<RequestsHandler::ProvisioningInfo> RequestsHandler::findProvisioningInfo(
	const std::string& mac)
{
	std::lock_guard<std::mutex> lock(_mutex);
	for (const auto& d : _registrar.adoptedDevices())
	{
		if (d.mac == mac)
		{
			const bool authRequired = (d.state == Registrar::DeviceState::Secured) ||
				(_registrar.getMode() == Registrar::Mode::Secure);
			return ProvisioningInfo{d.extension, authRequired};
		}
	}
	return std::nullopt;
}

void RequestsHandler::setDndLocked(const std::string& extension, bool on)
{
	if (on)
	{
		// Bound the map so a flood of distinct extensions can't grow the heap
		// without limit. Mirror the client-pool cap; reject new keys past it.
		if (_dnd.find(extension) == _dnd.end() &&
			_dnd.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
		{
			queueLog("DND set ignored (table full) for extension " + extension, true);
		}
		else
		{
			_dnd[extension] = true;
		}
	}
	else
	{
		// Turning DND off frees the slot, so the map only ever holds the
		// extensions that are actively in DND (bounded by registrations).
		_dnd.erase(extension);
	}
	queueLog("DND " + std::string(on ? "enabled" : "disabled") + " for extension " + extension);

	// Refresh the DND view in the dashboard snapshot immediately so the UI
	// reflects the change without waiting for the next tick(). Same refresh
	// regardless of whether the mutation came from the HTTP setter or a DTMF
	// CLASS code (Issue #77) — both funnel through here.
	{
		std::lock_guard<std::mutex> snapLock(_snapshotMutex);
		_snapshot.dnd.clear();
		_snapshot.dnd.reserve(_dnd.size());
		for (const auto& [ext, enabled] : _dnd)
		{
			if (enabled) _snapshot.dnd.push_back(ext);
		}
	}
}

void RequestsHandler::setDnd(const std::string& extension, bool on)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		setDndLocked(extension, on);
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

bool RequestsHandler::isDndEnabled(const std::string& extension)
{
	// Internal lookup: invoked from onInvite() which already holds _mutex, so this
	// must NOT take _mutex (std::mutex is non-recursive). Bounded map lookup.
	auto it = _dnd.find(extension);
	return it != _dnd.end() && it->second;
}

std::vector<std::string> RequestsHandler::getDndExtensions()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.dnd;
}

// ── Call forwarding (CFU/CFB/CFNA) ───────────────────────────────────────────

std::string RequestsHandler::getForwardTarget(const std::string& extension, const std::string& trigger) const
{
	// Internal lookup invoked from onInvite()/onBusy()/tick(), all of which already
	// hold _mutex — must NOT lock (non-recursive). Bounded map lookup.
	auto it = _forwards.find(extension);
	if (it == _forwards.end())
	{
		return {};
	}
	if (trigger == "always")   return it->second.always;
	if (trigger == "busy")     return it->second.busy;
	if (trigger == "noanswer") return it->second.noAnswer;
	return {};
}

void RequestsHandler::setForwardLocked(const std::string& extension, const std::string& trigger, const std::string& target)
{
	// Reject the virtual extensions outright; they are not real endpoints.
	// Previously only the HTTP-facing setForward() applied this guard — the
	// DTMF *73/*72NNNN inline path skipped it entirely (Issue #77), so a
	// crafted mid-dialog INFO with From: 777/999 could set up a "forward" on
	// a virtual extension. Routing both callers through here closes that gap.
	if (extension == "777" || extension == "999")
	{
		queueLog("Forward set ignored for virtual extension " + extension, true);
	}
	else
	{
		auto it = _forwards.find(extension);
		bool isNew = (it == _forwards.end());

		// Bound the table like _dnd: refuse a brand-new extension past the cap.
		if (isNew && _forwards.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
		{
			queueLog("Forward set ignored (table full) for extension " + extension, true);
		}
		else
		{
			pbx::ForwardConfig& cfg = _forwards[extension];
			if (trigger == "always")        cfg.always   = target;
			else if (trigger == "busy")     cfg.busy     = target;
			else if (trigger == "noanswer") cfg.noAnswer = target;

			// Drop the entry entirely once no trigger remains set, so the map only
			// holds actively-forwarded extensions (bounded by registrations).
			if (cfg.empty())
			{
				_forwards.erase(extension);
			}
			queueLog("Forward " + trigger + " for " + extension +
				(target.empty() ? " cleared" : (" -> " + target)));
			persistForwards();
		}
	}

	// Refresh the dashboard snapshot immediately (mirror setDnd). Same refresh
	// regardless of whether the mutation came from the HTTP setter or a DTMF
	// CLASS code (Issue #77) — both funnel through here.
	{
		std::lock_guard<std::mutex> snapLock(_snapshotMutex);
		_snapshot.forwards.clear();
		_snapshot.forwards.reserve(_forwards.size());
		for (const auto& [ext, cfg] : _forwards)
		{
			_snapshot.forwards.emplace_back(ext, cfg.always, cfg.busy, cfg.noAnswer);
		}
	}
}

void RequestsHandler::setForward(const std::string& extension, const std::string& trigger, const std::string& target)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		setForwardLocked(extension, trigger, target);
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

std::vector<std::tuple<std::string, std::string, std::string, std::string>> RequestsHandler::getForwards()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.forwards;
}

// ── Ring / hunt groups ───────────────────────────────────────────────────────

const pbx::RingGroup* RequestsHandler::findRingGroup(const std::string& extension) const
{
	// Internal lookup from onInvite() (already holds _mutex). Bounded map lookup.
	auto it = _ringGroups.find(extension);
	return (it == _ringGroups.end()) ? nullptr : &it->second;
}

void RequestsHandler::setRingGroup(const std::string& groupExt, const std::string& members, const std::string& mode)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		if (groupExt == "777" || groupExt == "999")
		{
			queueLog("Ring group ignored for reserved extension " + groupExt, true);
		}
		else
		{
			std::vector<std::string> list = pbx::splitMembers(members);
			if (list.empty())
			{
				// Empty membership deletes the group.
				_ringGroups.erase(groupExt);
				queueLog("Ring group " + groupExt + " deleted");
				persistRingGroups();
			}
			else
			{
				bool isNew = (_ringGroups.find(groupExt) == _ringGroups.end());
				if (isNew && _ringGroups.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
				{
					queueLog("Ring group ignored (table full) for " + groupExt, true);
				}
				else
				{
					pbx::RingGroup& g = _ringGroups[groupExt];
					g.members = std::move(list);
					g.mode = (mode == "hunt") ? pbx::GroupMode::Hunt : pbx::GroupMode::RingAll;
					queueLog("Ring group " + groupExt + " (" +
						(g.mode == pbx::GroupMode::Hunt ? "hunt" : "ringall") + ") = " +
						pbx::joinMembers(g.members));
					persistRingGroups();
				}
			}
		}

		// Refresh dashboard snapshot immediately.
		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot.ringGroups.clear();
			_snapshot.ringGroups.reserve(_ringGroups.size());
			for (const auto& [ext, g] : _ringGroups)
			{
				_snapshot.ringGroups.emplace_back(ext,
					g.mode == pbx::GroupMode::Hunt ? "hunt" : "ringall",
					pbx::joinMembers(g.members));
			}
		}

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

std::vector<std::tuple<std::string, std::string, std::string>> RequestsHandler::getRingGroups()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.ringGroups;
}

// ── Registrar mode (STAGE 2) ──────────────────────────────────────────────────

void RequestsHandler::setRegistrarMode(RegistrarMode mode)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_registrar.setMode(mode);
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}
	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

RequestsHandler::RegistrarMode RequestsHandler::getRegistrarMode() const
{
	// Lock-free read: the dashboard polls this; onRegister() branches on it on the
	// hot path. The Registrar's atomic guarantees a torn-free load.
	return _registrar.getMode();
}

// ── Device registry (STAGE 2: Learn-mode adoption) ────────────────────────────

void RequestsHandler::refreshDeviceSnapshot()
{
	// Caller holds _mutex. Mirror the Registrar's registry (including the volatile
	// online flags it tracks) into the dashboard snapshot.
	auto devices = _registrar.adoptedDevices();
	std::lock_guard<std::mutex> snapLock(_snapshotMutex);
	_snapshot.devices = std::move(devices);
}

void RequestsHandler::applyDeviceChange(Registrar::Change change)
{
	// Caller holds _mutex; takes _snapshotMutex internally.
	//
	// An online flip leaves the row set identical, so patching the flags in place
	// avoids rebuilding the vector and its two strings per device. That is the
	// common case by a wide margin — every registration and every lease expiry
	// flips a flag, and a post-reboot storm flips one per phone, which would
	// otherwise make the mirror cost O(devices) allocations per REGISTER.
	switch (change)
	{
		case Registrar::Change::None:
			return;
		case Registrar::Change::OnlineOnly:
		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_registrar.copyOnlineFlagsInto(_snapshot.devices);
			return;
		}
		case Registrar::Change::Structural:
			refreshDeviceSnapshot();
			return;
	}
}

std::vector<RequestsHandler::AdoptedDevice> RequestsHandler::getAdoptedDevices()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.devices;
}

bool RequestsHandler::secureDevice(const std::string& macOrExt)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		changed = _registrar.secure(macOrExt);
		applyDeviceChange(_registrar.consumeDevicesChange());
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}
	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
	return changed;
}

bool RequestsHandler::forgetDevice(const std::string& macOrExt)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	bool removed = false;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		removed = _registrar.forget(macOrExt);
		applyDeviceChange(_registrar.consumeDevicesChange());
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}
	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
	return removed;
}

// ── Outbound SIP MESSAGE (STAGE 2) ────────────────────────────────────────────

bool RequestsHandler::sendMessageTo(const std::string& ext, const std::string& text)
{
	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> localOutbox;
	bool sent = false;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		auto client = findClient(ext);
		if (!client.has_value())
		{
			// Not registered → nothing to send to. Best-effort, no enqueue.
			return false;
		}

		const sockaddr_in& addr = client.value()->getAddress();
		std::string destIpPort = sipwire::addrToIpPort(addr);

		std::string activeIp = _localIp;
		std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

		std::string callId  = IDGen::GenerateID(16) + "@" + activeIp;
		std::string branch  = "z9hG4bK" + IDGen::GenerateID(12);
		std::string fromTag = IDGen::GenerateID(9);

		// Bound the body so the whole datagram stays well under a typical MTU; a
		// notify is short by design.
		std::string body = text.size() > 512 ? text.substr(0, 512) : text;

		std::ostringstream ss;
		ss << "MESSAGE sip:" << ext << "@" << destIpPort << " SIP/2.0\r\n"
		   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
		   << "From: \"PocketDial\" <sip:pbx@" << srcIpPort << ">;tag=" << fromTag << "\r\n"
		   << "To: <sip:" << ext << "@" << activeIp << ">\r\n"
		   << "Call-ID: " << callId << "\r\n"
		   << "CSeq: 1 MESSAGE\r\n"
		   << "Max-Forwards: 70\r\n"
		   << "User-Agent: pocket-dial\r\n"
		   << "Content-Type: text/plain\r\n"
		   << "Content-Length: " << body.size() << "\r\n\r\n"
		   << body;

		auto msg = getMessageFromPool(ss.str(), addr);
		if (!msg) return false;   // pool exhausted: drop, peer retransmits (#101A)
		msg->syncContentLength();
		_outbox.emplace_back(addr, std::move(msg));
		sent = true;

		// Drain into a local vector and dispatch outside the lock (no IO under lock).
		localOutbox = drainOutbox();
	}

	for (auto& [addr, msg] : localOutbox)
	{
		_onHandled(addr, std::move(msg));
	}
	return sent;
}

size_t RequestsHandler::getClientCount()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.clients.size();
}

size_t RequestsHandler::getSessionCount()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.sessions.size();
}

void RequestsHandler::tick()
{
	auto now = std::chrono::steady_clock::now();
	if (now - _lastTick < std::chrono::seconds(1))
	{
		return;
	}
	_lastTick = now;

	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> localOutbox;
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_outbox.clear();

		sweepExpired();

		// RFC 3261 §17: retransmit timed-out INVITE forks and free completed slots.
		_txLayer.sweep(now);
		// RFC 4028: BYE sessions that have exceeded their session-expires timer.
		sweepSessionTimers(now);
		// BLF subscription expiry.
		_blf.sweepExpired();
		// Call-park timeout: ring back the parker or BYE the parked party.
		_park.sweep(now);

		// Belt-and-suspenders (Fix #4): drop DTMF accumulators whose dialog is gone,
		// in case a teardown path bypassed endCall(). Bounded by the small session pool.
		for (auto dit = _dtmfState.begin(); dit != _dtmfState.end(); )
		{
			if (_sessions.find(dit->first) == _sessions.end()) dit = _dtmfState.erase(dit);
			else ++dit;
		}

		// No-answer timers (CFNA + hunt-group progression). Poll the armed sessions
		// and act on any that have run past their ring deadline without connecting.
		// Collected first so we don't mutate _sessions while iterating it.
		std::vector<std::string> expiredCallIds;
		for (const auto& [callID, session] : _sessions)
		{
			if (session->isRingExpired(now) && session->getState() == Session::State::Invited)
			{
				expiredCallIds.push_back(callID);
			}
		}
		for (const auto& callID : expiredCallIds)
		{
			auto sit = _sessions.find(callID);
			if (sit == _sessions.end()) continue;
			auto session = sit->second;
			session->clearRingTimer();

			if (session->isHunt())
			{
				// Advance to the next hunt member; CANCEL the member that timed out.
				for (const auto& t : session->getPendingTargets())
				{
					auto invite = session->getInviteMessage();
					if (invite)
					{
						auto cancel = buildCancel(invite, t);
						if (cancel) _outbox.emplace_back(t->getAddress(), std::move(cancel));
					}
				}
				if (!huntRingNext(session))
				{
					// List exhausted: 480 to the caller and tear down.
					auto invite = session->getInviteMessage();
					if (invite && session->getSrc())
					{
						auto resp = getMessageFromPool(*invite);
						if (!resp) continue;   // pool exhausted: skip this session's 480 (#101A)
						resp->setHeader(SipMessageTypes::UNAVAILABLE);
						resp->clearBody();
						resp->setContact(buildContact(session->getGroupExt()));
						_outbox.emplace_back(invite->getSource(), std::move(resp));
						endCall(callID, session->getSrc()->getNumber(), session->getGroupExt(), "hunt group no answer");
					}
				}
			}
			else
			{
				// CFNA: CANCEL the original callee leg and INVITE the no-answer target.
				auto invite = session->getInviteMessage();
				auto dest = session->getDest();
				auto src = session->getSrc();
				std::string cfna = session->getNoAnswerTarget();
				if (invite && src && !cfna.empty())
				{
					if (dest)
					{
						auto cancel = buildCancel(invite, dest);
						if (cancel) _outbox.emplace_back(dest->getAddress(), std::move(cancel));
					}
					queueLog("CFNA: no answer, forwarding -> " + cfna);
					endCall(callID, src->getNumber(), std::string(invite->getToNumber()), "no answer (CFNA)");
					redirectInvite(invite, src, cfna);
				}
			}
		}

		// Sweep rate-limit buckets older than 60 seconds (Issue #58). The bucket map
		// now lives under _rateMutex (so per-packet admission never serializes on the
		// big _mutex), so take it here too. Nesting order is _mutex → _rateMutex;
		// handle() only ever holds them disjointly, so there is no deadlock.
		{
			std::lock_guard<std::mutex> rlock(_rateMutex);
			for (auto rit = _rateBuckets.begin(); rit != _rateBuckets.end(); )
			{
				if (now - rit->second.last > std::chrono::seconds(60))
				{
					rit = _rateBuckets.erase(rit);
				}
				else
				{
					++rit;
				}
			}
		}

		for (auto& client : _clientPool)
		{
			if (client->getNumber().empty()) continue;
			if (now - client->getLastPingTime() >= std::chrono::seconds(5))
			{
				client->setLastPingTime(now);
				auto ping = buildOptionsPing(client);
				// Skip this client's keepalive if the pool refused; the next tick
				// re-pings, and a missed OPTIONS only delays liveness detection.
				if (ping) _outbox.emplace_back(client->getAddress(), std::move(ping));
			}
		}

		// Register-beep timeouts: CANCEL unanswered beep INVITEs and free overdue
		// slots. Done here, under _mutex, enqueuing to _outbox; the actual sendto()
		// happens after the lock is dropped.
		_beeper.sweep(now);

		// Build snapshot under registrar mutex lock, then save it under snapshot mutex lock
		RegistrarSnapshot nextSnapshot;
		nextSnapshot.packetsProcessed = _packetsProcessed.load(std::memory_order_relaxed);
		nextSnapshot.packetsDropped = _packetsDropped.load(std::memory_order_relaxed);
		for (const auto& client : _clientPool)
		{
			if (client->getNumber().empty()) continue;
			const auto& addr = client->getAddress();
			std::string ipPort = sipwire::addrToIpPort(addr);
			nextSnapshot.clients.emplace_back(client->getNumber(), ipPort);
		}

		nextSnapshot.sessions.reserve(_sessions.size());
		for (const auto& [callID, session] : _sessions)
		{
			std::string caller = session->getSrc() ? session->getSrc()->getNumber() : "?";
			std::string callee = session->getDest() ? session->getDest()->getNumber() : "?";

			int durationSec = 0;
			if (session->getState() == Session::State::Connected)
			{
				durationSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
					now - session->getStartTime()).count());
			}
			nextSnapshot.sessions.emplace_back(caller, callee, sessionStateToString(session->getState()), durationSec);
		}

		// CDR view: copy the ring out newest-first into the snapshot.
		nextSnapshot.cdr.reserve(_cdrCount);
		for (size_t i = 0; i < _cdrCount; ++i)
		{
			// _cdrHead points one past the newest; walk backwards with wrap.
			size_t idx = (_cdrHead + POCKETDIAL_CDR_RECORDS - 1 - i) % POCKETDIAL_CDR_RECORDS;
			nextSnapshot.cdr.push_back(_cdrRing[idx]);
		}

		// DND view: extensions currently in DND.
		nextSnapshot.dnd.reserve(_dnd.size());
		for (const auto& [ext, enabled] : _dnd)
		{
			if (enabled) nextSnapshot.dnd.push_back(ext);
		}

		// Call-forward view.
		nextSnapshot.forwards.reserve(_forwards.size());
		for (const auto& [ext, cfg] : _forwards)
		{
			nextSnapshot.forwards.emplace_back(ext, cfg.always, cfg.busy, cfg.noAnswer);
		}

		// Ring/hunt-group view.
		nextSnapshot.ringGroups.reserve(_ringGroups.size());
		for (const auto& [ext, g] : _ringGroups)
		{
			nextSnapshot.ringGroups.emplace_back(ext,
				g.mode == pbx::GroupMode::Hunt ? "hunt" : "ringall",
				pbx::joinMembers(g.members));
		}

		// Parked calls view: {orbit, parkedExt, parker, secondsParked}. This full
		// rebuild already reflects anything _park.sweep() just did above, so clear
		// the dirty flag here rather than leaving it to trigger a redundant mirror
		// on the next packet.
		nextSnapshot.parkedCalls = _park.snapshotRows(now, /*onlyParked=*/true);
		_park.consumeParkChanged();

		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			// `devices` and `pageZones` are NOT rebuilt above: they are mirrored out
			// of band (applyDeviceChange on a registry change, and the page-zone
			// config path) because their sources only move on an admin action or a
			// REGISTER. Carry them across the swap — assigning a fresh snapshot over
			// the old one would blank both every tick, so the dashboard's adopted
			// devices and paging zones would flash empty a second after any update
			// and stay empty until the next change.
			nextSnapshot.devices   = std::move(_snapshot.devices);
			nextSnapshot.pageZones = std::move(_snapshot.pageZones);
			_snapshot = std::move(nextSnapshot);
		}

		localOutbox = drainOutbox();

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}

	for (auto& event : localOutbox)
	{
		_onHandled(event.first, std::move(event.second));
	}
}

std::optional<std::shared_ptr<SipClient>> RequestsHandler::findClientByAddress(const sockaddr_in& addr)
{
	for (auto& client : _clientPool)
	{
		if (client->getNumber().empty()) continue;
		if (client->getAddress().sin_addr.s_addr == addr.sin_addr.s_addr &&
			client->getAddress().sin_port == addr.sin_port)
		{
			return client;
		}
	}
	return {};
}

std::shared_ptr<SipMessage> RequestsHandler::buildOptionsPing(const std::shared_ptr<SipClient>& client)
{
	std::string clientNum = client->getNumber();

	std::string destIpPort = sipwire::addrToIpPort(client->getAddress());

	std::string activeIp = _localIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

	std::string callId = IDGen::GenerateID(16) + "@" + activeIp;
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);
	std::string fromTag = IDGen::GenerateID(9);

	std::ostringstream ss;
	ss << "OPTIONS sip:" << clientNum << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "To: <sip:" << clientNum << "@" << destIpPort << ">\r\n"
	   << "From: <sip:server@" << srcIpPort << ">;tag=" << fromTag << "\r\n"
	   << "Call-ID: " << callId << "\r\n"
	   << "CSeq: 1 OPTIONS\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Length: 0\r\n\r\n";

	return getMessageFromPool(ss.str(), client->getAddress());
}

std::shared_ptr<SipClient> RequestsHandler::allocateClient(std::string number, sockaddr_in address, int expiresSeconds)
{
	// Re-REGISTER: find existing slot by number and refresh it in-place
	for (auto& client : _clientPool)
	{
		if (client->getNumber() == number)
		{
			queueLog("Re-registered: " + number);
			client->reset(number, address, expiresSeconds);
			return client;
		}
	}

	// New client: find the first free slot
	for (auto& client : _clientPool)
	{
		if (client->getNumber().empty())
		{
			queueLog("New Client: " + number);
			client->reset(std::move(number), address, expiresSeconds);
			return client;
		}
	}

	// No free slots! Evict the oldest expired client
	auto now = std::chrono::steady_clock::now();
	for (auto& client : _clientPool)
	{
		if (client->isExpired(now))
		{
			client->reset(std::move(number), address, expiresSeconds);
			return client;
		}
	}

	// Out of space!
	return nullptr;
}

std::shared_ptr<Session> RequestsHandler::allocateSession(std::string callID, std::shared_ptr<SipClient> src)
{
	for (auto& session : _sessionPool)
	{
		const std::string& slotId = session->getCallID();
		if (slotId.empty() || _sessions.find(slotId) == _sessions.end())
		{
			session->reset(std::move(callID), src);
			return session;
		}
	}
	return nullptr;
}

bool RequestsHandler::ipAllowed(const sockaddr_in& src) const
{
	if (_allowMask == 0) return true; // No allowlist configured
	uint32_t ip = ntohl(src.sin_addr.s_addr);
	return (ip & _allowMask) == _allowNet;
}

bool RequestsHandler::allowPacket(const sockaddr_in& src)
{
	auto now = std::chrono::steady_clock::now();
	uint32_t ip = src.sin_addr.s_addr; // Key by raw network-byte-order IP

	auto it = _rateBuckets.find(ip);
	if (it == _rateBuckets.end())
	{
		if (_rateBuckets.size() >= 256)
		{
			// Fail-safe drop if maximum buckets exceeded
			return false;
		}
		// New bucket: burst 40, sustained 20 pkt/s
		_rateBuckets[ip] = { 40.0, now };
		return true;
	}

	auto& bucket = it->second;
	double elapsedSec = std::chrono::duration<double>(now - bucket.last).count();
	bucket.last = now;

	// Replenish tokens (sustained rate = 20 tokens/sec)
	bucket.tokens = (std::min)(40.0, bucket.tokens + elapsedSec * 20.0);

	if (bucket.tokens >= 1.0)
	{
		bucket.tokens -= 1.0;
		return true;
	}

	return false; // Denied (Rate limit exceeded)
}

bool RequestsHandler::isValidAor(std::string_view s) const
{
	if (s.empty()) return false;
	for (char c : s)
	{
		// Alnum + RFC 3261 user-part punctuation we accept, plus '*' and '#' so star/pound
		// feature codes (e.g. *55) are dialable AORs. Tab/newline stay excluded, which the NVS
		// blob persistence relies on as field/record delimiters.
		if (!std::isalnum(static_cast<unsigned char>(c)) &&
			c != '.' && c != '-' && c != '_' && c != '+' &&
			c != '*' && c != '#')
		{
			return false;
		}
	}
	return true;
}

void RequestsHandler::queueLog(std::string msg, bool isError)
{
	_logQueue.push_back({isError, std::move(msg)});
}

// ── NVS persistence: PBX config (forwards / ring groups) + CDR ring ──────────
//
// All five helpers are no-ops on host (the in-memory maps/ring ARE the store) and
// gate their NVS access on ESP_PLATFORM. Each table is serialized as a single blob
// (one record per line; fields tab-separated) under one NVS key, so a mutation
// rewrites exactly one key — bounded size, low flash wear. Records are bounded by
// POCKETDIAL_MAX_CLIENTS / POCKETDIAL_CDR_RECORDS, so the blob can never grow
// without limit. The AOR charset (isValidAor) excludes tab/newline, so the
// delimiters are safe. Callers hold _mutex (except construction-time loads, which
// run single-threaded before any handler dispatches).

using pbxpersist::deserializeBlob;

void RequestsHandler::loadPbxConfig()
{
	if (_pbxConfigLoaded)
	{
		return;
	}
	_pbxConfigLoaded = true;

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}

	auto readBlob = [&](const char* key) -> std::string {
		size_t len = 0;
		if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0)
		{
			return {};
		}
		std::string buf(len, '\0');
		if (nvs_get_str(h, key, buf.data(), &len) != ESP_OK)
		{
			return {};
		}
		if (!buf.empty() && buf.back() == '\0') buf.pop_back(); // drop NUL terminator
		return buf;
	};

	// Forwards: ext \t always \t busy \t noAnswer
	for (const auto& rec : deserializeBlob(readBlob("forwards")))
	{
		if (rec.size() < 4 || rec[0].empty()) continue;
		if (_forwards.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS)) break;
		pbx::ForwardConfig cfg;
		cfg.always = rec[1];
		cfg.busy = rec[2];
		cfg.noAnswer = rec[3];
		if (!cfg.empty()) _forwards[rec[0]] = std::move(cfg);
	}

	// Ring groups: ext \t mode \t m1,m2,...
	for (const auto& rec : deserializeBlob(readBlob("groups")))
	{
		if (rec.size() < 3 || rec[0].empty()) continue;
		if (_ringGroups.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS)) break;
		pbx::RingGroup g;
		g.mode = (rec[1] == "hunt") ? pbx::GroupMode::Hunt : pbx::GroupMode::RingAll;
		g.members = pbx::splitMembers(rec[2]);
		if (!g.members.empty()) _ringGroups[rec[0]] = std::move(g);
	}

	// Page zones: ext \t m1,m2,...
	for (const auto& rec : deserializeBlob(readBlob("pzones")))
	{
		if (rec.size() < 2 || rec[0].empty()) continue;
		if (_pageZones.size() >= static_cast<size_t>(POCKETDIAL_MAX_PAGE_ZONES)) break;
		pbx::PageZone z;
		z.members = pbx::splitZoneMembers(rec[1]);
		if (!z.members.empty()) _pageZones[rec[0]] = std::move(z);
	}

	nvs_close(h);
#endif
}

void RequestsHandler::persistForwards()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	std::string blob;
	for (const auto& [ext, cfg] : _forwards)
	{
		blob += ext; blob += '\t';
		blob += cfg.always; blob += '\t';
		blob += cfg.busy; blob += '\t';
		blob += cfg.noAnswer; blob += '\n';
	}
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_str(h, "forwards", blob.c_str());
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

void RequestsHandler::persistRingGroups()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	std::string blob;
	for (const auto& [ext, g] : _ringGroups)
	{
		blob += ext; blob += '\t';
		blob += (g.mode == pbx::GroupMode::Hunt ? "hunt" : "ringall"); blob += '\t';
		blob += pbx::joinMembers(g.members); blob += '\n';
	}
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_str(h, "groups", blob.c_str());
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

void RequestsHandler::persistPageZones()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	std::string blob;
	for (const auto& [ext, z] : _pageZones)
	{
		blob += ext; blob += '\t';
		blob += pbx::joinMembers(z.members); blob += '\n';
	}
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_str(h, "pzones", blob.c_str());
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

// ── Registrar mode + device registry persistence (STAGE 2) ───────────────────

void RequestsHandler::loadAdminHttpTtl()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}
	uint16_t v = 0;
	esp_err_t err = nvs_get_u16(h, "admin_http_ttl", &v);
	nvs_close(h);
	if (err == ESP_OK && v > 0)
	{
		_adminHttpTtlSec.store(v, std::memory_order_relaxed);
	}
	// else: keep the compile-time default (600s).
#endif
}

void RequestsHandler::loadCdrRing()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	nvs_handle_t h;
	if (nvs_open(NVS_CDR_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}
	size_t len = 0;
	if (nvs_get_str(h, "ring", nullptr, &len) == ESP_OK && len > 0)
	{
		std::string buf(len, '\0');
		if (nvs_get_str(h, "ring", buf.data(), &len) == ESP_OK)
		{
			if (!buf.empty() && buf.back() == '\0') buf.pop_back();
			// Record: caller \t callee \t startMs \t durationSec \t result(int)
			for (const auto& rec : deserializeBlob(buf))
			{
				if (rec.size() < 5) continue;
				if (_cdrCount >= POCKETDIAL_CDR_RECORDS) break;
				CallDetailRecord r;
				r.caller = rec[0];
				r.callee = rec[1];
				r.startMs = static_cast<uint64_t>(strtoull(rec[2].c_str(), nullptr, 10));
				r.durationSec = static_cast<uint32_t>(strtoul(rec[3].c_str(), nullptr, 10));
				int ri = atoi(rec[4].c_str());
				r.result = (ri >= 0 && ri <= static_cast<int>(CdrResult::Failed))
					? static_cast<CdrResult>(ri) : CdrResult::Failed;
				// Records were serialized oldest-first; append preserving order.
				_cdrRing[_cdrHead] = std::move(r);
				_cdrHead = (_cdrHead + 1) % POCKETDIAL_CDR_RECORDS;
				++_cdrCount;
			}
		}
	}
	nvs_close(h);
#endif
}

void RequestsHandler::persistCdrRing()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	// Serialize the ring oldest-first (same order loadCdrRing replays). Bounded by
	// POCKETDIAL_CDR_RECORDS, so the blob is fixed-footprint. Write-through on each
	// teardown: the CDR ring is small (default 32) and calls end infrequently
	// relative to flash endurance, so a per-call rewrite is acceptable — see summary.
	std::string blob;
	for (size_t i = 0; i < _cdrCount; ++i)
	{
		size_t idx = (_cdrHead + POCKETDIAL_CDR_RECORDS - _cdrCount + i) % POCKETDIAL_CDR_RECORDS;
		const CallDetailRecord& r = _cdrRing[idx];
		blob += r.caller; blob += '\t';
		blob += r.callee; blob += '\t';
		blob += std::to_string(r.startMs); blob += '\t';
		blob += std::to_string(r.durationSec); blob += '\t';
		blob += std::to_string(static_cast<int>(r.result)); blob += '\n';
	}
	nvs_handle_t h;
	if (nvs_open(NVS_CDR_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_str(h, "ring", blob.c_str());
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

// ── Task 2B: Admin extension NVS key ─────────────────────────────────────────

void RequestsHandler::loadAdminExt()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}
	char buf[32] = {0};
	size_t len = sizeof(buf);
	esp_err_t err = nvs_get_str(h, "admin_ext", buf, &len);
	nvs_close(h);
	if (err == ESP_OK && buf[0] != '\0')
	{
		_adminExt = buf;
	}
	// else: keep the in-class default "1001"
#endif
}

void RequestsHandler::saveAdminExt(const std::string& ext)
{
	_adminExt = ext;
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_str(h, "admin_ext", ext.c_str());
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

std::string RequestsHandler::getAdminExt() const
{
	return _adminExt;
}

// ── Task 2C: DTMF digit-collection state machine + CLASS service codes ────────

void RequestsHandler::onDtmfInfo(std::shared_ptr<SipMessage> data)
{
	// --- 1. Parse "Signal=X" from the body -----------------------------------
	const std::string& raw = data->toString();
	char digit = 0;
	{
		size_t sep = raw.find("\r\n\r\n");
		if (sep == std::string::npos) sep = raw.find("\n\n");
		if (sep != std::string::npos)
		{
			std::string body = raw.substr(sep);
			size_t sigPos = body.find("Signal=");
			if (sigPos == std::string::npos) sigPos = body.find("signal=");
			if (sigPos != std::string::npos)
			{
				size_t valIdx = sigPos + 7; // after "Signal="
				while (valIdx < body.size() && body[valIdx] == ' ') ++valIdx;
				if (valIdx < body.size())
				{
					digit = body[valIdx];
				}
			}
		}
	}
	if (digit == 0)
	{
		return; // malformed / no signal — nothing to do
	}

	// --- 2. Look up or create the per-Call-ID accumulator -------------------
	std::string callId(data->getCallID());
	auto& accum = _dtmfState[callId];

	// --- 3. Timeout: reset accumulator if > TIMEOUT_MS since last digit -----
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	TickType_t now = xTaskGetTickCount();
	uint32_t elapsedMs = (now - accum.lastTick) * portTICK_PERIOD_MS;
#else
	uint32_t now = static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	uint32_t elapsedMs = (accum.lastTick == 0) ? 0 : (now - accum.lastTick);
#endif
	if (accum.lastTick != 0 && elapsedMs > DtmfAccum::TIMEOUT_MS)
	{
		accum.digits.clear();
		accum.starCodeFiredAtTick = 0;
	}
	accum.lastTick = now;

	// --- 4. Append digit ----------------------------------------------------
	accum.digits += digit;
	const std::string& seq = accum.digits;
	std::string callerExt(data->getFromNumber());

	// --- 5. Admin menu gate (Task 2C-5): *PIN + 3-digit code ----------------
	// Pattern: * + PIN(4+) + 3-digit-code  (minimum 8 chars total after '*')
	// Admin gate fires only when the caller IS the admin extension.
	if (callerExt == _adminExt && !seq.empty() && seq[0] == '*')
	{
		if (seq == "*4887")
		{
			// PLAN_ADMIN_HTTP_ONLY.md: dedicated star-code (spells HTTP on a phone
			// keypad: H=4 T=8 T=8 P=7), no PIN. *<PIN>#010 does not work on real
			// hardphones — '#' is bound to Send/Call on Yealink (and most SIP
			// phones' keypads), both pre-dial and mid-call, so the sequence never
			// reaches the phone's DTMF-relay path intact. Trust model: registered
			// as the admin extension + signaling from that registration's bound
			// IP is sufficient to open the transport. Opening the transport does
			// NOT bypass PIN/session auth on the endpoints themselves once
			// reachable — this only shortens the no-PIN-needed step to "have the
			// admin handset."
			auto adminClient = findClient(_adminExt);
			bool sourceOk = adminClient.has_value() &&
				data->getSource().sin_addr.s_addr ==
				adminClient.value()->getAddress().sin_addr.s_addr;

			if (!adminClient.has_value() || !sourceOk)
			{
				queueLog("[admin] HTTP-open DTMF trigger rejected: ext " + _adminExt +
					(adminClient.has_value() ? " source IP mismatch" : " not registered"), true);
			}
			else
			{
				uint64_t untilMs = nowEpochMs() +
					static_cast<uint64_t>(_adminHttpTtlSec.load()) * 1000ULL;
				_adminHttpOpenUntilMs.store(untilMs, std::memory_order_release);
				queueLog("[admin] HTTP admin plane opened via DTMF *4887, ext " + _adminExt +
					", ttl=" + std::to_string(_adminHttpTtlSec.load()) + "s");
			}
			// Issue #93: this fires (accept or reject, above) the instant the
			// accumulated sequence equals "*4887" — which can be mid-entry if the
			// admin's actual PIN happens to begin with those four digits. Remember
			// it so the next digits, landing in the fresh accumulator this clear()
			// creates, can be checked for a pattern consistent with a continued
			// *PIN#code the admin never got to finish.
			accum.starCodeFiredAtTick = now;
			accum.digits.clear();
			return;
		}

		// Format: '*' + PIN(>=4 digits) + '#' + 3-digit code [+ confirm digit].
		// The '#' terminates the PIN so its length is unambiguous: we verify the
		// PIN EXACTLY ONCE per completed code. (The old version looped over every
		// candidate PIN length calling verifyPin() for each, so a single normal
		// admin entry charged several failed attempts against the brute-force
		// lockout and could lock the admin out of both DTMF and the dashboard.)
		bool adminMatched = false;
		size_t hashPos = seq.find('#');
		if (hashPos != std::string::npos && hashPos >= 5 && (seq.size() - hashPos - 1) >= 3)
		{
			std::string pinCandidate = seq.substr(1, hashPos - 1);
			std::string rest = seq.substr(hashPos + 1);   // CODE[confirm]
			std::string code = rest.substr(0, 3);
			// PIN must be all digits.
			bool allDigits = !pinCandidate.empty();
			for (char c : pinCandidate)
			{
				if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }
			}
			// Single verify — a wrong PIN is exactly one counted failed attempt.
			if (!allDigits || !AdminAuth::verifyPin(pinCandidate))
			{
				queueLog("[admin] DTMF admin auth failed", true);
				accum.digits.clear();
				return;
			}

			// PIN verified — execute the command code.
			if (code == "001")
			{
				// NTP resync (esp_sntp_restart is ESP-IDF v5+; fall back to log if absent)
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
				esp_sntp_restart();
#else
				queueLog("[admin] NTP sync requested via DTMF (esp_sntp_restart not available on this IDF version)");
#endif
#endif
				queueLog("[admin] NTP sync requested via DTMF");
				adminMatched = true;
			}
			else if (code == "101")
			{
				// Topology switch: toggle wifi_mode between 1 (CLIENT) and 2 (AP).
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
				nvs_handle_t h;
				if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK)
				{
					uint8_t mode = 1;
					nvs_get_u8(h, "wifi_mode", &mode);
					mode = (mode == 1) ? 2 : 1;
					nvs_set_u8(h, "wifi_mode", mode);
					nvs_commit(h);
					nvs_close(h);
				}
				queueLog("[admin] topology switch via DTMF, restarting");
				esp_restart();
#else
				queueLog("[admin] topology switch requested (stub on host)");
#endif
				adminMatched = true;
			}
			else if (code == "200")
			{
				// Extension target config stub
				queueLog("[admin] targets config: dial new ext (stub)");
				adminMatched = true;
			}
			else if (code == "999")
			{
				// Factory reset — requires a follow-up confirm digit '1'.
				if (rest.size() >= 4)
				{
					if (rest[3] == '1')
					{
						queueLog("[admin] factory reset confirmed via DTMF");
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
						nvs_flash_erase();
						esp_restart();
#else
						queueLog("[admin] factory reset (stub on host)");
#endif
					}
					else
					{
						queueLog("[admin] factory reset aborted (confirm != '1')");
					}
					adminMatched = true;
				}
				else
				{
					// Confirm digit not yet received — keep the accumulator and
					// wait. Do NOT set adminMatched (the tail would clear it).
					queueLog("[admin] factory reset: awaiting confirm digit '1'");
					return;
				}
			}
			if (adminMatched)
			{
				accum.digits.clear();
				return;
			}
		}

		// If the sequence starts with *NNNN (4+ digits) but no code matched yet,
		// and the wrong caller is trying, send 403.
	}
	else if (callerExt == _adminExt && accum.starCodeFiredAtTick != 0 &&
	         seq.find('#') != std::string::npos && (seq.size() - seq.find('#') - 1) >= 3)
	{
		// Issue #93: the *4887 star-code just fired for this dialog (above), and
		// the admin kept dialing into something shaped like the tail of an
		// interrupted *PIN#code (no leading '*' — the accumulator that produced
		// this `seq` started fresh when the star-code cleared it). This is only
		// ever a symptom of a PIN that begins "4887": that prefix is reserved
		// (POST /api/admin/set-pin rejects it going forward), but a device
		// provisioned before that guard existed can still be carrying one, and
		// the hash can't be reversed to confirm it — so this is a best-effort,
		// imperfect nudge rather than a definite diagnosis.
		queueLog("[admin] DTMF entry right after *4887 fired looks like an "
			"interrupted *PIN#code from ext " + _adminExt + " — if the admin PIN "
			"begins with 4887 it is shadowed by the HTTP-open star-code and DTMF "
			"admin commands can never complete; rotate it via the dashboard "
			"(POST /api/admin/set-pin) — see docs/THREAT_MODEL.md", true);
		accum.starCodeFiredAtTick = 0;   // one warning per incident
	}
	else if (callerExt != _adminExt && !seq.empty() && seq[0] == '*' &&
	         seq.find('#') != std::string::npos)
	{
		// A non-admin caller attempting the admin-menu pattern (*PIN#…): reject.
		// CLASS service codes (*60/*72/…) have no '#', so they fall through to the
		// per-subscriber feature handling below for any registered caller.
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader("SIP/2.0 403 Forbidden");
		response->clearBody();
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		accum.digits.clear();
		return;
	}

	// --- 6. CLASS feature code matching (Task 2C-4) --------------------------

	// *60 — Enable Selective Call Rejection (DND=true) for caller's extension.
	if (seq == "*60")
	{
		// Issue #77: route through the same lock-already-held core setDnd()
		// uses (we're already inside _mutex here, via handle()) so the
		// dashboard snapshot refreshes immediately instead of only on the
		// next unrelated HTTP-side setDnd() call.
		setDndLocked(callerExt, true);
		accum.digits.clear();
		return;
	}

	// *80 — Disable SCR/DND for caller's extension.
	if (seq == "*80")
	{
		setDndLocked(callerExt, false);
		accum.digits.clear();
		return;
	}

	// *73 — Disable CFU for caller's extension.
	if (seq == "*73")
	{
		// Issue #77: setForwardLocked with an empty target clears the
		// "always" trigger exactly like the old inline erase did, but also
		// refreshes the dashboard snapshot and applies the virtual-extension
		// guard that the old inline path skipped.
		setForwardLocked(callerExt, "always", "");
		accum.digits.clear();
		return;
	}

	// *69 — Speak last-caller extension: redirect call to echo ext 777 and log CDR lookup.
	if (seq == "*69")
	{
		// Find the last CDR entry where callee == callerExt (i.e. last inbound call).
		std::string lastCaller;
		for (size_t i = 0; i < _cdrCount; ++i)
		{
			size_t idx = (_cdrHead + POCKETDIAL_CDR_RECORDS - 1 - i) % POCKETDIAL_CDR_RECORDS;
			if (_cdrRing[idx].callee == callerExt && !_cdrRing[idx].caller.empty())
			{
				lastCaller = _cdrRing[idx].caller;
				break;
			}
		}
		if (!lastCaller.empty())
		{
			queueLog("*69 last caller for " + callerExt + " is " + lastCaller);
			// Reroute to extension 777 (echo loopback) so the caller hears tones.
			// Find the active session for this Call-ID and redirect its RTP to 777.
			auto session = getSession(callId);
			if (session.has_value())
			{
				// Per-session dummy dest (never a shared client) so concurrent
				// star-code/777/440 calls can't clobber each other's destination.
				auto dummy = std::make_shared<SipClient>();
				dummy->reset("777", session.value()->getSrc()
					? session.value()->getSrc()->getAddress() : sockaddr_in{}, 3600);
				session.value()->setDest(dummy);
			}
		}
		else
		{
			queueLog("*69 no last caller found for " + callerExt);
		}
		accum.digits.clear();
		return;
	}

	// *11 — Echo loopback: reroute active call's RTP endpoint to extension 777.
	if (seq == "*11")
	{
		auto session = getSession(callId);
		if (session.has_value())
		{
			auto src = session.value()->getSrc();
			if (src)
			{
				// Per-session dummy dest (never a shared client) — see *69 above.
				auto dummy = std::make_shared<SipClient>();
				dummy->reset("777", src->getAddress(), 3600);
				session.value()->setDest(dummy);
				queueLog("*11 echo loopback for call " + callId);
			}
		}
		accum.digits.clear();
		return;
	}

	// *72NNNN — Enable CFU for caller's extension to NNNN (4+ digits after *72).
	// Requires the full sequence to be collected; we match once it's ≥6 chars and
	// none of the above shorter patterns matched.
	if (seq.size() >= 6 && seq[0] == '*' && seq[1] == '7' && seq[2] == '2')
	{
		std::string target = seq.substr(3);
		if (target.size() >= 4 && isValidAor(target))
		{
			// Issue #77: setForwardLocked applies the same table-full guard and
			// the virtual-extension guard setForward() has always had (which
			// this inline path used to skip), and refreshes the dashboard
			// snapshot immediately instead of leaving it stale.
			setForwardLocked(callerExt, "always", target);
			accum.digits.clear();
			return;
		}
		// else: keep accumulating (target not yet 4 digits)
	}
}

// ── File-scope static helpers ─────────────────────────────────────────────────

static bool sameAddress(const sockaddr_in& a, const sockaddr_in& b)
{
	return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// Forward to the shared header helper (see SipHeaderUtil.hpp) — kept as a
// file-static name so the many call sites in this TU stay unchanged.
static std::string stripHeaderName(std::string_view h)
{
	return siphdr::stripHeaderName(h);
}

// ── Mid-dialog re-INVITE (RFC 3261 §12.2 hold/resume) ────────────────────────

void RequestsHandler::onReinvite(std::shared_ptr<SipMessage> data)
{
	auto sessionOpt = getSession(data->getCallID());
	if (!sessionOpt.has_value())
	{
		return;
	}
	auto session = sessionOpt.value();
	auto src = session->getSrc();
	auto dest = session->getDest();

	// Virtual-extension legs (777 echo) have no real peer to relay the offer to —
	// decline the re-INVITE so the holding phone keeps the call on the original SDP.
	const std::string destNum = dest ? dest->getNumber() : "";
	if (destNum == "777" || !src || !dest)
	{
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader("SIP/2.0 488 Not Acceptable Here");
		response->clearBody();
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	// Identify the sending leg by source address; the relay target is the peer.
	std::shared_ptr<SipClient> peer;
	if (sameAddress(data->getSource(), src->getAddress()))
	{
		peer = dest;
	}
	else if (sameAddress(data->getSource(), dest->getAddress()))
	{
		peer = src;
	}
	if (!peer)
	{
		return; // not from either leg of this dialog: ignore
	}

	// Relay UNTOUCHED — no clearBody()/enforceG711() — so the hold SDP
	// (a=sendonly/inactive) and its Content-Length reach the peer intact.
	_outbox.emplace_back(peer->getAddress(), data);

	// A re-INVITE from either leg is evidence the endpoint is alive — it counts
	// as a session-timer refresh.
	if (session->getSessionExpiresSeconds() > 0)
	{
		session->armSessionTimer(session->getSessionExpiresSeconds(),
		                         session->isRefresher(),
		                         std::chrono::steady_clock::now());
	}

	// Track hold state from the offered SDP direction. RFC 3264: an absent
	// direction attribute implies sendrecv (an active call).
	const auto dir = data->getSdpDirection();
	if (dir == SipMessage::SdpDirection::SendOnly ||
		dir == SipMessage::SdpDirection::RecvOnly ||
		dir == SipMessage::SdpDirection::Inactive)
	{
		session->setState(Session::State::Held);
		queueLog("Hold: " + std::string(data->getFromNumber()) + " held call " + std::string(data->getCallID()));
	}
	else
	{
		session->setState(Session::State::Connected);
		queueLog("Hold: call " + std::string(data->getCallID()) + " resumed");
	}
}

// ── Mid-dialog UPDATE (RFC 3311 hold/resume / session-timer keep-alive) ──────

void RequestsHandler::onUpdate(std::shared_ptr<SipMessage> data)
{
	auto sessionOpt = getSession(data->getCallID());
	if (!sessionOpt.has_value())
	{
		auto resp = getMessageFromPool(*data);
		if (!resp) return;   // pool exhausted: drop, peer retransmits (#101A)
		resp->setHeader("SIP/2.0 481 Call/Transaction Does Not Exist");
		resp->clearBody();
		_outbox.emplace_back(data->getSource(), std::move(resp));
		return;
	}
	auto session = sessionOpt.value();
	std::string activeIp = _localIp;

	if (!data->hasSdp())
	{
		// Bodiless UPDATE: session-timer refresh — 200 OK and reset expiry.
		auto resp = getMessageFromPool(*data);
		if (!resp) return;   // pool exhausted: drop, peer retransmits (#101A)
		resp->setHeader(SipMessageTypes::OK);
		resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		resp->clearBody();
		resp->syncContentLength();
		_outbox.emplace_back(data->getSource(), std::move(resp));

		if (session->getSessionExpiresSeconds() > 0)
		{
			session->armSessionTimer(session->getSessionExpiresSeconds(),
			                         session->isRefresher(),
			                         std::chrono::steady_clock::now());
		}
		return;
	}

	// SDP-bearing UPDATE: relay to the peer leg (same logic as onReinvite).
	auto src  = session->getSrc();
	auto dest = session->getDest();
	const std::string destNum = dest ? dest->getNumber() : "";

	if (destNum == "777" || !src || !dest)
	{
		auto resp = getMessageFromPool(*data);
		if (!resp) return;   // pool exhausted: drop, peer retransmits (#101A)
		resp->setHeader("SIP/2.0 488 Not Acceptable Here");
		resp->clearBody();
		_outbox.emplace_back(data->getSource(), std::move(resp));
		return;
	}

	std::shared_ptr<SipClient> peer;
	if (sameAddress(data->getSource(), src->getAddress()))
		peer = dest;
	else if (sameAddress(data->getSource(), dest->getAddress()))
		peer = src;

	if (!peer) return;

	_outbox.emplace_back(peer->getAddress(), data);

	if (session->getSessionExpiresSeconds() > 0)
	{
		session->armSessionTimer(session->getSessionExpiresSeconds(),
		                         session->isRefresher(),
		                         std::chrono::steady_clock::now());
	}

	const auto dir = data->getSdpDirection();
	if (dir == SipMessage::SdpDirection::SendOnly ||
	    dir == SipMessage::SdpDirection::RecvOnly ||
	    dir == SipMessage::SdpDirection::Inactive)
	{
		session->setState(Session::State::Held);
		queueLog("Update/Hold: " + std::string(data->getFromNumber()) +
		         " held call " + std::string(data->getCallID()));
	}
	else
	{
		session->setState(Session::State::Connected);
		queueLog("Update/Resume: call " + std::string(data->getCallID()) + " resumed");
	}
}

// ── BYE source authorization ──────────────────────────────────────────────────

bool RequestsHandler::isDialogSourceAuthorized(const std::shared_ptr<Session>& session,
	const sockaddr_in& source) const
{
	if (!session)
	{
		// Out-of-dialog BYE on a non-existent Call-ID: harmless — nothing to tear down.
		return true;
	}

	auto src  = session->getSrc();
	auto dest = session->getDest();

	// Fail-open for any dialog with a missing leg (half-set-up session).
	if (!src || !dest)
	{
		return true;
	}

	// Compare source IP only (port-agnostic): a phone may send the BYE from the same
	// contact IP but a different ephemeral UDP port than the original INVITE.
	const uint32_t fromIp = source.sin_addr.s_addr;
	return fromIp == src->getAddress().sin_addr.s_addr ||
	       fromIp == dest->getAddress().sin_addr.s_addr;
}

// ── BLF presence: FSM lives in BlfSubscriptions.cpp ──────────────────────────

void RequestsHandler::onSubscribe(std::shared_ptr<SipMessage> data)
{
	_blf.onSubscribe(data);
}

// ── RFC 4028 session timers ───────────────────────────────────────────────────

void RequestsHandler::armSessionTimer(Session* session,
                                       const std::shared_ptr<SipMessage>& ok200)
{
	uint32_t secs = ok200->getSessionExpiresSecs();
	if (secs == 0) return;
	auto ref = ok200->getSessionExpiresRefresher();
	bool weRefresh = (ref == "uas");
	session->armSessionTimer(secs, weRefresh, std::chrono::steady_clock::now());
	session->setDialogHeaders(std::string(ok200->getFrom()), std::string(ok200->getTo()));
}

void RequestsHandler::sweepSessionTimers(std::chrono::steady_clock::time_point now)
{
	std::vector<std::string> toExpire;
	for (const auto& [callID, session] : _sessions)
	{
		if (session->getSessionExpiresSeconds() == 0) continue;
		const auto st = session->getState();
		if (st != Session::State::Connected && st != Session::State::Held) continue;

		if (now >= session->getSessionExpiry())
		{
			toExpire.push_back(callID);
		}
	}

	for (const auto& callID : toExpire)
	{
		auto it = _sessions.find(callID);
		if (it == _sessions.end()) continue;
		auto session = it->second;
		auto src  = session->getSrc();
		auto dest = session->getDest();
		const std::string& dFrom = session->getDialogFrom();
		const std::string& dTo   = session->getDialogTo();

		// Guard on both dialog headers: a BYE with an empty From or To is malformed
		// and phones will drop it, leaving the session alive and re-firing every sweep
		// tick. dTo can be empty if armSessionTimer was invoked before the 200 OK set
		// dialog headers (e.g. a partial onReinvite path). (#72)
		if (src && !dFrom.empty() && !dTo.empty())
		{
			auto b = buildServerBye(src->getNumber(), src->getAddress(), callID, dTo, dFrom);
			if (b) _outbox.emplace_back(src->getAddress(), std::move(b));
		}
		if (dest && !dFrom.empty() && !dTo.empty())
		{
			auto b = buildServerBye(dest->getNumber(), dest->getAddress(), callID, dFrom, dTo);
			if (b) _outbox.emplace_back(dest->getAddress(), std::move(b));
		}
		queueLog("[session timer] expired — BYE sent for " + callID, true);
		endCall(callID,
		        src  ? src->getNumber()  : "",
		        dest ? dest->getNumber() : "",
		        "session timer expired");
	}
}

// ── Paging zones (980–989) ────────────────────────────────────────────────────

const pbx::PageZone* RequestsHandler::findPageZone(const std::string& extension) const
{
	auto it = _pageZones.find(extension);
	return (it == _pageZones.end()) ? nullptr : &it->second;
}

bool RequestsHandler::isPageZoneDialog(const std::string& extension) const
{
	return pbx::isPageZoneExt(extension) && _pageZones.find(extension) != _pageZones.end();
}

void RequestsHandler::setPageZone(const std::string& zoneExt, const std::string& members)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		if (!pbx::isPageZoneExt(zoneExt))
		{
			queueLog("Page zone ignored for non-zone extension " + zoneExt +
				" (zones are 980-989)", true);
		}
		else
		{
			std::vector<std::string> list = pbx::splitZoneMembers(members);
			if (list.empty())
			{
				_pageZones.erase(zoneExt);
				queueLog("Page zone " + zoneExt + " deleted");
				persistPageZones();
			}
			else
			{
				bool isNew = (_pageZones.find(zoneExt) == _pageZones.end());
				if (isNew && _pageZones.size() >= static_cast<size_t>(POCKETDIAL_MAX_PAGE_ZONES))
				{
					queueLog("Page zone ignored (table full) for " + zoneExt, true);
				}
				else
				{
					pbx::PageZone& z = _pageZones[zoneExt];
					z.members = std::move(list);
					queueLog("Page zone " + zoneExt + " = " + pbx::joinMembers(z.members));
					persistPageZones();
				}
			}
		}

		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot.pageZones.clear();
			_snapshot.pageZones.reserve(_pageZones.size());
			for (const auto& [ext, z] : _pageZones)
			{
				_snapshot.pageZones.emplace_back(ext, pbx::joinMembers(z.members));
			}
		}

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

std::vector<std::pair<std::string, std::string>> RequestsHandler::getPageZones()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.pageZones;
}

// ── Call parking (orbits 700–709): FSM lives in ParkOrbit.cpp ────────────────

std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> RequestsHandler::drainOutbox()
{
	// The single exit every deferred message passes through. RFC 3261 §17:
	// register outgoing INVITEs for retransmit here, so Timer A/B coverage is
	// structural rather than something each flush site re-implements — a new
	// flush path would otherwise send one-shot UDP INVITEs that are simply lost
	// on a dropped packet. maybeTrack filters to INVITE requests, so responses
	// and NOTIFYs are ignored.
	//
	// Ordering matters (#70): the scan must run after everything that appends to
	// _outbox during this pass (BLF NOTIFYs, tick()-originated forks — park
	// ring-back, hunt-group next-ring, CFNA redirect), which is exactly why it
	// belongs at the drain rather than at any individual enqueue.
	for (const auto& [addr, msg] : _outbox)
	{
		_txLayer.maybeTrack(addr, msg);
		// Issue #33: /api/pcap capture, outbound side. Same single choke point as
		// the retransmit registration above — every deferred message leaves
		// through here regardless of which call site (handle(), tick(),
		// sendMessageTo()) queued it.
		msg->toString(_pcapCapture.recordInto(/*outbound=*/true, addr));
	}

	auto drained = std::move(_outbox);
	_outbox.clear();
	return drained;
}

void RequestsHandler::refreshParkSnapshot()
{
	auto rows = _park.snapshotRows(std::chrono::steady_clock::now(), /*onlyParked=*/false);
	std::lock_guard<std::mutex> snapLock(_snapshotMutex);
	_snapshot.parkedCalls = std::move(rows);
}

std::vector<std::tuple<std::string, std::string, std::string, int>> RequestsHandler::getParkedCalls()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.parkedCalls;
}

// ── Build helpers ─────────────────────────────────────────────────────────────

std::shared_ptr<SipMessage> RequestsHandler::buildOkWithSdp(
	const std::shared_ptr<SipMessage>& inviteMsg,
	const std::string& activeIp,
	const std::string& toTag,
	const std::string& sdpBody)
{
	auto ok = getMessageFromPool(*inviteMsg);
	if (!ok) return nullptr;   // pool exhausted: propagate, caller drops (#101A)
	ok->setHeader(SipMessageTypes::OK);
	ok->setVia(std::string(inviteMsg->getVia()) + ";received=" + activeIp);
	ok->setTo(std::string(inviteMsg->getTo()) + ";tag=" + toTag);
	ok->setContact(buildContact(inviteMsg->getToNumber()));
	ok->clearBody();
	{
		std::string raw = ok->toString();
		size_t sep = raw.find("\r\n\r\n");
		if (sep != std::string::npos)
		{
			std::string_view headerView(raw.data(), sep);
			if (headerView.find("application/sdp") == std::string_view::npos)
			{
				raw.insert(sep, "\r\nContent-Type: application/sdp");
				sep = raw.find("\r\n\r\n");
			}
			raw.erase(sep + 4);
			raw += sdpBody;
		}
		ok->reset(std::move(raw), inviteMsg->getSource());
	}
	ok->enforceG711();
	ok->syncContentLength();
	return ok;
}

std::shared_ptr<SipMessage> RequestsHandler::buildServerBye(
	const std::string& destExt,
	const sockaddr_in& destAddr,
	const std::string& callId,
	const std::string& fromHeader,
	const std::string& toHeader)
{
	std::string activeIp = _localIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	std::string destIpPort = sipwire::addrToIpPort(destAddr);
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);

	std::ostringstream ss;
	ss << "BYE sip:" << destExt << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From: " << stripHeaderName(fromHeader) << "\r\n"
	   << "To: " << stripHeaderName(toHeader) << "\r\n"
	   << "Call-ID: " << stripHeaderName(callId) << "\r\n"
	   << "CSeq: 2 BYE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";

	return getMessageFromPool(ss.str(), destAddr);
}

std::shared_ptr<SipClient> RequestsHandler::allocateVirtualPeer(std::string number, sockaddr_in address, int expiresSeconds)
{
	// A virtual peer is owned solely by the Session._dest it backs, so a pool slot is
	// free precisely when only the pool itself still references it (use_count()==1).
	for (auto& peer : _virtualPeerPool)
	{
		if (peer.use_count() == 1)
		{
			peer->reset(std::move(number), address, expiresSeconds);
			return peer;
		}
	}
	// Pool drained: fall back to the heap, bounded the same way the message pool
	// is (Issue #101(A)). Past the ceiling this returns nullptr and the caller
	// abandons the park/BLF operation rather than allocating without limit.
	//
	// No pool lock here, unlike acquirePooledMessage(): _virtualPeerPool is a
	// per-instance member and every caller — the internal sites and ParkOrbit via
	// PbxEnv::allocVirtualPeer — already runs under _mutex. The counter is still
	// atomic because its decrement happens in the deleter, which runs wherever
	// the owning Session finally releases it.
	if (s_vpeerHeapFallbacksInFlight.load(std::memory_order_relaxed) >= POCKETDIAL_VPEER_HEAP_FALLBACK_MAX)
	{
		static std::atomic<std::size_t> warnCount{0};
		logPoolExhausted("Virtual-peer", warnCount);
		return nullptr;
	}

	SipClient* raw = nullptr;
	try
	{
		raw = new SipClient(std::move(number), address, expiresSeconds);
	}
	catch (const std::bad_alloc&)
	{
		return nullptr;   // budget untouched
	}
	s_vpeerHeapFallbacksInFlight.fetch_add(1, std::memory_order_relaxed);
	try
	{
		return std::shared_ptr<SipClient>(raw, VpeerFallbackDeleter{});
	}
	catch (const std::bad_alloc&)
	{
		// Constructor already ran the deleter on `raw` — freed and decremented.
		return nullptr;
	}
}
