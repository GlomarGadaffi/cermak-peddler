#ifndef REQUESTS_HANDLER_HPP
#define REQUESTS_HANDLER_HPP

#include <functional>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <vector>
#include <tuple>
#include <cstdint>
#include "SipMessage.hpp"
#include "SipClient.hpp"
#include "Session.hpp"

class RequestsHandler
{
public:

	using OnHandledEvent = std::function<void(const sockaddr_in&, std::shared_ptr<SipMessage>)>;

	RequestsHandler(std::string serverIp, int serverPort,
		OnHandledEvent onHandledEvent);

	void handle(std::shared_ptr<SipMessage> request);

	std::optional<std::shared_ptr<Session>> getSession(const std::string& callID);

	// ── Dashboard query API (thread-safe) ────────────────────────────
	std::vector<std::pair<std::string, std::string>> getActiveClients();
	std::vector<std::tuple<std::string, std::string, std::string>> getActiveSessions();
	void forceDisconnect(const std::string& extension);
	uint64_t getPacketsProcessed() const;
	size_t getClientCount();
	size_t getSessionCount();

private:
	void initHandlers();

	// SIP request handlers (camelCase to match C++ convention)
	void onRegister(std::shared_ptr<SipMessage> data);
	void onCancel(std::shared_ptr<SipMessage> data);
	void onReqTerminated(std::shared_ptr<SipMessage> data);
	void onInvite(std::shared_ptr<SipMessage> data);
	void onTrying(std::shared_ptr<SipMessage> data);
	void onRinging(std::shared_ptr<SipMessage> data);
	void onBusy(std::shared_ptr<SipMessage> data);
	void onUnavailable(std::shared_ptr<SipMessage> data);
	void onBye(std::shared_ptr<SipMessage> data);
	void onOk(std::shared_ptr<SipMessage> data);
	void onAck(std::shared_ptr<SipMessage> data);

	bool setCallState(const std::string& callID, Session::State state);
	void endCall(const std::string& callID, const std::string& srcNumber, const std::string& destNumber, const std::string& reason = "");

	bool registerClient(std::shared_ptr<SipClient> client);
	void unregisterClient(const std::string& number);

	std::optional<std::shared_ptr<SipClient>> findClient(const std::string& number);

	void endHandle(const std::string& destNumber, std::shared_ptr<SipMessage> message);
	std::string buildContact(const std::string& number) const;

	std::unordered_map<std::string, std::function<void(std::shared_ptr<SipMessage> request)>> _handlers;
	std::unordered_map<std::string, std::shared_ptr<Session>>   _sessions;
	std::unordered_map<std::string, std::shared_ptr<SipClient>> _clients;

	std::mutex _mutex;
	OnHandledEvent _onHandled;

	std::string _serverIp;
	int         _serverPort;

	std::atomic<uint64_t> _packetsProcessed{0};
};

#endif
