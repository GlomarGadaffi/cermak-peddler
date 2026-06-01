#ifndef SESSION_HPP
#define SESSION_HPP

// Session.hpp: Issue #28 resolved.
#include <memory>
#include <chrono>
#include <vector>

#include "SipClient.hpp"
#include "SipMessage.hpp"

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

	// ── Broadcast / all-page support (Issue #37) ─────────────────────
	// A paging session forks one INVITE to every registered endpoint; the
	// first 200 OK wins and the rest are CANCELled. The answerer can't be
	// identified by the To-number (it is the virtual page extension), so we
	// retain the target list and the original INVITE for cancellation.
	void setPaging(bool paging);
	bool isPaging() const;
	void addPagedTarget(std::shared_ptr<SipClient> target);
	const std::vector<std::shared_ptr<SipClient>>& getPagedTargets() const;
	void setPagingInvite(std::shared_ptr<SipMessage> invite);
	std::shared_ptr<SipMessage> getPagingInvite() const;

private:
	std::string _callID;
	std::shared_ptr<SipClient> _src;
	std::shared_ptr<SipClient> _dest;
	State _state;
	std::chrono::steady_clock::time_point _startTime;

	bool _isPaging = false;
	std::vector<std::shared_ptr<SipClient>> _pagedTargets;
	std::shared_ptr<SipMessage> _pagingInvite;
};

#endif
