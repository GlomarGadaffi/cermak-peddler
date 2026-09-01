// SDP parser hardening against the UNISOC T612 VoLTE RCE class (SSD advisory,
// 2026 — CWE-674, uncontrolled recursion in _SDPDEC_AcapDecoder). The Unisoc
// modem parsed a body of repeated `a=acap:1 acap:1 ...` by recursing per token
// with no depth limit until its task stack overflowed into a neighbour and
// became code execution.
//
// pocket-dial is not built that way: its SDP parser is a flat, non-recursive
// line scan that recognises only the six session-level line types and ignores
// a= attributes entirely, and inbound SIP is UDP-capped at 2048 bytes. These
// tests pin that property so it cannot regress — especially as this branch adds
// SDP negotiation, which is exactly where a= attribute parsing would be
// introduced. They assert that the literal attack payload is parsed as bounded,
// harmless data: the audio port is still recovered and the attributes never
// influence any recognised field.

#include <gtest/gtest.h>

#include <string>

#include "SipSdpMessage.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace
{
	sockaddr_in localAddr()
	{
		sockaddr_in s{};
		s.sin_family      = AF_INET;
		s.sin_addr.s_addr = inet_addr("127.0.0.1");
		s.sin_port        = htons(5060);
		return s;
	}

	std::string inviteWith(const std::string& body)
	{
		return "INVITE sip:200@server SIP/2.0\r\n"
		       "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK1\r\n"
		       "From: <sip:100@server>;tag=a\r\n"
		       "To: <sip:200@server>\r\n"
		       "Call-ID: hardening-1\r\n"
		       "CSeq: 1 INVITE\r\n"
		       "Content-Type: application/sdp\r\n"
		       "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
	}
}

// The exact shape from the advisory: many acap tokens on a single a= line. The
// parser must treat the whole line as one unrecognised attribute and keep the
// real media port intact — no recursion, no crash, no corruption of m=.
TEST(SipSdpHardening, AcapFloodOnOneLineIsIgnoredAndPortSurvives)
{
	std::string acapLine = "a=acap:1";
	for (int i = 0; i < 200; ++i) acapLine += " acap:1";

	const std::string body =
		"v=0\r\n"
		"o=- 111 111 IN IP4 192.168.1.10\r\n"
		"s=call\r\n"
		"c=IN IP4 192.168.1.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 0\r\n"
		+ acapLine + "\r\n";

	SipSdpMessage sdp(inviteWith(body), localAddr());

	EXPECT_EQ(sdp.getRtpPort(), 4000);
	EXPECT_EQ(sdp.getMedia(), "m=audio 4000 RTP/AVP 0");
	// The a= flood is not one of the six recognised line types, so it must not
	// have leaked into any of them.
	EXPECT_EQ(sdp.getConnectionInformation(), "c=IN IP4 192.168.1.10");
	EXPECT_EQ(sdp.getVersion(), "v=0");
}

// Same attack spread across one acap attribute PER LINE, many lines. This is the
// case the kMaxSdpLines cap exists for: hundreds of attribute lines must not
// turn parsing into a function of the attacker's line count. The media line is
// placed up front (as a well-formed offer always does) so it is recovered
// regardless of how much junk trails it.
TEST(SipSdpHardening, ManyAttributeLinesStayBoundedAndPortSurvives)
{
	std::string body =
		"v=0\r\n"
		"o=- 1 1 IN IP4 10.0.0.1\r\n"
		"s=call\r\n"
		"c=IN IP4 10.0.0.1\r\n"
		"t=0 0\r\n"
		"m=audio 5004 RTP/AVP 0\r\n";
	for (int i = 0; i < 1000; ++i) body += "a=acap:1\r\n";

	SipSdpMessage sdp(inviteWith(body), localAddr());

	// Front-loaded session/media lines are recovered; the trailing attribute
	// flood is parsed as nothing and cannot crash, hang, or corrupt state.
	EXPECT_EQ(sdp.getRtpPort(), 5004);
	EXPECT_EQ(sdp.getMedia(), "m=audio 5004 RTP/AVP 0");
	EXPECT_EQ(sdp.getConnectionInformation(), "c=IN IP4 10.0.0.1");
}

// A body that is nothing but the attack: no recognised fields at all. Every
// accessor must report "absent" (empty view / port 0) rather than throw out of
// the middle of a SIP handler.
TEST(SipSdpHardening, PureAttackBodyYieldsNoFieldsAndDoesNotThrow)
{
	std::string body;
	for (int i = 0; i < 500; ++i) body += "a=acap:1\r\n";

	SipSdpMessage sdp(inviteWith(body), localAddr());

	EXPECT_EQ(sdp.getRtpPort(), 0);
	EXPECT_TRUE(sdp.getMedia().empty());
	EXPECT_TRUE(sdp.getVersion().empty());
	EXPECT_TRUE(sdp.getConnectionInformation().empty());
}
