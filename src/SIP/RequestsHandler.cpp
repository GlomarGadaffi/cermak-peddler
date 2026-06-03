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

std::vector<std::shared_ptr<SipMessage>> RequestsHandler::_messagePool;

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
	// Pre-allocate pools (Issue #53)
	for (int i = 0; i < 32; ++i)
	{
		_clientPool.push_back(std::make_shared<SipClient>());
	}
	for (int i = 0; i < 8; ++i)
	{
		_sessionPool.push_back(std::make_shared<Session>());
	}
	if (_messagePool.empty())
	{
		for (int i = 0; i < 32; ++i)
		{
			_messagePool.push_back(std::make_shared<SipSdpMessage>("", sockaddr_in{}));
		}
	}
	_dummyClient = std::make_shared<SipClient>();
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
}

void RequestsHandler::handle(std::shared_ptr<SipMessage> request)
{
	// Input validation: Drop null or structurally malformed packets instantly (SEC-02)
	if (!request || !request->isValidMessage())
	{
		_packetsDropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> localOutbox;
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		// Per-source IP Rate Limiting (Issue #38 / SEC-02)
		if (!ipAllowed(request->getSource()) || !allowPacket(request->getSource()))
		{
			_packetsDropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		_packetsProcessed.fetch_add(1, std::memory_order_relaxed);
		_outbox.clear();

		auto client = findClientByAddress(request->getSource());
		if (client.has_value())
		{
			client.value()->markActive();
		}

		maybeSweep();
		auto it = _handlers.find(std::string(request->getType()));
		if (it != _handlers.end())
		{
			it->second(std::move(request));
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
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

#ifndef POCKETDIAL_OPEN_REGISTRAR
	// Closed mode: reject registrations as unauthenticated/forbidden since we don't have registered credentials.
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader("SIP/2.0 403 Forbidden");
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}
#endif

	if (requestedExpires <= 0)
	{
		// expires=0 (or an explicit zero) is a de-registration request.
		unregisterClient(fromNumber);
	}
	else
	{
		grantedExpires = (std::max)(MIN_EXPIRES, (std::min)(requestedExpires, MAX_EXPIRES));
		// Always update address so re-REGISTER after a NAT rebind works correctly
		auto newClient = allocateClient(std::string(data->getFromNumber()), data->getSource(), grantedExpires);
		if (newClient)
		{
			registerClient(newClient);
		}
		else
		{
			// Server full: reply with 503 Service Unavailable
			auto response = getMessageFromPool(data->toString(), data->getSource());
			response->setHeader("SIP/2.0 503 Service Unavailable");
			response->clearBody();
			std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
			response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
			_outbox.emplace_back(data->getSource(), std::move(response));
			return;
		}
	}

	auto response = getMessageFromPool(data->toString(), data->getSource());
	response->setHeader(SipMessageTypes::OK);
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
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
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
	response->setContact(buildContact(data->getFromNumber()));
	_outbox.emplace_back(data->getSource(), std::move(response));
}

void RequestsHandler::onCancel(std::shared_ptr<SipMessage> data)
{
	std::string destNumber(data->getToNumber());
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
	if (!isValidAor(data->getFromNumber()) || !isValidAor(data->getToNumber()))
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader("SIP/2.0 400 Bad Request");
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
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
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
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
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
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

		_dummyClient->reset("777", data->getSource(), 3600);
		auto newSession = allocateSession(std::string(data->getCallID()), caller.value());
		if (newSession)
		{
			newSession->setDest(_dummyClient);
			_sessions.emplace(data->getCallID(), newSession);
			newSession->setState(Session::State::Connected);
		}
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
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
			responseObj->setHeader(SipMessageTypes::NOT_FOUND);
			responseObj->clearBody();
			responseObj->setContact(buildContact(caller.value()->getNumber()));
			_outbox.emplace_back(data->getSource(), std::move(responseObj));
			return;
		}

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
		newSession->setPendingTargets(targets);
		newSession->setInviteMessage(data);
		_sessions.emplace(data->getCallID(), newSession);

		auto ringing = getMessageFromPool(data->toString(), data->getSource());
		ringing->setHeader("SIP/2.0 180 Ringing");
		ringing->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		ringing->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		ringing->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
		ringing->setContact(buildContact("999"));
		_outbox.emplace_back(data->getSource(), std::move(ringing));

		for (auto& target : targets)
		{
			auto inviteFork = getMessageFromPool(data->toString(), data->getSource());
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

	auto response = getMessageFromPool(data->toString(), data->getSource());
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
		session.value()->removePendingTarget(std::string(data->getFromNumber()));
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
	if (destNumber == "777")
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (destNumber == "999")
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
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
						queueLog("Couldn't get SDP from: " + answeringClient->getNumber() + "'s broadcast OK message.", true);
						return;
					}

					session->get()->setDest(answeringClient);
					session->get()->setState(Session::State::Connected);

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
			if (data && data->hasSdp())
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
			std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
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
	if (_sessions.erase(std::string(callID)) > 0)
	{
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
}

bool RequestsHandler::registerClient(std::shared_ptr<SipClient> client)
{
	// Always update the entry so a re-REGISTER after a NAT rebind refreshes the address
	queueLog((_clients.count(client->getNumber()) ? "Re-registered: " : "New Client: ") + client->getNumber());
	_clients[client->getNumber()] = client;
	return true;
}

void RequestsHandler::unregisterClient(std::string_view number)
{
	queueLog("Unregistered client: " + std::string(number));
	_clients.erase(std::string(number));
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
	for (auto it = _clients.begin(); it != _clients.end(); )
	{
		bool keepAliveTimedOut = (now - it->second->getLastActiveTime() > std::chrono::seconds(15));
		bool leaseExpired = it->second->isExpired(now);

		if (keepAliveTimedOut || leaseExpired)
		{
			if (keepAliveTimedOut)
			{
				queueLog("Pruning client due to missed OPTIONS keepalive pings: " + it->first);
			}
			else
			{
				queueLog("Registration lease expired: " + it->first);
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
				{
					std::string callID = sit->first;
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

std::optional<std::shared_ptr<SipClient>> RequestsHandler::findClient(std::string_view number)
{
	auto it = _clients.find(std::string(number));
	if (it != _clients.end())
	{
		return it->second;
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
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
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
			{
				std::string callID = it->first;
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

		// Sweep rate-limit buckets older than 60 seconds (Issue #58)
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

		for (auto& [number, client] : _clients)
		{
			if (now - client->getLastPingTime() >= std::chrono::seconds(5))
			{
				client->setLastPingTime(now);
				auto ping = buildOptionsPing(client);
				_outbox.emplace_back(client->getAddress(), std::move(ping));
			}
		}

		// Build snapshot under registrar mutex lock, then save it under snapshot mutex lock
		RegistrarSnapshot nextSnapshot;
		nextSnapshot.packetsProcessed = _packetsProcessed.load(std::memory_order_relaxed);
		nextSnapshot.packetsDropped = _packetsDropped.load(std::memory_order_relaxed);
		nextSnapshot.clients.reserve(_clients.size());
		for (const auto& [number, client] : _clients)
		{
			const auto& addr = client->getAddress();
			char ipBuf[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
			std::string ipPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));
			nextSnapshot.clients.emplace_back(number, ipPort);
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

		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot = std::move(nextSnapshot);
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

	return getMessageFromPool(ss.str(), client->getAddress());
}

std::shared_ptr<SipClient> RequestsHandler::allocateClient(std::string number, sockaddr_in address, int expiresSeconds)
{
	// Search if already exists in _clients
	auto it = _clients.find(number);
	if (it != _clients.end())
	{
		it->second->reset(std::move(number), address, expiresSeconds);
		return it->second;
	}

	// Otherwise, find an unused slot in _clientPool
	for (auto& client : _clientPool)
	{
		if (client->getNumber().empty())
		{
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
	bucket.tokens = std::min(40.0, bucket.tokens + elapsedSec * 20.0);

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
		if (!std::isalnum(static_cast<unsigned char>(c)) &&
			c != '.' && c != '-' && c != '_' && c != '+')
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
