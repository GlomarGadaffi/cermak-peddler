// Session.cpp: Issue #28 resolved.
#include "Session.hpp"

Session::Session(std::string callID, std::shared_ptr<SipClient> src) :
	_callID(std::move(callID)), _src(src), _state(State::Invited),
	_startTime(std::chrono::steady_clock::now())
{
}

void Session::setState(State state)
{
	if (state == _state)
		return;
	_state = state;
	if (state == State::Connected)
	{
		_startTime = std::chrono::steady_clock::now(); // Reset start time to when talk time begins
		if (!_dest)
		{
			std::cerr << "Session::setState(Connected): destination not set for call " << _callID << '\n';
			return;
		}
		std::cout << "Session Created between " << _src->getNumber() << " and " << _dest->getNumber() << '\n';
	}
}

void Session::setDest(std::shared_ptr<SipClient> dest)
{
	_dest = dest;
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

std::chrono::steady_clock::time_point Session::getStartTime() const
{
	return _startTime;
}

// ── Broadcast / all-page support (Issue #37) ─────────────────────────────────
void Session::setPaging(bool paging)
{
	_isPaging = paging;
}

bool Session::isPaging() const
{
	return _isPaging;
}

void Session::addPagedTarget(std::shared_ptr<SipClient> target)
{
	_pagedTargets.push_back(std::move(target));
}

const std::vector<std::shared_ptr<SipClient>>& Session::getPagedTargets() const
{
	return _pagedTargets;
}

void Session::setPagingInvite(std::shared_ptr<SipMessage> invite)
{
	_pagingInvite = std::move(invite);
}

std::shared_ptr<SipMessage> Session::getPagingInvite() const
{
	return _pagingInvite;
}
