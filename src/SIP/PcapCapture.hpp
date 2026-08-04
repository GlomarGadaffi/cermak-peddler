#ifndef PCAP_CAPTURE_HPP
#define PCAP_CAPTURE_HPP

// PcapCapture.hpp — bounded ring buffer of recent SIP signaling packets, and a
// minimal classic libpcap file-format serializer so they can be downloaded and
// opened directly in Wireshark (Issue #33).
//
// RTP/media is deliberately NOT captured here: pocket-dial brokers RTP
// peer-to-peer between phones and never relays it, so there is nothing
// server-side to capture for a call's media — only the SIP signaling that set
// it up, which is also what "SIP Signaling Research" actually wants to inspect.
//
// This is an app-layer capture, not a raw-socket one: pocket-dial has no
// visibility below its own recvfrom()/sendto() calls (no raw sockets, no
// libpcap on ESP32), so each entry is synthesized into a minimal
// Ethernet+IPv4+UDP frame around the exact SIP bytes handled/sent, using dummy
// MACs (the link layer carries no information this server has) and the real
// peer/local IP:port (flipped by capture direction) so Wireshark's SIP
// dissector (triggered by port 5060) decodes it exactly as a real capture
// would. Timestamps are steady-clock-relative, not wall-clock — same basis as
// CallDetailRecord's startMs (see CallDetailRecord.hpp): no RTC is guaranteed
// on the device, so there is no reliable epoch to stamp these with.

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <lwip/sockets.h>
#elif defined(__linux__)
#include <arpa/inet.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#include <ws2tcpip.h>
#endif

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

// Number of recent SIP packets retained for /api/pcap. Each entry costs
// roughly the size of the SIP message it captured (a few hundred bytes to
// ~1.5 KB for an INVITE+SDP) rather than a fixed slot, so — unlike the object
// pools in PoolConfig.hpp — this bounds *count*, not a static footprint.
// Compile-time tunable (-DPOCKETDIAL_PCAP_RING_SIZE=N); lower it to claw back
// RAM on a constrained node.
#ifndef POCKETDIAL_PCAP_RING_SIZE
#define POCKETDIAL_PCAP_RING_SIZE 64
#endif

class PcapCapture
{
public:
	// Record one SIP message. `outbound` is from the server's own perspective:
	// false for something received from `peer`, true for something sent to
	// `peer`. Not internally synchronized — callers capture under whatever lock
	// already guards the call site (RequestsHandler captures under its own
	// _mutex, the same lock guarding everything else a packet touches).
	void record(bool outbound, const sockaddr_in& peer, std::string_view bytes)
	{
		if (_entries.size() >= POCKETDIAL_PCAP_RING_SIZE)
		{
			_entries.pop_front();
		}
		_entries.push_back(Entry{outbound, peer, std::string(bytes), monotonicUs()});
	}

	std::size_t size() const { return _entries.size(); }
	void clear() { _entries.clear(); }

	// Serialize the ring into a classic libpcap file (24-byte global header +
	// one 16-byte record header + synthesized frame per captured packet).
	// `localIp`/`localPort` fill in the server's own side of each frame.
	std::string toPcapFile(const std::string& localIp, uint16_t localPort) const
	{
		std::string out;
		out.reserve(24 + _entries.size() * 128);

		// Global header. LE container (magic 0xa1b2c3d4 written little-endian);
		// LINKTYPE_ETHERNET = 1.
		appendU32LE(out, 0xa1b2c3d4u);
		appendU16LE(out, 2);
		appendU16LE(out, 4);
		appendU32LE(out, 0);       // thiszone
		appendU32LE(out, 0);       // sigfigs
		appendU32LE(out, 65535u);  // snaplen
		appendU32LE(out, 1u);      // LINKTYPE_ETHERNET

		const uint32_t localAddr = static_cast<uint32_t>(inet_addr(localIp.c_str()));

		for (const auto& e : _entries)
		{
			std::string frame = frameFor(e, localAddr, localPort);
			const uint32_t sec  = static_cast<uint32_t>(e.tsUs / 1000000ULL);
			const uint32_t usec = static_cast<uint32_t>(e.tsUs % 1000000ULL);
			const uint32_t len  = static_cast<uint32_t>(frame.size());
			appendU32LE(out, sec);
			appendU32LE(out, usec);
			appendU32LE(out, len);   // incl_len
			appendU32LE(out, len);   // orig_len: never truncated, so equal
			out.append(frame);
		}
		return out;
	}

private:
	struct Entry
	{
		bool        outbound;
		sockaddr_in peer;
		std::string bytes;
		uint64_t    tsUs;
	};
	std::deque<Entry> _entries;

	static uint64_t monotonicUs()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	static void appendU16LE(std::string& out, uint16_t v)
	{
		out.push_back(static_cast<char>(v & 0xFF));
		out.push_back(static_cast<char>((v >> 8) & 0xFF));
	}
	static void appendU32LE(std::string& out, uint32_t v)
	{
		out.push_back(static_cast<char>(v & 0xFF));
		out.push_back(static_cast<char>((v >> 8) & 0xFF));
		out.push_back(static_cast<char>((v >> 16) & 0xFF));
		out.push_back(static_cast<char>((v >> 24) & 0xFF));
	}
	static void appendU16BE(std::string& out, uint16_t v)
	{
		uint16_t n = htons(v);
		out.append(reinterpret_cast<const char*>(&n), 2);
	}
	static void appendU32Raw(std::string& out, uint32_t netOrderValue)
	{
		// Already in network byte order (an in_addr_t straight from a
		// sockaddr_in / inet_addr()) — append verbatim, no host<->network swap.
		out.append(reinterpret_cast<const char*>(&netOrderValue), 4);
	}

	// Standard 16-bit one's-complement checksum over an IPv4 header (RFC 791
	// §3.1), computed with the checksum field itself treated as zero.
	static uint16_t ipChecksum(const uint8_t* data, std::size_t len)
	{
		uint32_t sum = 0;
		std::size_t i = 0;
		for (; i + 1 < len; i += 2)
		{
			sum += (static_cast<uint16_t>(data[i]) << 8) | data[i + 1];
		}
		if (i < len) sum += static_cast<uint16_t>(data[i]) << 8;
		while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
		return static_cast<uint16_t>(~sum);
	}

	// Synthesizes one Ethernet+IPv4+UDP frame around a captured entry's raw SIP
	// bytes. `localAddr` is already network-byte-order (from inet_addr()), as is
	// `e.peer.sin_addr.s_addr`; `localPort`/`e.peer.sin_port` are host/network
	// order respectively and get normalized through appendU16BE.
	static std::string frameFor(const Entry& e, uint32_t localAddr, uint16_t localPort)
	{
		std::string frame;
		frame.reserve(14 + 20 + 8 + e.bytes.size());

		// Ethernet (14 bytes): locally-administered dummy MACs — the link layer
		// carries no real information here — then EtherType IPv4.
		static const unsigned char dstMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
		static const unsigned char srcMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
		frame.append(reinterpret_cast<const char*>(dstMac), 6);
		frame.append(reinterpret_cast<const char*>(srcMac), 6);
		appendU16BE(frame, 0x0800);

		const uint32_t srcIp = e.outbound ? localAddr : e.peer.sin_addr.s_addr;
		const uint32_t dstIp = e.outbound ? e.peer.sin_addr.s_addr : localAddr;
		const uint16_t srcPort = e.outbound ? localPort : ntohs(e.peer.sin_port);
		const uint16_t dstPort = e.outbound ? ntohs(e.peer.sin_port) : localPort;

		const uint16_t udpLen     = static_cast<uint16_t>(8 + e.bytes.size());
		const uint16_t ipTotalLen = static_cast<uint16_t>(20 + udpLen);

		// IPv4 header (20 bytes, no options).
		const std::size_t ipHdrStart = frame.size();
		frame.push_back(static_cast<char>(0x45));  // version 4, IHL 5 (x4 = 20 bytes)
		frame.push_back(static_cast<char>(0x00));  // DSCP/ECN
		appendU16BE(frame, ipTotalLen);
		appendU16BE(frame, 0);       // identification
		appendU16BE(frame, 0x4000);  // flags=DF, fragment offset 0
		frame.push_back(static_cast<char>(64));    // TTL
		frame.push_back(static_cast<char>(17));    // protocol = UDP
		appendU16BE(frame, 0);       // checksum placeholder, patched below
		appendU32Raw(frame, srcIp);
		appendU32Raw(frame, dstIp);

		const uint16_t csum = ipChecksum(
			reinterpret_cast<const uint8_t*>(frame.data() + ipHdrStart), 20);
		frame[ipHdrStart + 10] = static_cast<char>((csum >> 8) & 0xFF);
		frame[ipHdrStart + 11] = static_cast<char>(csum & 0xFF);

		// UDP header (8 bytes) + payload. Checksum 0 = "not computed", valid for
		// IPv4/UDP (RFC 768) and simpler than summing the SIP payload for a
		// value Wireshark doesn't need to re-verify this capture.
		appendU16BE(frame, srcPort);
		appendU16BE(frame, dstPort);
		appendU16BE(frame, udpLen);
		appendU16BE(frame, 0);
		frame.append(e.bytes);

		return frame;
	}
};

#endif
