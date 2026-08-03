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

class SipMessage;

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
};

#endif
