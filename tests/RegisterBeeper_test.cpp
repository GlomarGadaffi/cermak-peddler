// Wire-level tests for the register-beep UAC dialog (src/SIP/RegisterBeeper.cpp).
// The beep builds its INVITE/ACK/BYE by hand, so only a byte-level assertion
// catches header defects — the ACK used to stamp "To: " on top of getTo(), which
// already returns the full "To: ..." header line.

#include <gtest/gtest.h>

#include "FakePbxEnv.hpp"
#include "RegisterBeeper.hpp"

namespace
{
	// The phone's 200 OK to our beep INVITE, echoing the Call-ID we generated.
	std::shared_ptr<SipMessage> okFor(const std::string& callId, const sockaddr_in& from)
	{
		const std::string raw =
			"SIP/2.0 200 OK\r\n"
			"Via: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bKbeep\r\n"
			"From: \"PocketDial\" <sip:pbx@192.168.1.10:5060>;tag=servertag\r\n"
			"To: <sip:101@192.168.1.10>;tag=phonetag\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 1 INVITE\r\n"
			"Content-Length: 0\r\n\r\n";
		return std::make_shared<SipMessage>(raw, from);
	}

	// Pull the Call-ID value out of the INVITE the beeper just sent.
	std::string callIdOf(const std::string& raw)
	{
		auto p = raw.find("Call-ID: ");
		if (p == std::string::npos) return {};
		p += 9;
		return raw.substr(p, raw.find("\r\n", p) - p);
	}
}

TEST(RegisterBeeper, AnsweredBeepAcksAndByesWithWellFormedHeaders)
{
	FakePbxEnv env;
	RegisterBeeper beeper(env);

	const sockaddr_in phoneAddr = FakePbxEnv::addr("192.168.1.50", 5060);
	beeper.sendBeep(std::make_shared<SipClient>("101", phoneAddr));

	ASSERT_EQ(env.sent.size(), 1u);
	const std::string invite = env.sentRaw(0);
	EXPECT_EQ(invite.rfind("INVITE sip:101@192.168.1.50:5060", 0), 0u) << invite;
	EXPECT_NE(invite.find("a=inactive"), std::string::npos) << invite;

	const std::string callId = callIdOf(invite);
	ASSERT_FALSE(callId.empty());

	// Phone auto-answers: expect ACK then BYE.
	EXPECT_TRUE(beeper.handleOk(okFor(callId, phoneAddr)));
	ASSERT_EQ(env.sent.size(), 3u);

	const std::string ack = env.sentRaw(1);
	EXPECT_EQ(ack.rfind("ACK sip:101@", 0), 0u) << ack;
	EXPECT_EQ(FakePbxEnv::countOf(ack, "To:"), 1) << ack;
	EXPECT_EQ(FakePbxEnv::countOf(ack, "From:"), 1) << ack;
	EXPECT_EQ(FakePbxEnv::countOf(ack, "Call-ID:"), 1) << ack;
	// The phone's tag must survive onto the ACK or the dialog does not match.
	EXPECT_NE(ack.find("tag=phonetag"), std::string::npos) << ack;
	EXPECT_NE(ack.find("Call-ID: " + callId + "\r\n"), std::string::npos) << ack;

	// The BYE is delegated to PbxEnv::serverBye (the engine's single server-BYE
	// builder), so assert on what the beeper handed it — asserting on the bytes
	// would only be testing FakePbxEnv's own stand-in for that builder.
	ASSERT_EQ(env.byeCalls.size(), 1u);
	const auto& bye = env.byeCalls[0];
	EXPECT_EQ(bye.destExt, "101");
	EXPECT_EQ(bye.callId, callId);                    // bare value, not the header line
	EXPECT_NE(bye.toHeader.find("tag=phonetag"), std::string::npos) << bye.toHeader;
	EXPECT_NE(bye.fromHeader.find("<sip:pbx@"), std::string::npos) << bye.fromHeader;
	EXPECT_NE(bye.fromHeader.find(";tag="), std::string::npos) << bye.fromHeader;
}

// A 200 OK for an unknown Call-ID is not ours — handleOk must decline it so the
// engine goes on to its normal session lookup.
TEST(RegisterBeeper, ForeignOkIsNotConsumed)
{
	FakePbxEnv env;
	RegisterBeeper beeper(env);
	const sockaddr_in phoneAddr = FakePbxEnv::addr("192.168.1.50", 5060);

	EXPECT_FALSE(beeper.handleOk(okFor("not-a-beep-dialog@192.168.1.50", phoneAddr)));
	EXPECT_TRUE(env.sent.empty());
}
