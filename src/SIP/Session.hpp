#ifndef SESSION_HPP
#define SESSION_HPP

// Session.hpp: Issue #28 resolved.
#include <memory>
#include <chrono>

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


	Session(std::string callID, std::shared_ptr<SipClient> src);

	void setState(State state);
	void setDest(std::shared_ptr<SipClient> dest);

	const std::string& getCallID() const;
	std::shared_ptr<SipClient> getSrc() const;
	std::shared_ptr<SipClient> getDest() const;
	State getState() const;
	std::chrono::steady_clock::time_point getStartTime() const;

private:
	std::string _callID;
	std::shared_ptr<SipClient> _src;
	std::shared_ptr<SipClient> _dest;
	State _state;
	std::chrono::steady_clock::time_point _startTime;

};

#endif
