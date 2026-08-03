#ifndef SIP_WIRE_UTIL_HPP
#define SIP_WIRE_UTIL_HPP

// Shared request-building plumbing for the decomposed SIP state machines
// (ParkOrbit, RegisterBeeper, BlfSubscriptions, ...). Each of them mints raw
// requests by hand and needs the same two pieces: an "ip:port" authority string
// and the minimal held offer. Kept out of SipHeaderUtil.hpp deliberately —
// that header is pure string work and must stay free of socket includes.

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <lwip/sockets.h>
#elif defined(__linux__)
#include <arpa/inet.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#include <ws2tcpip.h>   // inet_ntop / INET_ADDRSTRLEN live here, not in WinSock2.h
#endif

#include <string>

namespace sipwire
{
	// "a.b.c.d:port" for a v4 peer — the authority the machines stamp into a
	// request-URI and into the Via/Contact host of server-minted requests.
	inline std::string addrToIpPort(const sockaddr_in& addr)
	{
		char ipBuf[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
		return std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));
	}

	// Minimal, well-formed a=inactive offer: the server sources no RTP, so the
	// far end holds the stream. Used by the park hold answer and by the register
	// beep INVITE — they must not drift apart, or a phone that tolerates one
	// would choke on the other.
	inline std::string makeInactiveHoldSdp(const std::string& localIp)
	{
		return
			"v=0\r\n"
			"o=- 0 0 IN IP4 " + localIp + "\r\n"
			"s=pocket-dial\r\n"
			"c=IN IP4 " + localIp + "\r\n"
			"t=0 0\r\n"
			"m=audio 9 RTP/AVP 0\r\n"
			"a=inactive\r\n";
	}
}

#endif
