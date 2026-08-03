// Wire-level tests for the park-orbit state machine (src/SIP/ParkOrbit.cpp).
// The park paths had no host coverage at all, which is how a doubled Call-ID
// header survived in the retrieve re-INVITE from the pre-decomposition monolith.

#include <gtest/gtest.h>

#include "FakePbxEnv.hpp"
#include "ParkOrbit.hpp"

namespace
{
	// `mediaIp` is the caller's own IP: it lands in the SDP c= line, which is what
	// the retrieve path swaps between the two legs.
	std::shared_ptr<SipMessage> inviteTo(const std::string& orbit, const std::string& fromExt,
		const std::string& callId, const sockaddr_in& src, const std::string& mediaIp)
	{
		const std::string body =
			"v=0\r\n"
			"o=- 1 1 IN IP4 " + mediaIp + "\r\n"
			"s=call\r\n"
			"c=IN IP4 " + mediaIp + "\r\n"
			"t=0 0\r\n"
			"m=audio 4000 RTP/AVP 0\r\n";
		const std::string raw =
			"INVITE sip:" + orbit + "@192.168.1.10 SIP/2.0\r\n"
			"Via: SIP/2.0/UDP " + mediaIp + ":5060;branch=z9hG4bKcaller\r\n"
			"From: <sip:" + fromExt + "@192.168.1.10>;tag=callertag\r\n"
			"To: <sip:" + orbit + "@192.168.1.10>\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 1 INVITE\r\n"
			"Contact: <sip:" + fromExt + "@" + mediaIp + ":5060>\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		return std::make_shared<SipMessage>(raw, src);
	}

	// Count non-overlapping occurrences of `needle` in `hay`.
	int countOf(const std::string& hay, const std::string& needle)
	{
		int n = 0;
		for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
			++n;
		return n;
	}
}

// Park a call on 700, then retrieve it from a second extension. The retrieve
// sends a re-INVITE to the parked party; its Call-ID must be the parked dialog's
// Call-ID exactly once — "Call-ID: Call-ID: x@host" is what the phone rejects.
TEST(ParkOrbit, RetrieveReinviteCarriesSingleCallIdHeader)
{
	FakePbxEnv env;
	ParkOrbit park(env);

	const sockaddr_in parkedAddr    = FakePbxEnv::addr("192.168.1.50", 5060);
	const sockaddr_in retrieverAddr = FakePbxEnv::addr("192.168.1.51", 5060);

	auto parkedClient    = std::make_shared<SipClient>("101", parkedAddr);
	auto retrieverClient = std::make_shared<SipClient>("102", retrieverAddr);

	ASSERT_EQ(park.orbitIndex("700"), 0);

	park.onInvite(inviteTo("700", "101", "parked-call-1@192.168.1.50", parkedAddr, "192.168.1.50"),
		parkedClient, 0);
	ASSERT_EQ(env.sent.size(), 1u);           // 200 OK (hold) to the parked party

	park.onInvite(inviteTo("700", "102", "retrieve-call-1@192.168.1.51", retrieverAddr, "192.168.1.51"),
		retrieverClient, 0);
	ASSERT_EQ(env.sent.size(), 3u);           // + 200 OK to retriever, + re-INVITE

	const std::string reinvite = env.sentRaw(2);
	EXPECT_EQ(reinvite.rfind("INVITE sip:101@", 0), 0u) << reinvite;
	EXPECT_EQ(countOf(reinvite, "Call-ID:"), 1) << reinvite;
	EXPECT_NE(reinvite.find("Call-ID: parked-call-1@192.168.1.50\r\n"), std::string::npos)
		<< reinvite;
	// The re-INVITE must offer the retriever's SDP so media re-points at them.
	EXPECT_NE(reinvite.find("c=IN IP4 192.168.1.51"), std::string::npos) << reinvite;
}

// The parked party's 200 OK to that re-INVITE is ACKed, and the ACK likewise
// carries exactly one Call-ID header line.
TEST(ParkOrbit, ReinviteOkIsAckedWithSingleCallIdHeader)
{
	FakePbxEnv env;
	ParkOrbit park(env);

	const sockaddr_in parkedAddr    = FakePbxEnv::addr("192.168.1.50", 5060);
	const sockaddr_in retrieverAddr = FakePbxEnv::addr("192.168.1.51", 5060);

	park.onInvite(inviteTo("700", "101", "parked-call-2@192.168.1.50", parkedAddr, "192.168.1.50"),
		std::make_shared<SipClient>("101", parkedAddr), 0);
	park.onInvite(inviteTo("700", "102", "retrieve-call-2@192.168.1.51", retrieverAddr, "192.168.1.51"),
		std::make_shared<SipClient>("102", retrieverAddr), 0);
	const std::size_t before = env.sent.size();

	const std::string okRaw =
		"SIP/2.0 200 OK\r\n"
		"Via: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bKpark\r\n"
		"From: <sip:700@192.168.1.10:5060>;tag=servertag\r\n"
		"To: <sip:101@192.168.1.10>;tag=callertag\r\n"
		"Call-ID: parked-call-2@192.168.1.50\r\n"
		"CSeq: 2 INVITE\r\n"
		"Content-Length: 0\r\n\r\n";
	auto ok = std::make_shared<SipMessage>(okRaw, parkedAddr);

	EXPECT_TRUE(park.handleOk(ok));
	ASSERT_EQ(env.sent.size(), before + 1);

	const std::string ack = env.sentRaw(before);
	EXPECT_EQ(ack.rfind("ACK sip:", 0), 0u) << ack;
	EXPECT_EQ(countOf(ack, "Call-ID:"), 1) << ack;
	EXPECT_EQ(countOf(ack, "From:"), 1) << ack;
	EXPECT_EQ(countOf(ack, "To:"), 1) << ack;
}

// A retrieve that cannot get a session must 503 the retriever and leave the
// orbit occupied rather than half-tearing-down the parked leg (#71).
TEST(ParkOrbit, RetrieveWithExhaustedSessionPoolLeavesSlotParked)
{
	FakePbxEnv env;
	ParkOrbit park(env);

	const sockaddr_in parkedAddr    = FakePbxEnv::addr("192.168.1.50", 5060);
	const sockaddr_in retrieverAddr = FakePbxEnv::addr("192.168.1.51", 5060);

	park.onInvite(inviteTo("700", "101", "parked-call-3@192.168.1.50", parkedAddr, "192.168.1.50"),
		std::make_shared<SipClient>("101", parkedAddr), 0);

	env.sessionPoolAvailable = false;
	park.onInvite(inviteTo("700", "102", "retrieve-call-3@192.168.1.51", retrieverAddr, "192.168.1.51"),
		std::make_shared<SipClient>("102", retrieverAddr), 0);

	EXPECT_NE(env.sentRaw(1).find("503 Service Unavailable"), std::string::npos)
		<< env.sentRaw(1);
	// Slot still holds the parked call: it shows up in the dashboard rows.
	auto rows = park.snapshotRows(std::chrono::steady_clock::now(), /*onlyParked=*/true);
	ASSERT_EQ(rows.size(), 1u);
	EXPECT_EQ(std::get<0>(rows[0]), "700");
	EXPECT_EQ(std::get<1>(rows[0]), "101");
}
