// RequestsHandler.cpp: Issues #24 and #28 resolved.
#include "RequestsHandler.hpp"
#include <atomic>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include "SipMessageTypes.h"
#include "SipSdpMessage.hpp"
#include "IDGen.hpp"
#include "IPHelper.hpp"
#include "PoolConfig.hpp"
#include "CallDetailRecord.hpp"
#include "PbxConfig.hpp"
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
static std::string parkTagOf(std::string_view header);
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
	// Task 2B: load the admin extension from NVS (defaults to "101" if absent).
	loadAdminExt();
	// STAGE 2: load the registrar mode (defaults to the POCKETDIAL_OPEN_REGISTRAR
	// seed) and the adopted-device registry from NVS.
	loadRegistrarMode();
	loadDevices();
	// Prewarm the per-extension HA1 cache off the REGISTER hot path so the first
	// Secure REGISTER does not pay a blocking NVS read while holding _mutex.
	SipSecretStore::warmCache();
	// Seed the dashboard snapshot with the loaded devices so the TUI sees adopted
	// devices immediately on boot (online flags start false until each re-REGISTERs).
	refreshDeviceSnapshot();
}

std::shared_ptr<SipMessage> RequestsHandler::getMessageFromPool(std::string message, sockaddr_in src)
{
	for (auto& msg : _messagePool)
	{
		if (msg.use_count() == 1)
		{
			msg->reset(std::move(message), src);
			return msg;
		}
	}
	std::cerr << "[WARNING] SIP Message pool exhausted! Fallback to heap allocation.\n";
	return std::make_shared<SipSdpMessage>(std::move(message), src);
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

		auto client = findClientByAddress(request->getSource());
		if (client.has_value())
		{
			client.value()->markActive();
		}

		maybeSweep();

		// RFC 3261 §17 transaction layer: advance the state machine for any tracked
		// InviteClient transaction before the TU handler runs. A 1xx moves Calling →
		// Proceeding (stops retransmitting); a 2xx/3xx-6xx → Accepted/Completed.
		matchAndAdvanceTx(request);

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
				auto infoOk = getMessageFromPool(request->toString(), request->getSource());
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

		// BLF change detection: one pass after every handled packet covers
		// registration appear/disappear, session create/transition/teardown.
		// NOTIFYs land in _outbox and ride out with this pass (after unlock).
		refreshSubscriptions();

		// RFC 3261 §17: register any new outgoing INVITE in _outbox for retransmit.
		// Scan after BLF NOTIFYs are added so the full outbox is covered; classifyTxType
		// filters to INVITE requests only so NOTIFYs and responses are ignored.
		for (const auto& [addr, msg] : _outbox)
		{
			auto txType = classifyTxType(msg);
			if (txType != SipTransaction::Type::None)
				registerTx(txType, addr, msg);
		}

		localOutbox = std::move(_outbox);
		_outbox.clear();

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
		auto response = getMessageFromPool(data->toString(), data->getSource());
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
	const RegistrarMode mode = _registrarMode.load(std::memory_order_relaxed);
	if (mode != RegistrarMode::Open)
	{
		std::string rejectReason;
		AuthDecision decision = (mode == RegistrarMode::Secure)
			? admitSecure(data, extStr, rejectReason)
			: admitLearn(data, extStr, rejectReason);

		if (decision == AuthDecision::Challenge)
		{
			// admitSecure already enqueued the 401 + WWW-Authenticate.
			return;
		}
		if (decision == AuthDecision::Reject)
		{
			sendForbidden(data, rejectReason.empty() ? "Forbidden" : rejectReason);
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
		if (deviceMac.has_value()) markDeviceOnline(*deviceMac, false);
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
				sendRegisterBeep(newClient);
			}
			if (deviceMac.has_value()) markDeviceOnline(*deviceMac, true);
		}
		else
		{
			// Server full: reply with 503 Service Unavailable
			auto response = getMessageFromPool(data->toString(), data->getSource());
			response->setHeader("SIP/2.0 503 Service Unavailable");
			response->clearBody();
			std::string activeIp = _localIp;
			response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
			_outbox.emplace_back(data->getSource(), std::move(response));
			return;
		}
	}

	auto response = getMessageFromPool(data->toString(), data->getSource());
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
	auto response = getMessageFromPool(data->toString(), data->getSource());
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
		sendForbidden(data, "Forbidden");
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

	if (parkOrbitIndex(destNumber) >= 0)
	{
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		refreshParkSnapshot();
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
				auto cancelMsg = getMessageFromPool(data->toString(), data->getSource());
				char ipBuf[INET_ADDRSTRLEN]{};
				inet_ntop(AF_INET, &target->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
				std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(target->getAddress().sin_port));

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
		auto response = getMessageFromPool(data->toString(), data->getSource());
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
		auto response = getMessageFromPool(data->toString(), data->getSource());
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
		auto ringing = getMessageFromPool(data->toString(), data->getSource());
		ringing->setHeader("SIP/2.0 180 Ringing");
		ringing->clearBody();
		std::string activeIp = _localIp;
		ringing->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		std::string toTag = IDGen::GenerateID(9);
		ringing->setTo(std::string(data->getTo()) + ";tag=" + toTag);
		ringing->setContact(buildContact("777"));
		_outbox.emplace_back(data->getSource(), std::move(ringing));

		auto okResponse = getMessageFromPool(data->toString(), data->getSource());
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
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
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
				std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
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
		int orbitIdx = parkOrbitIndex(destNumber);
		if (orbitIdx >= 0)
		{
			onParkInvite(data, caller.value(), orbitIdx);
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
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
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
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
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
		auto ringing = getMessageFromPool(data->toString(), data->getSource());
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
		auto response = getMessageFromPool(data->toString(), data->getSource());
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
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
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
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
		responseObj->setHeader(SipMessageTypes::BAD_REQUEST);
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	auto newSession = allocateSession(std::string(data->getCallID()), caller.value());
	if (!newSession)
	{
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
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

	auto response = getMessageFromPool(data->toString(), data->getSource());
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
		auto busy = getMessageFromPool(data->toString(), data->getSource());
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
		auto bad = getMessageFromPool(data->toString(), data->getSource());
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
		auto err = getMessageFromPool(data->toString(), data->getSource());
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
		auto busy = getMessageFromPool(data->toString(), data->getSource());
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
	auto ok = getMessageFromPool(data->toString(), data->getSource());
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
		sendForbidden(data, "Forbidden");
		return;
	}

	if (destNumber == "777")
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
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
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (destNumber == "999" || isPageZoneDialog(destNumber))
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));

		if (session.has_value())
		{
			auto answeringClient = session.value()->getDest();
			if (answeringClient)
			{
				auto byeFork = getMessageFromPool(data->toString(), data->getSource());
				std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

				char ipBuf[INET_ADDRSTRLEN]{};
				inet_ntop(AF_INET, &answeringClient->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
				std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(answeringClient->getAddress().sin_port));

				byeFork->setHeader("BYE sip:" + answeringClient->getNumber() + "@" + targetIpPort + " SIP/2.0");

				std::string originalTo(data->getTo());
				std::string newTo = "To: <sip:" + answeringClient->getNumber() + "@" + serverIpPort + ">";
				size_t tagPos = originalTo.find(";tag=");
				if (tagPos != std::string::npos)
				{
					newTo += originalTo.substr(tagPos);
				}
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

	// Register-beep dialog (server-originated UAC). These have NO Session — they are
	// tracked in _beepDialogs by Call-ID. A 200 OK to our INVITE means the phone
	// auto-answered (the tone played): ACK it, then BYE to end the call. A 200 OK to
	// our BYE just frees the slot. Recognised before the normal session lookup.
	if (BeepDialog* bd = findBeepByCallID(data->getCallID()))
	{
		std::string cseq(data->getCSeq());
		if (bd->state == BeepState::AwaitingInviteOk &&
			cseq.find(SipMessageTypes::INVITE) != std::string::npos)
		{
			auto ack = buildBeepAck(data);
			if (ack) _outbox.emplace_back(bd->addr, std::move(ack));
			auto bye = buildBeepBye(data);
			if (bye) _outbox.emplace_back(bd->addr, std::move(bye));
			bd->state    = BeepState::AwaitingByeOk;
			// Re-arm the deadline so a phone that never 200s our BYE still frees its
			// slot from tick() rather than lingering until the original INVITE timeout.
			bd->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			queueLog("Register beep: answered by " + bd->ext + ", ACK+BYE sent");
		}
		else if (cseq.find(SipMessageTypes::BYE) != std::string::npos)
		{
			*bd = BeepDialog{};   // BYE acknowledged: dialog fully torn down, free slot
		}
		return;
	}

	// Park dialogs (server-originated re-INVITE ACKs + ring-back answers).
	if (handleParkOk(data))
		return;
	// Attended-transfer re-INVITE ACKs.
	if (handleTransferOk(data))
		return;

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

					auto response = getMessageFromPool(data->toString(), data->getSource());
					response->setContact(buildContact(answeringClient->getNumber()));

					if (inviteMsg)
					{
						std::string originalTo(inviteMsg->getTo());
						std::string bTo(data->getTo());
						size_t tagPos = bTo.find(";tag=");
						if (tagPos != std::string::npos)
						{
							originalTo += bTo.substr(tagPos);
						}
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
								auto cancelMsg = getMessageFromPool(inviteMsg->toString(), inviteMsg->getSource());
								char ipBuf[INET_ADDRSTRLEN]{};
								inet_ntop(AF_INET, &target->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
								std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(target->getAddress().sin_port));

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
				std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
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
			auto response = getMessageFromPool(data->toString(), data->getSource());
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
			auto ackFork = getMessageFromPool(data->toString(), data->getSource());
			std::string activeIp = _localIp;
			std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

			char ipBuf[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &answeringClient->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
			std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(answeringClient->getAddress().sin_port));

			ackFork->setHeader("ACK sip:" + answeringClient->getNumber() + "@" + targetIpPort + " SIP/2.0");

			std::string originalTo(data->getTo());
			std::string newTo = "To: <sip:" + answeringClient->getNumber() + "@" + serverIpPort + ">";
			size_t tagPos = originalTo.find(";tag=");
			if (tagPos != std::string::npos)
			{
				newTo += originalTo.substr(tagPos);
			}
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
		auto response = getMessageFromPool(data->toString(), data->getSource());
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
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::BAD_REQUEST);
		response->clearBody();
		std::string activeIp = _localIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	// 202 Accepted to the transferor (RFC 3515 §2.4.4).
	{
		auto accepted = getMessageFromPool(data->toString(), data->getSource());
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
	auto response = getMessageFromPool(data->toString(), data->getSource());
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
	auto inviteFork = getMessageFromPool(invite->toString(), invite->getSource());
	inviteFork->setContact(buildContact(caller->getNumber()));

	std::string activeIp = _localIp;
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &target->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(target->getAddress().sin_port));
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
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(invite->toString(), invite->getSource());
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

	auto ringing = getMessageFromPool(invite->toString(), invite->getSource());
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
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(invite->toString(), invite->getSource());
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
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &transferor->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(transferor->getAddress().sin_port));
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);

	std::string body = sipfrag + "\r\n";
	std::string subState = terminated ? "terminated;reason=noresource" : "active;expires=60";

	std::ostringstream ss;
	ss << "NOTIFY sip:" << transferor->getNumber() << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From: " << refer->getTo() << "\r\n"
	   << "To: " << refer->getFrom() << "\r\n"
	   << "Call-ID: " << refer->getCallID() << "\r\n"
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

	auto cancelMsg = getMessageFromPool(invite->toString(), invite->getSource());

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &target->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(target->getAddress().sin_port));

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
	freeTxsForCallId(callID);
	// Free any park orbit slot holding this call's parked leg.
	freeParkSlot(callID);

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
		auto notFound = getMessageFromPool(message->toString(), message->getSource());
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

void RequestsHandler::setDnd(const std::string& extension, bool on)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
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
		// reflects the change without waiting for the next tick().
		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot.dnd.clear();
			_snapshot.dnd.reserve(_dnd.size());
			for (const auto& [ext, enabled] : _dnd)
			{
				if (enabled) _snapshot.dnd.push_back(ext);
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

void RequestsHandler::setForward(const std::string& extension, const std::string& trigger, const std::string& target)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		// Reject the virtual extensions outright; they are not real endpoints.
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

		// Refresh the dashboard snapshot immediately (mirror setDnd).
		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot.forwards.clear();
			_snapshot.forwards.reserve(_forwards.size());
			for (const auto& [ext, cfg] : _forwards)
			{
				_snapshot.forwards.emplace_back(ext, cfg.always, cfg.busy, cfg.noAnswer);
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
		_registrarMode.store(mode, std::memory_order_relaxed);
		persistRegistrarMode();
		const char* name = (mode == RegistrarMode::Open)   ? "open"
		                 : (mode == RegistrarMode::Learn)  ? "learn"
		                                                   : "secure";
		queueLog(std::string("Registrar mode set to ") + name);
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
	// hot path. The atomic guarantees a torn-free load.
	return _registrarMode.load(std::memory_order_relaxed);
}

// ── Device registry (STAGE 2: Learn-mode adoption) ────────────────────────────

void RequestsHandler::refreshDeviceSnapshot()
{
	// Caller holds _mutex. Mirror _devices into the dashboard snapshot, preserving
	// the previously-published online flags (online is tracked in the snapshot, not
	// in _devices, since it is volatile registration state — not persisted).
	std::lock_guard<std::mutex> snapLock(_snapshotMutex);

	// Index the existing online flags by MAC so a rebuild doesn't lose them.
	std::unordered_map<std::string, bool> wasOnline;
	wasOnline.reserve(_snapshot.devices.size());
	for (const auto& d : _snapshot.devices)
	{
		wasOnline[d.mac] = d.online;
	}

	_snapshot.devices.clear();
	_snapshot.devices.reserve(_devices.size());
	for (const auto& [mac, rec] : _devices)
	{
		AdoptedDevice d;
		d.mac = mac;
		d.extension = rec.extension;
		d.state = rec.state;
		auto it = wasOnline.find(mac);
		d.online = (it != wasOnline.end()) ? it->second : false;
		_snapshot.devices.push_back(std::move(d));
	}
}

void RequestsHandler::markDeviceOnline(const std::string& mac, bool online)
{
	// Caller holds _mutex. Online state lives only in the snapshot (volatile, not
	// persisted). No-op if the device isn't adopted (e.g. Open mode never records).
	std::lock_guard<std::mutex> snapLock(_snapshotMutex);
	for (auto& d : _snapshot.devices)
	{
		if (d.mac == mac)
		{
			d.online = online;
			return;
		}
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

		// Accept either a 12-hex MAC (direct key) or an extension (find the device
		// currently adopted under it).
		auto it = _devices.find(macOrExt);
		if (it == _devices.end())
		{
			for (auto cand = _devices.begin(); cand != _devices.end(); ++cand)
			{
				if (cand->second.extension == macOrExt) { it = cand; break; }
			}
		}

		if (it != _devices.end())
		{
			// Footgun guard: promoting a device to Secured makes admitSecure() demand a
			// digest for it. If the extension has NO stored secret, that would lock the
			// phone out on its next REGISTER ("Extension Not Provisioned"). Refuse, and
			// tell the operator to assign a secret first.
			if (!SipSecretStore::hasSecret(it->second.extension))
			{
				queueLog("secureDevice: ext " + it->second.extension +
					" has no SIP secret — assign one before securing", true);
			}
			else
			{
				if (it->second.state != DeviceState::Secured)
				{
					it->second.state = DeviceState::Secured;
					persistDevices();
					changed = true;
				}
				queueLog("Device " + it->first + " (ext " + it->second.extension + ") secured");
			}
		}
		else
		{
			queueLog("secureDevice: no adopted device for '" + macOrExt + "'", true);
		}

		if (changed) refreshDeviceSnapshot();
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

		auto it = _devices.find(macOrExt);
		if (it == _devices.end())
		{
			for (auto cand = _devices.begin(); cand != _devices.end(); ++cand)
			{
				if (cand->second.extension == macOrExt) { it = cand; break; }
			}
		}

		if (it != _devices.end())
		{
			queueLog("Device " + it->first + " (ext " + it->second.extension + ") forgotten");
			_devices.erase(it);
			persistDevices();
			removed = true;
			refreshDeviceSnapshot();
		}
		else
		{
			queueLog("forgetDevice: no adopted device for '" + macOrExt + "'", true);
		}

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

// ── REGISTER admission helpers (STAGE 2) ──────────────────────────────────────
// All run under _mutex (called from onRegister, which holds it via handle()).

void RequestsHandler::sendChallenge(const std::shared_ptr<SipMessage>& data, bool stale)
{
	auto response = getMessageFromPool(data->toString(), data->getSource());
	response->setHeader("SIP/2.0 401 Unauthorized");
	response->clearBody();
	std::string activeIp = _localIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
	// Fresh stateless nonce per challenge; realm MUST match SipSecretStore::kRealm.
	response->addHeader("WWW-Authenticate",
		SipDigest::buildWwwAuthenticate(SipSecretStore::kRealm,
			SipDigest::generateNonce(), stale));
	response->syncContentLength();
	_outbox.emplace_back(data->getSource(), std::move(response));
}

void RequestsHandler::sendForbidden(const std::shared_ptr<SipMessage>& data, const std::string& reason)
{
	auto response = getMessageFromPool(data->toString(), data->getSource());
	response->setHeader("SIP/2.0 403 " + reason);
	response->clearBody();
	std::string activeIp = _localIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->syncContentLength();
	_outbox.emplace_back(data->getSource(), std::move(response));
}

RequestsHandler::AuthDecision RequestsHandler::admitSecure(
	const std::shared_ptr<SipMessage>& data, const std::string& ext, std::string& outRejectReason)
{
	// Secure mode: a provisioned extension MUST present a valid digest. An ext with
	// NO stored secret is unprovisioned — reject with a clear reason (the SAFE
	// default; "allow first-time" would defeat the point of secure mode).
	auto ha1 = SipSecretStore::getHa1(ext);
	if (!ha1.has_value())
	{
		outRejectReason = "Extension Not Provisioned";
		queueLog("Secure REGISTER for unprovisioned ext " + ext + " rejected", true);
		return AuthDecision::Reject;
	}

	SipDigest::DigestAuth auth;
	std::string_view authHdr = data->getAuthorization();
	if (authHdr.empty() || !SipDigest::parseAuthorization(std::string(authHdr), auth))
	{
		// No (parseable) credentials → challenge with a fresh nonce.
		sendChallenge(data, /*stale=*/false);
		return AuthDecision::Challenge;
	}

	// Validate the nonce we issued. A forged/garbage nonce is a hard re-challenge
	// (not stale); an expired-but-ours nonce → challenge with stale=true so the
	// phone silently retries.
	bool expired = false;
	if (!SipDigest::validateNonce(auth.nonce, &expired))
	{
		sendChallenge(data, /*stale=*/expired);
		return AuthDecision::Challenge;
	}

	// Recompute + constant-time compare. Method is REGISTER.
	if (!SipDigest::verify(auth, *ha1, std::string(data->getType())))
	{
		outRejectReason = "Bad Credentials";
		queueLog("Secure REGISTER for ext " + ext + " failed digest verify", true);
		return AuthDecision::Reject;
	}

	return AuthDecision::Accept;
}

RequestsHandler::AuthDecision RequestsHandler::admitLearn(
	const std::shared_ptr<SipMessage>& data, const std::string& ext, std::string& outRejectReason)
{
	// Learn mode = TOFU + MAC-lock.
	//   UNKNOWN mac            -> accept WITHOUT verifying, record {mac, ext, Learned}.
	//   KNOWN + Secured mac    -> enforce digest (same path as secure mode).
	//   ext Secured to a DIFFERENT mac -> reject (anti-spoof lock).
	//   first-packet ARP miss  -> accept + defer the lock to the next REGISTER.
	auto macOpt = ArpLookup::pdLookupMac(data->getSource());
	if (!macOpt.has_value())
	{
		// Cache miss (or host). Accept now; the server's 200 OK + beep + OPTIONS
		// populates the ARP cache so the NEXT REGISTER resolves and locks. Do NOT
		// hard-fail — that would brick the very first registration.
		queueLog("Learn REGISTER ext " + ext + ": ARP miss, deferring MAC-lock");
		return AuthDecision::Accept;
	}
	const std::string mac = ArpLookup::toHex12(*macOpt);

	// Anti-spoof: if this extension is already Secured to a DIFFERENT mac, reject.
	for (const auto& [m, rec] : _devices)
	{
		if (rec.extension == ext && rec.state == DeviceState::Secured && m != mac)
		{
			outRejectReason = "Extension Locked To Another Device";
			queueLog("Learn REGISTER ext " + ext + " from " + mac +
				" rejected: locked to " + m, true);
			return AuthDecision::Reject;
		}
	}

	auto it = _devices.find(mac);
	if (it == _devices.end())
	{
		// First time we've seen this MAC: trust-on-first-use. Bound the table like
		// _dnd/_forwards — a flood of distinct MACs can't grow the heap unbounded.
		if (_devices.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
		{
			outRejectReason = "Device Table Full";
			queueLog("Learn REGISTER: device table full, rejecting " + mac, true);
			return AuthDecision::Reject;
		}
		DeviceRecord rec;
		rec.extension = ext;
		rec.state = DeviceState::Learned;
		_devices.emplace(mac, std::move(rec));
		persistDevices();
		refreshDeviceSnapshot();
		queueLog("Learn: adopted device " + mac + " as ext " + ext);
		return AuthDecision::Accept;
	}

	// Known MAC. Keep its extension in sync if the phone re-provisioned to a new AOR.
	if (it->second.extension != ext)
	{
		it->second.extension = ext;
		persistDevices();
		refreshDeviceSnapshot();
	}

	if (it->second.state == DeviceState::Secured)
	{
		// Promoted device: enforce digest exactly as secure mode does.
		return admitSecure(data, ext, outRejectReason);
	}

	// Known + still Learned → accept (TOFU continues until an admin secures it).
	return AuthDecision::Accept;
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
		char ipBuf[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
		std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));

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
		msg->syncContentLength();
		_outbox.emplace_back(addr, std::move(msg));
		sent = true;

		// Drain into a local vector and dispatch outside the lock (no IO under lock).
		localOutbox = std::move(_outbox);
		_outbox.clear();
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
		sweepTransactions(now);
		// RFC 4028: BYE sessions that have exceeded their session-expires timer.
		sweepSessionTimers(now);
		// BLF subscription expiry.
		sweepSubscriptions();
		// Call-park timeout: ring back the parker or BYE the parked party.
		parkSweep(now);

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
						auto resp = getMessageFromPool(invite->toString(), invite->getSource());
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
				_outbox.emplace_back(client->getAddress(), std::move(ping));
			}
		}

		// Register-beep timeouts: any beep dialog whose deadline has passed without the
		// phone answering (AwaitingInviteOk) is CANCELled and freed — no retransmit
		// storm, no leak. A dialog still awaiting the 200-to-BYE just frees its slot
		// (the BYE was already sent best-effort). Done here, under _mutex, enqueuing to
		// _outbox; the actual sendto() happens after the lock is dropped.
		for (std::size_t i = 0; i < _beepDialogs.size(); ++i)
		{
			auto& bd = _beepDialogs[i];
			if (bd.state == BeepState::Free || now < bd.deadline)
			{
				continue;
			}
			if (bd.state == BeepState::AwaitingInviteOk)
			{
				auto cancel = buildBeepCancel(i);
				if (cancel) _outbox.emplace_back(bd.addr, std::move(cancel));
				queueLog("Register beep: no answer from " + bd.ext + ", cancelled");
			}
			bd = BeepDialog{};   // free the slot
		}

		// Build snapshot under registrar mutex lock, then save it under snapshot mutex lock
		RegistrarSnapshot nextSnapshot;
		nextSnapshot.packetsProcessed = _packetsProcessed.load(std::memory_order_relaxed);
		nextSnapshot.packetsDropped = _packetsDropped.load(std::memory_order_relaxed);
		for (const auto& client : _clientPool)
		{
			if (client->getNumber().empty()) continue;
			const auto& addr = client->getAddress();
			char ipBuf[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
			std::string ipPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));
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

		// Parked calls view: {orbit, parkedExt, parker, secondsParked}.
		nextSnapshot.parkedCalls.clear();
		for (const auto& slot : _parkSlots)
		{
			if (slot.state == ParkState::Parked)
			{
				int sec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
					now - slot.parkedAt).count());
				nextSnapshot.parkedCalls.emplace_back(slot.orbit, slot.parkedExt, slot.parker, sec);
			}
		}

		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot = std::move(nextSnapshot);
		}

		// RFC 3261 §17: register any new outgoing INVITEs for retransmit (mirrors
		// the same scan in handle()). tick()-originated INVITE forks (park ring-back,
		// hunt-group next-ring, CFNA redirect) need Timer A/B coverage too. (#70)
		for (const auto& [addr, msg] : _outbox)
		{
			auto txType = classifyTxType(msg);
			if (txType != SipTransaction::Type::None)
				registerTx(txType, addr, msg);
		}

		localOutbox = std::move(_outbox);
		_outbox.clear();

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

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &client->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(client->getAddress().sin_port));

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

// ── Register beep (signaling-only intercom tone) ─────────────────────────────
//
// On a new REGISTER, the registrar acts as a UAC: it sends the phone a brief
// auto-answer INVITE (same header set the 999 all-page uses) so the handset plays
// its intercom alert tone, then immediately tears the call back down (ACK → BYE on
// the phone's 200, or CANCEL on timeout). NO RTP is sourced — the SDP offers a
// single payload at a=inactive purely so the offer is well-formed. Each dialog is a
// small fixed record in _beepDialogs, bounded by POCKETDIAL_MAX_BEEPS; if the table
// is full the beep is simply skipped (it is cosmetic). All helpers below assume the
// caller already holds _mutex (non-recursive) and only enqueue to _outbox.

RequestsHandler::BeepDialog* RequestsHandler::findBeepByCallID(std::string_view callID)
{
	for (auto& bd : _beepDialogs)
	{
		if (bd.state != BeepState::Free && bd.callID == callID)
		{
			return &bd;
		}
	}
	return nullptr;
}

void RequestsHandler::sendRegisterBeep(const std::shared_ptr<SipClient>& phone)
{
	if (!phone || phone->getNumber().empty())
	{
		return;
	}

	// Grab a free beep slot. Full table → skip (cosmetic); no leak, no blocking.
	BeepDialog* slot = nullptr;
	for (auto& bd : _beepDialogs)
	{
		if (bd.state == BeepState::Free) { slot = &bd; break; }
	}
	if (!slot)
	{
		queueLog("Register beep: table full, skipping beep for " + phone->getNumber());
		return;
	}

	std::string clientNum = phone->getNumber();
	const sockaddr_in& addr = phone->getAddress();

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));

	std::string activeIp = _localIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

	std::string callId  = IDGen::GenerateID(16) + "@" + activeIp;
	std::string branch  = "z9hG4bK" + IDGen::GenerateID(12);
	std::string fromTag = IDGen::GenerateID(9);

	// Record the dialog BEFORE sending so a (synchronous) response can never race
	// ahead of the bookkeeping.
	slot->state    = BeepState::AwaitingInviteOk;
	slot->callID   = callId;
	slot->branch   = branch;
	slot->fromTag  = fromTag;
	slot->ext      = clientNum;
	slot->addr     = addr;
	slot->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

	// Minimal, well-formed SDP. a=inactive: no RTP will flow (server sources none).
	std::string body =
		"v=0\r\n"
		"o=- 0 0 IN IP4 " + activeIp + "\r\n"
		"s=pocket-dial\r\n"
		"c=IN IP4 " + activeIp + "\r\n"
		"t=0 0\r\n"
		"m=audio 9 RTP/AVP 0\r\n"
		"a=inactive\r\n";

	std::ostringstream ss;
	ss << "INVITE sip:" << clientNum << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From: \"PocketDial\" <sip:pbx@" << srcIpPort << ">;tag=" << fromTag << "\r\n"
	   << "To: <sip:" << clientNum << "@" << activeIp << ">\r\n"
	   << "Call-ID: " << callId << "\r\n"
	   << "CSeq: 1 INVITE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Contact: <sip:pbx@" << srcIpPort << ";transport=UDP>\r\n"
	   // Auto-answer / intercom headers — identical intent to the 999 all-page fork:
	   // make a Yealink auto-answer in intercom mode and so play its alert tone.
	   << "Call-Info: <sip:any>;answer-after=0\r\n"
	   << "Alert-Info: info=alert-autoanswer\r\n"
	   << "Alert-Info: answer-after=0\r\n"
	   << "Alert-Info: intercom=true\r\n"
	   << "P-Auto-Answer: normal\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: application/sdp\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	auto invite = getMessageFromPool(ss.str(), addr);
	// Normalise the codec list and (re)derive Content-Length from the actual body —
	// a wrong Content-Length silently breaks the offer on UDP (the 777-path bug).
	invite->enforceG711();
	invite->syncContentLength();
	_outbox.emplace_back(addr, std::move(invite));
	queueLog("Register beep: INVITE -> " + clientNum);
}

std::shared_ptr<SipMessage> RequestsHandler::buildBeepAck(const std::shared_ptr<SipMessage>& ok)
{
	// ACK the phone's 200 OK to our beep INVITE (RFC 3261 §13.2.2.4 / §17.1.1.3).
	// Same Call-ID/branch/From-tag as the INVITE; To carries the phone's tag from the
	// 200. CSeq stays "1 ACK" (matches the INVITE transaction). No body.
	BeepDialog* bd = findBeepByCallID(ok->getCallID());
	if (!bd)
	{
		return nullptr;
	}

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &bd->addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(bd->addr.sin_port));

	std::string activeIp = _localIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

	std::ostringstream ss;
	ss << "ACK sip:" << bd->ext << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << bd->branch << "\r\n"
	   << "From: \"PocketDial\" <sip:pbx@" << srcIpPort << ">;tag=" << bd->fromTag << "\r\n"
	   << "To: " << ok->getTo() << "\r\n"
	   << "Call-ID: " << bd->callID << "\r\n"
	   << "CSeq: 1 ACK\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";

	return getMessageFromPool(ss.str(), bd->addr);
}

std::shared_ptr<SipMessage> RequestsHandler::buildBeepBye(const std::shared_ptr<SipMessage>& ok)
{
	// BYE to end the beep call immediately after the tone. New transaction (fresh
	// branch, CSeq 2 BYE) within the established dialog. To carries the phone's tag.
	BeepDialog* bd = findBeepByCallID(ok->getCallID());
	if (!bd)
	{
		return nullptr;
	}

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &bd->addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(bd->addr.sin_port));

	std::string activeIp = _localIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);

	std::ostringstream ss;
	ss << "BYE sip:" << bd->ext << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From: \"PocketDial\" <sip:pbx@" << srcIpPort << ">;tag=" << bd->fromTag << "\r\n"
	   << "To: " << ok->getTo() << "\r\n"
	   << "Call-ID: " << bd->callID << "\r\n"
	   << "CSeq: 2 BYE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";

	return getMessageFromPool(ss.str(), bd->addr);
}

std::shared_ptr<SipMessage> RequestsHandler::buildBeepCancel(std::size_t slot)
{
	// CANCEL the outstanding beep INVITE when the phone never auto-answered. Same
	// Call-ID/branch/From-tag as the INVITE; To has NO tag yet (no final response).
	// CSeq method becomes CANCEL but keeps the INVITE sequence number (RFC 3261 §9.1).
	if (slot >= _beepDialogs.size())
	{
		return nullptr;
	}
	BeepDialog& bd = _beepDialogs[slot];

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &bd.addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(bd.addr.sin_port));

	std::string activeIp = _localIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

	std::ostringstream ss;
	ss << "CANCEL sip:" << bd.ext << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << bd.branch << "\r\n"
	   << "From: \"PocketDial\" <sip:pbx@" << srcIpPort << ">;tag=" << bd.fromTag << "\r\n"
	   << "To: <sip:" << bd.ext << "@" << activeIp << ">\r\n"
	   << "Call-ID: " << bd.callID << "\r\n"
	   << "CSeq: 1 CANCEL\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";

	return getMessageFromPool(ss.str(), bd.addr);
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

namespace
{
	// Split a serialized blob into newline-delimited records, then each record into
	// tab-separated fields. Pure; used by the loaders.
	std::vector<std::vector<std::string>> deserializeBlob(const std::string& blob)
	{
		std::vector<std::vector<std::string>> records;
		size_t pos = 0;
		while (pos < blob.size())
		{
			size_t nl = blob.find('\n', pos);
			std::string line = blob.substr(pos, (nl == std::string::npos ? blob.size() : nl) - pos);
			pos = (nl == std::string::npos) ? blob.size() : nl + 1;
			if (line.empty()) continue;

			std::vector<std::string> fields;
			size_t fp = 0;
			while (true)
			{
				size_t tab = line.find('\t', fp);
				fields.push_back(line.substr(fp, (tab == std::string::npos ? line.size() : tab) - fp));
				if (tab == std::string::npos) break;
				fp = tab + 1;
			}
			records.push_back(std::move(fields));
		}
		return records;
	}
}

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

void RequestsHandler::loadRegistrarMode()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}
	uint8_t v = 0;
	esp_err_t err = nvs_get_u8(h, "reg_mode", &v);
	nvs_close(h);
	if (err == ESP_OK && v <= static_cast<uint8_t>(RegistrarMode::Secure))
	{
		_registrarMode.store(static_cast<RegistrarMode>(v), std::memory_order_relaxed);
	}
	// else: keep the compile-time-seeded default (Open under POCKETDIAL_OPEN_REGISTRAR).
#endif
}

void RequestsHandler::persistRegistrarMode()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_u8(h, "reg_mode",
			static_cast<uint8_t>(_registrarMode.load(std::memory_order_relaxed)));
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

void RequestsHandler::loadDevices()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}
	size_t len = 0;
	if (nvs_get_str(h, "devices", nullptr, &len) == ESP_OK && len > 0)
	{
		std::string buf(len, '\0');
		if (nvs_get_str(h, "devices", buf.data(), &len) == ESP_OK)
		{
			if (!buf.empty() && buf.back() == '\0') buf.pop_back();
			// Record: mac \t extension \t state(int)
			for (const auto& rec : deserializeBlob(buf))
			{
				if (rec.size() < 3 || rec[0].empty()) continue;
				if (_devices.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS)) break;
				DeviceRecord r;
				r.extension = rec[1];
				int si = atoi(rec[2].c_str());
				r.state = (si == static_cast<int>(DeviceState::Secured))
					? DeviceState::Secured : DeviceState::Learned;
				_devices[rec[0]] = std::move(r);
			}
		}
	}
	nvs_close(h);
#endif
}

void RequestsHandler::persistDevices()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	// mac \t extension \t state(int). Bounded by POCKETDIAL_MAX_CLIENTS, so the blob
	// is fixed-footprint. Write-through after each adoption / secure / forget. Caller
	// holds _mutex; online state is NOT persisted (it is volatile registration state).
	std::string blob;
	for (const auto& [mac, rec] : _devices)
	{
		blob += mac; blob += '\t';
		blob += rec.extension; blob += '\t';
		blob += std::to_string(static_cast<int>(rec.state)); blob += '\n';
	}
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_str(h, "devices", blob.c_str());
		nvs_commit(h);
		nvs_close(h);
	}
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
	// else: keep the in-class default "101"
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
	else if (callerExt != _adminExt && !seq.empty() && seq[0] == '*' &&
	         seq.find('#') != std::string::npos)
	{
		// A non-admin caller attempting the admin-menu pattern (*PIN#…): reject.
		// CLASS service codes (*60/*72/…) have no '#', so they fall through to the
		// per-subscriber feature handling below for any registered caller.
		auto response = getMessageFromPool(data->toString(), data->getSource());
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
		// isDndEnabled / setDnd operate on the _dnd map. We're already inside _mutex.
		if (_dnd.find(callerExt) == _dnd.end() &&
			_dnd.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
		{
			queueLog("*60 SCR: DND table full for " + callerExt, true);
		}
		else
		{
			_dnd[callerExt] = true;
			queueLog("*60 SCR enabled for " + callerExt);
		}
		accum.digits.clear();
		return;
	}

	// *80 — Disable SCR/DND for caller's extension.
	if (seq == "*80")
	{
		_dnd.erase(callerExt);
		queueLog("*80 SCR disabled for " + callerExt);
		accum.digits.clear();
		return;
	}

	// *73 — Disable CFU for caller's extension.
	if (seq == "*73")
	{
		auto it = _forwards.find(callerExt);
		if (it != _forwards.end())
		{
			it->second.always.clear();
			if (it->second.empty()) _forwards.erase(it);
			persistForwards();
		}
		queueLog("*73 CFU disabled for " + callerExt);
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
			bool isNew = (_forwards.find(callerExt) == _forwards.end());
			if (isNew && _forwards.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
			{
				queueLog("*72 CFU: forward table full for " + callerExt, true);
			}
			else
			{
				_forwards[callerExt].always = target;
				persistForwards();
				queueLog("*72 CFU enabled: " + callerExt + " -> " + target);
			}
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

// Extract the ;tag= value from a From/To header line.
static std::string parkTagOf(std::string_view header)
{
	size_t p = header.find(";tag=");
	if (p == std::string_view::npos) return {};
	p += 5;
	size_t e = p;
	while (e < header.size() && header[e] != ';' && header[e] != '>' &&
		header[e] != ' ' && header[e] != '\r' && header[e] != '\n')
	{
		++e;
	}
	return std::string(header.substr(p, e - p));
}

// Strip a leading "HeaderName:" prefix (e.g. "From:", "To:", "Call-ID:") so
// server-minted requests emit a clean value and never ship a doubled prefix.
// Safe on bare values (the check is that the text before ':' contains only
// letter/hyphen chars — so "<sip:...>" is left untouched). Idempotent.
static std::string stripHeaderName(std::string_view h)
{
	size_t colon = h.find(':');
	if (colon == std::string_view::npos || colon == 0 || colon > 15)
		return std::string(h);
	for (size_t i = 0; i < colon; ++i)
	{
		char c = h[i];
		if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '-'))
			return std::string(h);
	}
	size_t v = colon + 1;
	while (v < h.size() && (h[v] == ' ' || h[v] == '\t')) ++v;
	size_t e = h.size();
	while (e > v && (h[e - 1] == '\r' || h[e - 1] == '\n')) --e;
	return std::string(h.substr(v, e - v));
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
		auto response = getMessageFromPool(data->toString(), data->getSource());
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
		auto resp = getMessageFromPool(data->toString(), data->getSource());
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
		auto resp = getMessageFromPool(data->toString(), data->getSource());
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
		auto resp = getMessageFromPool(data->toString(), data->getSource());
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

// ── BLF presence (RFC 6665 SUBSCRIBE / RFC 4235 dialog-event NOTIFY) ─────────

std::string RequestsHandler::parseEventPackage(const std::string& raw)
{
	size_t pos = 0;
	while (pos < raw.size())
	{
		size_t nl = raw.find('\n', pos);
		size_t lineEnd = (nl == std::string::npos) ? raw.size() : nl;
		size_t next = (nl == std::string::npos) ? raw.size() : nl + 1;
		if (raw[pos] == '\r' || raw[pos] == '\n') break;

		size_t colon = raw.find(':', pos);
		if (colon != std::string::npos && colon < lineEnd)
		{
			std::string name = raw.substr(pos, colon - pos);
			std::transform(name.begin(), name.end(), name.begin(),
				[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
			if (name == "event" || name == "o")
			{
				size_t vBegin = colon + 1;
				size_t vEnd = lineEnd;
				std::string val = raw.substr(vBegin, vEnd - vBegin);
				size_t semi = val.find(';');
				if (semi != std::string::npos) val.erase(semi);
				size_t b = val.find_first_not_of(" \t\r");
				size_t e = val.find_last_not_of(" \t\r");
				if (b == std::string::npos) return "";
				return val.substr(b, e - b + 1);
			}
		}
		pos = next;
	}
	return "";
}

std::string RequestsHandler::buildDialogInfoXml(const std::string& entity, unsigned version,
	const std::string& dialogId, const std::string& state, const std::string& direction)
{
	// Minimal RFC 4235 document, always state="full": each NOTIFY carries the
	// complete (zero-or-one element) dialog set, so watchers never need to merge
	// partials. An empty `state` omits the <dialog> element — the canonical
	// "idle lamp" body that Yealink/Grandstream BLF keys expect.
	std::ostringstream xml;
	xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
	    << "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\""
	    << version << "\" state=\"full\" entity=\"" << entity << "\">\r\n";
	if (!state.empty())
	{
		xml << "<dialog id=\"" << dialogId << "\"";
		if (!direction.empty()) xml << " direction=\"" << direction << "\"";
		xml << "><state>" << state << "</state></dialog>\r\n";
	}
	xml << "</dialog-info>\r\n";
	return xml.str();
}

std::string RequestsHandler::computeDialogState(const std::string& targetAor,
	std::string& outDirection, std::string& outDialogId) const
{
	std::string best;
	auto rank = [](const std::string& s) -> int {
		if (s == "confirmed") return 3;
		if (s == "early")     return 2;
		if (s == "trying")    return 1;
		return 0;
	};
	for (const auto& [callID, session] : _sessions)
	{
		bool isSrc  = session->getSrc()  && session->getSrc()->getNumber()  == targetAor;
		bool isDest = session->getDest() && session->getDest()->getNumber() == targetAor;
		if (!isSrc && !isDest) continue;

		std::string state, dir;
		switch (session->getState())
		{
			case Session::State::Invited:
				state = isDest ? "early" : "trying";
				dir   = isDest ? "recipient" : "initiator";
				break;
			case Session::State::Connected:
			case Session::State::Held:
				// RFC 4235 §4: a held call is an established dialog; state stays
				// "confirmed" so the watcher's BLF lamp remains lit (#53).
				state = "confirmed";
				dir   = isSrc ? "initiator" : "recipient";
				break;
			default:
				continue;
		}
		if (rank(state) > rank(best))
		{
			best = state;
			outDirection = dir;
			outDialogId  = callID;
		}
	}
	if (best.empty()) { outDirection.clear(); outDialogId.clear(); }
	return best;
}

std::shared_ptr<SipMessage> RequestsHandler::buildDialogNotify(DialogSubscription& sub,
	const std::string& state, const std::string& direction, const std::string& dialogId,
	bool terminated, const char* termReason)
{
	std::string activeIp = _localIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &sub.addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(sub.addr.sin_port));
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);

	std::string entity = "sip:" + sub.targetAor + "@" + srcIpPort;
	std::string body = buildDialogInfoXml(entity, sub.version, dialogId, state, direction);

	int remaining = 0;
	if (!terminated)
	{
		auto now = std::chrono::steady_clock::now();
		remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
			sub.deadline - now).count());
		if (remaining < 0) remaining = 0;
	}
	std::string subState = terminated
		? ("terminated;reason=" + std::string(termReason))
		: ("active;expires=" + std::to_string(remaining));

	// NOTIFY runs inside the subscription dialog: our To-tag becomes the From,
	// the watcher's From becomes the To (RFC 6665 §4.4.1 — roles swap).
	std::string fromHdr = sub.subTo;
	std::string toHdr   = sub.watcherFrom;
	auto stripName = [](const std::string& h) {
		size_t c = h.find(':');
		return (c == std::string::npos) ? h : h.substr(c + 1);
	};

	std::ostringstream ss;
	ss << "NOTIFY sip:" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From:" << stripName(fromHdr) << "\r\n"
	   << "To:" << stripName(toHdr) << "\r\n"
	   << "Call-ID: " << sub.callId << "\r\n"
	   << "CSeq: " << sub.cseq++ << " NOTIFY\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Event: dialog\r\n"
	   << "Subscription-State: " << subState << "\r\n"
	   << "Contact: <sip:" << sub.targetAor << "@" << srcIpPort << ">\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: application/dialog-info+xml\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	return getMessageFromPool(ss.str(), sub.addr);
}

void RequestsHandler::onSubscribe(std::shared_ptr<SipMessage> data)
{
	std::string activeIp = _localIp;

	// 1. Event-package gate: only the RFC 4235 "dialog" package is implemented.
	std::string pkg = parseEventPackage(data->toString());
	if (pkg != "dialog")
	{
		auto resp = getMessageFromPool(data->toString(), data->getSource());
		resp->setHeader(SipMessageTypes::BAD_EVENT);
		resp->clearBody();
		resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		resp->addHeader("Allow-Events", "dialog");
		_outbox.emplace_back(data->getSource(), std::move(resp));
		return;
	}

	// 2. Watched-target validation.
	std::string target(data->getToNumber());
	if (!isValidAor(target))
	{
		auto resp = getMessageFromPool(data->toString(), data->getSource());
		resp->setHeader(SipMessageTypes::BAD_REQUEST);
		resp->clearBody();
		resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(resp));
		return;
	}

	const int expires = parseRequestedExpires(data);
	std::string callId(data->getCallID());
	{
		size_t c = callId.find(':');
		if (c != std::string::npos) callId.erase(0, c + 1);
		size_t b = callId.find_first_not_of(" \t");
		size_t e = callId.find_last_not_of(" \t\r");
		callId = (b == std::string::npos) ? "" : callId.substr(b, e - b + 1);
	}

	// 3. Refresh / unsubscribe: match existing subscription by Call-ID.
	DialogSubscription* sub = nullptr;
	for (auto& s : _subscriptions)
	{
		if (s.used && s.callId == callId) { sub = &s; break; }
	}

	if (sub == nullptr && expires > 0)
	{
		for (auto& s : _subscriptions)
		{
			if (!s.used) { sub = &s; break; }
		}
		if (sub == nullptr)
		{
			auto resp = getMessageFromPool(data->toString(), data->getSource());
			resp->setHeader("SIP/2.0 503 Service Unavailable");
			resp->clearBody();
			resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
			_outbox.emplace_back(data->getSource(), std::move(resp));
			queueLog("BLF: subscription pool exhausted, 503 to watcher of " + target, true);
			return;
		}
		*sub = DialogSubscription{};
		sub->used        = true;
		sub->callId      = callId;
		sub->watcherFrom = std::string(data->getFrom());
		sub->subTo       = std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9);
		sub->targetAor   = target;
		queueLog("BLF: new subscription, watcher " + std::string(data->getFromNumber())
			+ " -> target " + target);
	}

	// 4. 202 Accepted (RFC 6665 §4.2.1).
	{
		auto resp = getMessageFromPool(data->toString(), data->getSource());
		resp->setHeader(SipMessageTypes::ACCEPTED);
		resp->clearBody();
		resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		if (sub) resp->setTo(sub->subTo);
		else     resp->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
		resp->setContact(buildContact(target));
		resp->addHeader("Expires", std::to_string(expires));
		_outbox.emplace_back(data->getSource(), std::move(resp));
	}

	if (sub == nullptr) return;

	sub->addr       = data->getSource();
	sub->expiresSec = expires;
	sub->deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(expires);

	// 5. Immediate NOTIFY (RFC 6665 §4.2.1.4).
	std::string dir, dialogId;
	std::string state = computeDialogState(target, dir, dialogId);
	const bool terminating = (expires == 0);
	auto notify = buildDialogNotify(*sub, state, dir, dialogId, terminating, "noresource");
	_outbox.emplace_back(sub->addr, std::move(notify));
	sub->lastState = state + "|" + dir + "|" + dialogId;
	sub->version++;

	if (terminating)
	{
		queueLog("BLF: unsubscribe, watcher of " + target + " released");
		*sub = DialogSubscription{};
	}
}

void RequestsHandler::refreshSubscriptions()
{
	for (auto& sub : _subscriptions)
	{
		if (!sub.used) continue;
		std::string dir, dialogId;
		std::string state = computeDialogState(sub.targetAor, dir, dialogId);
		std::string token = state + "|" + dir + "|" + dialogId;
		if (token == sub.lastState) continue;
		auto notify = buildDialogNotify(sub, state, dir, dialogId, false, "");
		_outbox.emplace_back(sub.addr, std::move(notify));
		sub.lastState = token;
		sub.version++;
	}
}

void RequestsHandler::sweepSubscriptions()
{
	auto now = std::chrono::steady_clock::now();
	for (auto& sub : _subscriptions)
	{
		if (!sub.used || now < sub.deadline) continue;
		std::string dir, dialogId;
		std::string state = computeDialogState(sub.targetAor, dir, dialogId);
		auto notify = buildDialogNotify(sub, state, dir, dialogId, true, "timeout");
		_outbox.emplace_back(sub.addr, std::move(notify));
		queueLog("BLF: subscription to " + sub.targetAor + " expired");
		sub = DialogSubscription{};
	}
}

// ── RFC 3261 §17 INVITE-client transaction (Timer A retransmit / Timer B timeout)

RequestsHandler::SipTransaction::Type
RequestsHandler::classifyTxType(const std::shared_ptr<SipMessage>& msg)
{
	if (!msg || msg->getStatusInfo().has_value()) return SipTransaction::Type::None;
	if (msg->getType() == "INVITE") return SipTransaction::Type::InviteClient;
	return SipTransaction::Type::None;
}

void RequestsHandler::registerTx(SipTransaction::Type type,
                                  const sockaddr_in& peer,
                                  const std::shared_ptr<SipMessage>& msg)
{
	SipTransaction* slot = nullptr;
	for (auto& tx : _txPool)
	{
		if (tx.type == SipTransaction::Type::None) { slot = &tx; break; }
	}
	if (!slot)
	{
		queueLog("[tx] pool exhausted — INVITE sent without retransmit tracking", true);
		return;
	}

	auto raw = msg->toString();
	auto now = std::chrono::steady_clock::now();
	constexpr uint32_t kT1ms     = 500;
	constexpr uint32_t kTimerBms = 64 * kT1ms; // 32 s

	slot->type          = type;
	slot->state         = SipTransaction::State::Calling;
	slot->peer          = peer;
	slot->msgTruncated  = (raw.size() >= sizeof(slot->msg));
	slot->msgLen        = raw.size() < sizeof(slot->msg) ? raw.size() : sizeof(slot->msg) - 1;
	std::memcpy(slot->msg, raw.data(), slot->msgLen);
	slot->msg[slot->msgLen] = '\0';

	auto branch = msg->getViaBranch();
	auto bLen   = branch.size() < sizeof(slot->viaBranch) ? branch.size() : sizeof(slot->viaBranch) - 1;
	std::memcpy(slot->viaBranch, branch.data(), bLen);
	slot->viaBranch[bLen] = '\0';

	auto method = msg->getCSeqMethod();
	auto mLen   = method.size() < sizeof(slot->cseqMethod) ? method.size() : sizeof(slot->cseqMethod) - 1;
	std::memcpy(slot->cseqMethod, method.data(), mLen);
	slot->cseqMethod[mLen] = '\0';

	auto callId = msg->getCallID();
	auto cLen   = callId.size() < sizeof(slot->callId) ? callId.size() : sizeof(slot->callId) - 1;
	std::memcpy(slot->callId, callId.data(), cLen);
	slot->callId[cLen] = '\0';

	slot->nextRetransmit     = now + std::chrono::milliseconds(kT1ms);
	slot->transactionTimeout = now + std::chrono::milliseconds(kTimerBms);
	slot->absorbDeadline     = {};
	slot->retransmitCount    = 0;
	slot->currentIntervalMs  = kT1ms;
}

bool RequestsHandler::matchAndAdvanceTx(const std::shared_ptr<SipMessage>& msg)
{
	if (!msg) return false;
	auto viaBranch = msg->getViaBranch();
	if (viaBranch.empty()) return false;
	auto cseqMethod = msg->getCSeqMethod();

	bool matched = false;
	auto now = std::chrono::steady_clock::now();
	constexpr uint32_t kTimerLms = 64 * 500; // RFC 6026: 32 s absorb after 2xx

	for (auto& tx : _txPool)
	{
		if (tx.type == SipTransaction::Type::None) continue;
		if (viaBranch != std::string_view(tx.viaBranch)) continue;
		if (!cseqMethod.empty() && cseqMethod != std::string_view(tx.cseqMethod)) continue;

		matched = true;
		auto si = msg->getStatusInfo();
		if (!si.has_value()) continue;

		using Cls = PocketDial::SipStatusClass;
		switch (si->klass)
		{
			case Cls::Provisional:
				if (tx.state == SipTransaction::State::Calling)
					tx.state = SipTransaction::State::Proceeding;
				break;
			case Cls::Success:
				tx.state = SipTransaction::State::Accepted;
				tx.absorbDeadline = now + std::chrono::milliseconds(kTimerLms);
				break;
			default:
				tx.state = SipTransaction::State::Completed;
				tx.absorbDeadline = now + std::chrono::milliseconds(kTimerLms);
				break;
		}
	}
	return matched;
}

void RequestsHandler::sweepTransactions(std::chrono::steady_clock::time_point now)
{
	for (auto& tx : _txPool)
	{
		if (tx.type == SipTransaction::Type::None) continue;

		if (tx.state == SipTransaction::State::Completed ||
		    tx.state == SipTransaction::State::Accepted)
		{
			if (now >= tx.absorbDeadline)
				tx.type = SipTransaction::Type::None;
			continue;
		}

		if (tx.state == SipTransaction::State::Calling &&
		    now >= tx.transactionTimeout)
		{
			queueLog(std::string("[tx] Timer B expired — INVITE for ") + tx.callId
				+ " timed out (no provisional response)", true);
			tx.type = SipTransaction::Type::None;
			continue;
		}

		if (tx.state == SipTransaction::State::Calling &&
		    now >= tx.nextRetransmit)
		{
			if (tx.msgLen > 0 && !tx.msgTruncated)
			{
				std::string retxStr(tx.msg, tx.msgLen);
				auto retx = getMessageFromPool(retxStr, tx.peer);
				if (retx) _outbox.emplace_back(tx.peer, std::move(retx));
			}
			tx.currentIntervalMs *= 2;
			tx.nextRetransmit     = now + std::chrono::milliseconds(tx.currentIntervalMs);
			tx.retransmitCount++;
		}
	}
}

void RequestsHandler::freeTxsForCallId(std::string_view callId)
{
	for (auto& tx : _txPool)
	{
		if (tx.type != SipTransaction::Type::None &&
		    std::string_view(tx.callId) == callId)
		{
			tx.type = SipTransaction::Type::None;
		}
	}
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

// ── Call parking (orbits 700–709) ─────────────────────────────────────────────

int RequestsHandler::parkOrbitIndex(std::string_view ext) const
{
	if (ext.size() != 3 || ext[0] != '7' || ext[1] != '0') return -1;
	if (ext[2] < '0' || ext[2] > '9') return -1;
	int idx = ext[2] - '0';
	return (idx < static_cast<int>(_parkSlots.size())) ? idx : -1;
}

void RequestsHandler::onParkInvite(std::shared_ptr<SipMessage> data,
	const std::shared_ptr<SipClient>& caller, int orbitIdx)
{
	if (orbitIdx < 0 || orbitIdx >= static_cast<int>(_parkSlots.size()) || !caller)
	{
		return;
	}
	ParkSlot& slot = _parkSlots[orbitIdx];
	const std::string activeIp = _localIp;
	const std::string orbit = "70" + std::to_string(orbitIdx);
	const std::string toTag = IDGen::GenerateID(9);

	if (slot.state == ParkState::Free)
	{
		// ── PARK ── answer with an a=inactive hold SDP so the parked party is held.
		const std::string holdSdp =
			"v=0\r\n"
			"o=- 0 0 IN IP4 " + activeIp + "\r\n"
			"s=pocket-dial\r\n"
			"c=IN IP4 " + activeIp + "\r\n"
			"t=0 0\r\n"
			"m=audio 9 RTP/AVP 0\r\n"
			"a=inactive\r\n";
		auto ok = getMessageFromPool(data->toString(), data->getSource());
		ok->setHeader(SipMessageTypes::OK);
		ok->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		ok->setTo(std::string(data->getTo()) + ";tag=" + toTag);
		ok->setContact(buildContact(orbit));
		ok->setBody(holdSdp);
		ok->syncContentLength();
		_outbox.emplace_back(data->getSource(), ok);

		slot = ParkSlot{};
		slot.state      = ParkState::Parked;
		slot.orbit      = orbit;
		slot.callID     = std::string(data->getCallID());
		slot.parkedExt  = caller->getNumber();
		slot.parkedAddr = data->getSource();
		slot.parked        = true;
		slot.parkedSdp     = std::string(data->getBody());
		slot.parkedFromTag = parkTagOf(data->getFrom());
		slot.localTag   = toTag;
		slot.parker     = caller->getNumber();
		slot.parkedAt   = std::chrono::steady_clock::now();

		auto virt = allocateVirtualPeer(orbit, data->getSource());
		if (auto session = allocateSession(std::string(data->getCallID()), caller))
		{
			session->setDest(virt);
			session->setLocalTag(toTag);
			session->setInviteMessage(data);
			_sessions.emplace(std::string(data->getCallID()), session);
			session->setState(Session::State::Connected);
		}
		queueLog("Park: " + caller->getNumber() + " parked on " + orbit);
		refreshParkSnapshot();
		return;
	}

	// ── RETRIEVE ── the orbit is occupied: SDP-swap retrieve.
	// Allocate the retriever session FIRST (#71): if the pool is exhausted we 503
	// the retriever and leave the slot intact rather than sending a re-INVITE to
	// the parked party and then having no session to track the bridge.
	const std::string parkedSdp(slot.parkedSdp);
	const std::string retrieverSdp(data->getBody());

	auto virt = allocateVirtualPeer(slot.parkedExt, slot.parkedAddr);
	auto rsession = allocateSession(std::string(data->getCallID()), caller);
	if (!rsession)
	{
		auto resp = getMessageFromPool(data->toString(), data->getSource());
		resp->setHeader("SIP/2.0 503 Service Unavailable");
		resp->clearBody();
		resp->syncContentLength();
		_outbox.emplace_back(data->getSource(), std::move(resp));
		queueLog("Park: retrieve rejected — session pool exhausted", true);
		return;
	}

	auto ok = getMessageFromPool(data->toString(), data->getSource());
	ok->setHeader(SipMessageTypes::OK);
	ok->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	ok->setTo(std::string(data->getTo()) + ";tag=" + toTag);
	ok->setContact(buildContact(orbit));
	if (!parkedSdp.empty()) ok->setBody(parkedSdp);
	ok->enforceG711();
	ok->syncContentLength();
	_outbox.emplace_back(data->getSource(), ok);

	rsession->setDest(virt);
	rsession->setPeerCallID(slot.callID);
	rsession->setLocalTag(toTag);
	rsession->setInviteMessage(data);
	_sessions.emplace(std::string(data->getCallID()), rsession);
	rsession->setState(Session::State::Connected);

	if (auto parked = getSession(slot.callID); parked.has_value())
	{
		parked.value()->setPeerCallID(std::string(data->getCallID()));
	}
	sendParkReinvite(slot, retrieverSdp);
	queueLog("Park: " + caller->getNumber() + " retrieved " + slot.parkedExt + " from " + orbit);

	slot = ParkSlot{};
	refreshParkSnapshot();
}

void RequestsHandler::sendParkReinvite(ParkSlot& slot, const std::string& sdp)
{
	if (slot.callID.empty() || !slot.parked) return;
	const std::string activeIp = _localIp;
	const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &slot.parkedAddr.sin_addr, ipBuf, sizeof(ipBuf));
	const std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(slot.parkedAddr.sin_port));

	const std::string theirTag = slot.parkedFromTag;
	const std::string branch = "z9hG4bK" + IDGen::GenerateID(12);
	const std::string body = sdp;

	std::ostringstream ss;
	ss << "INVITE sip:" << slot.parkedExt << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From: <sip:" << slot.orbit << "@" << srcIpPort << ">;tag=" << slot.localTag << "\r\n"
	   << "To: <sip:" << slot.parkedExt << "@" << activeIp << ">;tag=" << theirTag << "\r\n"
	   << "Call-ID: " << slot.callID << "\r\n"
	   << "CSeq: 2 INVITE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Contact: <sip:" << slot.orbit << "@" << srcIpPort << ";transport=UDP>\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: application/sdp\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	auto inv = getMessageFromPool(ss.str(), slot.parkedAddr);
	inv->enforceG711();
	inv->syncContentLength();
	_outbox.emplace_back(slot.parkedAddr, std::move(inv));
	_parkPendingAcks.push_back(slot.callID);
	queueLog("Park: re-INVITE -> parked party " + slot.parkedExt);
}

void RequestsHandler::byeParkedParty(const ParkSlot& slot)
{
	if (slot.callID.empty()) return;
	const std::string activeIp = _localIp;
	const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	const std::string fromHeader = "<sip:" + slot.orbit + "@" + srcIpPort + ">;tag=" + slot.localTag;
	const std::string toHeader = "<sip:" + slot.parkedExt + "@" + activeIp + ">;tag=" + slot.parkedFromTag;
	auto bye = buildServerBye(slot.parkedExt, slot.parkedAddr,
		stripHeaderName(slot.callID), fromHeader, toHeader);
	if (bye) _outbox.emplace_back(slot.parkedAddr, std::move(bye));
}

void RequestsHandler::startParkRingback(ParkSlot& slot, const std::shared_ptr<SipClient>& parker,
	std::chrono::steady_clock::time_point now)
{
	if (!parker) { byeParkedParty(slot); freeParkSlot(slot.callID); return; }
	const std::string activeIp = _localIp;
	const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	const sockaddr_in& addr = parker->getAddress();

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
	const std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));

	slot.rbCallID  = "Call-ID: " + IDGen::GenerateID(16) + "@" + activeIp;
	slot.rbFromTag = IDGen::GenerateID(9);
	slot.rbBranch  = "z9hG4bK" + IDGen::GenerateID(12);
	slot.rbAddr    = addr;
	slot.state     = ParkState::RingingBack;
	slot.deadline  = now + std::chrono::seconds(30);

	const std::string body = slot.parkedSdp;
	std::ostringstream ss;
	ss << "INVITE sip:" << parker->getNumber() << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << slot.rbBranch << "\r\n"
	   << "From: \"PocketDial Park\" <sip:" << slot.orbit << "@" << srcIpPort << ">;tag=" << slot.rbFromTag << "\r\n"
	   << "To: <sip:" << parker->getNumber() << "@" << activeIp << ">\r\n"
	   << slot.rbCallID << "\r\n"
	   << "CSeq: 1 INVITE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Contact: <sip:" << slot.orbit << "@" << srcIpPort << ";transport=UDP>\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: application/sdp\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	auto inv = getMessageFromPool(ss.str(), addr);
	inv->enforceG711();
	inv->syncContentLength();
	_outbox.emplace_back(addr, std::move(inv));
	queueLog("Park: timeout on " + slot.orbit + " — ringing back parker " + parker->getNumber());
}

bool RequestsHandler::handleParkOk(const std::shared_ptr<SipMessage>& data)
{
	const std::string cseq(data->getCSeq());
	const std::string callID(data->getCallID());
	const bool isInviteOk = cseq.find(SipMessageTypes::INVITE) != std::string::npos;
	if (!isInviteOk) return false;

	// (a) 200 OK to a park re-INVITE sent to the parked party: ACK and free tracking entry.
	if (auto it = std::find(_parkPendingAcks.begin(), _parkPendingAcks.end(), callID);
		it != _parkPendingAcks.end())
	{
		const std::string activeIp = _localIp;
		const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
		const sockaddr_in srcAddr = data->getSource();
		char ipBuf[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &srcAddr.sin_addr, ipBuf, sizeof(ipBuf));
		const std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(srcAddr.sin_port));
		std::ostringstream ss;
		ss << "ACK sip:" << data->getToNumber() << "@" << destIpPort << " SIP/2.0\r\n"
		   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=z9hG4bK" << IDGen::GenerateID(12) << "\r\n"
		   << "From: " << stripHeaderName(data->getFrom()) << "\r\n"
		   << "To: " << stripHeaderName(data->getTo()) << "\r\n"
		   << callID << "\r\n"
		   << "CSeq: 2 ACK\r\n"
		   << "Max-Forwards: 70\r\n"
		   << "Content-Length: 0\r\n\r\n";
		_outbox.emplace_back(data->getSource(), getMessageFromPool(ss.str(), data->getSource()));
		_parkPendingAcks.erase(it);
		return true;
	}

	// (b) 200 OK to a ring-back INVITE sent to the parker: ACK parker, bridge both legs.
	for (auto& slot : _parkSlots)
	{
		if (slot.state == ParkState::RingingBack && slot.rbCallID == callID)
		{
			const std::string activeIp = _localIp;
			const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
			char ipBuf[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &slot.rbAddr.sin_addr, ipBuf, sizeof(ipBuf));
			const std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(slot.rbAddr.sin_port));
			std::ostringstream ss;
			ss << "ACK sip:" << data->getToNumber() << "@" << destIpPort << " SIP/2.0\r\n"
			   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << slot.rbBranch << "\r\n"
			   << "From: \"PocketDial Park\" <sip:" << slot.orbit << "@" << srcIpPort << ">;tag=" << slot.rbFromTag << "\r\n"
			   << "To: " << stripHeaderName(data->getTo()) << "\r\n"
			   << slot.rbCallID << "\r\n"
			   << "CSeq: 1 ACK\r\n"
			   << "Max-Forwards: 70\r\n"
			   << "Content-Length: 0\r\n\r\n";
			_outbox.emplace_back(slot.rbAddr, getMessageFromPool(ss.str(), slot.rbAddr));

			sendParkReinvite(slot, std::string(data->getBody()));

			if (auto parker = findClient(slot.parker); parker.has_value())
			{
				auto virt = allocateVirtualPeer(slot.parkedExt, slot.parkedAddr);
				if (auto rb = allocateSession(slot.rbCallID, parker.value()))
				{
					rb->setDest(virt);
					rb->setPeerCallID(slot.callID);
					rb->setLocalTag(slot.rbFromTag);
					rb->setParkUac(true);
					rb->setInviteMessage(data);
					_sessions.emplace(slot.rbCallID, rb);
					rb->setState(Session::State::Connected);
				}
				if (auto parked = getSession(slot.callID); parked.has_value())
				{
					parked.value()->setPeerCallID(slot.rbCallID);
				}
			}

			queueLog("Park: parker answered ring-back on " + slot.orbit + " — bridging");
			slot = ParkSlot{};
			refreshParkSnapshot();
			return true;
		}
	}
	return false;
}

bool RequestsHandler::handleTransferOk(const std::shared_ptr<SipMessage>& data)
{
	const std::string cseq(data->getCSeq());
	const std::string callID(data->getCallID());
	if (cseq.find(SipMessageTypes::INVITE) == std::string::npos) return false;

	auto it = std::find(_transferPendingAcks.begin(), _transferPendingAcks.end(), callID);
	if (it == _transferPendingAcks.end()) return false;

	const std::string activeIp = _localIp;
	const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	const sockaddr_in src = data->getSource();
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &src.sin_addr, ipBuf, sizeof(ipBuf));
	const std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(src.sin_port));

	std::ostringstream ss;
	ss << "ACK sip:" << data->getToNumber() << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=z9hG4bK" << IDGen::GenerateID(12) << "\r\n"
	   << "From: " << stripHeaderName(data->getFrom()) << "\r\n"
	   << "To: " << stripHeaderName(data->getTo()) << "\r\n"
	   << callID << "\r\n"
	   << "CSeq: 100 ACK\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";
	_outbox.emplace_back(data->getSource(), getMessageFromPool(ss.str(), data->getSource()));
	_transferPendingAcks.erase(it);
	return true;
}

void RequestsHandler::parkSweep(std::chrono::steady_clock::time_point now)
{
	for (auto& slot : _parkSlots)
	{
		if (slot.state == ParkState::Parked &&
			(now - slot.parkedAt) >= _parkTimeout)
		{
			auto parker = findClient(slot.parker);
			if (parker.has_value())
			{
				startParkRingback(slot, parker.value(), now);
			}
			else
			{
				queueLog("Park: timeout on " + slot.orbit + " — parker " + slot.parker +
					" gone, tearing down");
				byeParkedParty(slot);
				freeParkSlot(slot.callID);
			}
		}
		else if (slot.state == ParkState::RingingBack && now >= slot.deadline)
		{
			queueLog("Park: ring-back on " + slot.orbit + " not answered — tearing down");
			byeParkedParty(slot);
			freeParkSlot(slot.callID);
		}
	}
	refreshParkSnapshot();
}

void RequestsHandler::freeParkSlot(std::string_view callID)
{
	for (auto& slot : _parkSlots)
	{
		if (slot.state != ParkState::Free && slot.callID == callID)
		{
			slot = ParkSlot{};
		}
	}
	_parkPendingAcks.erase(
		std::remove(_parkPendingAcks.begin(), _parkPendingAcks.end(), std::string(callID)),
		_parkPendingAcks.end());
}

void RequestsHandler::refreshParkSnapshot()
{
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> snapLock(_snapshotMutex);
	_snapshot.parkedCalls.clear();
	for (const auto& slot : _parkSlots)
	{
		if (slot.state == ParkState::Free) continue;
		int secs = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
			now - slot.parkedAt).count());
		_snapshot.parkedCalls.emplace_back(slot.orbit, slot.parkedExt, slot.parker, secs);
	}
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
	auto ok = getMessageFromPool(inviteMsg->toString(), inviteMsg->getSource());
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
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &destAddr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(destAddr.sin_port));
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
	// Pool drained: fall back to a one-off heap SipClient rather than failing the call.
	std::cerr << "[WARNING] Virtual-peer pool exhausted! Fallback to heap allocation.\n";
	return std::make_shared<SipClient>(std::move(number), address, expiresSeconds);
}
