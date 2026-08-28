// BroadcastHoldResume_test.cpp — Issue #74 regression coverage.
//
// A phone in a broadcast/ring-group (999-style) call that sends a re-INVITE to
// hold gets its offer relayed fine (onReinvite() never checked isBroadcast()),
// but the peer's 200 OK answering that re-INVITE used to be silently dropped:
// onOk()'s generic Connected/Held relay block explicitly excluded broadcast
// sessions (`!isBroadcast()`), and the broadcast-specific block below it only
// handles a first answer (state == Invited) — so a Held/Connected 200 OK fell
// through to nothing. The re-INVITE transaction then timed out client-side and
// the phone reported hold as failed even though the offer got through.
//
// This drives a real RequestsHandler through handle() end-to-end (mirroring
// Invite777SessionPool_test.cpp's pattern): register three extensions, place a
// 999 broadcast call, let one target answer (the other gets CANCELed — the
// ordinary, correct broadcast-connect behavior), then hold and resume the
// established call and assert the 200 OK for each actually reaches the FAR
// leg (not just that the offer/relay of the re-INVITE itself works, and not
// just that nothing crashes). It also pins the #69b fix: the connect/cancel
// path must never re-fire once the call is up, so exactly one CANCEL is ever
// sent across the whole hold/resume sequence.

#include <gtest/gtest.h>

#include <string>
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
	sockaddr_in addrFor(const std::string& ip, uint16_t port = 5060)
	{
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = inet_addr(ip.c_str());
		a.sin_port = htons(port);
		return a;
	}

	std::shared_ptr<SipMessage> makeRegister(const std::string& ext, const std::string& ip,
	                                          const std::string& callId)
	{
		std::string raw =
			"REGISTER sip:server SIP/2.0\r\n"
			"Via: SIP/2.0/UDP " + ip + ":5060;branch=z9hG4bKr" + callId + "\r\n"
			"From: <sip:" + ext + "@server>;tag=rt" + callId + "\r\n"
			"To: <sip:" + ext + "@server>\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 1 REGISTER\r\n"
			"Contact: <sip:" + ext + "@" + ip + ":5060>;expires=3600\r\n"
			"Content-Length: 0\r\n\r\n";
		return RequestsHandler::getMessageFromPool(raw, addrFor(ip));
	}

	std::string sdpBody(const char* direction)
	{
		std::string body =
			"v=0\r\n"
			"o=- 0 0 IN IP4 10.0.0.1\r\n"
			"s=-\r\n"
			"c=IN IP4 10.0.0.1\r\n"
			"t=0 0\r\n"
			"m=audio 10000 RTP/AVP 0\r\n"
			"a=rtpmap:0 PCMU/8000\r\n";
		if (direction && *direction)
		{
			body += std::string("a=") + direction + "\r\n";
		}
		return body;
	}

	std::shared_ptr<SipMessage> makeBroadcastInvite(const std::string& fromExt, const std::string& ip,
	                                                 const std::string& callId)
	{
		std::string body = sdpBody("sendrecv");
		std::string raw =
			"INVITE sip:999@server SIP/2.0\r\n"
			"Via: SIP/2.0/UDP " + ip + ":5060;branch=z9hG4bKi" + callId + "\r\n"
			"From: <sip:" + fromExt + "@server>;tag=ft" + callId + "\r\n"
			"To: <sip:999@server>\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 1 INVITE\r\n"
			"Max-Forwards: 70\r\n"
			"Contact: <sip:" + fromExt + "@" + ip + ":5060>\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		return RequestsHandler::getMessageFromPool(raw, addrFor(ip));
	}

	// Finds the most recent message this test recorded whose destination address
	// matches `addr` and whose serialized text contains `needle` (a request line
	// or status line fragment). Returns an empty string if none matches.
	std::string findSentTo(const std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>>& sent,
	                       const sockaddr_in& addr, const std::string& needle)
	{
		for (auto it = sent.rbegin(); it != sent.rend(); ++it)
		{
			if (it->first.sin_addr.s_addr != addr.sin_addr.s_addr) continue;
			if (it->first.sin_port != addr.sin_port) continue;
			if (!it->second) continue;
			std::string raw = it->second->toString();
			if (raw.find(needle) != std::string::npos) return raw;
		}
		return {};
	}

	size_t countContaining(const std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>>& sent,
	                       const std::string& needle)
	{
		size_t n = 0;
		for (const auto& [addr, msg] : sent)
		{
			if (!msg) continue;
			if (msg->toString().find(needle) != std::string::npos) ++n;
		}
		return n;
	}

	// Extracts a single header's full raw line (e.g. "To: <sip:999@server>;tag=x")
	// out of a serialized SIP message, so a hand-built follow-on request can reuse
	// dialog-identifying headers verbatim, the way a real UA would.
	std::string extractHeaderLine(const std::string& raw, const std::string& name)
	{
		size_t pos = 0;
		while (pos < raw.size())
		{
			size_t eol = raw.find("\r\n", pos);
			if (eol == std::string::npos) eol = raw.size();
			std::string line = raw.substr(pos, eol - pos);
			if (line.size() > name.size() &&
				line.compare(0, name.size(), name) == 0)
			{
				return line;
			}
			pos = eol + 2;
		}
		return {};
	}
}

TEST(BroadcastHoldResume, HoldThenResumeRelaysTheAnsweringPartysOkToTheHeldLeg)
{
	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> sent;
	RequestsHandler handler("192.168.20.1", 5060,
		[&sent](const sockaddr_in& addr, std::shared_ptr<SipMessage> msg) {
			sent.emplace_back(addr, std::move(msg));
		});

	const sockaddr_in callerAddr = addrFor("192.168.20.10");
	const sockaddr_in answererAddr = addrFor("192.168.20.20");
	const sockaddr_in losingAddr = addrFor("192.168.20.30");

	handler.handle(makeRegister("100", "192.168.20.10", "reg-100"));
	handler.handle(makeRegister("106", "192.168.20.20", "reg-106"));
	handler.handle(makeRegister("107", "192.168.20.30", "reg-107"));

	// ── Place the 999 broadcast call: forks to both 106 and 107 ────────────────
	const std::string callId = "bcast-74";
	handler.handle(makeBroadcastInvite("100", "192.168.20.10", callId));

	std::string forkTo106 = findSentTo(sent, answererAddr, "INVITE sip:106@");
	ASSERT_FALSE(forkTo106.empty()) << "999 broadcast must fork an INVITE to 106";
	std::string forkTo107 = findSentTo(sent, losingAddr, "INVITE sip:107@");
	ASSERT_FALSE(forkTo107.empty()) << "999 broadcast must fork an INVITE to 107";

	// ── 106 answers first: this is the ONLY leg allowed to run the connect path
	// (state == Invited). 107 must be CANCELed exactly once as a side effect. ──
	{
		std::string fromLine = extractHeaderLine(forkTo106, "From:");
		std::string via106   = extractHeaderLine(forkTo106, "Via:");
		ASSERT_FALSE(fromLine.empty());
		std::string body = sdpBody("sendrecv");
		std::string raw =
			"SIP/2.0 200 OK\r\n" +
			via106 + "\r\n" +
			fromLine + "\r\n"
			"To: <sip:106@server>;tag=ans106\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 1 INVITE\r\n"
			"Contact: <sip:106@192.168.20.20:5060>\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		handler.handle(RequestsHandler::getMessageFromPool(raw, answererAddr));
	}

	ASSERT_EQ(countContaining(sent, "CANCEL sip:107@"), 1u)
		<< "the losing fork target (107) must be CANCELed exactly once on connect";
	ASSERT_EQ(countContaining(sent, "CANCEL sip:106@"), 0u)
		<< "the answering target must never be CANCELed";

	auto sessionOpt = handler.getSession("Call-ID: " + callId);
	ASSERT_TRUE(sessionOpt.has_value());
	ASSERT_EQ(sessionOpt.value()->getState(), Session::State::Connected);
	ASSERT_TRUE(sessionOpt.value()->getDest());
	EXPECT_EQ(sessionOpt.value()->getDest()->getNumber(), "106");

	// Capture the 200 OK the CALLER received for the 999 INVITE — its To header
	// (original-To + the answerer's tag) is exactly what a real phone echoes
	// back, with the tag, on every subsequent re-INVITE in this dialog.
	std::string callerOk = findSentTo(sent, callerAddr, "SIP/2.0 200 OK");
	ASSERT_FALSE(callerOk.empty()) << "the caller must receive the 999 call's 200 OK";
	std::string dialogTo = extractHeaderLine(callerOk, "To:");
	ASSERT_NE(dialogTo.find("tag="), std::string::npos)
		<< "the caller's dialog To-header must carry the answerer's tag";

	// ── Caller places the established call on HOLD (re-INVITE, a=sendonly) ────
	{
		std::string body = sdpBody("sendonly");
		std::string raw =
			"INVITE sip:999@server SIP/2.0\r\n"
			"Via: SIP/2.0/UDP 192.168.20.10:5060;branch=z9hG4bKhold1\r\n"
			"From: <sip:100@server>;tag=ft" + callId + "\r\n" +
			dialogTo + "\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 2 INVITE\r\n"
			"Max-Forwards: 70\r\n"
			"Contact: <sip:100@192.168.20.10:5060>\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		handler.handle(RequestsHandler::getMessageFromPool(raw, callerAddr));
	}

	std::string holdOfferAt106 = findSentTo(sent, answererAddr, "a=sendonly");
	ASSERT_FALSE(holdOfferAt106.empty())
		<< "the hold offer must be relayed untouched to the held leg (106)";

	sessionOpt = handler.getSession("Call-ID: " + callId);
	ASSERT_TRUE(sessionOpt.has_value());
	EXPECT_EQ(sessionOpt.value()->getState(), Session::State::Held);
	ASSERT_TRUE(sessionOpt.value()->getDest());
	EXPECT_EQ(sessionOpt.value()->getDest()->getNumber(), "106")
		<< "hold must not re-run the broadcast connect path and reassign dest (#69b)";

	// ── 106 answers the hold re-INVITE with its own 200 OK ─────────────────────
	// THE REGRESSION: before the fix, onOk's Connected/Held relay explicitly
	// excluded broadcast sessions, and the broadcast-specific block only handles
	// state == Invited, so this 200 OK was silently dropped -- the caller's phone
	// never saw it and Timer D fired 32s later, reporting hold as failed.
	{
		std::string body = sdpBody("sendonly");
		std::string raw =
			"SIP/2.0 200 OK\r\n"
			"Via: SIP/2.0/UDP 192.168.20.10:5060;branch=z9hG4bKhold1\r\n"
			"From: <sip:100@server>;tag=ft" + callId + "\r\n" +
			dialogTo + "\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 2 INVITE\r\n"
			"Contact: <sip:106@192.168.20.20:5060>\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		handler.handle(RequestsHandler::getMessageFromPool(raw, answererAddr));
	}

	// Keyed on "CSeq: 2 INVITE", not the generic "SIP/2.0 200 OK" text: the
	// connect-time answer earlier in this same run also said 200 OK to the
	// caller, so a needle that could match it would pass even with the bug
	// (nothing sent for CSeq 2 ever reaches the caller without the fix).
	std::string holdOkAtCaller = findSentTo(sent, callerAddr, "CSeq: 2 INVITE");
	ASSERT_FALSE(holdOkAtCaller.empty())
		<< "#74: the hold re-INVITE's 200 OK must actually reach the caller, not be dropped";
	EXPECT_NE(holdOkAtCaller.find("SIP/2.0 200 OK"), std::string::npos)
		<< "the relayed message must be the hold transaction's 200 OK";

	// Still exactly one CANCEL in the whole run: the hold OK must not re-trigger
	// the broadcast connect/cancel path against the (already long gone) loser.
	EXPECT_EQ(countContaining(sent, "CANCEL sip:107@"), 1u)
		<< "the hold OK must not re-run the connect path and re-CANCEL 107 (#69b)";

	sessionOpt = handler.getSession("Call-ID: " + callId);
	ASSERT_TRUE(sessionOpt.has_value());
	EXPECT_EQ(sessionOpt.value()->getState(), Session::State::Held)
		<< "relaying the hold OK must not itself change hold state";

	// ── Caller RESUMES the call (re-INVITE, a=sendrecv) ────────────────────────
	{
		std::string body = sdpBody("sendrecv");
		std::string raw =
			"INVITE sip:999@server SIP/2.0\r\n"
			"Via: SIP/2.0/UDP 192.168.20.10:5060;branch=z9hG4bKresume1\r\n"
			"From: <sip:100@server>;tag=ft" + callId + "\r\n" +
			dialogTo + "\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 3 INVITE\r\n"
			"Max-Forwards: 70\r\n"
			"Contact: <sip:100@192.168.20.10:5060>\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		handler.handle(RequestsHandler::getMessageFromPool(raw, callerAddr));
	}

	std::string resumeOfferAt106 = findSentTo(sent, answererAddr, "CSeq: 3 INVITE");
	ASSERT_FALSE(resumeOfferAt106.empty())
		<< "the resume offer must be relayed to the held leg (106)";
	EXPECT_NE(resumeOfferAt106.find("a=sendrecv"), std::string::npos);

	sessionOpt = handler.getSession("Call-ID: " + callId);
	ASSERT_TRUE(sessionOpt.has_value());
	EXPECT_EQ(sessionOpt.value()->getState(), Session::State::Connected);

	// ── 106 answers the resume re-INVITE: the resume OK must also be relayed ──
	{
		std::string body = sdpBody("sendrecv");
		std::string raw =
			"SIP/2.0 200 OK\r\n"
			"Via: SIP/2.0/UDP 192.168.20.10:5060;branch=z9hG4bKresume1\r\n"
			"From: <sip:100@server>;tag=ft" + callId + "\r\n" +
			dialogTo + "\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 3 INVITE\r\n"
			"Contact: <sip:106@192.168.20.20:5060>\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		handler.handle(RequestsHandler::getMessageFromPool(raw, answererAddr));
	}

	std::string resumeOkAtCaller = findSentTo(sent, callerAddr, "CSeq: 3 INVITE");
	ASSERT_FALSE(resumeOkAtCaller.empty())
		<< "the resume re-INVITE's 200 OK must reach the caller too";
	EXPECT_NE(resumeOkAtCaller.find("SIP/2.0 200 OK"), std::string::npos);

	sessionOpt = handler.getSession("Call-ID: " + callId);
	ASSERT_TRUE(sessionOpt.has_value());
	EXPECT_EQ(sessionOpt.value()->getState(), Session::State::Connected)
		<< "resume must restore the Connected state";
	ASSERT_TRUE(sessionOpt.value()->getDest());
	EXPECT_EQ(sessionOpt.value()->getDest()->getNumber(), "106")
		<< "dest must remain the originally-answering leg throughout hold/resume";

	// Across the entire hold->resume sequence: never more than the one CANCEL
	// issued at connect time, and nothing sent to the long-gone loser (107).
	EXPECT_EQ(countContaining(sent, "CANCEL sip:107@"), 1u);
	EXPECT_TRUE(findSentTo(sent, losingAddr, "CSeq: 2 INVITE").empty())
		<< "the loser (107) must never receive any hold traffic";
	EXPECT_TRUE(findSentTo(sent, losingAddr, "CSeq: 3 INVITE").empty())
		<< "the loser (107) must never receive any resume traffic";
}
