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
	_onHandled(onHandledEvent),
	_serverIp(std::move(serverIp)),
	_serverPort(serverPort)
{
	initHandlers();
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
	std::string destNumber = data->getToNumber();
	if (destNumber == "777")
	{
		endCall(data->getCallID(), data->getFromNumber(), "777");
		return;
	}

	if (destNumber == "999")
	{
		auto session = getSession(data->getCallID());
		if (session.has_value())
		{
			std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
			std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);
			std::string originalCSeq = data->getCSeq();
			size_t invitePos = originalCSeq.find("INVITE");
			if (invitePos != std::string::npos)
			{
				originalCSeq.replace(invitePos, 6, "CANCEL");
			}

			for (const auto& target : session.value()->getPendingTargets())
			{
				auto cancelMsg = std::make_shared<SipMessage>(*data);
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
		endCall(data->getCallID(), data->getFromNumber(), "999");
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
	// Check if the caller is registered
	auto caller = findClient(data->getFromNumber());
	if (!caller.has_value())
	{
		return;
	}

	std::string destNumber = data->getToNumber();
	if (destNumber == "777")
	{
		// SDP loopback echo test
		auto ringing = std::make_shared<SipMessage>(*data);
		ringing->setHeader("SIP/2.0 180 Ringing");
		ringing->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		ringing->setVia(data->getVia() + ";received=" + activeIp);
		std::string toTag = IDGen::GenerateID(9);
		ringing->setTo(data->getTo() + ";tag=" + toTag);
		ringing->setContact(buildContact("777"));
		_outbox.emplace_back(data->getSource(), std::move(ringing));

		auto okResponse = std::make_shared<SipMessage>(*data);
		okResponse->setHeader(SipMessageTypes::OK);
		okResponse->setVia(data->getVia() + ";received=" + activeIp);
		okResponse->setTo(data->getTo() + ";tag=" + toTag);
		okResponse->setContact(buildContact("777"));
		okResponse->enforceG711();
		_outbox.emplace_back(data->getSource(), std::move(okResponse));

		auto dummyClient = std::make_shared<SipClient>("777", data->getSource(), 3600);
		auto newSession = std::make_shared<Session>(data->getCallID(), caller.value());
		newSession->setDest(dummyClient);
		_sessions.emplace(data->getCallID(), newSession);
		newSession->setState(Session::State::Connected);
		return;
	}

	if (destNumber == "999")
	{
		// All Page / Broadcast
		std::vector<std::shared_ptr<SipClient>> targets;
		for (const auto& [num, client] : _clients)
		{
			if (num != caller.value()->getNumber())
			{
				targets.push_back(client);
			}
		}

		if (targets.empty())
		{
			std::shared_ptr<SipMessage> responseObj = std::make_shared<SipMessage>(*data);
			responseObj->setHeader(SipMessageTypes::NOT_FOUND);
			responseObj->setContact(buildContact(caller.value()->getNumber()));
			_outbox.emplace_back(data->getSource(), std::move(responseObj));
			return;
		}

		auto newSession = std::make_shared<Session>(data->getCallID(), caller.value());
		newSession->setBroadcast(true);
		newSession->setPendingTargets(targets);
		newSession->setInviteMessage(data);
		_sessions.emplace(data->getCallID(), newSession);

		auto ringing = std::make_shared<SipMessage>(*data);
		ringing->setHeader("SIP/2.0 180 Ringing");
		ringing->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		ringing->setVia(data->getVia() + ";received=" + activeIp);
		ringing->setTo(data->getTo() + ";tag=" + IDGen::GenerateID(9));
		ringing->setContact(buildContact("999"));
		_outbox.emplace_back(data->getSource(), std::move(ringing));

		for (auto& target : targets)
		{
			auto inviteFork = std::make_shared<SipMessage>(*data);
			inviteFork->setContact(buildContact(caller.value()->getNumber()));

			char ipBuf[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &target->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
			std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(target->getAddress().sin_port));
			std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

			inviteFork->setHeader("INVITE sip:" + target->getNumber() + "@" + targetIpPort + " SIP/2.0");
			inviteFork->setTo("To: <sip:" + target->getNumber() + "@" + serverIpPort + ">");

			inviteFork->addHeader("Call-Info", "<sip:any>;answer-after=0");
			inviteFork->addHeader("Alert-Info", "info=alert-autoanswer");
			inviteFork->addHeader("Alert-Info", "answer-after=0");
			inviteFork->addHeader("Alert-Info", "intercom=true");
			inviteFork->addHeader("P-Auto-Answer", "normal");
			inviteFork->enforceG711();
			_outbox.emplace_back(target->getAddress(), std::move(inviteFork));
		}
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

	SipSdpMessage* message = nullptr;
	if (data && data->hasSdp())
	{
		message = static_cast<SipSdpMessage*>(data.get());
	}
	if (!message) 
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
		session.value()->removePendingTarget(data->getFromNumber());
		if (session.value()->getPendingTargets().empty() && session.value()->getState() == Session::State::Invited)
		{
			endHandle(session.value()->getSrc()->getNumber(), data);
			endCall(data->getCallID(), session.value()->getSrc()->getNumber(), "999", "all targets busy");
		}
		return;
	}
	setCallState(data->getCallID(), Session::State::Busy);
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onUnavailable(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		session.value()->removePendingTarget(data->getFromNumber());
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
	std::string destNumber = data->getToNumber();
	if (destNumber == "777")
	{
		auto response = std::make_shared<SipMessage>(*data);
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(data->getVia() + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (destNumber == "999")
	{
		auto response = std::make_shared<SipMessage>(*data);
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(data->getVia() + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));

		if (session.has_value())
		{
			auto answeringClient = session.value()->getDest();
			if (answeringClient)
			{
				auto byeFork = std::make_shared<SipMessage>(*data);
				std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

				char ipBuf[INET_ADDRSTRLEN]{};
				inet_ntop(AF_INET, &answeringClient->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
				std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(answeringClient->getAddress().sin_port));

				byeFork->setHeader("BYE sip:" + answeringClient->getNumber() + "@" + targetIpPort + " SIP/2.0");

				std::string originalTo = data->getTo();
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
			if (session.value()->isBroadcast())
			{
				if (session.value()->getState() != Session::State::Connected)
				{
					auto clientOpt = findClientByAddress(data->getSource());
					if (!clientOpt.has_value())
					{
						return;
					}
					auto answeringClient = clientOpt.value();

					SipSdpMessage* sdpMessage = nullptr;
					if (data && data->hasSdp())
					{
						sdpMessage = static_cast<SipSdpMessage*>(data.get());
					}
					if (!sdpMessage)
					{
						std::cerr << "Couldn't get SDP from: " << answeringClient->getNumber() << "'s broadcast OK message.\n";
						return;
					}

					session->get()->setDest(answeringClient);
					session->get()->setState(Session::State::Connected);

					auto inviteMsg = session.value()->getInviteMessage();

					auto response = std::make_shared<SipMessage>(*data);
					response->setContact(buildContact(answeringClient->getNumber()));

					if (inviteMsg)
					{
						std::string originalTo = inviteMsg->getTo();
						std::string bTo = data->getTo();
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
						std::string originalCSeq = inviteMsg->getCSeq();
						size_t invitePos = originalCSeq.find("INVITE");
						if (invitePos != std::string::npos)
						{
							originalCSeq.replace(invitePos, 6, "CANCEL");
						}

						for (const auto& target : session.value()->getPendingTargets())
						{
							if (target->getNumber() != answeringClient->getNumber())
							{
								auto cancelMsg = std::make_shared<SipMessage>(*inviteMsg);
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
			if (data && data->hasSdp())
			{
				sdpMessage = static_cast<SipSdpMessage*>(data.get());
			}
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

void RequestsHandler::onAck(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (!session.has_value())
	{
		return;
	}

	std::string destNumber = data->getToNumber();
	if (destNumber == "777")
	{
		return;
	}

	if (destNumber == "999")
	{
		auto answeringClient = session.value()->getDest();
		if (answeringClient)
		{
			auto ackFork = std::make_shared<SipMessage>(*data);
			std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
			std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

			char ipBuf[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &answeringClient->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
			std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(answeringClient->getAddress().sin_port));

			ackFork->setHeader("ACK sip:" + answeringClient->getNumber() + "@" + targetIpPort + " SIP/2.0");

			std::string originalTo = data->getTo();
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
		int val = 0;
		for (size_t i = from; i < end; ++i)
		{
			if (val > 200000000) return 200000000;
			val = val * 10 + (s[i] - '0');
		}
		return val;
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
	for (auto it = _clients.begin(); it != _clients.end(); )
	{
		bool keepAliveTimedOut = (now - it->second->getLastActiveTime() > std::chrono::seconds(15));
		bool leaseExpired = it->second->isExpired(now);

		if (keepAliveTimedOut || leaseExpired)
		{
			if (keepAliveTimedOut)
			{
				std::cout << "Pruning client due to missed OPTIONS keepalive pings: " << it->first << '\n';
			}
			else
			{
				std::cout << "Registration lease expired: " << it->first << '\n';
			}

			// Clean up sessions involving this client
			std::string extension = it->first;
			for (auto sit = _sessions.begin(); sit != _sessions.end(); )
			{
				bool involved = false;
				if (sit->second->getSrc() && sit->second->getSrc()->getNumber() == extension)
					involved = true;
				if (sit->second->getDest() && sit->second->getDest()->getNumber() == extension)
					involved = true;
				if (involved)
					sit = _sessions.erase(sit);
				else
					++sit;
			}

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
			if (now - client->getLastPingTime() >= std::chrono::seconds(5))
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
