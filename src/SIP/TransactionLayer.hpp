#ifndef TRANSACTION_LAYER_HPP
#define TRANSACTION_LAYER_HPP

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

#include "PbxEnv.hpp"
#include "PoolConfig.hpp"
#include "SipMessage.hpp"

// ── RFC 3261 §17 INVITE client transaction layer ──────────────────────────────
// One slot per outgoing INVITE fork. Retransmit interval (Timer A) doubles from
// T1=500 ms each tick until a provisional response advances the state to
// Proceeding (no more retransmits) or Timer B (32 s) fires. Pool exhaustion →
// message sent once with no retransmit (graceful degradation).
//
// Locking: every method assumes the caller holds the engine's _mutex, matching
// the convention of the RequestsHandler code this was extracted from.
class TransactionLayer
{
public:
	explicit TransactionLayer(PbxEnv& env) : _env(env) {}

	// Track `msg` for Timer A/B retransmit if it is an INVITE request headed to
	// `peer`; responses and non-INVITE requests are ignored. Called for each
	// outbox entry just before a handle()/tick() pass flushes it.
	void maybeTrack(const sockaddr_in& peer, const std::shared_ptr<SipMessage>& msg);

	// Advance the state machine for any tracked transaction matching this
	// response's Via branch + CSeq method. A 1xx moves Calling → Proceeding
	// (stops retransmitting); a 2xx → Accepted, 3xx-6xx → Completed (both arm
	// the RFC 6026 Timer L absorb window). Returns true if a slot matched.
	bool matchAndAdvance(const std::shared_ptr<SipMessage>& msg);

	// Retransmit timed-out INVITE forks and free completed/absorbed slots.
	void sweep(std::chrono::steady_clock::time_point now);

	// Free every slot tracking retransmits for a call being torn down.
	void freeForCallId(std::string_view callId);

private:
	struct SipTransaction
	{
		enum class Type  : uint8_t { None, InviteClient };
		enum class State : uint8_t { Calling, Proceeding, Completed, Accepted };

		Type  type  = Type::None;
		State state = State::Calling;

		sockaddr_in peer{};
		char msg[1500]{};       // serialized bytes ready for retransmit (Ethernet MTU safe)
		size_t msgLen       = 0;
		bool   msgTruncated = false;

		char callId[128]{};    // Call-ID for freeForCallId() lifecycle linkage
		char viaBranch[72]{};  // z9hG4bK… branch param (primary matching key)
		char cseqMethod[12]{}; // "INVITE" etc. — disambiguates CANCEL sharing the branch

		std::chrono::steady_clock::time_point nextRetransmit{};     // next Timer A fire
		std::chrono::steady_clock::time_point transactionTimeout{}; // Timer B (32 s)
		std::chrono::steady_clock::time_point absorbDeadline{};     // Timer L (RFC 6026, 2xx)

		uint32_t retransmitCount   = 0;
		uint32_t currentIntervalMs = 500; // Timer A: starts at T1, doubles each retransmit
	};

	static SipTransaction::Type classify(const std::shared_ptr<SipMessage>& msg);

	std::array<SipTransaction, POCKETDIAL_MAX_TRANSACTIONS> _pool{};
	PbxEnv& _env;
};

#endif
