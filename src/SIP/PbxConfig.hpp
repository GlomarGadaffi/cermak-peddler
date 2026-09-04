#ifndef PBX_CONFIG_HPP
#define PBX_CONFIG_HPP

// PbxConfig.hpp — per-extension call-forwarding and ring/hunt-group configuration
// for the "Class A" PBX feature sweep.
//
// Design mirrors the existing _dnd map (see RequestsHandler): a bounded
// std::unordered_map keyed by extension, guarded by the registrar's _mutex, and
// NVS-persisted so the config survives reboot. Like _dnd, the maps can never grow
// past the client-pool depth because the dashboard/API only ever sets config for
// real (validated) extensions, and clearing an entry erases it.
//
// All NVS access is gated on ESP_PLATFORM so the desktop gtest build stays
// host-compilable (on host the maps are pure RAM; load/persist are no-ops). The
// parsing helpers (parseReferToTarget, splitMembers) are pure and free-standing so
// the unit tests can exercise the routing logic without the full RequestsHandler.

#include <chrono>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <algorithm>
#include <cctype>

#include "PoolConfig.hpp"

namespace pbx
{
	// How long a rung member (direct CFNA target or one hunt-group leg) is given
	// to answer before the caller gives up on it. Shared by RequestsHandler's
	// direct-call CFNA arm (onInvite) and CallForker::huntRingNext, which is why
	// it lives here rather than file-local to either translation unit.
	inline constexpr std::chrono::seconds kNoAnswerTimeout{20};

	// How a ring group fans an inbound INVITE out to its members.
	enum class GroupMode
	{
		RingAll,   // fork to every member at once; first to answer wins (like 999)
		Hunt       // ring members one at a time with a per-member timeout
	};

	// The three independent call-forward targets for one extension. An empty
	// string means "not configured" for that trigger. Stored value type.
	struct ForwardConfig
	{
		std::string always;     // CFU  — forward unconditionally, before ringing
		std::string busy;       // CFB  — forward when the callee returns 486 Busy
		std::string noAnswer;   // CFNA — forward when the callee never answers

		bool empty() const
		{
			return always.empty() && busy.empty() && noAnswer.empty();
		}
	};

	// One ring/hunt group: an ordered member list plus the fan-out mode.
	struct RingGroup
	{
		std::vector<std::string> members;
		GroupMode mode = GroupMode::RingAll;
	};

	// ── Pure parsing helpers (unit-tested directly) ───────────────────────────

	// Extract the target extension (AOR user-part) from a Refer-To header value.
	// Handles the common forms produced by SIP UAs, e.g.:
	//   Refer-To: <sip:200@host:5060>
	//   Refer-To: "Bob" <sip:200@host>;some=param
	//   Refer-To: sip:200@host
	// Returns the user-part ("200") or "" if no sip: URI / user-part is present.
	// Mirrors SipMessage::extractNumber but works on a header VALUE that may carry
	// a display name and angle brackets, and tolerates a missing '@'.
	inline std::string parseReferToTarget(const std::string& referTo)
	{
		auto sipPos = referTo.find("sip:");
		if (sipPos == std::string::npos)
		{
			return {};
		}
		size_t start = sipPos + 4;

		// The user-part ends at '@' (user@host) or, if there is no host part, at the
		// first URI delimiter: '>', ';', '?' or whitespace.
		size_t end = referTo.size();
		for (size_t i = start; i < referTo.size(); ++i)
		{
			char c = referTo[i];
			if (c == '@' || c == '>' || c == ';' || c == '?' ||
				c == ' ' || c == '\t' || c == '\r' || c == '\n')
			{
				end = i;
				break;
			}
		}
		if (end <= start)
		{
			return {};
		}
		return referTo.substr(start, end - start);
	}

	// Split a comma/whitespace-separated member list (as stored in NVS / posted from
	// the dashboard) into individual extensions, trimming surrounding whitespace and
	// dropping empties. Bounded by POCKETDIAL_MAX_CLIENTS so a malicious list can't
	// allocate without limit.
	inline std::vector<std::string> splitMembers(const std::string& csv)
	{
		std::vector<std::string> out;
		size_t i = 0;
		while (i < csv.size() && out.size() < static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
		{
			// Skip separators (comma or whitespace).
			while (i < csv.size() &&
				(csv[i] == ',' || std::isspace(static_cast<unsigned char>(csv[i]))))
			{
				++i;
			}
			size_t start = i;
			while (i < csv.size() && csv[i] != ',' &&
				!std::isspace(static_cast<unsigned char>(csv[i])))
			{
				++i;
			}
			if (i > start)
			{
				out.push_back(csv.substr(start, i - start));
			}
		}
		return out;
	}

	// Join a member list back into the canonical comma-separated form used for NVS
	// persistence and the dashboard snapshot.
	inline std::string joinMembers(const std::vector<std::string>& members)
	{
		std::string out;
		for (size_t i = 0; i < members.size(); ++i)
		{
			if (i) out.push_back(',');
			out += members[i];
		}
		return out;
	}

	// ── Paging zones (the 980–989 virtual extensions) ─────────────────────────

	// One paging zone: an unordered member set (stored as a deduped, capped list).
	// A dial of the zone extension forks an intercom (auto-answer) INVITE to every
	// registered member — exactly the 999 all-page machinery, scoped to the zone.
	struct PageZone
	{
		std::vector<std::string> members;
	};

	// True iff `ext` is in the reserved paging-zone dial range 980–989. Pure, so
	// the routing predicate is unit-testable without linking the registrar.
	inline bool isPageZoneExt(const std::string& ext)
	{
		return ext.size() == 3 && ext[0] == '9' && ext[1] == '8' &&
			std::isdigit(static_cast<unsigned char>(ext[2]));
	}

	// Split a zone member list (same CSV grammar as splitMembers), then dedupe
	// (first occurrence wins, order preserved) and clamp to POCKETDIAL_ZONE_MEMBER_CAP.
	inline std::vector<std::string> splitZoneMembers(const std::string& csv)
	{
		std::vector<std::string> out;
		for (auto& m : splitMembers(csv))
		{
			if (out.size() >= static_cast<size_t>(POCKETDIAL_ZONE_MEMBER_CAP))
				break;
			if (std::find(out.begin(), out.end(), m) == out.end())
				out.push_back(m);
		}
		return out;
	}

	// ── Directed / group call pickup (Issue #68) ──────────────────────────────
	//
	// Design choice: pickup groups are NOT a new config table. They reuse
	// ring-group membership (RingGroup::members, above) exactly as-is: two
	// extensions are pickup-eligible for each other iff they are both members
	// of at least one configured ring group — regardless of that group's mode
	// (RingAll or Hunt; pickup only cares who's in the list, not how it rings).
	// An operator who already set up a "sales" or "support" ring group gets a
	// pickup group for free, with no new dashboard/NVS surface to add or keep
	// in sync. The acceptance criteria for this feature ("only same-pickup-
	// group calls eligible") is written without carving out directed pickup,
	// so BOTH *8 (group) and **<ext> (directed) are restricted to the picker's
	// own ring-group co-members — see RequestsHandler::pickupPeersOf().
	//
	// *8 is the group-pickup code; **<ext> is directed pickup of a specific
	// extension. Both are dialed as the To user-part of an INVITE, exactly
	// like the 700-709 park orbits and 980-989 page zones (isValidAor already
	// allows '*' in an AOR for this reason).

	// True iff `ext` is the group-pickup star code.
	inline bool isGroupPickupCode(const std::string& ext)
	{
		return ext == "*8";
	}

	// Extracts the target extension from a directed-pickup code ("**204" ->
	// "204"). Returns "" for anything not prefixed "**" and for "**" with no
	// extension following it — both are treated as "no target" by the caller.
	inline std::string directedPickupTarget(const std::string& ext)
	{
		if (ext.size() > 2 && ext[0] == '*' && ext[1] == '*')
		{
			return ext.substr(2);
		}
		return {};
	}
}

#endif
