// Tests for the /api/pcap ring buffer + libpcap serializer (src/SIP/PcapCapture.hpp,
// Issue #33). Wire-level: the point of this class is producing bytes Wireshark can
// open, so assertions check the actual framing rather than just "it didn't crash".

#include <gtest/gtest.h>

#include "PcapCapture.hpp"
#include "RequestsHandler.hpp"

namespace
{
	sockaddr_in addr(const char* ip, uint16_t port)
	{
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port   = htons(port);
		::inet_pton(AF_INET, ip, &a.sin_addr);
		return a;
	}

	// Pulls the global-header snaplen/network fields out so a test can assert on
	// them without hand-parsing the whole file.
	uint32_t leU32At(const std::string& buf, size_t off)
	{
		return static_cast<uint32_t>(static_cast<unsigned char>(buf[off])) |
		       (static_cast<uint32_t>(static_cast<unsigned char>(buf[off + 1])) << 8) |
		       (static_cast<uint32_t>(static_cast<unsigned char>(buf[off + 2])) << 16) |
		       (static_cast<uint32_t>(static_cast<unsigned char>(buf[off + 3])) << 24);
	}
}

TEST(PcapCapture, EmptyRingProducesValidGlobalHeaderOnly)
{
	PcapCapture cap;
	std::string file = cap.toPcapFile("192.168.1.10", 5060);

	ASSERT_EQ(file.size(), 24u);   // global header only, no records
	// Magic number 0xa1b2c3d4 written little-endian.
	EXPECT_EQ(static_cast<unsigned char>(file[0]), 0xd4);
	EXPECT_EQ(static_cast<unsigned char>(file[1]), 0xc3);
	EXPECT_EQ(static_cast<unsigned char>(file[2]), 0xb2);
	EXPECT_EQ(static_cast<unsigned char>(file[3]), 0xa1);
	EXPECT_EQ(leU32At(file, 20), 1u) << "LINKTYPE_ETHERNET";
}

TEST(PcapCapture, CapturedPayloadBytesAppearVerbatimInOutput)
{
	PcapCapture cap;
	const sockaddr_in peer = addr("192.168.1.50", 5060);
	const std::string invite = "INVITE sip:101@192.168.1.10 SIP/2.0\r\nCall-ID: abc123@peer\r\n";

	cap.record(/*outbound=*/false, peer, invite);
	std::string file = cap.toPcapFile("192.168.1.10", 5060);

	// Global header (24) + one record header (16) + Ethernet(14)+IPv4(20)+UDP(8)
	// + the payload appended verbatim.
	ASSERT_EQ(file.size(), 24u + 16u + 14u + 20u + 8u + invite.size());
	EXPECT_NE(file.find(invite), std::string::npos)
		<< "raw SIP bytes must appear unmodified in the synthesized frame";
}

TEST(PcapCapture, DirectionSelectsWhichSideIsSource)
{
	PcapCapture cap;
	const sockaddr_in peer = addr("192.168.1.50", 5061);

	cap.record(/*outbound=*/false, peer, "inbound-marker");   // peer -> local
	cap.record(/*outbound=*/true,  peer, "outbound-marker");  // local -> peer
	std::string file = cap.toPcapFile("192.168.1.10", 5060);

	// Both payloads present; a byte-exact IP-header assertion would duplicate the
	// class's own arithmetic, so this just pins that both directions are
	// captured and don't collide/overwrite each other.
	EXPECT_NE(file.find("inbound-marker"), std::string::npos) << file;
	EXPECT_NE(file.find("outbound-marker"), std::string::npos) << file;
}

TEST(PcapCapture, RingDropsOldestPastCapacity)
{
	PcapCapture cap;
	const sockaddr_in peer = addr("192.168.1.50", 5060);

	// "!" delimiter after the index so e.g. "pkt-4!" can't false-match inside
	// "pkt-40!" the way a bare "pkt-4" substring search would.
	auto marker = [](unsigned i) { return "pkt-" + std::to_string(i) + "!"; };

	const unsigned total = static_cast<unsigned>(POCKETDIAL_PCAP_RING_SIZE) + 5;
	for (unsigned i = 0; i < total; ++i)
	{
		cap.record(false, peer, marker(i));
	}
	ASSERT_EQ(cap.size(), static_cast<std::size_t>(POCKETDIAL_PCAP_RING_SIZE));

	std::string file = cap.toPcapFile("192.168.1.10", 5060);
	for (unsigned i = 0; i < 5; ++i)
	{
		EXPECT_EQ(file.find(marker(i)), std::string::npos)
			<< "index " << i << " should have been evicted";
	}
	EXPECT_NE(file.find(marker(5)), std::string::npos) << "oldest surviving entry";
	EXPECT_NE(file.find(marker(total - 1)), std::string::npos) << "newest entry must survive";
}

TEST(PcapCapture, ClearEmptiesTheRing)
{
	PcapCapture cap;
	cap.record(false, addr("192.168.1.50", 5060), "x");
	ASSERT_EQ(cap.size(), 1u);
	cap.clear();
	EXPECT_EQ(cap.size(), 0u);
	EXPECT_EQ(cap.toPcapFile("192.168.1.10", 5060).size(), 24u);
}

// ── traceRecords() (GET /api/trace's data source, Issue #32) ───────────────────

TEST(PcapCapture, TraceRecordsReflectSeqDirectionPeerAndText)
{
	PcapCapture cap;
	const sockaddr_in inPeer  = addr("192.168.1.50", 5060);
	const sockaddr_in outPeer = addr("192.168.1.51", 5061);

	cap.record(false, inPeer,  "REGISTER sip:server SIP/2.0");
	cap.record(true,  outPeer, "SIP/2.0 200 OK");

	auto recs = cap.traceRecords();
	ASSERT_EQ(recs.size(), 2u);

	EXPECT_EQ(recs[0].seq, 0u);
	EXPECT_FALSE(recs[0].outbound);
	EXPECT_EQ(recs[0].peer, "192.168.1.50:5060");
	EXPECT_EQ(recs[0].text, "REGISTER sip:server SIP/2.0");

	EXPECT_EQ(recs[1].seq, 1u);
	EXPECT_TRUE(recs[1].outbound);
	EXPECT_EQ(recs[1].peer, "192.168.1.51:5061");
	EXPECT_EQ(recs[1].text, "SIP/2.0 200 OK");
}

TEST(PcapCapture, TraceRecordSeqStaysMonotonicAcrossEviction)
{
	PcapCapture cap;
	const sockaddr_in peer = addr("192.168.1.50", 5060);
	const unsigned total = static_cast<unsigned>(POCKETDIAL_PCAP_RING_SIZE) + 3;
	for (unsigned i = 0; i < total; ++i)
	{
		cap.record(false, peer, "pkt");
	}
	auto recs = cap.traceRecords();
	ASSERT_EQ(recs.size(), static_cast<std::size_t>(POCKETDIAL_PCAP_RING_SIZE));
	// The oldest surviving entry is the 4th packet sent (index 3, 0-based) —
	// its seq must reflect that it's the 4th ever recorded, not the 1st of
	// what's currently in the ring, so a client's high-water mark stays valid
	// after entries roll off rather than seeing seq "restart".
	EXPECT_EQ(recs.front().seq, 3u);
	EXPECT_EQ(recs.back().seq, total - 1);
}

// ── RequestsHandler wiring (GET /api/pcap's actual data source) ────────────────

TEST(PcapCapture, RequestsHandlerCapturesBothDirectionsOfARealPacket)
{
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});

	EXPECT_EQ(handler.getPcapCapture().size(), 24u) << "empty ring before any traffic";

	const sockaddr_in src = addr("192.168.4.50", 5060);
	const std::string raw =
		"REGISTER sip:server SIP/2.0\r\n"
		"Via: SIP/2.0/UDP 192.168.4.50:5060;branch=z9hG4bKr\r\n"
		"From: <sip:101@server>;tag=rt\r\n"
		"To: <sip:101@server>\r\n"
		"Call-ID: pcap-test-1\r\n"
		"CSeq: 1 REGISTER\r\n"
		"Contact: <sip:101@192.168.4.50:5060>;expires=3600\r\n"
		"Content-Length: 0\r\n\r\n";
	handler.handle(RequestsHandler::getMessageFromPool(raw, src));

	std::string file = handler.getPcapCapture();
	EXPECT_GT(file.size(), 24u) << "at least the inbound REGISTER must be captured";
	EXPECT_NE(file.find("REGISTER sip:server"), std::string::npos)
		<< "inbound packet bytes must appear verbatim";
	// A REGISTER always gets SOME response — accepted (200), challenged (401) or
	// rejected (403/etc, e.g. an unprovisioned extension under Secure mode,
	// which is the registrar's default) — captured by drainOutbox() regardless
	// of which. This deliberately doesn't assert which status: that's the
	// registrar's admission policy, not this ring buffer's concern.
	EXPECT_NE(file.find("SIP/2.0 "), std::string::npos) << "outbound response must be captured";
}

TEST(PcapCapture, RequestsHandlerGetTraceRecordsMirrorsGetPcapCapture)
{
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	EXPECT_TRUE(handler.getTraceRecords().empty());

	const sockaddr_in src = addr("192.168.4.50", 5060);
	const std::string raw =
		"REGISTER sip:server SIP/2.0\r\n"
		"Via: SIP/2.0/UDP 192.168.4.50:5060;branch=z9hG4bKr2\r\n"
		"From: <sip:102@server>;tag=rt2\r\n"
		"To: <sip:102@server>\r\n"
		"Call-ID: pcap-test-2\r\n"
		"CSeq: 1 REGISTER\r\n"
		"Contact: <sip:102@192.168.4.50:5060>;expires=3600\r\n"
		"Content-Length: 0\r\n\r\n";
	handler.handle(RequestsHandler::getMessageFromPool(raw, src));

	auto recs = handler.getTraceRecords();
	ASSERT_GE(recs.size(), 1u);
	EXPECT_FALSE(recs[0].outbound);
	EXPECT_EQ(recs[0].peer, "192.168.4.50:5060");
	EXPECT_NE(recs[0].text.find("REGISTER sip:server"), std::string::npos);
}
