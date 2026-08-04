#ifndef REGISTER_BEEPER_HPP
#define REGISTER_BEEPER_HPP

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "PbxEnv.hpp"
#include "PoolConfig.hpp"
#include "SipClient.hpp"
#include "SipMessage.hpp"

// ── Register beep (signaling-only intercom tone) ──────────────────────────────
// On a NEW registration, send the registering phone a brief auto-answer INVITE
// so it plays its own intercom tone (the "beep"), then tear the call straight
// back down. NO RTP is ever sourced — the tone is the phone's local intercom
// alert. Each outbound UAC dialog lives in a small bounded ring keyed by
// Call-ID so handleOk() can drive ACK→BYE and sweep() can time it out / CANCEL
// it.
//
// State machine (per beep dialog):
//   sendBeep()          : allocate a slot, send INVITE (auto-answer headers), arm
//                         a deadline; if no slot free, skip the beep (it's cosmetic).
//   handleOk() INVITE 200 : send ACK, then BYE, advance to AwaitingByeOk.
//   handleOk() BYE 200    : free the slot.
//   sweep() deadline    : if still AwaitingInviteOk, CANCEL and free; if
//                         AwaitingByeOk, just free (best-effort BYE already sent).
//
// Locking: every method assumes the caller holds the engine's _mutex
// (non-recursive), matching the RequestsHandler code this was extracted from.
class RegisterBeeper
{
public:
	explicit RegisterBeeper(PbxEnv& env) : _env(env) {}

	// Kick off a beep dialog toward a freshly registered phone. Bounded and
	// best-effort — if the table is full the beep is simply skipped.
	void sendBeep(const std::shared_ptr<SipClient>& phone);

	// Route a 200 OK that belongs to a beep dialog (matched by Call-ID). Returns
	// true if the response was consumed (the caller must not process it further).
	bool handleOk(const std::shared_ptr<SipMessage>& data);

	// Time out overdue dialogs: CANCEL an unanswered INVITE, free the slot.
	void sweep(std::chrono::steady_clock::time_point now);

private:
	// AwaitingCancelDone: CANCEL sent, lingering until the 487 final response is
	// ACKed (or a bounded deadline frees the slot). Added for #90.
	enum class BeepState { Free, AwaitingInviteOk, AwaitingByeOk, AwaitingCancelDone };
	struct BeepDialog
	{
		BeepState state = BeepState::Free;
		std::string callID;        // fresh per beep; how handleOk()/sweep() find this slot
		std::string branch;        // Via branch (reused for INVITE/CANCEL)
		std::string fromTag;       // our (server) From tag
		std::string ext;           // target extension (phone number)
		sockaddr_in addr{};        // phone's contact address
		std::chrono::steady_clock::time_point deadline{};
	};

	BeepDialog* findByCallID(std::string_view callID);
	// buildAck/buildBye take the dialog handleOk() already located — they used to
	// re-scan the table by Call-ID themselves, three linear passes over the same
	// Call-ID per answered beep, all inside the _mutex-held 200-OK dispatch.
	std::shared_ptr<SipMessage> buildAck(const BeepDialog& bd, const std::shared_ptr<SipMessage>& ok);
	std::shared_ptr<SipMessage> buildBye(const BeepDialog& bd, const std::shared_ptr<SipMessage>& ok);
	std::shared_ptr<SipMessage> buildCancel(std::size_t slot);

	// Bounded outbound-UAC dialog table. Tiny fixed footprint; if all slots are
	// busy a new registration just skips its beep.
	std::array<BeepDialog, POCKETDIAL_MAX_BEEPS> _dialogs;
	PbxEnv& _env;
};

#endif
