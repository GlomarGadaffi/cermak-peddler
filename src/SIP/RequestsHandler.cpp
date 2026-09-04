// RequestsHandler.cpp: Issues #24 and #28 resolved.
#include "RequestsHandler.hpp"
#include "SipMessagePool.hpp"
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

	// NVS namespace for the persisted PBX config, pbxcfg (loadAdminHttpTtl here;
	// DtmfFeatureCodes::load()/saveAdminExt() too) is pbxpersist::kNvsNamespace
	// (PbxPersist.hpp) so there is exactly one definition of "pbxcfg" in the
	// codebase. The CDR ring's own namespace ("cdrlog") lives on CdrRing.cpp.
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
	sipmsgpool::ensureInitialized();
	_virtualPeerPool.reserve(POCKETDIAL_VIRTUAL_PEERS);
	for (int i = 0; i < POCKETDIAL_VIRTUAL_PEERS; ++i)
	{
		_virtualPeerPool.push_back(std::make_shared<SipClient>());
	}

	// Reload persisted PBX config (call-forward / ring groups) and the CDR ring from
	// NVS so they survive reboot. No-ops on host. Construction is single-threaded
	// (no handler is dispatching yet), so these run without holding _mutex.
	_cfg.loadPbxConfig();
	_cdr.load();
	// Task 2B: load the admin extension from NVS (defaults to "1001" if absent).
	_dtmf.load();
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

// Same bounded-fallback bookkeeping for the virtual-peer pool as the message
// pool uses (SipMessagePool.cpp). Separate budget: a virtual peer is a
// long-lived per-park-slot stand-in, not a per-packet object.
static std::atomic<std::size_t> s_vpeerHeapFallbacksInFlight{0};

namespace
{
	// Same no-locking rule as SipMessagePool's HeapFallbackDeleter: the last
	// reference can drop while a pool-critical-section lock is held elsewhere,
	// so the deleter must not itself take any lock.
	struct VpeerFallbackDeleter
	{
		void operator()(SipClient* p) const noexcept
		{
			delete p;
			s_vpeerHeapFallbacksInFlight.fetch_sub(1, std::memory_order_relaxed);
		}
	};
}

// Forwarders onto the static pool in SipMessagePool.cpp (Issue #53 / #101(A) /
// #101(E)). Kept as public statics on RequestsHandler because SipMessageFactory,
// the handler table, and the test suite all call them by this name.
std::shared_ptr<SipMessage> RequestsHandler::getMessageFromPool(std::string_view message, sockaddr_in src)
{
	return sipmsgpool::getMessageFromPool(message, src);
}

std::shared_ptr<SipMessage> RequestsHandler::getMessageFromPool(const SipMessage& source)
{
	return sipmsgpool::getMessageFromPool(source);
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

void RequestsHandler::handle(std::shared_ptr<SipMessage> request, std::string_view rawBytes)
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
		// Written straight into the ring slot — no per-packet temporary inside
		// the critical section (Issue #101(D)).
		std::string& pcapSlot = _pcapCapture.recordInto(/*outbound=*/false, request->getSource());
		if (!rawBytes.empty())
		{
			// Issue #105: capture the exact bytes recvfrom() delivered, not a
			// re-serialization of the parsed message — whitespace, compact header
			// forms (f:/t:/v:/i:), CRLF-vs-LF tolerance, or any malformed-but-
			// tolerated line the parser normalized must survive in the capture.
			pcapSlot.assign(rawBytes.data(), rawBytes.size());
		}
		else
		{
			// No wire bytes offered (a message built in-process, or a test calling
			// handle() directly with no UdpServer/SipServer involved) — the parsed
			// form is genuinely what such a caller means to inspect.
			request->toString(pcapSlot);
		}

		// ── SDP admission gate (docs/THREAT_MODEL.md T-7) ──────────────────────
		// Every SDP body is structurally checked HERE, once, before any handler
		// decodes it and before it can be relayed to a peer phone -- initial
		// INVITE, re-INVITE, UPDATE, ACK and every response alike. A well-formed
		// SIP start line says nothing about the body: the UNISOC T612 RCE rode in
		// on a normal MMTel video offer whose a= lines were the poison. The check
		// is one flat, allocation-free pass (SipMessage::checkSdp); a violation is
		// a hard error for the whole message, never "keep tokenizing". It sits
		// after the pcap capture on purpose, so the refused bytes are there to
		// look at.
		bool sdpRefused = false;
		if (request->hasSdp() && !request->getBody().empty())
		{
			const auto verdict = request->checkSdp();
			if (verdict != SipMessage::SdpVerdict::Ok)
			{
				rejectSdp(request, verdict);
				sdpRefused = true;
			}
		}

		auto client = findClientByAddress(request->getSource());
		if (client.has_value())
		{
			client.value()->markActive();
		}

		maybeSweep();

		// A refused SDP body skips the transaction layer and the handler table
		// entirely: a poison 200 OK must not "accept" an INVITE transaction any
		// more than it may be relayed. The pool slot is released with the shared_ptr
		// like any other unhandled packet.
		if (!sdpRefused)
		{
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
				_dtmf.onInfo(request);
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
		}   // !sdpRefused

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

	if (destNumber == ConferenceRoom::EXT)
	{
		// CANCEL of a conference dial-in: endCall() drops the leg (see its
		// ConferenceRoom::leave call), so the room needs nothing extra here.
		endCall(data->getCallID(), data->getFromNumber(), ConferenceRoom::EXT);
		return;
	}

	if (_park.orbitIndex(destNumber) >= 0)
	{
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (destNumber == "999" || _cfg.isPageZoneDialog(destNumber))
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

	// Codec gate, before any session is allocated: an offer with no audio codec
	// this PBX will relay (Opus-only, G.729-only ...) gets a clean 488 now,
	// instead of a 200 OK whose rewritten m-line advertised payloads the phone
	// never offered -- the "signalling completes, media is dead" failure
	// PHONE_COMPATIBILITY.md used to document as a phone-side setting.
	if (data->hasSdp() && !data->offersSupportedAudio(/*allowWideband=*/true))
	{
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader("SIP/2.0 488 Not Acceptable Here");
		response->clearBody();
		response->addHeader("Warning", "304 " + _localIp + " \"No compatible audio codec (PCMU/PCMA/G722)\"");
		response->setVia(std::string(data->getVia()) + ";received=" + _localIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	// Secure mode: registration auth alone leaves call setup open to anyone who
	// can reach UDP/5060 (drawbridge #125). Challenge the INVITE with the same
	// digest machinery -- admitSecure() takes the method from the request line,
	// so it verifies against INVITE. The stateless 401 needs no session; the
	// credentialed retry arrives with CSeq+1 and falls through here. Learn mode
	// keeps its TOFU semantics and Open mode never challenges.
	if (_registrar.getMode() == RegistrarMode::Secure)
	{
		std::string rejectReason;
		const Registrar::AuthDecision decision =
			_registrar.admitSecure(data, std::string(data->getFromNumber()), rejectReason);
		if (decision == Registrar::AuthDecision::Challenge) return;   // 401 enqueued
		if (decision == Registrar::AuthDecision::Reject)
		{
			_registrar.sendForbidden(data, rejectReason.empty() ? "Forbidden" : rejectReason);
			return;
		}
	}

	std::string destNumber(data->getToNumber());
	if (destNumber == "777")
	{
		// SDP loopback echo test. Issue #115: allocateSession() must be called
		// FIRST, before any 180/200 is built, mirroring the ordinary call-to-call
		// INVITE path below (and the hunt-group path above) — otherwise a full
		// session pool still gets a "successful" 200 OK with no _sessions entry
		// backing it, silently bypassing POCKETDIAL_MAX_SESSIONS.
		auto newSession = allocateSession(std::string(data->getCallID()), caller.value());
		if (!newSession)
		{
			auto responseObj = getMessageFromPool(*data);
			if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
			responseObj->setHeader("SIP/2.0 503 Service Unavailable");
			responseObj->clearBody();
			responseObj->setContact(buildContact(caller.value()->getNumber()));
			_outbox.emplace_back(data->getSource(), std::move(responseObj));
			return;
		}

		// Draw BOTH responses before publishing the session (mirrors the 440 media
		// path's "OK drawn before the session is published" comment above
		// onMediaInvite): once _sessions.emplace()+Connected below has run, a
		// message-pool refusal here would abandon a dialog with no 180/200 ever
		// sent and no clean way back (the retransmission guard at the top of this
		// function would then silently drop the caller's retry). Refusing now, before
		// either mutation, leaves nothing behind — allocateSession() already reset
		// newSession's Call-ID but never published it to _sessions, so the next
		// allocateSession() call reclaims this same slot as free (see its scan for a
		// slot whose Call-ID is absent from _sessions).
		auto ringing = getMessageFromPool(*data);
		if (!ringing) return;   // pool exhausted: drop, peer retransmits (#101A)
		auto okResponse = getMessageFromPool(*data);
		if (!okResponse) return;   // pool exhausted: drop, peer retransmits (#101A)

		// Per-session dummy dest (never the old shared _dummyClient): a concurrent
		// 777/440 call must not overwrite this session's destination identity. The
		// shared_ptr lives as long as the session references it (released on
		// teardown / pool reuse) — bounded by the session pool, not the packet path.
		auto dummy777 = std::make_shared<SipClient>();
		dummy777->reset("777", data->getSource(), 3600);
		newSession->setDest(dummy777);
		_sessions.emplace(data->getCallID(), newSession);
		newSession->setState(Session::State::Connected);

		ringing->setHeader("SIP/2.0 180 Ringing");
		ringing->clearBody();
		std::string activeIp = _localIp;
		ringing->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		std::string toTag = IDGen::GenerateID(9);
		ringing->setTo(std::string(data->getTo()) + ";tag=" + toTag);
		ringing->setContact(buildContact("777"));
		_outbox.emplace_back(data->getSource(), std::move(ringing));

		okResponse->setHeader(SipMessageTypes::OK);
		okResponse->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		okResponse->setTo(std::string(data->getTo()) + ";tag=" + toTag);
		okResponse->setContact(buildContact("777"));
		okResponse->enforceG711();
		_outbox.emplace_back(data->getSource(), std::move(okResponse));
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
		if (const pbx::PageZone* zone = _cfg.findPageZone(destNumber))
		{
			routePageZone(data, caller.value(), *zone);
			return;
		}
	}

	if (destNumber == "440")
	{
		// Media beachhead: the server answers and sources a one-way RTP tone stream.
		onMediaInvite(data, caller.value());
		return;
	}

	if (destNumber == ConferenceRoom::EXT)
	{
		// Meet-me conference (888): the server answers, joins this caller to the one
		// shared MixBus, and mixes. N callers dialing 888 are N legs of one conference.
		onConferenceInvite(data, caller.value());
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

	// Directed / group call pickup (Issue #68): *8 answers the oldest ringing
	// call among the picker's ring-group co-members; **<ext> answers a named
	// extension's ringing call. Both are restricted to the picker's own
	// pickup group (same ring-group co-membership check) — see
	// PbxConfig.hpp's doc comment for why directed pickup isn't exempted.
	if (pbx::isGroupPickupCode(destNumber))
	{
		onPickup(data, caller.value(), _cfg.pickupPeersOf(caller.value()->getNumber()));
		return;
	}
	if (std::string target = pbx::directedPickupTarget(destNumber); !target.empty())
	{
		auto peers = _cfg.pickupPeersOf(caller.value()->getNumber());
		bool eligible = target != caller.value()->getNumber() &&
			std::find(peers.begin(), peers.end(), target) != peers.end();
		onPickup(data, caller.value(), eligible ? std::vector<std::string>{ target } : std::vector<std::string>{});
		return;
	}

	// Ring / hunt groups (Class A sweep): a configured group extension (e.g. 6xx)
	// maps to an ordered member list. Ring-all reuses the broadcast fork (without the
	// intercom auto-answer headers, so members ring normally); hunt rings members one
	// at a time, driven from tick(). Resolved before DND because a group ext is not a
	// real endpoint and so never carries its own DND/forward config.
	if (const pbx::RingGroup* group = _cfg.findRingGroup(destNumber))
	{
		routeRingGroup(data, caller.value(), destNumber, *group);
		return;
	}

	// Dial plan (Issue #69): the bounded, ordered pattern → action rule table.
	// Deliberately evaluated HERE — after every reserved virtual extension (777,
	// 999, 98x zones, 440, 70x orbits) and after the direct ring-group lookup, but
	// before CFU/DND/extension lookup. That ordering is what makes the feature
	// additive rather than a behavior change: a rule can only ever intercept a
	// dialed number that would otherwise have reached the ordinary extension
	// lookup (and, for an unknown number, 404). No rule can shadow the echo test,
	// a park retrieval or a configured group extension — so even a catch-all "*"
	// pattern leaves the built-ins working. Nothing matched ⇒ routeDialPlan()
	// returns false and everything below runs exactly as it did before #69.
	if (routeDialPlan(data, caller.value(), destNumber))
	{
		return;
	}

	// Call Forward Unconditional (CFU): if the destination has an "always" forward
	// target, redirect the call to that target before ringing the original callee.
	{
		std::string cfu = _cfg.getForwardTarget(destNumber, "always");
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
	if (_cfg.isDndEnabled(destNumber))
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

	// Retain the original INVITE on every direct-call session — not only when
	// the callee has a conditional forward (busy/no-answer) configured.
	// onBusy()/tick() already relied on this for CFB/CFNA; it's now also how
	// isSessionRingingExt() finds "who's ringing" for call pickup (Issue #68)
	// without needing to pre-populate Session::dest before an answer exists.
	newSession->setInviteMessage(data);
	std::string cfb  = _cfg.getForwardTarget(destNumber, "busy");
	std::string cfna = _cfg.getForwardTarget(destNumber, "noanswer");
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

std::string RequestsHandler::buildMediaSdp(const std::string& serverIp, int rtpPort, bool sendrecv)
{
	// The server's OWN SDP offer/answer for a server-media call. PCMU (PT 0) only —
	// matches enforceG711()/the codec the rest of the PBX speaks. Sendonly (the 440
	// tone default): the server streams to the caller and ignores any media the caller
	// sends back. Sendrecv is what a conference leg (888) needs — the mix is only a
	// mix if the phone actually sends its own audio up.
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
	s += sendrecv ? "a=sendrecv\r\n" : "a=sendonly\r\n";
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
	// The 200 OK is drawn BEFORE the session is published, because past
	// _sessions.emplace() + Connected there is no clean way back: the caller's
	// INVITE retransmit would hit the _rtpSender.isActive() guard at the top of
	// this function and be answered 486 Busy Here, with the tone still streaming
	// to a peer that never got an answer. Unwinding the stream here — the same
	// thing the session-pool-full path above does — leaves the retransmit a clean
	// retry (#101A).
	auto ok = getMessageFromPool(*data);
	if (!ok)
	{
		_rtpSender.stop(std::string(data->getCallID()));
		queueLog("440 media: message pool exhausted, stream unwound", true);
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

// ── Local N-way conference (virtual extension 888) ───────────────────────────
// Issue #75 / docs/CONFERENCE_MIXER.md §7. Every caller that dials 888 gets its own
// leg on ONE shared ConferenceRoom: a MediaBridge in BUS mode, its own RTP receive
// port, and one MixBus port. The bus's single tick then hands each leg the saturated
// sum of the OTHER legs, so a third caller joining is just a third port — no signaling
// fan-out, no per-pair wiring, and a leg leaving only marks its port Draining.
//
// Ordering discipline is copied deliberately from onMediaInvite() above (Issue #115):
// parse the caller's RTP -> join the room -> allocate the session -> draw the 200 OK
// from the message pool -> only THEN publish the session and answer. Every failure
// before the answer unwinds what it started, so the caller's INVITE retransmit finds
// a clean slate instead of a half-joined leg with no dialog behind it.
void RequestsHandler::onConferenceInvite(std::shared_ptr<SipMessage> data,
	const std::shared_ptr<SipClient>& caller)
{
	const std::string activeIp = _localIp;
	const std::string callID(data->getCallID());
	const std::string confExt(ConferenceRoom::EXT);

	auto refuse = [&](const char* statusLine, const char* why) {
		auto msg = getMessageFromPool(*data);
		if (!msg) return;   // pool exhausted: drop, peer retransmits (#101A)
		msg->setHeader(statusLine);
		msg->clearBody();
		msg->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		msg->setContact(buildContact(confExt));
		_outbox.emplace_back(data->getSource(), std::move(msg));
		queueLog("888 conference: " + std::string(why) + " for "
			+ std::string(data->getFromNumber()), true);
	};

	// Where does this phone want its audio? Same c=/m= parse the 440 path uses.
	std::string destIp;
	uint16_t destPort = 0;
	if (!parseCallerRtp(data, destIp, destPort))
	{
		refuse(SipMessageTypes::BAD_REQUEST, "no usable RTP destination in INVITE");
		return;
	}

	// The room (and its single mix-tick driver) is built on the first dial-in and then
	// kept for the life of the process: standing the tick task up and down underneath
	// legs whose RTP tasks may still be in flight is exactly the teardown race the bus's
	// Draining state exists to avoid. Idle cost is the bus rings; see PoolConfig.hpp.
	if (!_conference)
	{
		_conference = std::make_unique<ConferenceRoom>();
		_conference->startDriver();
		queueLog("888 conference: room created (" + std::to_string(ConferenceRoom::MAX_LEGS)
			+ " legs, " + std::to_string(ConferenceRoom::TICK_MS) + " ms mix tick)");
	}

	// Join first: a full room must not consume a session slot. The leg index is also
	// the proof the media actually came up, so nothing is answered on a dead leg.
	const int leg = _conference->join(callID, std::string(caller->getNumber()), destIp, destPort);
	if (leg < 0)
	{
		refuse("SIP/2.0 486 Busy Here", "room full or media failed to start");
		return;
	}

	auto newSession = allocateSession(callID, caller);
	if (!newSession)
	{
		_conference->leave(callID);
		refuse("SIP/2.0 503 Service Unavailable", "session pool full");
		return;
	}

	// Draw the answer BEFORE publishing the session — past _sessions.emplace() the
	// retransmission guard at the top of onInvite() silently drops the caller's retry,
	// so a pool refusal here would strand a joined leg with no answer ever sent.
	// The SDP advertises THIS LEG's receive port (not the 440 sender's port): that is
	// where the handset must send its audio for the mix to hear it.
	const std::string toTag = IDGen::GenerateID(9);
	const std::string sdpBody = buildMediaSdp(activeIp, _conference->rtpPortFor(callID),
		/*sendrecv=*/true);
	auto ok = buildOkWithSdp(data, activeIp, toTag, sdpBody);
	if (!ok)
	{
		_conference->leave(callID);
		queueLog("888 conference: message pool exhausted, leg unwound", true);
		return;
	}

	// Per-session dummy dest (never a shared client) so a concurrent 777/440/888 call
	// can't overwrite this call's destination identity.
	auto dummyConf = allocateVirtualPeer(confExt, data->getSource());
	newSession->setDest(dummyConf);
	_sessions.emplace(callID, newSession);
	newSession->setState(Session::State::Connected);

	_outbox.emplace_back(data->getSource(), std::move(ok));

	queueLog("888 conference: " + std::string(caller->getNumber()) + " joined leg "
		+ std::to_string(leg) + " (" + std::to_string(_conference->legCount()) + "/"
		+ std::to_string(ConferenceRoom::MAX_LEGS) + "), media to "
		+ destIp + ":" + std::to_string(destPort));
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
		std::string cfb = _cfg.getForwardTarget(busyExt, "busy");
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

	if (destNumber == ConferenceRoom::EXT)
	{
		// Conference hang-up: 200 OK the BYE and end the call. endCall() releases the
		// MixBus port (Active -> Draining); the remaining legs keep mixing untouched.
		auto response = getMessageFromPool(*data);
		if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (destNumber == "999" || _cfg.isPageZoneDialog(destNumber))
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
				// NOT a `return` on refusal: the caller's 200 OK is already queued
				// above, so it has its final response and will never retransmit this
				// BYE. Bailing here would skip endCall() below and leak the session
				// with no CDR. Losing the fork only leaves the paged phone to time
				// out its own dialog; losing the teardown is unrecoverable (#101A).
				auto byeFork = getMessageFromPool(*data);
				if (byeFork)
				{
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
		}
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	// Cross-dialog bridge teardown (Issue #68 call pickup — and, as a
	// byproduct, ParkOrbit's retrieve/ring-back legs, which set peerCallID
	// too but had no relay wired up for it before this). A BYE here ends ONE
	// of two independently-dialogued legs that only the server's own
	// bookkeeping links together; relaying the raw BYE as-is (its own
	// Call-ID) would be rejected 481 by the peer's phone (wrong dialog), so a
	// peer-addressed BYE has to be built fresh from the PEER session's own
	// dialog identifiers — exactly what sweepSessionTimers() already does for
	// a single session's own two legs, generalized here across two sessions.
	if (session.has_value() && !session.value()->getPeerCallID().empty())
	{
		std::string peerCallId = session.value()->getPeerCallID();
		if (auto peerSession = getSession(peerCallId); peerSession.has_value())
		{
			auto peer = peerSession.value();
			// Issue #72's guard, reused: a BYE with an empty From or To is
			// malformed and phones drop it. onPickup() always captures both
			// via setDialogHeaders(), but ParkOrbit's retrieve/ring-back legs
			// (the other peerCallID-setting path) don't yet — so a park leg
			// still gets the endCall() cleanup below, just not a peer-phone
			// BYE, rather than emitting a "From: \r\nTo: \r\n" packet.
			if (auto notify = peer->getSrc();
				notify && !peer->getDialogFrom().empty() && !peer->getDialogTo().empty())
			{
				auto bye = buildServerBye(notify->getNumber(), notify->getAddress(),
					peerCallId, peer->getDialogTo(), peer->getDialogFrom());
				if (bye) _outbox.emplace_back(notify->getAddress(), std::move(bye));
			}
			endCall(peerCallId, peer->getSrc() ? peer->getSrc()->getNumber() : "",
				peer->getDest() ? peer->getDest()->getNumber() : "", "peer leg ended (bridged call)");
		}

		auto response = getMessageFromPool(*data);
		if (response)
		{
			response->setHeader(SipMessageTypes::OK);
			response->setVia(std::string(data->getVia()) + ";received=" + _localIp);
			response->clearBody();
			_outbox.emplace_back(data->getSource(), std::move(response));
		}
		endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), "bridged call ended");
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
			// Applies equally to a broadcast/ring-group session once it is
			// Connected or Held (#74): after the first-answer connect path runs,
			// getSrc()/getDest() name exactly the two live legs (original caller,
			// answering client) the same way a unicast session's do, so the same
			// source-address peer lookup relays a hold/resume 200 OK for either.
			{
				const auto st = session.value()->getState();
				if (st == Session::State::Connected || st == Session::State::Held)
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
				// the connect path below. A hold/resume re-INVITE's 200 OK arrives while
				// the session is Connected or Held, which the block above already relays
				// and returns from (#74) — so only a genuine new answer from a pending
				// fork ever reaches this point (#69b).
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

					// Drawn BEFORE the session is advanced: the branch above only
					// re-enters while the state is still Invited, so refusing after
					// setState(Connected) would strand the call permanently — the
					// answering phone's 200 OK retransmit could never get back in
					// here. Acquire first, mutate second (#101A).
					auto response = getMessageFromPool(*data);
					if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)

					session->get()->setDest(answeringClient);
					session->get()->setState(Session::State::Connected);
					session->get()->clearRingTimer();   // hunt answered: disarm timeout

					auto inviteMsg = session.value()->getInviteMessage();

					response->setContact(buildContact(answeringClient->getNumber()));

					if (inviteMsg)
					{
						std::string originalTo(inviteMsg->getTo());
						std::string bTo(data->getTo());
						siphdr::appendTagFrom(originalTo, bTo);
						response->setTo(originalTo);
					}

					// Relayed answer on a peer-to-peer leg: keep the callee's codec
					// pick and order (it already intersected the caller's offer),
					// dropping only what this PBX won't carry. Never the blind
					// "0 8 101" rewrite -- that advertised payloads the caller
					// never offered and broke wideband/dynamic-PT phones.
					(void)response->filterAudioCodecs(/*allowWideband=*/true);
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

			// Issue #68 pickup race: once this session is already connected to
			// someone ELSE (a directed/group pickup answered first), a late
			// 200 OK from this call's now-superseded (CANCELed) fork must not
			// resurrect/steal the session — a CANCEL and a 2xx can legally
			// cross on the wire. Silently drop it, same treatment the
			// isBroadcast() branch above already gives a post-connect answer.
			// A genuine retransmit of the WINNING answer (dest already ==
			// this responder) still falls through to the normal path below.
			if (session.value()->getState() != Session::State::Invited &&
				!(session.value()->getDest() &&
					session.value()->getDest()->getNumber() == client.value()->getNumber()))
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

	if (destNumber == ConferenceRoom::EXT)
	{
		// The server is the UAS on a conference leg (as with 777): the ACK completes
		// our own 200 OK and there is no second leg to relay it to. Media is already
		// flowing — the leg joined the bus when the INVITE was answered.
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

bool RequestsHandler::buildInviteFork(const std::shared_ptr<SipMessage>& invite,
	const std::shared_ptr<SipClient>& caller,
	const std::shared_ptr<SipClient>& target,
	bool intercom)
{
	auto inviteFork = getMessageFromPool(*invite);
	// Returns bool because callers report success on this function's behalf —
	// huntRingNext arms a 20 s no-answer timer, redirectInvite NOTIFYs the
	// transferor. A silent void return had them announcing an INVITE that was
	// never sent (#101A).
	if (!inviteFork) return false;
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
	// Caller's offer relayed peer-to-peer: preserve its preference order, drop
	// only unsupported payloads (onInvite already 488'd offers with nothing left).
	(void)inviteFork->filterAudioCodecs(/*allowWideband=*/true);
	_outbox.emplace_back(target->getAddress(), std::move(inviteFork));
	return true;
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
	std::string contactExt = intercom ? std::string("999") : std::string(invite->getToNumber());

	// Drawn BEFORE the session is published. Refusing after _sessions.emplace()
	// would register a session under this Call-ID with no target ever invited and
	// — unlike the hunt path — no ring timer for tick() to sweep, so it would sit
	// there permanently; the INVITE retransmit's duplicate emplace() is a silent
	// no-op, so it would not replace the zombie either (#101A).
	auto ringing = getMessageFromPool(*invite);
	if (!ringing) return;   // pool exhausted: drop, peer retransmits (#101A)

	newSession->setBroadcast(true);
	newSession->setPendingTargets(targets);
	newSession->setInviteMessage(invite);
	_sessions.emplace(invite->getCallID(), newSession);

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
		if (!buildInviteFork(invite, caller, mc.value(), /*intercom=*/false))
		{
			// Nothing was rung, so do NOT arm the no-answer timer — that would burn
			// the full NO_ANSWER_TIMEOUT waiting on a member that never got an
			// INVITE. Try the next member instead (#101A).
			continue;
		}
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
			// true, not false, even though nothing was sent. false here means
			// "target not registered — fall through", which on the CFU path would
			// ring the extension the subscriber explicitly forwarded away from, and
			// on the REFER path would report "target not registered" for a target
			// that is. The target WAS resolved; we just could not serve it. Same
			// answer as the 503 branch below (#101A).
			if (!responseObj) return true;
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
	// this dialog's entry so DtmfFeatureCodes's _dtmfState can't grow unbounded
	// across calls (Fix #4).
	_dtmf.forgetCall(callID);

	// RFC 3261 §17: free any transaction slots tracking retransmits for this call.
	_txLayer.freeForCallId(callID);
	// Free any park orbit slot holding this call's parked leg.
	_park.freeForCallId(callID);
	// Drop this call's conference leg, if it had one. Idempotent for every other
	// call, so this is the ONE place a 888 leg is released — BYE, CANCEL, a session
	// timer expiring and the orphan sweep all funnel through endCall(), and none of
	// them has to remember the room exists. Releasing the leg only marks its MixBus
	// port Draining; the next mix tick reclaims the rings without touching the others.
	if (_conference)
	{
		_conference->leave(std::string(callID));
	}

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
		_cdr.record(ending, srcNumber, destNumber);

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

uint64_t RequestsHandler::getSdpRejected() const
{
	return _sdpRejected.load(std::memory_order_relaxed);
}

void RequestsHandler::rejectSdp(const std::shared_ptr<SipMessage>& request, SipMessage::SdpVerdict verdict)
{
	_sdpRejected.fetch_add(1, std::memory_order_relaxed);
	const char* why = SipMessage::sdpVerdictText(verdict);

	// Only a request other than ACK takes a final response (RFC 3261 §17.1.1.3,
	// §8.2.6). A poison response or ACK is simply not acted on: not relayed, not
	// matched against a transaction, not used to advance a session.
	const bool isResponse = request->getStatusInfo().has_value();
	if (isResponse || request->getType() == SipMessageTypes::ACK)
	{
		queueLog("[SIP] SDP refused (" + std::string(why) + "), dropped: " +
			std::string(request->getHeader()) + " from " + std::string(request->getFromNumber()), true);
		return;
	}

	auto response = getMessageFromPool(*request);
	if (!response) return;   // pool exhausted: drop, peer retransmits (#101A)
	response->setHeader("SIP/2.0 488 Not Acceptable Here");
	response->clearBody();
	// Warning 399 (miscellaneous, RFC 3261 §20.43) with the reason, so the
	// refusal is diagnosable from the phone's SIP trace rather than a mystery 488.
	response->addHeader("Warning", "399 " + _localIp + " \"SDP refused: " + why + "\"");
	response->setVia(std::string(request->getVia()) + ";received=" + _localIp);
	_outbox.emplace_back(request->getSource(), std::move(response));
	queueLog("[SIP] SDP refused (" + std::string(why) + "), 488 to " +
		std::string(request->getFromNumber()) + " for " + std::string(request->getHeader()), true);
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
			// Defense in depth (Issue #107): the .cfg served for this device
			// interpolates the extension into `key = value\r\n` lines, so a CR/LF in
			// it would inject config lines nobody wrote. Every path that adopts an
			// extension today goes through onRegister()'s isValidAor() gate, whose
			// charset excludes CR/LF -- this re-checks that invariant at the point of
			// use so provisioning does not silently depend on a gate three call layers
			// away. Fails closed: no info -> the endpoint 404s.
			if (!isValidAor(d.extension))
			{
				queueLog("Provisioning refused for " + mac +
					": adopted extension fails the AOR charset", true);
				return std::nullopt;
			}
			const bool authRequired = (d.state == Registrar::DeviceState::Secured) ||
				(_registrar.getMode() == Registrar::Mode::Secure);
			return ProvisioningInfo{d.extension, authRequired};
		}
	}
	return std::nullopt;
}

void RequestsHandler::setDnd(const std::string& extension, bool on)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_cfg.setDndLocked(extension, on);
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

std::vector<std::string> RequestsHandler::getDndExtensions()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.dnd;
}

// ── Call forwarding (CFU/CFB/CFNA) ───────────────────────────────────────────

void RequestsHandler::setForward(const std::string& extension, const std::string& trigger, const std::string& target)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_cfg.setForwardLocked(extension, trigger, target);
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

void RequestsHandler::setRingGroup(const std::string& groupExt, const std::string& members, const std::string& mode)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_cfg.setRingGroup(groupExt, members, mode);
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

// Mirrors one of _cfg's five tables into the dashboard snapshot (Issue #77);
// see PbxFeatureConfig's class comment and RequestsHandler.hpp's _cfg member
// comment for why this indirection exists. Caller holds _mutex (this is
// invoked synchronously from inside _cfg's Locked mutation cores).
void RequestsHandler::refreshPbxConfigSnapshot(PbxFeatureConfig::Table t)
{
	std::lock_guard<std::mutex> snapLock(_snapshotMutex);
	switch (t)
	{
	case PbxFeatureConfig::Table::Dnd:
		_snapshot.dnd = _cfg.dndSnapshot();
		break;
	case PbxFeatureConfig::Table::Forwards:
		_snapshot.forwards = _cfg.forwardsSnapshot();
		break;
	case PbxFeatureConfig::Table::RingGroups:
		_snapshot.ringGroups = _cfg.ringGroupsSnapshot();
		break;
	case PbxFeatureConfig::Table::PageZones:
		_snapshot.pageZones = _cfg.pageZonesSnapshot();
		break;
	case PbxFeatureConfig::Table::DialRules:
		_snapshot.dialRules = _cfg.dialRulesSnapshot();
		break;
	}
}

// ── Action dispatch shared by the built-in codes and the dial plan (#69) ─────

void RequestsHandler::routePageZone(const std::shared_ptr<SipMessage>& data,
	const std::shared_ptr<SipClient>& caller,
	const pbx::PageZone& zone)
{
	// A zone page is a scoped 999: fork an intercom (auto-answer) INVITE to every
	// registered member of the zone. Lifted verbatim out of onInvite()'s 98x
	// branch so the dial plan reaches the same code. Unlike routeRingGroup this
	// needs no zone-extension parameter: startBroadcastFork stamps the intercom
	// Contact as 999 for every page, built-in or dial-rule-aliased alike, so the
	// zone's own extension never appears on the wire.
	std::vector<std::shared_ptr<SipClient>> targets;
	for (const auto& m : zone.members)
	{
		if (m == caller->getNumber()) continue;
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
		responseObj->setContact(buildContact(caller->getNumber()));
		_outbox.emplace_back(data->getSource(), std::move(responseObj));
		return;
	}

	startBroadcastFork(data, caller, std::move(targets), /*intercom=*/true);
}

void RequestsHandler::routeRingGroup(const std::shared_ptr<SipMessage>& data,
	const std::shared_ptr<SipClient>& caller,
	const std::string& groupExt, const pbx::RingGroup& group)
{
	// Ring-all reuses the broadcast fork (without the intercom auto-answer headers,
	// so members ring normally); hunt rings members one at a time, driven from
	// tick(). Lifted verbatim out of onInvite()'s ring-group branch. `groupExt` is
	// the REAL group extension — under a dial rule it differs from the dialed
	// number, and Session::setGroupExt feeds the hunt-exhausted CDR and the
	// no-answer Contact, both of which want the group's identity, not the alias.

	// Collect the registered members (skip the caller and any offline member).
	std::vector<std::shared_ptr<SipClient>> members;
	std::vector<std::string> huntOrder;
	for (const auto& m : group.members)
	{
		if (m == caller->getNumber()) continue;
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
		responseObj->setContact(buildContact(caller->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	if (group.mode == pbx::GroupMode::RingAll)
	{
		startBroadcastFork(data, caller, std::move(members), /*intercom=*/false);
		return;
	}

	// Hunt (sequential): build a broadcast-style session but ring one at a time.
	auto newSession = allocateSession(std::string(data->getCallID()), caller);
	if (!newSession)
	{
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(*data);
		if (!responseObj) return;   // pool exhausted: drop, peer retransmits (#101A)
		responseObj->setHeader("SIP/2.0 503 Service Unavailable");
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller->getNumber()));
		_outbox.emplace_back(data->getSource(), std::move(responseObj));
		return;
	}
	newSession->setBroadcast(true);
	newSession->setHunt(true);
	newSession->setGroupExt(groupExt);
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
	ringing->setContact(buildContact(groupExt));
	_outbox.emplace_back(data->getSource(), std::move(ringing));

	huntRingNext(newSession);   // ring the first member, arm its timeout
}

// ── Dial plan (Issue #69) ────────────────────────────────────────────────────

bool RequestsHandler::routeDialPlan(const std::shared_ptr<SipMessage>& data,
	const std::shared_ptr<SipClient>& caller,
	const std::string& destNumber)
{
	// Fast path: the table is empty on a default install, so the overwhelmingly
	// common case costs one size check and nothing else.
	if (_cfg.dialPlan().empty())
	{
		return false;
	}

	const pbx::DialRule* rule = _cfg.dialPlan().match(destNumber);
	if (!rule)
	{
		return false;   // fallthrough — routing continues exactly as it did pre-#69
	}

	queueLog("Dial plan: " + destNumber + " matched \"" + rule->pattern + "\" -> " +
		pbx::dialActionName(rule->action) + " " + rule->target);

	switch (rule->action)
	{
	case pbx::DialActionType::RingGroup:
		if (const pbx::RingGroup* group = _cfg.findRingGroup(rule->target))
		{
			routeRingGroup(data, caller, rule->target, *group);
			return true;
		}
		break;

	case pbx::DialActionType::PageZone:
		if (const pbx::PageZone* zone = _cfg.findPageZone(rule->target))
		{
			routePageZone(data, caller, *zone);
			return true;
		}
		break;

	case pbx::DialActionType::ParkOrbit:
	{
		// Park/retrieve is stateless config-wise — the orbit always exists, so the
		// only way this fails is a target outside this build's orbit range, which
		// setDialRule() already refuses. Re-checked here anyway: POCKETDIAL_PARK_SLOTS
		// can shrink under a rebuilt firmware that reloads an older NVS blob.
		const int orbitIdx = _park.orbitIndex(rule->target);
		if (orbitIdx >= 0)
		{
			_park.onInvite(data, caller, orbitIdx);
			return true;
		}
		break;
	}
	}

	// The rule matched but its target no longer resolves — a group or zone deleted
	// after the rule was written, or an orbit outside this build's range. Answer
	// 404 rather than falling through: falling through would silently ring a real
	// extension that happens to share the dialed digits, which is a mis-routed
	// call, whereas a stale rule failing loudly is diagnosable from one 404.
	queueLog("Dial plan: rule \"" + rule->pattern + "\" -> " +
		pbx::dialActionName(rule->action) + " " + rule->target +
		" has no such target; answering 404", true);
	auto responseObj = getMessageFromPool(*data);
	if (!responseObj) return true;   // pool exhausted: drop, peer retransmits (#101A)
	responseObj->setHeader(SipMessageTypes::NOT_FOUND);
	responseObj->clearBody();
	responseObj->setContact(buildContact(caller->getNumber()));
	_outbox.emplace_back(data->getSource(), std::move(responseObj));
	return true;
}

void RequestsHandler::setDialRule(const std::string& pattern, const std::string& action,
	const std::string& target)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_cfg.setDialRule(pattern, action, target);
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

std::vector<std::tuple<std::string, std::string, std::string>> RequestsHandler::getDialRules()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.dialRules;
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

int RequestsHandler::getConferenceLegs()
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _conference ? _conference->legCount() : 0;
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
		_dtmf.sweepStale();

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
				auto ping = buildOptionsPing(client);
				// Stamp the ping time only once one actually exists. Stamping first
				// would re-arm the interval gate for a ping that was never sent, so
				// a refusal would cost a whole keepalive interval instead of being
				// retried on the next tick — worst behavior exactly when the box is
				// already overloaded (#101A).
				if (ping)
				{
					client->setLastPingTime(now);
					_outbox.emplace_back(client->getAddress(), std::move(ping));
				}
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

		// CDR view: newest-first copy of the ring into the snapshot.
		nextSnapshot.cdr = _cdr.snapshot();

		// DND view: extensions currently in DND.
		nextSnapshot.dnd = _cfg.dndSnapshot();

		// Call-forward view.
		nextSnapshot.forwards = _cfg.forwardsSnapshot();

		// Ring/hunt-group view.
		nextSnapshot.ringGroups = _cfg.ringGroupsSnapshot();

		// Dial-plan rules (Issue #69), in table order. Rebuilt from _cfg's dial
		// plan here alongside ringGroups rather than mirrored out of band like
		// pageZones, so the snapshot swap below can never blank or re-order them.
		nextSnapshot.dialRules = _cfg.dialRulesSnapshot();

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

// ── NVS persistence ────────────────────────────────────────────────────────────
//
// The PBX-config tables (forwards / ring groups / page zones / dial plan:
// loadPbxConfig, persistForwards, persistRingGroups, persistPageZones,
// persistDialPlan) now live on PbxFeatureConfig (see _cfg), and the CDR ring
// (load/persist, its own NVS namespace and record shape) now lives on CdrRing
// (see _cdr) — both still no-ops on host.

// ── Registrar mode + device registry persistence (STAGE 2) ───────────────────

void RequestsHandler::loadAdminHttpTtl()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	nvs_handle_t h;
	if (nvs_open(pbxpersist::kNvsNamespace, NVS_READWRITE, &h) != ESP_OK)
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

// ── Task 2B/2C: admin extension identity + DTMF digit-collection state machine
// + CLASS service codes, now on DtmfFeatureCodes (see _dtmf) ──────────────────

std::string RequestsHandler::getAdminExt() const
{
	return _dtmf.adminExt();
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

	// Virtual-extension legs (777 echo, 888 conference) have no real peer to relay the
	// offer to — their "dest" is a stand-in SipClient carrying the CALLER's own address,
	// so relaying would send the phone its own re-INVITE back. Decline instead, so the
	// holding phone keeps the call on the original SDP.
	const std::string destNum = dest ? dest->getNumber() : "";
	if (destNum == "777" || destNum == ConferenceRoom::EXT || !src || !dest)
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

	// Same virtual-leg guard as onReinvite() above: 777/888 have no peer leg.
	if (destNum == "777" || destNum == ConferenceRoom::EXT || !src || !dest)
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
// findPageZone/isPageZoneDialog now live on _cfg (PbxFeatureConfig).

// ── Directed / group call pickup (Issue #68) ──────────────────────────────────
// See PbxConfig.hpp's isGroupPickupCode/directedPickupTarget doc comment: pickup
// groups are ring-group membership, reused as-is.

bool RequestsHandler::isSessionRingingExt(const std::shared_ptr<Session>& session, const std::string& ext) const
{
	if (!session || session->getState() != Session::State::Invited)
	{
		return false;
	}
	if (session->isBroadcast())
	{
		// Ring-all / hunt: the currently-ringing member(s) live in
		// pendingTargets (hunt keeps exactly one entry there at a time).
		for (const auto& t : session->getPendingTargets())
		{
			if (t && t->getNumber() == ext) return true;
		}
		return false;
	}
	// Direct (proxied 1:1) call: the callee extension is the stored original
	// INVITE's own To user-part — always retained now (see onInvite's direct-
	// call branch), not only when a conditional forward is configured.
	auto inv = session->getInviteMessage();
	return inv && inv->getToNumber() == ext;
}

std::shared_ptr<Session> RequestsHandler::findRingingSessionAmong(const std::vector<std::string>& candidates,
	std::string& outCallId, std::string& outExt) const
{
	std::shared_ptr<Session> best;
	std::chrono::steady_clock::time_point bestStart;
	for (const auto& [callId, session] : _sessions)
	{
		if (!session) continue;
		for (const auto& ext : candidates)
		{
			if (!isSessionRingingExt(session, ext)) continue;
			if (!best || session->getStartTime() < bestStart)
			{
				best = session;
				bestStart = session->getStartTime();
				outCallId = callId;
				outExt = ext;
			}
			break;   // this session matched; no need to try its other candidates
		}
	}
	return best;
}

void RequestsHandler::onPickup(const std::shared_ptr<SipMessage>& data, const std::shared_ptr<SipClient>& picker,
	const std::vector<std::string>& candidates)
{
	auto reject486 = [&]()
	{
		auto resp = getMessageFromPool(*data);
		if (!resp) return;   // pool exhausted: drop, peer retransmits (#101A)
		resp->setHeader(SipMessageTypes::BUSY);
		resp->clearBody();
		resp->setVia(std::string(data->getVia()) + ";received=" + _localIp);
		resp->setContact(buildContact(picker->getNumber()));
		_outbox.emplace_back(data->getSource(), std::move(resp));
	};

	std::string ringingCallId, ringingExt;
	auto ringing = candidates.empty() ? nullptr
		: findRingingSessionAmong(candidates, ringingCallId, ringingExt);
	if (!ringing)
	{
		// Nothing eligible is ringing right now (wrong/no pickup group, no
		// ringing call for a directed target, or the race already lost — see
		// onOk's late-answer guard). Acceptance criterion: 486, never a hang.
		reject486();
		return;
	}

	auto originalCaller = ringing->getSrc();
	auto invite = ringing->getInviteMessage();
	if (!originalCaller || !invite || !invite->hasSdp() || !data->hasSdp())
	{
		// Can't build a valid P2P O/A without both SDPs — decline cleanly
		// rather than half-connect the call.
		reject486();
		return;
	}

	// Draw the picker's session and BOTH 200 OKs before mutating anything
	// (#101A / #71 discipline): a refusal here must leave the original call
	// still ringing, not half-torn-down.
	auto pickerSession = allocateSession(std::string(data->getCallID()), picker);
	if (!pickerSession)
	{
		auto resp = getMessageFromPool(*data);
		if (resp)
		{
			resp->setHeader("SIP/2.0 503 Service Unavailable");
			resp->clearBody();
			resp->setContact(buildContact(picker->getNumber()));
			_outbox.emplace_back(data->getSource(), std::move(resp));
		}
		return;
	}
	auto okToCaller = getMessageFromPool(*invite);
	if (!okToCaller) return;   // pool exhausted: drop, original call keeps ringing (#101A)
	auto okToPicker = getMessageFromPool(*data);
	if (!okToPicker) return;   // pool exhausted: drop, original call keeps ringing (#101A)

	// ── Cancel every other still-ringing fork of the picked-up call ─────────
	if (ringing->isBroadcast())
	{
		for (const auto& t : ringing->getPendingTargets())
		{
			auto cancel = buildCancel(invite, t);
			if (cancel) _outbox.emplace_back(t->getAddress(), std::move(cancel));
		}
		ringing->setPendingTargets({});
	}
	else if (auto target = findClient(ringingExt); target.has_value())
	{
		auto cancel = buildCancel(invite, target.value());
		if (cancel) _outbox.emplace_back(target.value()->getAddress(), std::move(cancel));
	}
	ringing->clearRingTimer();

	// ── Complete the caller's original (still-open) INVITE transaction with
	// the picker's SDP as the answer ────────────────────────────────────────
	const std::string callerToTag = IDGen::GenerateID(9);
	std::string callerTo(invite->getTo());
	callerTo += ";tag=" + callerToTag;

	okToCaller->setHeader(SipMessageTypes::OK);
	okToCaller->setVia(std::string(invite->getVia()) + ";received=" + _localIp);
	okToCaller->setTo(callerTo);
	okToCaller->setContact(buildContact(picker->getNumber()));
	okToCaller->setBody(std::string(data->getBody()));
	(void)okToCaller->filterAudioCodecs(/*allowWideband=*/true);
	okToCaller->syncContentLength();

	// ── Answer the picker's own INVITE with the caller's original SDP ───────
	const std::string pickerToTag = IDGen::GenerateID(9);
	std::string toForPicker(data->getTo());
	toForPicker += ";tag=" + pickerToTag;

	okToPicker->setHeader(SipMessageTypes::OK);
	okToPicker->setVia(std::string(data->getVia()) + ";received=" + _localIp);
	okToPicker->setTo(toForPicker);
	okToPicker->setContact(buildContact(ringingExt));
	okToPicker->setBody(std::string(invite->getBody()));
	(void)okToPicker->filterAudioCodecs(/*allowWideband=*/true);
	okToPicker->syncContentLength();

	// ── Bridge the two independent dialogs. Both legs are REAL registered
	// clients (unlike ParkOrbit's orbit stand-in), so neither session needs a
	// virtual peer — but the Call-IDs still differ, so peerCallID + the
	// dialog headers captured here are what let onBye's peerCallID branch
	// translate a hangup on one leg into a correctly-addressed BYE on the
	// other's own dialog. ──────────────────────────────────────────────────
	ringing->setDest(picker);
	ringing->setLocalTag(callerToTag);
	ringing->setState(Session::State::Connected);
	ringing->setPeerCallID(std::string(data->getCallID()));
	ringing->setDialogHeaders(std::string(invite->getFrom()), callerTo);

	pickerSession->setDest(originalCaller);
	pickerSession->setInviteMessage(data);
	pickerSession->setLocalTag(pickerToTag);
	pickerSession->setPeerCallID(ringingCallId);
	pickerSession->setDialogHeaders(std::string(data->getFrom()), toForPicker);
	pickerSession->setState(Session::State::Connected);
	_sessions.emplace(data->getCallID(), pickerSession);

	_outbox.emplace_back(originalCaller->getAddress(), std::move(okToCaller));
	_outbox.emplace_back(data->getSource(), std::move(okToPicker));

	queueLog("Pickup: " + picker->getNumber() + " picked up " + ringingExt +
		"'s ringing call from " + originalCaller->getNumber());
}

void RequestsHandler::setPageZone(const std::string& zoneExt, const std::string& members)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_cfg.setPageZone(zoneExt, members);
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
	// No pool lock here, unlike SipMessagePool's acquirePooledMessage(): _virtualPeerPool is a
	// per-instance member and every caller — the internal sites and ParkOrbit via
	// PbxEnv::allocVirtualPeer — already runs under _mutex. The counter is still
	// atomic because its decrement happens in the deleter, which runs wherever
	// the owning Session finally releases it.
	static std::atomic<std::size_t> vpeerWarnCount{0};
	if (s_vpeerHeapFallbacksInFlight.load(std::memory_order_relaxed) >= POCKETDIAL_VPEER_HEAP_FALLBACK_MAX)
	{
		sipmsgpool::logPoolExhausted("Virtual-peer", sipmsgpool::PoolPressure::Refused, vpeerWarnCount);
		return nullptr;
	}
	sipmsgpool::logPoolExhausted("Virtual-peer", sipmsgpool::PoolPressure::Fallback, vpeerWarnCount);

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
