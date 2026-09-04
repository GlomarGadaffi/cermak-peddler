#ifndef CALL_PICKUP_HPP
#define CALL_PICKUP_HPP

#include <memory>
#include <string>

#include "CallForker.hpp"
#include "PbxEnv.hpp"

class SipMessage;
class SipClient;
class Session;

// ── Directed / group call pickup completion (Issue #68), extracted out of
// RequestsHandler ───────────────────────────────────────────────────────────
// The session-table scan that FINDS the oldest ringing session among a set of
// candidate extensions (isSessionRingingExt/findRingingSessionAmong) stays in
// the engine — it's a raw scan over the private session table, the same
// "highest session coupling" reason the plan calls out for keeping it there,
// matching the Step 5 precedent of keeping the 999 target-selection loop in
// onInvite rather than growing PbxEnv with a mutable all-sessions visitor.
// CallPickup takes the ALREADY-RESOLVED result (a nullable Session handle
// plus its Call-ID/extension) and does everything after that: the 486
// rejection (no eligible ringing session, or missing SDP on either leg), the
// two 200 OKs that bridge the caller's and picker's independent dialogs, and
// cancelling every other still-ringing fork of the picked-up call. Caller
// (onInvite, via the two pickup branches) holds the engine's _mutex.
class CallPickup
{
public:
	CallPickup(PbxEnv& env, CallForker& forker) : _env(env), _forker(forker) {}

	// `ringing` is the session findRingingSessionAmong resolved (or nullptr if
	// nothing was eligible — that case still 486s from in here, matching the
	// original onPickup's "acceptance criterion: 486, never a hang").
	// `ringingCallId`/`ringingExt` are only meaningful when `ringing` is
	// non-null. Caller holds _mutex.
	void complete(const std::shared_ptr<SipMessage>& data,
		const std::shared_ptr<SipClient>& picker,
		const std::shared_ptr<Session>& ringing,
		const std::string& ringingCallId,
		const std::string& ringingExt);

private:
	PbxEnv& _env;
	CallForker& _forker;
};

#endif
