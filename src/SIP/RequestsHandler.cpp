// RequestsHandler.cpp: Issues #24 and #28 resolved.
#include "RequestsHandler.hpp"
#include <atomic>
#include <sstream>
#include <cctype>
#include <algorithm>
#include "SipMessageTypes.h"
#include "SipSdpMessage.hpp"
#include "IDGen.hpp"
#include "IPHelper.hpp"

// ── Compile-time tunables (Issues #47, #31) ──────────────────────────────────
// Override any of these with -D on the command line / build_flags to suit a
// known fleet. Defaults are chosen to be interoperable with softphones that do
// not answer out-of-dialog OPTIONS (a SHOULD, not a MUST, in RFC 3261).
#ifndef POCKETDIAL_KEEPALIVE_PING_SECS
#define POCKETDIAL_KEEPALIVE_PING_SECS 30   // how often we OPTIONS-ping each client
#endif
#ifndef POCKETDIAL_KEEPALIVE_TIMEOUT_SECS
#define POCKETDIAL_KEEPALIVE_TIMEOUT_SECS 90  // endpoint silence that flags an orphaned call
#endif

// Issue #38: per-source-IP flood protection. RATE_BURST is the bucket capacity
// (max packets accepted back-to-back); RATE_PER_SEC is the sustained refill rate.
#ifndef POCKETDIAL_RATE_BURST
#define POCKETDIAL_RATE_BURST 40
#endif
#ifndef POCKETDIAL_RATE_PER_SEC
#define POCKETDIAL_RATE_PER_SEC 20
#endif
// Optional allowlist, e.g. -DPOCKETDIAL_ALLOW_CIDR=\"192.168.1.0/24\". Undefined =
// accept every source (rate limiting still applies).

// Issue #37: virtual broadcast / all-page extension. An INVITE addressed here is
// forked to every other registered endpoint; the first to answer wins. Reserve
// this number — do not register a real client under it. Override with -D.
#ifndef POCKETDIAL_PAGE_EXTENSION
#define POCKETDIAL_PAGE_EXTENSION "999"
#endif

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

	// Issue #47: OPTIONS ping cadence (was a bare 5s).
	constexpr auto KEEPALIVE_PING_INTERVAL = std::chrono::seconds(POCKETDIAL_KEEPALIVE_PING_SECS);
	// Issue #31/#47: an endpoint silent this long is treated as gone for the
	// purpose of reaping its *active sessions*. It does NOT by itself unregister
	// the client — registration rides the REGISTER lease (see sweepExpired).
	constexpr auto KEEPALIVE_TIMEOUT = std::chrono::seconds(POCKETDIAL_KEEPALIVE_TIMEOUT_SECS);

	// Sessions in a terminal/handshake state are not subject to orphan reaping;
	// only an established (Connected) call can leak when an endpoint vanishes.
	bool isLiveCall(Session::State s)
	{
		return s == Session::State::Connected;
	}

	// True if two endpoints are the same IPv4 socket (address + port).
	bool addrEqual(const sockaddr_in& a, const sockaddr_in& b)
	{
		return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
	}
}

RequestsHandler::RequestsHandler(std::string serverIp, int serverPort,
	OnHandledEvent onHandledEvent) :
	_serverIp(std::move(serverIp)), _serverPort(serverPort),
	_onHandled(onHandledEvent)
{
	initHandlers();

#ifdef POCKETDIAL_ALLOW_CIDR
	// Issue #38: parse the compile-time CIDR allowlist once. Anything that fails
	// to parse leaves the allowlist disabled (fail-open, rate limiting still on).
	{
		std::string cidr = POCKETDIAL_ALLOW_CIDR;
		auto slash = cidr.find('/');
		std::string ipPart = (slash == std::string::npos) ? cidr : cidr.substr(0, slash);
		int prefix = 32;
		if (slash != std::string::npos)
		{
			try { prefix = std::stoi(cidr.substr(slash + 1)); } catch (...) { prefix = -1; }
		}
		struct in_addr a {};
		if (prefix >= 0 && prefix <= 32 && inet_pton(AF_INET, ipPart.c_str(), &a) == 1)
		{
			_allowNet  = ntohl(a.s_addr);
			_allowMask = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
		}
	}
#endif
}

bool RequestsHandler::ipAllowed(const sockaddr_in& src) const
{
	// No allowlist configured → accept everyone.
	if (_allowMask == 0)
	{
		return true;
	}
	uint32_t ip = ntohl(src.sin_addr.s_addr);
	return (ip & _allowMask) == (_allowNet & _allowMask);
}

bool RequestsHandler::allowPacket(const sockaddr_in& src)
{
	auto now = std::chrono::steady_clock::now();
	uint32_t key = src.sin_addr.s_addr;   // network order is fine as a map key
	auto& bucket = _rateBuckets[key];

	if (bucket.last.time_since_epoch().count() == 0)
	{
		// First packet from this IP — start the bucket full.
		bucket.tokens = static_cast<double>(POCKETDIAL_RATE_BURST);
	}
	else
	{
		double elapsed = std::chrono::duration<double>(now - bucket.last).count();
		bucket.tokens = (std::min)(static_cast<double>(POCKETDIAL_RATE_BURST),
			bucket.tokens + elapsed * static_cast<double>(POCKETDIAL_RATE_PER_SEC));
	}
	bucket.last = now;

	if (bucket.tokens >= 1.0)
	{
		bucket.tokens -= 1.0;
		return true;
	}
	return false;
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
}

void RequestsHandler::handle(std::shared_ptr<SipMessage> request)
{
	_packetsProcessed.fetch_add(1, std::memory_order_relaxed);
	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> localOutbox;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_outbox.clear();

		// Issue #38: drop traffic from non-allowlisted IPs or sources that are
		// flooding us, before any parsing/registry work or markActive() — so a
		// flood can neither do work nor keep a registration artificially alive.
		if (!ipAllowed(request->getSource()) || !allowPacket(request->getSource()))
		{
			_packetsDropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		auto client = findClientByAddress(request->getSource());
		if (client.has_value())
		{
			client.value()->markActive();
		}

		maybeSweep();
		auto it = _handlers.find(request->getType());
		if (it != _handlers.end())
		{
			it->second(std::move(request));
		}
		localOutbox = std::move(_outbox);
		_outbox.clear();
	}

	// Issue #24 resolved: UDP socket syscall sendto is now executed outside the locked section to prevent lock contention.
	for (auto& event : localOutbox)
	{
		_onHandled(event.first, std::move(event.second));
	}
}

std::optional<std::shared_ptr<Session>> RequestsHandler::getSession(const std::string& callID)
{
	auto sessionIt = _sessions.find(callID);
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

	if (requestedExpires <= 0)
	{
		// expires=0 (or an explicit zero) is a de-registration request.
		unregisterClient(fromNumber);
	}
	else
	{
		grantedExpires = (std::max)(MIN_EXPIRES, (std::min)(requestedExpires, MAX_EXPIRES));
		// Always update address so re-REGISTER after a NAT rebind works correctly
		auto newClient = std::make_shared<SipClient>(data->getFromNumber(), data->getSource(), grantedExpires);
		registerClient(std::move(newClient));
	}

	auto response = std::make_shared<SipMessage>(*data);
	response->setHeader(SipMessageTypes::OK);
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	response->setVia(data->getVia() + ";received=" + activeIp);
	response->setTo(data->getTo() + ";tag=" + IDGen::GenerateID(9));
	// Echo the granted lease back in the Contact so the client knows when to refresh.
	response->setContact(buildContact(fromNumber) + ";expires=" + std::to_string(grantedExpires));
	endHandle(fromNumber, response);
}

void RequestsHandler::onOptions(std::shared_ptr<SipMessage> data)
{
	auto response = std::make_shared<SipMessage>(*data);
	response->setHeader(SipMessageTypes::OK);
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	response->setVia(data->getVia() + ";received=" + activeIp);
	response->setTo(data->getTo() + ";tag=" + IDGen::GenerateID(9));
	response->setContact(buildContact(data->getFromNumber()));
	_outbox.emplace_back(data->getSource(), std::move(response));
}

void RequestsHandler::onCancel(std::shared_ptr<SipMessage> data)
{
	setCallState(data->getCallID(), Session::State::Cancel);
	endHandle(data->getToNumber(), data);
}

void RequestsHandler::onReqTerminated(std::shared_ptr<SipMessage> data)
{
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onInvite(std::shared_ptr<SipMessage> data)
{
	// Check if the caller is registered
	auto caller = findClient(data->getFromNumber());
	if (!caller.has_value())
	{
		return;
	}

	// Issue #37: an INVITE to the virtual page extension fans out to everyone.
	if (data->getToNumber() == POCKETDIAL_PAGE_EXTENSION)
	{
		startPaging(std::move(data), caller.value());
		return;
	}

	// Check if the called is registered
	auto called = findClient(data->getToNumber());
	if (!called.has_value())
	{
		// Send "SIP/2.0 404 Not Found"
		std::shared_ptr<SipMessage> responseObj = std::make_shared<SipMessage>(*data);
		responseObj->setHeader(SipMessageTypes::NOT_FOUND);
		responseObj->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	// Issue #42: hasSdp() virtual dispatch instead of dynamic_cast (RTTI is off on Arduino).
	if (!data->hasSdp())
	{
		std::cerr << "Couldn't get SDP from " << data->getFromNumber() << "'s INVITE request." << std::endl;
		std::shared_ptr<SipMessage> responseObj = std::make_shared<SipMessage>(*data);
		responseObj->setHeader(SipMessageTypes::BAD_REQUEST);
		responseObj->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	auto newSession = std::make_shared<Session>(data->getCallID(), caller.value());
	_sessions.emplace(data->getCallID(), newSession);

	auto response = std::make_shared<SipMessage>(*data);
	response->setContact(buildContact(caller.value()->getNumber()));
	endHandle(data->getToNumber(), response);
}

// Issue #37: fork the caller's INVITE to every other registered endpoint.
void RequestsHandler::startPaging(std::shared_ptr<SipMessage> data, std::shared_ptr<SipClient> caller)
{
	// A page is still a call — require SDP, same as a normal INVITE.
	if (!data->hasSdp())
	{
		auto bad = std::make_shared<SipMessage>(*data);
		bad->setHeader(SipMessageTypes::BAD_REQUEST);
		bad->setContact(buildContact(caller->getNumber()));
		endHandle(caller->getNumber(), bad);
		return;
	}

	auto session = std::make_shared<Session>(data->getCallID(), caller);
	session->setPaging(true);
	session->setPagingInvite(data);   // retained so we can CANCEL losers later

	int targets = 0;
	for (auto& [number, client] : _clients)
	{
		if (number == caller->getNumber())
		{
			continue;   // don't page the caller back to themselves
		}
		session->addPagedTarget(client);

		// Forward the caller's INVITE verbatim (SDP intact) to this endpoint.
		auto fork = std::make_shared<SipMessage>(*data);
		fork->setContact(buildContact(caller->getNumber()));
		_outbox.emplace_back(client->getAddress(), std::move(fork));
		++targets;
	}

	if (targets == 0)
	{
		// Nobody else is registered — tell the caller there's no one to reach.
		auto nf = std::make_shared<SipMessage>(*data);
		nf->setHeader(SipMessageTypes::NOT_FOUND);
		nf->setContact(buildContact(caller->getNumber()));
		endHandle(caller->getNumber(), nf);
		return;
	}

	_sessions.emplace(data->getCallID(), std::move(session));
	std::cout << "Paging " << targets << " endpoint(s) on behalf of "
	          << caller->getNumber() << " via extension " << POCKETDIAL_PAGE_EXTENSION << '\n';
}

void RequestsHandler::onTrying(std::shared_ptr<SipMessage> data)
{
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onRinging(std::shared_ptr<SipMessage> data)
{
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onBusy(std::shared_ptr<SipMessage> data)
{
	setCallState(data->getCallID(), Session::State::Busy);
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onUnavailable(std::shared_ptr<SipMessage> data)
{
	setCallState(data->getCallID(), Session::State::Unavailable);
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onBye(std::shared_ptr<SipMessage> data)
{
	// Issue #37: a paging call's dialog uses the virtual page extension, so route
	// the BYE to whichever connected party did NOT send it (by source address).
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isPaging())
	{
		session.value()->setState(Session::State::Bye);
		auto src = session.value()->getSrc();
		auto dst = session.value()->getDest();
		std::shared_ptr<SipClient> other =
			(src && addrEqual(src->getAddress(), data->getSource())) ? dst : src;
		if (other)
		{
			_outbox.emplace_back(other->getAddress(), std::move(data));
		}
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
			// Issue #37: in a paging call the answerer can't be found by To-number
			// (it's the virtual page extension) — resolve by source and fork-cancel.
			if (session.value()->isPaging())
			{
				handlePagingAnswer(session.value(), std::move(data));
				return;
			}

			auto client = findClient(data->getToNumber());
			if (!client.has_value())
			{
				return;
			}

			// Issue #42: hasSdp() virtual dispatch instead of dynamic_cast (RTTI is off on Arduino).
			if (!data->hasSdp())
			{
				std::cerr << "Couldn't get SDP from: " << client.value()->getNumber() << "'s OK message.\n";
				std::shared_ptr<SipMessage> responseObj = std::make_shared<SipMessage>(*data);
				responseObj->setHeader(SipMessageTypes::BAD_REQUEST);
				responseObj->setContact(buildContact(data->getToNumber()));
				endHandle(data->getToNumber(), responseObj);
				endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), "SDP parse error.");
				return;
			}
			session->get()->setDest(client.value());
			session->get()->setState(Session::State::Connected);
			auto response = std::make_shared<SipMessage>(*data);
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

// Issue #37: first 200 OK to a page wins. Identify the answerer by source
// address, connect it to the caller, and CANCEL every other paged endpoint.
void RequestsHandler::handlePagingAnswer(const std::shared_ptr<Session>& session,
	std::shared_ptr<SipMessage> data)
{
	auto answerer = findClientByAddress(data->getSource());

	// A later endpoint answered after we already connected one: tell it to hang up.
	if (session->getState() == Session::State::Connected)
	{
		if (answerer.has_value())
		{
			auto bye = buildPagingBye(data, answerer.value());
			_outbox.emplace_back(answerer.value()->getAddress(), std::move(bye));
		}
		return;
	}

	if (!answerer.has_value() || !data->hasSdp())
	{
		return;   // can't attribute this 200 OK, or it carries no media
	}

	// Connect caller <-> first answerer.
	session->setDest(answerer.value());
	session->setState(Session::State::Connected);

	// Relay the 200 OK back to the caller so their dialog completes.
	auto response = std::make_shared<SipMessage>(*data);
	response->setContact(buildContact(answerer.value()->getNumber()));
	endHandle(data->getFromNumber(), std::move(response));

	// CANCEL the endpoints that lost the race.
	auto invite = session->getPagingInvite();
	for (const auto& target : session->getPagedTargets())
	{
		if (addrEqual(target->getAddress(), answerer.value()->getAddress()))
		{
			continue;   // the winner
		}
		if (invite)
		{
			auto cancel = buildCancel(invite, target);
			_outbox.emplace_back(target->getAddress(), std::move(cancel));
		}
	}

	std::cout << "Page answered by " << answerer.value()->getNumber()
	          << "; cancelling " << (session->getPagedTargets().size() - 1)
	          << " other endpoint(s).\n";
}

void RequestsHandler::onAck(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (!session.has_value())
	{
		return;
	}

	// Issue #37: the ACK in a paging call is addressed to the page extension;
	// route it to the endpoint that actually answered.
	if (session.value()->isPaging())
	{
		auto dest = session.value()->getDest();
		if (dest)
		{
			_outbox.emplace_back(dest->getAddress(), std::move(data));
		}
		return;
	}

	endHandle(data->getToNumber(), data);

	auto sessionState = session.value()->getState();
	std::string endReason;
	if (sessionState == Session::State::Busy)
	{
		endReason = data->getToNumber() + " is busy.";
		endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), endReason);
		return;
	}

	if (sessionState == Session::State::Unavailable)
	{
		endReason = data->getToNumber() + " is unavailable.";
		endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), endReason);
		return;
	}

	if (sessionState == Session::State::Cancel)
	{
		endReason = data->getFromNumber() + " canceled the session.";
		endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), endReason);
		return;
	}
}

bool RequestsHandler::setCallState(const std::string& callID, Session::State state)
{
	auto session = getSession(callID);
	if (session)
	{
		session->get()->setState(state);
		return true;
	}

	return false;
}

void RequestsHandler::endCall(const std::string& callID, const std::string& srcNumber, const std::string& destNumber, const std::string& reason)
{
	if (_sessions.erase(callID) > 0)
	{
		std::ostringstream message;
		message << "Session has been disconnected between " << srcNumber << " and " << destNumber;
		if (!reason.empty())
		{
			message << " because " << reason;
		}
		std::cout << message.str() << std::endl;
	}
}

bool RequestsHandler::registerClient(std::shared_ptr<SipClient> client)
{
	// Always update the entry so a re-REGISTER after a NAT rebind refreshes the address
	std::cout << ((_clients.count(client->getNumber()) ? "Re-registered" : "New Client") )
	          << ": " << client->getNumber() << '\n';
	_clients[client->getNumber()] = client;
	return true;
}

void RequestsHandler::unregisterClient(const std::string& number)
{
	std::cout << "Unregistered client: " << number << '\n';
	_clients.erase(number);
}

int RequestsHandler::parseRequestedExpires(const std::shared_ptr<SipMessage>& data) const
{
	// Helper: read a non-negative integer starting at `from`. Returns -1 if no digits.
	auto readInt = [](const std::string& s, size_t from) -> int {
		while (from < s.size() && std::isspace(static_cast<unsigned char>(s[from]))) ++from;
		size_t end = from;
		while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
		if (end == from) return -1;
		try { return std::stoi(s.substr(from, end - from)); } catch (...) { return -1; }
	};

	// 1. expires= parameter on the Contact header (most common form).
	const std::string& contact = data->getContact();
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

	// Drop every session that involves a given extension.
	auto eraseSessionsFor = [this](const std::string& extension) {
		for (auto sit = _sessions.begin(); sit != _sessions.end(); )
		{
			const bool involved =
				(sit->second->getSrc()  && sit->second->getSrc()->getNumber()  == extension) ||
				(sit->second->getDest() && sit->second->getDest()->getNumber() == extension);
			if (involved)
				sit = _sessions.erase(sit);
			else
				++sit;
		}
	};

	// ── Pass 1 (Issue #31): reap orphaned live calls ────────────────────────
	// The server is blind to peer-to-peer RTP, so a client that loses power or
	// walks out of range mid-call never sends BYE. Detect it by endpoint silence:
	// any inbound packet (REGISTER refresh or a 200 OK to our OPTIONS ping) calls
	// markActive(); an endpoint quiet past KEEPALIVE_TIMEOUT is treated as gone and
	// its established call is torn down — without unregistering the client itself.
	for (auto sit = _sessions.begin(); sit != _sessions.end(); )
	{
		const auto& session = sit->second;
		auto silent = [&](const std::shared_ptr<SipClient>& c) {
			return c && (now - c->getLastActiveTime() > KEEPALIVE_TIMEOUT);
		};
		if (isLiveCall(session->getState()) &&
			(silent(session->getSrc()) || silent(session->getDest())))
		{
			std::cout << "Reaping orphaned session (endpoint silent > "
			          << POCKETDIAL_KEEPALIVE_TIMEOUT_SECS << "s): " << sit->first << '\n';
			sit = _sessions.erase(sit);
		}
		else
		{
			++sit;
		}
	}

	// ── Pass 2 (Issue #47): unregister clients on REGISTER-lease expiry only ──
	// Responding to out-of-dialog OPTIONS is a SHOULD, not a MUST. We no longer
	// evict a client merely for ignoring our pings — that flapped live phones in
	// and out of the registry between REGISTERs. Registration now rides the lease.
	for (auto it = _clients.begin(); it != _clients.end(); )
	{
		if (it->second->isExpired(now))
		{
			std::cout << "Registration lease expired: " << it->first << '\n';
			eraseSessionsFor(it->first);
			it = _clients.erase(it);
		}
		else
		{
			++it;
		}
	}

	// ── Pass 3 (Issue #38): evict idle rate-limit buckets to bound memory ────
	// Critical on ESP32: without this an attacker spoofing many source IPs could
	// grow _rateBuckets without limit. A bucket idle for 5 min is fully refilled
	// anyway, so dropping it costs nothing.
	for (auto it = _rateBuckets.begin(); it != _rateBuckets.end(); )
	{
		if (now - it->second.last > std::chrono::minutes(5))
			it = _rateBuckets.erase(it);
		else
			++it;
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

std::optional<std::shared_ptr<SipClient>> RequestsHandler::findClient(const std::string& number)
{
	auto it = _clients.find(number);
	if (it != _clients.end())
	{
		return it->second;
	}

	return {};
}

void RequestsHandler::endHandle(const std::string& destNumber, std::shared_ptr<SipMessage> message)
{
	auto destClient = findClient(destNumber);
	if (destClient.has_value())
	{
		_outbox.emplace_back(destClient.value()->getAddress(), std::move(message));
	}
	else
	{
		// Clone the message so we don't mutate a shared object's header
		auto notFound = std::make_shared<SipMessage>(*message);
		notFound->setHeader(SipMessageTypes::NOT_FOUND);
		auto src = message->getSource();
		_outbox.emplace_back(src, std::move(notFound));
	}
}

std::string RequestsHandler::buildContact(const std::string& number) const
{
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	return "Contact: <sip:" + number + "@" + activeIp + ":" + std::to_string(_serverPort) + ";transport=UDP>";
}

// Issue #37: build a CANCEL that matches a forked INVITE. The header accessors
// already include their field name ("Via: …", "From: …", …), so we reuse those
// lines verbatim and only rewrite the request line's method and the CSeq method.
std::shared_ptr<SipMessage> RequestsHandler::buildCancel(const std::shared_ptr<SipMessage>& invite,
	const std::shared_ptr<SipClient>& target)
{
	// Request line: same Request-URI as the INVITE, method swapped to CANCEL.
	const std::string& start = invite->getHeader();   // "INVITE <ruri> SIP/2.0"
	auto sp = start.find(' ');
	std::string cancelStart = (sp == std::string::npos) ? std::string("CANCEL")
	                                                    : ("CANCEL" + start.substr(sp));

	// CSeq: same sequence number as the INVITE, method CANCEL (RFC 3261 §9.1).
	std::string cseqNum = "1";
	{
		const std::string& cseq = invite->getCSeq();   // "CSeq: 1 INVITE"
		size_t i = cseq.find(':');
		i = (i == std::string::npos) ? 0 : i + 1;
		while (i < cseq.size() && std::isspace(static_cast<unsigned char>(cseq[i]))) ++i;
		size_t j = i;
		while (j < cseq.size() && std::isdigit(static_cast<unsigned char>(cseq[j]))) ++j;
		if (j > i) cseqNum = cseq.substr(i, j - i);
	}

	std::ostringstream ss;
	ss << cancelStart << "\r\n"
	   << invite->getVia() << "\r\n"
	   << invite->getFrom() << "\r\n"
	   << invite->getTo() << "\r\n"
	   << invite->getCallID() << "\r\n"
	   << "CSeq: " << cseqNum << " CANCEL\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";
	return std::make_shared<SipMessage>(ss.str(), target->getAddress());
}

// Issue #37: best-effort BYE to an endpoint that answered a page too late (after
// another endpoint already won), so it doesn't sit off-hook waiting for an ACK.
std::shared_ptr<SipMessage> RequestsHandler::buildPagingBye(const std::shared_ptr<SipMessage>& ok,
	const std::shared_ptr<SipClient>& answerer)
{
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &answerer->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string ruri = "sip:" + answerer->getNumber() + "@" + ipBuf + ":"
	                 + std::to_string(ntohs(answerer->getAddress().sin_port));

	std::ostringstream ss;
	ss << "BYE " << ruri << " SIP/2.0\r\n"
	   << ok->getVia() << "\r\n"
	   << ok->getFrom() << "\r\n"
	   << ok->getTo() << "\r\n"
	   << ok->getCallID() << "\r\n"
	   << "CSeq: 2 BYE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";
	return std::make_shared<SipMessage>(ss.str(), answerer->getAddress());
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
	std::lock_guard<std::mutex> lock(_mutex);
	sweepExpired();
	std::vector<std::pair<std::string, std::string>> result;
	result.reserve(_clients.size());
	for (const auto& [number, client] : _clients)
	{
		const auto& addr = client->getAddress();
		char ipBuf[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
		std::string ipPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));
		result.emplace_back(number, ipPort);
	}
	return result;
}

std::vector<std::tuple<std::string, std::string, std::string, int>> RequestsHandler::getActiveSessions()
{
	std::lock_guard<std::mutex> lock(_mutex);
	std::vector<std::tuple<std::string, std::string, std::string, int>> result;
	result.reserve(_sessions.size());
	auto now = std::chrono::steady_clock::now();
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
		result.emplace_back(caller, callee, sessionStateToString(session->getState()), durationSec);
	}
	return result;
}

void RequestsHandler::forceDisconnect(const std::string& extension)
{
	std::lock_guard<std::mutex> lock(_mutex);
	std::cout << "Admin: force-disconnecting extension " << extension << '\n';
	_clients.erase(extension);
	// Also remove any sessions involving this extension
	for (auto it = _sessions.begin(); it != _sessions.end(); )
	{
		bool involved = false;
		if (it->second->getSrc() && it->second->getSrc()->getNumber() == extension)
			involved = true;
		if (it->second->getDest() && it->second->getDest()->getNumber() == extension)
			involved = true;
		if (involved)
			it = _sessions.erase(it);
		else
			++it;
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

size_t RequestsHandler::getClientCount()
{
	std::lock_guard<std::mutex> lock(_mutex);
	sweepExpired();
	return _clients.size();
}

size_t RequestsHandler::getSessionCount()
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _sessions.size();
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
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_outbox.clear();

		sweepExpired();

		for (auto& [number, client] : _clients)
		{
			if (now - client->getLastPingTime() >= KEEPALIVE_PING_INTERVAL)
			{
				client->setLastPingTime(now);
				auto ping = buildOptionsPing(client);
				_outbox.emplace_back(client->getAddress(), std::move(ping));
			}
		}

		localOutbox = std::move(_outbox);
		_outbox.clear();
	}

	for (auto& event : localOutbox)
	{
		_onHandled(event.first, std::move(event.second));
	}
}

std::optional<std::shared_ptr<SipClient>> RequestsHandler::findClientByAddress(const sockaddr_in& addr)
{
	for (auto& [num, client] : _clients)
	{
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

	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
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

	return std::make_shared<SipMessage>(ss.str(), client->getAddress());
}
