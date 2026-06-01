#ifndef SESSION_HPP
#define SESSION_HPP

// Session.hpp: Issue #28 resolved.
#include <memory>
#include <chrono>
#include <vector>

class SipMessage;
#include "SipClient.hpp"

class Session
{
public:

	enum class State
	{
		Invited,
		Busy,
		Unavailable,
		Cancel,
		Bye,
		Connected,
	};


	Session();
	Session(std::string callID, std::shared_ptr<SipClient> src);

	void reset(std::string callID, std::shared_ptr<SipClient> src);

	void setState(State state);
	void setDest(std::shared_ptr<SipClient> dest);

	const std::string& getCallID() const;
	std::shared_ptr<SipClient> getSrc() const;
	std::shared_ptr<SipClient> getDest() const;
	State getState() const;
	std::chrono::steady_clock::time_point getStartTime() const;

	// Broadcast / Forking helpers
	bool isBroadcast() const { return _isBroadcast; }
	void setBroadcast(bool val) { _isBroadcast = val; }

	const std::vector<std::shared_ptr<SipClient>>& getPendingTargets() const { return _pendingTargets; }
	void setPendingTargets(std::vector<std::shared_ptr<SipClient>> targets) { _pendingTargets = std::move(targets); }
	void removePendingTarget(const std::string& number);

	std::shared_ptr<SipMessage> getInviteMessage() const { return _inviteMessage; }
	void setInviteMessage(std::shared_ptr<SipMessage> msg) { _inviteMessage = msg; }

private:
	std::string _callID;
	std::shared_ptr<SipClient> _src;
	std::shared_ptr<SipClient> _dest;
	State _state;
	std::chrono::steady_clock::time_point _startTime;

	bool _isBroadcast = false;
	std::vector<std::shared_ptr<SipClient>> _pendingTargets;
	std::shared_ptr<SipMessage> _inviteMessage;

};

#endif
