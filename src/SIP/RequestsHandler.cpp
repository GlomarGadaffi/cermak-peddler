#include "RequestsHandler.hpp"
#include <atomic>
#include <sstream>
#include <cctype>
#include <algorithm>
#include "SipMessageTypes.h"
#include "SipSdpMessage.hpp"
#include "IDGen.hpp"
#include "IPHelper.hpp"

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
}

RequestsHandler::RequestsHandler(std::string serverIp, int serverPort,
	OnHandledEvent onHandledEvent) :
	_serverIp(std::move(serverIp)), _serverPort(serverPort),
	_onHandled(onHandledEvent)
{
	initHandlers();
}

void RequestsHandler::initHandlers()
{
	_handlers.emplace(SipMessageTypes::REGISTER,          std::bind(&RequestsHandler::onRegister,       this, std::placeholders::_1));
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
	std::lock_guard<std::mutex> lock(_mutex);
	maybeSweep();
	auto it = _handlers.find(request->getType());
	if (it != _handlers.end())
	{
		it->second(std::move(request));
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
		grantedExpires = std::max(MIN_EXPIRES, std::min(requestedExpires, MAX_EXPIRES));
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

	auto message = dynamic_cast<SipSdpMessage*>(data.get());
	if (!message) 
	{
		std::cerr << "Couldn't get SDP from " << data->getFromNumber() << "'s INVITE request." << std::endl;
		std::shared_ptr<SipMessage> responseObj = std::make_shared<SipMessage>(*data);
		responseObj->setHeader(SipMessageTypes::BAD_REQUEST);
		responseObj->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	auto newSession = std::make_shared<Session>(data->getCallID(), caller.value(), message->getRtpPort());
	_sessions.emplace(data->getCallID(), newSession);

	auto response = std::make_shared<SipMessage>(*data);
	response->setContact(buildContact(caller.value()->getNumber()));
	endHandle(data->getToNumber(), response);
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
	setCallState(data->getCallID(), Session::State::Bye);
	endHandle(data->getToNumber(), data);
}

void RequestsHandler::onOk(std::shared_ptr<SipMessage> data)
{
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
			auto client = findClient(data->getToNumber());
			if (!client.has_value())
			{
				return;
			}

			auto sdpMessage = dynamic_cast<SipSdpMessage*>(data.get());
			if (!sdpMessage) 
			{
				std::cerr << "Couldn't get SDP from: " << client.value()->getNumber() << "'s OK message.\n";
				std::shared_ptr<SipMessage> responseObj = std::make_shared<SipMessage>(*data);
				responseObj->setHeader(SipMessageTypes::BAD_REQUEST);
				responseObj->setContact(buildContact(data->getToNumber()));
				endHandle(data->getToNumber(), responseObj);
				endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), "SDP parse error.");
				return;
			}
			session->get()->setDest(client.value(), sdpMessage->getRtpPort());
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

void RequestsHandler::onAck(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (!session.has_value())
	{
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

	// 2. Standalone Expires header (case-insensitive line match).
	const std::string raw = data->toString();
	std::string lowered(raw.size(), '\0');
	std::transform(raw.begin(), raw.end(), lowered.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	size_t hpos = lowered.find("\nexpires:");
	if (hpos != std::string::npos)
	{
		int v = readInt(raw, hpos + 9); // offset past "\nexpires:"
		if (v >= 0) return v;
	}

	// 3. No expiry specified — grant the default lease.
	return DEFAULT_EXPIRES;
}

void RequestsHandler::sweepExpired()
{
	auto now = std::chrono::steady_clock::now();
	for (auto it = _clients.begin(); it != _clients.end(); )
	{
		if (it->second->isExpired(now))
		{
			std::cout << "Registration lease expired: " << it->first << '\n';
			it = _clients.erase(it);
		}
		else
		{
			++it;
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
		_onHandled(destClient.value()->getAddress(), std::move(message));
	}
	else
	{
		// Clone the message so we don't mutate a shared object's header
		auto notFound = std::make_shared<SipMessage>(*message);
		notFound->setHeader(SipMessageTypes::NOT_FOUND);
		auto src = message->getSource();
		_onHandled(src, std::move(notFound));
	}
}

std::string RequestsHandler::buildContact(const std::string& number) const
{
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	return "Contact: <sip:" + number + "@" + activeIp + ":" + std::to_string(_serverPort) + ";transport=UDP>";
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
		std::string ipPort = std::string(inet_ntoa(addr.sin_addr)) + ":" + std::to_string(ntohs(addr.sin_port));
		result.emplace_back(number, ipPort);
	}
	return result;
}

std::vector<std::tuple<std::string, std::string, std::string>> RequestsHandler::getActiveSessions()
{
	std::lock_guard<std::mutex> lock(_mutex);
	std::vector<std::tuple<std::string, std::string, std::string>> result;
	result.reserve(_sessions.size());
	for (const auto& [callID, session] : _sessions)
	{
		std::string caller = session->getSrc() ? session->getSrc()->getNumber() : "?";
		std::string callee = session->getDest() ? session->getDest()->getNumber() : "?";
		result.emplace_back(caller, callee, sessionStateToString(session->getState()));
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
