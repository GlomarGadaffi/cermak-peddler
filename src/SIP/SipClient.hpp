#ifndef SIP_CLIENT_HPP
#define SIP_CLIENT_HPP

#if defined(__linux__) || defined(ESP_PLATFORM)
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#endif

#include <iostream>
#include <string>
#include <chrono>

class SipClient
{
public:
	SipClient(std::string number, sockaddr_in address, int expiresSeconds = 3600);

	bool operator==(const SipClient& other) const;

	const std::string& getNumber() const;
	const sockaddr_in& getAddress() const;

	// ── Registration lease (RFC 3261 §10.2.1) ────────────────────────
	// True once the lease deadline has passed.
	bool isExpired(std::chrono::steady_clock::time_point now) const;
	// Negotiated lease length echoed back to the client in the 200 OK.
	int getExpiresSeconds() const;

private:
	std::string _number;
	sockaddr_in _address;
	int _expiresSeconds;
	std::chrono::steady_clock::time_point _expiresAt;
};

#endif
