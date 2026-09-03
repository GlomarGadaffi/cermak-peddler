#ifndef ADMIN_AUTH_HPP
#define ADMIN_AUTH_HPP

// AdminAuth: a small, self-contained, platform-portable admin credential and
// session manager for the HTTP dashboard.
//
// Why this exists: the dashboard's state-changing endpoints (/api/kill,
// /api/wifi/connect, /api/wifi/mode_ap, /api/factory-reset) were protected only
// by a same-origin/CSRF check. The device ships an OPEN WiFi AP, so any device
// that joins the AP could disconnect calls, rewrite WiFi credentials, switch
// modes, or factory-reset. AdminAuth adds a PIN-gated session layer on top of
// the existing same-origin check (defense in depth).
//
// Design constraints:
//   * Dependency-free beyond the C++17 standard library and the platform guards.
//   * Identical hashing on host and ESP (a self-contained, public-domain SHA-256
//     lives in AdminAuth.cpp) so a credential is portable and verifiable the same
//     way everywhere — and so the host build needs no external crypto dependency.
//   * Credential persistence: NVS namespace "storage" on ESP (keys admin_salt /
//     admin_hash); an in-process static on host (documented: host has no NVS).
//   * Thread-safety: all shared state is guarded by an internal std::mutex. The
//     HTTP server handles one client at a time per accept, but it detaches a
//     thread per connection, so concurrent access is possible.

#include <string>
#include <cstdint>

namespace AdminAuth
{
	// --- Tunables (documented in docs/THREAT_MODEL.md) ---
	constexpr size_t   kMinPinLength      = 4;        // reject PINs shorter than this
	constexpr size_t   kSessionTokenHex   = 32;       // >= 32 hex chars (128 bits)
	constexpr size_t   kMaxSessions       = 8;        // fixed-capacity session table
	constexpr uint64_t kSessionTtlMs      = 30ULL * 60ULL * 1000ULL;   // 30 min absolute expiry
	constexpr int      kMaxFailedAttempts = 5;        // consecutive failures before lockout
	constexpr uint64_t kLockoutMs         = 60ULL * 1000ULL;           // 60 s cooldown
	constexpr uint32_t kHashIterations    = 50000;    // PBKDF-style iterated SHA-256 rounds
	constexpr size_t   kCsrfTokenHex      = 32;       // per-session CSRF token (128 bits)
	// Brute-force accounting is per-client, not global. A single global counter
	// lets one attacker lock the legitimate admin out of new logins (docs/THREAT_MODEL.md
	// D-3), and it shares one budget across every peer on the AP. Buckets are a
	// fixed-size, least-recently-seen-evicted table so a flood of distinct source
	// addresses cannot grow memory.
	constexpr size_t   kMaxAttemptBuckets = 8;
	// Consecutive lockouts back off exponentially: kLockoutMs << min(trips-1, this).
	// 4 caps a single client at 60 s << 4 = 16 minutes per window.
	constexpr int      kMaxLockoutShift   = 4;
	// Aggregate backstop across ALL clients. Per-client buckets alone are not
	// enough on a shared link: source addresses are spoofable, so an attacker who
	// rotates them gets a fresh bucket — and a fresh escalation ladder — every
	// few guesses, which would be a WEAKER position than the single global
	// counter this replaced. This ceiling bounds the aggregate guess rate no
	// matter how many identities the attacker invents. It is set well above
	// kMaxFailedAttempts so ordinary fat-fingering never reaches it, which is
	// what keeps the D-3 self-DoS from coming back with it.
	constexpr int      kMaxFailedAttemptsGlobal = 20;

	// True iff an admin credential (salt + hash) is currently stored.
	bool isProvisioned();

	// Set/replace the admin PIN. Enforces kMinPinLength. Generates a fresh random
	// salt, computes a salted, iterated SHA-256 hash, and persists it (NVS on ESP,
	// in-memory on host). Returns false if the PIN is too short or persistence
	// fails. The caller (endpoint layer) decides *when* this is allowed.
	bool setPin(const std::string& pin);

	// Constant-time verification of a candidate PIN against the stored hash.
	// Honors the brute-force lockout: returns false while locked out (without even
	// hashing). On a correct PIN, resets the failure counter. On a wrong PIN,
	// increments it and may engage the lockout. Returns false if not provisioned.
	bool verifyPin(const std::string& pin);

	// As above, but accounts failures against `clientKey` (the HTTP client's IP)
	// rather than the shared unkeyed bucket, so one attacker cannot lock everyone
	// else out. An empty key selects the unkeyed bucket, which is what the DTMF
	// admin-menu path uses (it has no HTTP peer).
	bool verifyPin(const std::string& pin, const std::string& clientKey);

	// True while the brute-force lockout is engaged (cooldown not yet elapsed).
	bool isLockedOut();

	// True while `clientKey`'s own cooldown is engaged. Other clients are unaffected.
	bool isLockedOut(const std::string& clientKey);

	// Create a new server-side session and return its opaque random token
	// (kSessionTokenHex hex chars). Evicts the oldest/expired entry if the table
	// is full. Returns an empty string only on catastrophic RNG failure.
	std::string createSession();

	// True iff the token names a live (non-expired) session.
	bool validateSession(const std::string& token);

	// The CSRF token bound to a live session, or "" if the session is unknown or
	// expired. Generated with the session and stored beside it, so it is bound by
	// storage rather than derived from anything the client controls. It is rendered
	// into the dashboard page (never set as a cookie), so a cross-site page cannot
	// read it even though the browser would happily attach the cookie.
	std::string sessionCsrf(const std::string& token);

	// Constant-time check of a submitted CSRF token against the one bound to
	// `token`'s session. False if the session is unknown/expired or the token does
	// not match. Does NOT slide the session expiry (validateSession does that).
	bool validateCsrf(const std::string& token, const std::string& csrf);

	// Destroy a session by token (logout). No-op if unknown.
	void destroySession(const std::string& token);

	// Wipe the stored credential (salt + hash) AND all live sessions, and reset the
	// lockout state. Used by factory-reset so the device returns to the
	// unprovisioned/open state.
	void clearCredential();

	// True iff the NVS key "admin_hash" in namespace "storage" is non-empty.
	// Intended for the boot provisioning gate in app_main() — called before
	// the RTOS scheduler has spawned real-time tasks, so it may block briefly
	// on NVS I/O without issue.
	// On non-ESP (host) builds this delegates to isProvisioned() so the host
	// unit tests exercise the same code path.
	bool credentialIsSet();
}

#endif // ADMIN_AUTH_HPP
