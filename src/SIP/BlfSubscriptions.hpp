#ifndef BLF_SUBSCRIPTIONS_HPP
#define BLF_SUBSCRIPTIONS_HPP

#include <array>
#include <chrono>
#include <memory>
#include <string>

#include "PbxEnv.hpp"
#include "PoolConfig.hpp"
#include "SipMessage.hpp"

// ── BLF presence (RFC 6665 SUBSCRIBE / RFC 4235 dialog-event NOTIFY) ──────────
// One watcher dialog per fixed slot — no heap growth. onSubscribe gates the
// event package (489 on anything but "dialog"), validates the watched AOR,
// allocates/refreshes/releases a slot and answers 202 Accepted plus an
// immediate full-state NOTIFY. refresh() recomputes every watched target's
// dialog state after each handled packet and NOTIFYs slots whose state
// changed; sweepExpired() terminates overdue subscriptions (reason=timeout).
//
// Locking: every method assumes the caller holds the engine's _mutex, same
// convention as the RequestsHandler monolith this was extracted from.
class BlfSubscriptions
{
public:
	explicit BlfSubscriptions(PbxEnv& env) : _env(env) {}

	// Handle an inbound SUBSCRIBE (called from the engine's dispatch table).
	void onSubscribe(const std::shared_ptr<SipMessage>& data);

	// Change detection: NOTIFY every slot whose target's dialog state moved.
	void refresh();

	// Expire overdue subscriptions: terminal NOTIFY (reason=timeout) + slot free.
	void sweepExpired();

private:
	// One BLF watcher dialog. Fixed-size record in a std::array — no heap growth.
	struct DialogSubscription
	{
		bool        used = false;
		std::string callId;        // subscription dialog id (refresh/unsubscribe key)
		std::string watcherFrom;   // subscriber's full From header (incl. its tag)
		std::string subTo;         // our full To header (incl. the tag we minted)
		std::string targetAor;     // the extension being watched (the To user-part)
		std::string lastState;     // last NOTIFYed state token (change detection)
		unsigned    version = 0;   // dialog-info version counter (monotonic)
		unsigned    cseq = 1;      // NOTIFY CSeq within the subscription dialog
		int         expiresSec = 0;
		sockaddr_in addr{};        // where NOTIFYs go (the SUBSCRIBE source)
		std::chrono::steady_clock::time_point deadline{};
	};

	// Event-package parsing helper (pure / static / host-testable). Returns the
	// canonical package name (e.g. "dialog") or "".
	static std::string parseEventPackage(const std::string& raw);

	// RFC 4235 dialog-info XML builder (pure).
	static std::string buildDialogInfoXml(const std::string& entity, unsigned version,
		const std::string& dialogId, const std::string& state, const std::string& direction);

	// Compute the current RFC 4235 dialog state of `targetAor` from the engine's
	// session table: ""=idle, else trying/early/confirmed plus direction and
	// dialog id.
	std::string computeDialogState(const std::string& targetAor,
		std::string& outDirection, std::string& outDialogId) const;

	// Build one NOTIFY for a subscription slot carrying a dialog-info+xml body.
	// `terminated` selects Subscription-State: terminated;reason=<termReason>.
	std::shared_ptr<SipMessage> buildDialogNotify(DialogSubscription& sub,
		const std::string& state, const std::string& direction, const std::string& dialogId,
		bool terminated, const char* termReason);

	std::array<DialogSubscription, POCKETDIAL_MAX_SUBSCRIPTIONS> _subscriptions;
	PbxEnv& _env;
};

#endif
