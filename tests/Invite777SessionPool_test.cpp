// Invite777SessionPool_test.cpp — Issue #115 regression coverage.
//
// RequestsHandler::onInvite's destNumber == "777" branch (the SDP-loopback echo
// test) used to build and enqueue the 180 Ringing / 200 OK responses
// UNCONDITIONALLY, and only afterward call allocateSession(). If the session
// pool was already full, allocateSession() returned nullptr and the `if` body
// bookkeeping was simply skipped — but the caller had already been sent its
// 200 OK, with no _sessions entry ever created to back it. Compare the ordinary
// call-to-call INVITE path (~RequestsHandler.cpp:1009), which correctly calls
// allocateSession() FIRST and replies 503 Service Unavailable before ever
// building a success response.
//
// This drives a real RequestsHandler through handle() (mirroring the pattern
// in RequestsHandler_pool_test.cpp's end-to-end starved-pool test, but against
// the per-instance _sessionPool/_sessions rather than the process-global
// message pool): fill every one of POCKETDIAL_MAX_SESSIONS slots with ordinary
// calls, then send a 777 INVITE and assert it is refused with 503 — never a
// 200 — and that no new _sessions entry appears for it.

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "PoolConfig.hpp"
#include "RequestsHandler.hpp"

#if defined(_WIN32) || defined(_WIN64)
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#endif

namespace
{
	sockaddr_in addrFor(const std::string& ip)
	{
		sockaddr_in s{};
		s.sin_family = AF_INET;
		s.sin_addr.s_addr = inet_addr(ip.c_str());
		s.sin_port = htons(5060);
		return s;
	}

	std::shared_ptr<SipMessage> makeRegister(const std::string& ext, const std::string& srcIp,
	                                          const std::string& callId)
	{
		std::string raw =
			"REGISTER sip:server SIP/2.0\r\n"
			"Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bKr" + callId + "\r\n"
			"From: <sip:" + ext + "@server>;tag=rt" + callId + "\r\n"
			"To: <sip:" + ext + "@server>\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 1 REGISTER\r\n"
			"Contact: <sip:" + ext + "@" + srcIp + ":5060>;expires=3600\r\n"
			"Content-Length: 0\r\n\r\n";
		return RequestsHandler::getMessageFromPool(raw, addrFor(srcIp));
	}

	// A real, minimally-valid INVITE carrying an SDP offer — onInvite's ordinary
	// path requires hasSdp() (Content-Type: application/sdp) before it will even
	// reach allocateSession(); the 777 branch has no such gate, but building one
	// consistently for both keeps the fill calls and the echo call identical in
	// shape.
	std::shared_ptr<SipMessage> makeInvite(const std::string& fromExt, const std::string& toExt,
	                                        const std::string& srcIp, const std::string& callId)
	{
		std::string body =
			"v=0\r\n"
			"o=- 0 0 IN IP4 " + srcIp + "\r\n"
			"s=-\r\n"
			"c=IN IP4 " + srcIp + "\r\n"
			"t=0 0\r\n"
			"m=audio 10000 RTP/AVP 0\r\n"
			"a=rtpmap:0 PCMU/8000\r\n";
		std::string raw =
			"INVITE sip:" + toExt + "@server SIP/2.0\r\n"
			"Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bKi" + callId + "\r\n"
			"From: <sip:" + fromExt + "@server>;tag=ft" + callId + "\r\n"
			"To: <sip:" + toExt + "@server>\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 1 INVITE\r\n"
			"Max-Forwards: 70\r\n"
			"Contact: <sip:" + fromExt + "@" + srcIp + ":5060>\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		return RequestsHandler::getMessageFromPool(raw, addrFor(srcIp));
	}
}

TEST(Invite777SessionPool, RefusedWith503NotFalse200WhenSessionPoolIsFull)
{
	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> sent;
	RequestsHandler handler("192.168.7.1", 5060,
		[&sent](const sockaddr_in& addr, std::shared_ptr<SipMessage> msg) {
			sent.emplace_back(addr, std::move(msg));
		});

	handler.handle(makeRegister("500", "192.168.7.50", "reg-500"));
	handler.handle(makeRegister("600", "192.168.7.60", "reg-600"));

	// Drive every one of POCKETDIAL_MAX_SESSIONS slots full with ordinary
	// 500 -> 600 calls (the correct/reference path — allocateSession() first,
	// 503 on nullptr — already gates these and is not under test here).
	//
	// getSessionCount()/getClientCount() read the dashboard's _snapshot, which is
	// only rebuilt from _sessions inside tick() -- not synchronously by handle().
	// This test never calls tick(), so pool exhaustion is verified against the
	// live map via getSession() instead, which reads straight from _sessions.
	// getSession() keys on exactly what data->getCallID() returns, which (see
	// SipMessage::getCallID()) is the FULL raw header line, not just the value --
	// hence the "Call-ID: " prefix on every lookup key below.
	std::vector<std::string> fillCallIds;
	for (int i = 0; i < POCKETDIAL_MAX_SESSIONS; ++i)
	{
		std::string callId = "fill-" + std::to_string(i);
		handler.handle(makeInvite("500", "600", "192.168.7.50", callId));
		ASSERT_TRUE(handler.getSession("Call-ID: " + callId).has_value())
			<< "fill call " << callId << " did not create a session -- test setup is "
			   "broken, so the real assertions below would prove nothing";
		fillCallIds.push_back(std::move(callId));
	}

	sent.clear();   // isolate exactly what the 777 INVITE below produces

	const std::string echoCallId = "echo-over-limit";
	handler.handle(makeInvite("500", "777", "192.168.7.50", echoCallId));

	ASSERT_FALSE(sent.empty()) << "777 INVITE against a full session pool produced no response";

	bool saw503 = false;
	bool saw200 = false;
	bool saw180 = false;
	for (const auto& [addr, msg] : sent)
	{
		std::string raw = msg ? msg->toString() : std::string{};
		if (raw.find("503 Service Unavailable") != std::string::npos) saw503 = true;
		if (raw.find("SIP/2.0 200 OK") != std::string::npos) saw200 = true;
		if (raw.find("180 Ringing") != std::string::npos) saw180 = true;
	}

	EXPECT_TRUE(saw503)
		<< "a full session pool must produce a real 503 Service Unavailable for the "
		   "777 echo test, exactly as the ordinary call-to-call INVITE path does";
	EXPECT_FALSE(saw200)
		<< "issue #115: must never get a false 200 OK for 777 when the session pool "
		   "is exhausted";
	EXPECT_FALSE(saw180)
		<< "no 180 Ringing should be sent either -- nothing should be built before "
		   "a session slot is confirmed";

	EXPECT_FALSE(handler.getSession("Call-ID: " + echoCallId).has_value())
		<< "no _sessions entry may exist for a Call-ID that was answered with 503";

	// The refused 777 call must not have disturbed the sessions already holding
	// every pool slot.
	for (const auto& callId : fillCallIds)
	{
		EXPECT_TRUE(handler.getSession("Call-ID: " + callId).has_value())
			<< "pre-existing session " << callId << " should not have been touched "
			   "by the refused 777 INVITE";
	}
}
