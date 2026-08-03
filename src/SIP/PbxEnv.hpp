#ifndef PBX_ENV_HPP
#define PBX_ENV_HPP

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <lwip/sockets.h>
#elif defined(__linux__)
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#endif

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

class SipMessage;
class SipClient;
class Session;

// Narrow service surface the decomposed SIP state machines (TransactionLayer,
// Registrar, ParkOrbit, BlfSubscriptions, ...) use to reach the shared engine
// infrastructure owned by RequestsHandler: the deferred outbox, the fixed
// SipMessage pool and the deferred log queue. Every method assumes the caller
// already holds the engine's big _mutex — the machines keep the same
// caller-holds-lock convention as the monolith they were extracted from.
struct PbxEnv
{
	virtual ~PbxEnv() = default;

	// Queue a message for sending once the current handle()/tick() pass unlocks.
	virtual void enqueue(const sockaddr_in& to, std::shared_ptr<SipMessage> msg) = 0;

	// Draw a SipMessage from the fixed pool (heap fallback on exhaustion).
	virtual std::shared_ptr<SipMessage> messageFromPool(std::string raw, sockaddr_in src) = 0;

	// Append to the deferred log queue (flushed off-lock).
	virtual void log(std::string msg, bool isError = false) = 0;

	// The server's active local IP (resolved once at construction) and SIP port —
	// the identity the machines stamp into Via/From/Contact headers they build.
	virtual const std::string& localIp() const = 0;
	virtual int serverPort() const = 0;

	// ── Engine services (registration + session tables) ───────────────────────
	// Look up a live registration binding; nullptr when `number` isn't registered.
	virtual std::shared_ptr<SipClient> findRegistered(std::string_view number) = 0;
	// Draw a transient virtual-peer SipClient (777/440/park leg) from the fixed
	// pool (heap fallback on exhaustion — graceful, never a crash).
	virtual std::shared_ptr<SipClient> allocVirtualPeer(std::string number, const sockaddr_in& addr) = 0;
	// Draw a Session from the fixed pool; nullptr on exhaustion. NOT yet visible
	// to lookups — pair with insertSession once it is wired up.
	virtual std::shared_ptr<Session> allocSession(const std::string& callID,
		const std::shared_ptr<SipClient>& src) = 0;
	// Publish a session into the live-session table under its Call-ID.
	virtual void insertSession(const std::string& callID, const std::shared_ptr<Session>& session) = 0;
	// Find a live session by Call-ID; nullptr when unknown.
	virtual std::shared_ptr<Session> findSession(std::string_view callID) = 0;
	// Build the server's Contact header value for `number`.
	virtual std::string contactFor(std::string_view number) const = 0;
	// Build a server-initiated in-dialog BYE. From/To must include tags because
	// the dialog role differs per call path (beep = server UAC; park = server UAS).
	virtual std::shared_ptr<SipMessage> serverBye(const std::string& destExt,
		const sockaddr_in& destAddr, const std::string& callId,
		const std::string& fromHeader, const std::string& toHeader) = 0;
	// Read-only view of the live-session table (BLF dialog-state computation).
	virtual const std::unordered_map<std::string, std::shared_ptr<Session>>& sessionsView() const = 0;
	// AOR charset validation (the engine's isValidAor policy).
	virtual bool validAor(std::string_view s) const = 0;
	// Parse the requested registration/subscription lease from Expires/Contact
	// (RFC 3261 §10.2.1), clamped to the engine's default when absent.
	virtual int requestedExpires(const std::shared_ptr<SipMessage>& msg) const = 0;
};

#endif
