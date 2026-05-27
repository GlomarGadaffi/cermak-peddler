#include "Session.hpp"

Session::Session(std::string callID, std::shared_ptr<SipClient> src, uint32_t srcRtpPort) :
	_callID(std::move(callID)), _src(src), _state(State::Invited), _srcRtpPort(srcRtpPort), _destRtpPort(0)
{
}

void Session::setState(State state)
{
	if (state == _state)
		return;
	_state = state;
	if (state == State::Connected)
	{
		if (!_dest)
		{
			std::cerr << "Session::setState(Connected): destination not set for call " << _callID << '\n';
			return;
		}
		std::cout << "Session Created between " << _src->getNumber() << " and " << _dest->getNumber() << '\n';
	}
}

void Session::setDest(std::shared_ptr<SipClient> dest, uint32_t destRtpPort)
{
	_dest = dest;
	_destRtpPort = destRtpPort;
}

const std::string& Session::getCallID() const
{
	return _callID;
}

std::shared_ptr<SipClient> Session::getSrc() const
{
	return _src;
}

std::shared_ptr<SipClient> Session::getDest() const
{
	return _dest;
}

Session::State Session::getState() const
{
	return _state;
}
