// AdminHttpGate_test.cpp — Phase 0 of docs/PLAN_ADMIN_HTTP_ONLY.md: freezes the
// dark-by-default HTTP admin plane's test contract before any implementation.
//
// Tests prefixed DISABLED_ document behavior that lands in a later phase (named
// in each body). GoogleTest compiles DISABLED_ tests but does not run them by
// default, so this file must compile clean today without adding any of the
// production symbols those later phases introduce (RequestsHandler's
// _adminHttpOpenUntilMs atomic/getter, HttpServer's demand-activation, the
// onDtmfInfo code=="010" branch). Each DISABLED_ test is un-prefixed and given a
// real body in the phase whose "Done when" section names it — see the plan doc.

#include <gtest/gtest.h>
#include "HttpServer.hpp"
#include "RequestsHandler.hpp"
#include "SipMessage.hpp"
#include "AdminAuth.hpp"

#if defined(_WIN32) || defined(_WIN64)
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <chrono>
#include <thread>

namespace
{
	// True iff a TCP connect to 127.0.0.1:port succeeds within the OS's default
	// connect timeout. Used to observe HttpServer's actual listen state from the
	// outside, the same way a real admin browser would.
	bool canConnect(int port)
	{
#if defined(_WIN32) || defined(_WIN64)
		SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET) return false;
#else
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0) return false;
#endif
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<uint16_t>(port));
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#if defined(_WIN32) || defined(_WIN64)
		closesocket(s);
#else
		close(s);
#endif
		return rc == 0;
	}

	// Minimal blocking HTTP POST over a raw socket. Returns the full raw
	// response (status line + headers + body) so callers can extract a
	// Set-Cookie value, or just the status code. `cookie`, if non-empty, is
	// sent as a Cookie header. Used to drive HttpServer's real endpoints
	// end-to-end (not just AdminAuth directly), since the provisioning grace
	// window and keepalive are wired into the HTTP handler, not AdminAuth.
	std::string httpPostRaw(int port, const std::string& path, const std::string& body,
	                        const std::string& cookie = "",
	                        const std::string& csrf = "",
	                        const std::string& origin = "")
	{
#if defined(_WIN32) || defined(_WIN64)
		SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET) return "";
#else
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0) return "";
#endif
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<uint16_t>(port));
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
		{
#if defined(_WIN32) || defined(_WIN64)
			closesocket(s);
#else
			close(s);
#endif
			return "";
		}
		std::string cookieHeader = cookie.empty() ? "" : ("Cookie: " + cookie + "\r\n");
		std::string csrfHeader   = csrf.empty()   ? "" : ("X-CSRF: " + csrf + "\r\n");
		std::string originHeader = origin.empty() ? "" : ("Origin: " + origin + "\r\n");
		std::string req = "POST " + path + " HTTP/1.1\r\n"
			"Host: 127.0.0.1\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n"
			"Content-Type: application/x-www-form-urlencoded\r\n" +
			cookieHeader + csrfHeader + originHeader +
			"Connection: close\r\n\r\n" + body;
		send(s, req.c_str(), static_cast<int>(req.size()), 0);
		std::string resp;
		char buf[512];
		int n;
		while ((n = recv(s, buf, sizeof(buf), 0)) > 0)
		{
			resp.append(buf, static_cast<size_t>(n));
		}
#if defined(_WIN32) || defined(_WIN64)
		closesocket(s);
#else
		close(s);
#endif
		return resp;
	}

	// GET counterpart of httpPostRaw. `origin`, when set, exercises the
	// same-origin gate that the read endpoints (/api/pcap and friends) were
	// previously missing.
	std::string httpGetRaw(int port, const std::string& path,
	                       const std::string& cookie = "",
	                       const std::string& origin = "")
	{
#if defined(_WIN32) || defined(_WIN64)
		SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET) return "";
#else
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0) return "";
#endif
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<uint16_t>(port));
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
		{
#if defined(_WIN32) || defined(_WIN64)
			closesocket(s);
#else
			close(s);
#endif
			return "";
		}
		std::string cookieHeader = cookie.empty() ? "" : ("Cookie: " + cookie + "\r\n");
		std::string originHeader = origin.empty() ? "" : ("Origin: " + origin + "\r\n");
		std::string req = "GET " + path + " HTTP/1.1\r\n"
			"Host: 127.0.0.1\r\n" +
			cookieHeader + originHeader +
			"Connection: close\r\n\r\n";
		send(s, req.c_str(), static_cast<int>(req.size()), 0);
		std::string resp;
		char buf[512];
		int n;
		while ((n = recv(s, buf, sizeof(buf), 0)) > 0)
		{
			resp.append(buf, static_cast<size_t>(n));
		}
#if defined(_WIN32) || defined(_WIN64)
		closesocket(s);
#else
		close(s);
#endif
		return resp;
	}

	// Pulls the per-session CSRF token out of the login response body
	// ({"status":"ok","authenticated":true,"csrf":"<32 hex>"}).
	std::string csrfOf(const std::string& resp)
	{
		const std::string marker = "\"csrf\":\"";
		size_t pos = resp.find(marker);
		if (pos == std::string::npos) return "";
		size_t start = pos + marker.size();
		size_t end = resp.find('"', start);
		if (end == std::string::npos) return "";
		return resp.substr(start, end - start);
	}

	int statusOf(const std::string& resp)
	{
		size_t sp1 = resp.find(' ');
		if (sp1 == std::string::npos) return -1;
		size_t sp2 = resp.find(' ', sp1 + 1);
		if (sp2 == std::string::npos) return -1;
		return std::atoi(resp.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
	}

	// Extracts just the VALUE from a "Set-Cookie: name=value; ..." response
	// header, empty if absent.
	std::string cookieOf(const std::string& resp, const std::string& name)
	{
		std::string marker = "Set-Cookie: " + name + "=";
		size_t pos = resp.find(marker);
		if (pos == std::string::npos) return "";
		size_t start = pos + marker.size();
		size_t end = resp.find(';', start);
		if (end == std::string::npos) end = resp.find("\r\n", start);
		if (end == std::string::npos) return "";
		return resp.substr(start, end - start);
	}

	int httpPostStatus(int port, const std::string& path, const std::string& body)
	{
		return statusOf(httpPostRaw(port, path, body));
	}

	// -- DTMF trigger helpers (Phase 3) ----------------------------------------

	std::shared_ptr<SipMessage> makeRegisterFor(const std::string& from, const std::string& srcIp,
	                                             const std::string& callId)
	{
		sockaddr_in s{}; s.sin_family = AF_INET;
		s.sin_addr.s_addr = inet_addr(srcIp.c_str());
		s.sin_port = htons(5060);
		std::string raw =
			"REGISTER sip:server SIP/2.0\r\n"
			"Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bKr\r\n"
			"From: <sip:" + from + "@server>;tag=rt\r\n"
			"To: <sip:" + from + "@server>\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 1 REGISTER\r\n"
			"Contact: <sip:" + from + "@" + srcIp + ":5060>;expires=3600\r\n"
			"Content-Length: 0\r\n\r\n";
		return RequestsHandler::getMessageFromPool(raw, s);
	}

	// One SIP INFO carrying a single DTMF digit (RFC 2833/6086 application/dtmf-relay).
	std::shared_ptr<SipMessage> makeInfoDigit(const std::string& from, const std::string& srcIp,
	                                           const std::string& callId, char digit)
	{
		sockaddr_in s{}; s.sin_family = AF_INET;
		s.sin_addr.s_addr = inet_addr(srcIp.c_str());
		s.sin_port = htons(5060);
		std::string body = std::string("Signal=") + digit + "\r\nDuration=100\r\n";
		std::string head =
			"INFO sip:server SIP/2.0\r\n"
			"Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bKi\r\n"
			"From: <sip:" + from + "@server>;tag=it\r\n"
			"To: <sip:server>\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 1 INFO\r\n"
			"Content-Type: application/dtmf-relay\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
		return RequestsHandler::getMessageFromPool(head + body, s);
	}

	// Sends `seq` (e.g. "*123456#010") as one INFO per character, all sharing one
	// Call-ID, mirroring how a real handset accumulates DTMF digit by digit.
	void sendDtmfSequence(RequestsHandler& handler, const std::string& from,
	                      const std::string& srcIp, const std::string& callId,
	                      const std::string& seq)
	{
		for (char c : seq)
		{
			handler.handle(makeInfoDigit(from, srcIp, callId, c));
		}
	}
}

// ── Boot behavior ────────────────────────────────────────────────────────────

TEST(AdminHttpGate, Boot_Unprovisioned_ListensImmediately)
{
	// Regression pin for today's actual, pre-Phase-2 behavior: an unprovisioned
	// device holds SIP dark until a credential is committed via the web UI, so
	// HttpServer must accept connections unconditionally. This must keep passing
	// unchanged through every later phase.
	AdminAuth::clearCredential();
	ASSERT_FALSE(AdminAuth::isProvisioned());

	HttpServer server("127.0.0.1", 18080, nullptr);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_TRUE(canConnect(18080));
}

TEST(AdminHttpGate, SetPin_GrantsGraceWindow_StaysReachable)
{
	// Regression guard for a gap the plan itself didn't cover: setting a PIN is
	// exactly what flips AdminAuth::isProvisioned() to true, which is the
	// condition that puts the dark-by-default gate into effect. Without the
	// grace-window grant in sendApiAdminSetPin, the operator who just used the
	// web UI to provision the device would lose HTTP access on the very next
	// accept-loop tick, before they could finish onboarding.
	AdminAuth::clearCredential();
	ASSERT_FALSE(AdminAuth::isProvisioned());

	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18083, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	ASSERT_TRUE(canConnect(18083));

	int setPinStatus = httpPostStatus(18083, "/api/admin/set-pin", "pin=123456");
	ASSERT_EQ(setPinStatus, 200);
	ASSERT_TRUE(AdminAuth::isProvisioned());

	// Give the accept-loop several ticks to have re-evaluated open/closed.
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	EXPECT_TRUE(canConnect(18083))
		<< "HTTP must stay reachable immediately after provisioning so onboarding can continue";

	AdminAuth::clearCredential();
}

TEST(AdminHttpGate, Boot_Provisioned_DoesNotListen)
{
	// A provisioned device (a PIN has been set) must NOT accept connections
	// without a live admin-open deadline — invariant I1, fail closed.
	AdminAuth::clearCredential();
	ASSERT_TRUE(AdminAuth::setPin("123456"));
	ASSERT_TRUE(AdminAuth::isProvisioned());

	HttpServer server("127.0.0.1", 18081, nullptr);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_FALSE(canConnect(18081));

	AdminAuth::clearCredential();
}

// ── DTMF trigger ─────────────────────────────────────────────────────────────
// Trigger is *4887 (spells HTTP on a phone keypad), no PIN — *<PIN>#010 was
// dropped because '#' is bound to Send/Call on Yealink and most SIP hardphones
// (pre-dial AND mid-call), so a PIN+# sequence never reaches the DTMF-relay
// path intact on real hardware. Trust shifts to: registered as the admin
// extension + signaling from that registration's bound IP.

TEST(AdminHttpGate, Trigger_ValidStarCodeCorrectExtRegisteredMatchingIp_Opens)
{
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});

	// getAdminExt() defaults to "1001" as of Phase 0.
	handler.handle(makeRegisterFor(handler.getAdminExt(), "192.168.4.50", "reg-1"));

	sendDtmfSequence(handler, handler.getAdminExt(), "192.168.4.50", "dtmf-1", "*4887");

	uint64_t deadline = handler.getAdminHttpOpenUntilMs();
	EXPECT_NE(deadline, 0u) << "a valid trigger from the registered admin IP must set a deadline";
}

TEST(AdminHttpGate, Trigger_WrongExt_StaysClosed)
{
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	// Register and trigger from a DIFFERENT extension than getAdminExt() — the
	// outer `callerExt == _adminExt` gate must reject.
	handler.handle(makeRegisterFor("222", "192.168.4.60", "reg-2"));
	sendDtmfSequence(handler, "222", "192.168.4.60", "dtmf-2", "*4887");

	EXPECT_EQ(handler.getAdminHttpOpenUntilMs(), 0u);
}

TEST(AdminHttpGate, Trigger_RightExtNotRegistered_StaysClosed)
{
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	// No REGISTER sent at all — findClient(_adminExt) (internal) must return nullopt.
	sendDtmfSequence(handler, handler.getAdminExt(), "192.168.4.50", "dtmf-1", "*4887");

	EXPECT_EQ(handler.getAdminHttpOpenUntilMs(), 0u);
}

TEST(AdminHttpGate, Trigger_RightExtRegistered_SourceIpMismatch_StaysClosed)
{
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	// Registered from .50, but the DTMF trigger arrives from .99 — a spoofed
	// From-header replay from an attacker who doesn't control the real handset's
	// IP. Must reject this even though the extension matches.
	handler.handle(makeRegisterFor(handler.getAdminExt(), "192.168.4.50", "reg-1"));
	sendDtmfSequence(handler, handler.getAdminExt(), "192.168.4.99", "dtmf-1", "*4887");

	EXPECT_EQ(handler.getAdminHttpOpenUntilMs(), 0u);
}

TEST(AdminHttpGate, Trigger_ExpiresAfterTtl_ClosesAutomatically)
{
	// Exercises Phase 2's accept-loop gating directly via the test-only deadline
	// setter — no real DTMF trigger yet (that's Phase 3's onDtmfInfo code=="010"
	// branch, which just needs to call the same setter with a real deadline).
	AdminAuth::clearCredential();
	ASSERT_TRUE(AdminAuth::setPin("123456"));
	ASSERT_TRUE(AdminAuth::isProvisioned());

	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});

	HttpServer server("127.0.0.1", 18082, nullptr);
	server.attachHandler(&handler);
	server.start();

	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_FALSE(canConnect(18082)) << "closed before any deadline is set";

	uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
	handler.setAdminHttpOpenUntilMsForTest(now + 600);
	std::this_thread::sleep_for(std::chrono::milliseconds(400));
	EXPECT_TRUE(canConnect(18082)) << "open within the deadline window";

	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	EXPECT_FALSE(canConnect(18082)) << "closed again once the deadline lapses";

	AdminAuth::clearCredential();
}

TEST(AdminHttpGate, Trigger_SecondTriggerBeforeExpiry_ExtendsDeadline)
{
	// Documented default: a second trigger before expiry EXTENDS the deadline
	// rather than shortening the window (plan Phase 0 rationale: a surprise
	// early close mid-session is worse than a longer one). The implementation
	// always writes now()+ttl on a successful trigger, so this holds by
	// construction — this test pins that behavior against regression.
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	handler.handle(makeRegisterFor(handler.getAdminExt(), "192.168.4.50", "reg-1"));

	sendDtmfSequence(handler, handler.getAdminExt(), "192.168.4.50", "dtmf-1", "*4887");
	uint64_t firstDeadline = handler.getAdminHttpOpenUntilMs();
	ASSERT_NE(firstDeadline, 0u);

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	sendDtmfSequence(handler, handler.getAdminExt(), "192.168.4.50", "dtmf-2", "*4887");
	uint64_t secondDeadline = handler.getAdminHttpOpenUntilMs();

	EXPECT_GT(secondDeadline, firstDeadline);
}

TEST(AdminHttpGate, KeepAlive_Authenticated_ExtendsWindowOneHour)
{
	// The DTMF trigger's TTL is short (default 10 min) by design. An operator
	// already authenticated via a valid session can push the window out by a
	// full hour at a time instead of walking back to the admin handset.
	AdminAuth::clearCredential();
	ASSERT_FALSE(AdminAuth::isProvisioned());

	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18084, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	ASSERT_TRUE(canConnect(18084)) << "must be reachable pre-provisioning to set the PIN";

	// Provision via the real endpoint (not AdminAuth::setPin directly) — the
	// grace-window grant is a side effect of sendApiAdminSetPin, not of
	// AdminAuth::setPin itself.
	ASSERT_EQ(httpPostStatus(18084, "/api/admin/set-pin", "pin=123456"), 200);
	ASSERT_TRUE(AdminAuth::isProvisioned());

	std::string loginResp = httpPostRaw(18084, "/api/admin/login", "pin=123456");
	ASSERT_EQ(statusOf(loginResp), 200);
	std::string cookie = cookieOf(loginResp, "pd_session");
	ASSERT_FALSE(cookie.empty()) << "login must issue a pd_session cookie";
	// Keepalive moves the transport open deadline, so it is a mutating request
	// and now carries the per-session CSRF token like every other one.
	std::string csrf = csrfOf(loginResp);
	ASSERT_FALSE(csrf.empty()) << "login must return the session CSRF token";

	uint64_t beforeMs = handler.getAdminHttpOpenUntilMs();
	ASSERT_NE(beforeMs, 0u) << "set-pin's grace window must already be open";

	uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());

	std::string keepAliveResp = httpPostRaw(18084, "/api/admin/keepalive", "",
	                                        "pd_session=" + cookie, csrf);
	EXPECT_EQ(statusOf(keepAliveResp), 200);

	uint64_t afterMs = handler.getAdminHttpOpenUntilMs();
	EXPECT_GT(afterMs, beforeMs) << "keepalive must push the deadline further out";
	// Should land close to now+3600s, not the (much shorter) grace-window TTL.
	EXPECT_GT(afterMs, nowMs + 3500ULL * 1000ULL);
	EXPECT_LE(afterMs, nowMs + 3700ULL * 1000ULL);

	AdminAuth::clearCredential();
}

TEST(AdminHttpGate, KeepAlive_Unauthenticated_Rejected401)
{
	// Belt-and-suspenders: same assertion as the second half of the test above,
	// isolated so a future refactor of that test can't silently drop this case.
	AdminAuth::clearCredential();
	ASSERT_FALSE(AdminAuth::isProvisioned());

	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18085, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	ASSERT_EQ(httpPostStatus(18085, "/api/admin/set-pin", "pin=123456"), 200);

	int status = httpPostStatus(18085, "/api/admin/keepalive", "");
	EXPECT_EQ(status, 401);

	AdminAuth::clearCredential();
}

TEST(AdminHttpGate, Trigger_ContinuedEntryAfterStarCodeLogsShadowedPinWarning)
{
	// Issue #93: a PIN beginning "4887" (only possible on a device provisioned
	// before SetPin_RejectsReservedStarCodePrefix's guard existed) is shadowed —
	// "*4887" matches and clears the accumulator before the admin finishes
	// dialing *PIN#code. Simulate PIN "48871234" + code "001": the first four
	// PIN digits complete "*4887" (fires, accumulator resets with no leading
	// '*'), then the admin keeps dialing the rest ("8871234#001") into the same
	// dialog. That must produce a targeted warning rather than silently eating
	// the rest of the entry.
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	handler.handle(makeRegisterFor(handler.getAdminExt(), "192.168.4.50", "reg-1"));

	testing::internal::CaptureStderr();
	sendDtmfSequence(handler, handler.getAdminExt(), "192.168.4.50", "dtmf-race", "*4887");
	sendDtmfSequence(handler, handler.getAdminExt(), "192.168.4.50", "dtmf-race", "8871234#001");
	std::string err = testing::internal::GetCapturedStderr();

	EXPECT_NE(err.find("interrupted"), std::string::npos) << err;
	EXPECT_NE(err.find("4887"), std::string::npos) << err;

	// One-shot: a second, unrelated '#'-shaped continuation on the SAME dialog
	// must not warn again (starCodeFiredAtTick was consumed by the first).
	testing::internal::CaptureStderr();
	sendDtmfSequence(handler, handler.getAdminExt(), "192.168.4.50", "dtmf-race", "5678#002");
	std::string err2 = testing::internal::GetCapturedStderr();
	EXPECT_EQ(err2.find("interrupted"), std::string::npos) << err2;
}

// A star-code trigger with no PIN-shaped continuation afterward (the normal
// *4887-only case, already covered above) must never warn.
TEST(AdminHttpGate, Trigger_PlainStarCodeWithNoContinuationDoesNotWarn)
{
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	handler.handle(makeRegisterFor(handler.getAdminExt(), "192.168.4.50", "reg-1"));

	testing::internal::CaptureStderr();
	sendDtmfSequence(handler, handler.getAdminExt(), "192.168.4.50", "dtmf-plain", "*4887");
	std::string err = testing::internal::GetCapturedStderr();

	EXPECT_EQ(err.find("interrupted"), std::string::npos) << err;
}

TEST(AdminHttpGate, SetPin_RejectsReservedStarCodePrefix)
{
	// The DTMF HTTP-open trigger is the star-code *4887 (no '#'), matched by
	// onDtmfInfo the instant the accumulated sequence equals "*4887" — before
	// the *PIN#code parser. A PIN beginning with those four digits is therefore
	// unusable over DTMF (the star-code fires mid-entry and clears the
	// accumulator), so set-pin must reject that prefix rather than silently
	// provision an admin who can never drive the DTMF admin menu.
	AdminAuth::clearCredential();
	ASSERT_FALSE(AdminAuth::isProvisioned());

	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18086, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// A 4887-prefixed PIN is rejected and the device stays unprovisioned.
	EXPECT_EQ(httpPostStatus(18086, "/api/admin/set-pin", "pin=48871234"), 400);
	EXPECT_FALSE(AdminAuth::isProvisioned())
		<< "a rejected PIN must not provision the device";

	// A PIN that merely contains 4887 later on is fine — only the leading
	// four digits collide with the star-code.
	EXPECT_EQ(httpPostStatus(18086, "/api/admin/set-pin", "pin=14887"), 200);
	EXPECT_TRUE(AdminAuth::isProvisioned());

	AdminAuth::clearCredential();
}

// ─────────────────────────────────────────────────────────────────────────────
// CSRF tokens, the centralised gate, and the security headers.
//
// The same-origin check admits a request with NO Origin header by design (curl,
// native clients and tests/http/test_api.sh send none), so on a provisioned
// device it is the per-session token — rendered into the page, never set as a
// cookie — that actually stops a same-site page from driving mutating calls.
// ─────────────────────────────────────────────────────────────────────────────

TEST(WebHardening, Csrf_MissingToken_Rejected403)
{
	AdminAuth::clearCredential();
	// A real RequestsHandler is required: once a PIN exists the listen socket is
	// dark by default and only opens inside an admin-open window, which set-pin
	// grants as a grace period through the handler. With a null handler the
	// deadline reads 0, the server fails closed, and the test would be measuring
	// a refused connection rather than the gate.
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18090, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	ASSERT_EQ(httpPostStatus(18090, "/api/admin/set-pin", "pin=123456"), 200);
	std::string loginResp = httpPostRaw(18090, "/api/admin/login", "pin=123456");
	ASSERT_EQ(statusOf(loginResp), 200);
	std::string cookie = cookieOf(loginResp, "pd_session");
	ASSERT_FALSE(cookie.empty());

	// A valid session but no token: the cookie alone must not be enough.
	std::string resp = httpPostRaw(18090, "/api/ap-security", "regenerate=1",
	                               "pd_session=" + cookie);
	EXPECT_EQ(statusOf(resp), 403);

	AdminAuth::clearCredential();
}

TEST(WebHardening, Csrf_WrongToken_Rejected403)
{
	AdminAuth::clearCredential();
	// A real RequestsHandler is required: once a PIN exists the listen socket is
	// dark by default and only opens inside an admin-open window, which set-pin
	// grants as a grace period through the handler. With a null handler the
	// deadline reads 0, the server fails closed, and the test would be measuring
	// a refused connection rather than the gate.
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18091, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	ASSERT_EQ(httpPostStatus(18091, "/api/admin/set-pin", "pin=123456"), 200);
	std::string loginResp = httpPostRaw(18091, "/api/admin/login", "pin=123456");
	std::string cookie = cookieOf(loginResp, "pd_session");
	ASSERT_FALSE(cookie.empty());

	std::string resp = httpPostRaw(18091, "/api/ap-security", "regenerate=1",
	                               "pd_session=" + cookie,
	                               "00000000000000000000000000000000");
	EXPECT_EQ(statusOf(resp), 403);

	AdminAuth::clearCredential();
}

TEST(WebHardening, Csrf_ValidToken_Accepted)
{
	AdminAuth::clearCredential();
	// A real RequestsHandler is required: once a PIN exists the listen socket is
	// dark by default and only opens inside an admin-open window, which set-pin
	// grants as a grace period through the handler. With a null handler the
	// deadline reads 0, the server fails closed, and the test would be measuring
	// a refused connection rather than the gate.
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18092, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	ASSERT_EQ(httpPostStatus(18092, "/api/admin/set-pin", "pin=123456"), 200);
	std::string loginResp = httpPostRaw(18092, "/api/admin/login", "pin=123456");
	std::string cookie = cookieOf(loginResp, "pd_session");
	std::string csrf   = csrfOf(loginResp);
	ASSERT_FALSE(cookie.empty());
	ASSERT_EQ(csrf.size(), AdminAuth::kCsrfTokenHex);

	std::string resp = httpPostRaw(18092, "/api/ap-security", "regenerate=1",
	                               "pd_session=" + cookie, csrf);
	EXPECT_EQ(statusOf(resp), 200);

	AdminAuth::clearCredential();
}

TEST(WebHardening, Csrf_NotRequiredWhileUnprovisioned)
{
	// The onboarding window must keep working exactly as before: a factory-fresh
	// device has no session, so there is no token to bind and demanding one would
	// make the device unclaimable (and break tests/http/test_api.sh).
	AdminAuth::clearCredential();
	HttpServer server("127.0.0.1", 18093, nullptr);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	ASSERT_FALSE(AdminAuth::isProvisioned());
	EXPECT_EQ(httpPostStatus(18093, "/api/configuring", ""), 200);

	AdminAuth::clearCredential();
}

TEST(WebHardening, PcapAndTrace_RejectCrossOrigin)
{
	// Regression: these three served raw SIP bytes — Contact URIs, User-Agent
	// strings and Authorization digests — with a session check but no
	// same-origin check, because each route open-coded its own gate and this one
	// clause was missed. They now go through requireAdmin like everything else.
	AdminAuth::clearCredential();
	HttpServer server("127.0.0.1", 18094, nullptr);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	const std::string evil = "http://evil.example";
	EXPECT_EQ(statusOf(httpGetRaw(18094, "/api/pcap", "", evil)), 403);
	EXPECT_EQ(statusOf(httpGetRaw(18094, "/api/trace", "", evil)), 403);
	EXPECT_EQ(statusOf(httpGetRaw(18094, "/api/diagnostics/pcap", "", evil)), 403);

	// Same-origin (no Origin header at all) still reaches the handler.
	EXPECT_EQ(statusOf(httpGetRaw(18094, "/api/trace")), 200);

	AdminAuth::clearCredential();
}

TEST(WebHardening, Configuring_GatedOnceProvisioned)
{
	// /api/configuring previously had no gate at all, waived in a comment as
	// "harmless". It still moves device state on a POST, so once the device has
	// an owner it needs the owner's session.
	AdminAuth::clearCredential();
	// A real RequestsHandler is required: once a PIN exists the listen socket is
	// dark by default and only opens inside an admin-open window, which set-pin
	// grants as a grace period through the handler. With a null handler the
	// deadline reads 0, the server fails closed, and the test would be measuring
	// a refused connection rather than the gate.
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18095, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	ASSERT_EQ(httpPostStatus(18095, "/api/admin/set-pin", "pin=123456"), 200);
	ASSERT_TRUE(AdminAuth::isProvisioned());

	EXPECT_EQ(httpPostStatus(18095, "/api/configuring", ""), 401);

	AdminAuth::clearCredential();
}

TEST(WebHardening, SecurityHeadersOnEveryResponse)
{
	AdminAuth::clearCredential();
	HttpServer server("127.0.0.1", 18096, nullptr);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// A read endpoint and an error response: the headers are emitted centrally in
	// sendResponseWithHeader, so both must carry them.
	const std::string ok  = httpGetRaw(18096, "/api/status");
	const std::string err = httpGetRaw(18096, "/api/pcap", "", "http://evil.example");

	for (const std::string* r : {&ok, &err})
	{
		EXPECT_NE(r->find("Content-Security-Policy:"), std::string::npos);
		EXPECT_NE(r->find("X-Frame-Options: DENY"), std::string::npos);
		EXPECT_NE(r->find("X-Content-Type-Options: nosniff"), std::string::npos);
		EXPECT_NE(r->find("Cache-Control: no-store"), std::string::npos);
		EXPECT_NE(r->find("Referrer-Policy: same-origin"), std::string::npos);
		// No HSTS: the dashboard is plain HTTP on a LAN appliance, and pinning it
		// would make the host permanently unreachable over http://.
		EXPECT_EQ(r->find("Strict-Transport-Security"), std::string::npos);
	}

	AdminAuth::clearCredential();
}

TEST(WebHardening, LockoutCounterDoesNotResetOnTrip)
{
	// Regression: verifyPin used to zero the failure counter when the lockout
	// engaged, so every cooldown handed the attacker a fresh window of
	// kMaxFailedAttempts — a steady ~5 guesses/minute for as long as they cared
	// to keep going. The trip count now survives, so each lockout is longer than
	// the last and only a correct PIN clears it.
	AdminAuth::clearCredential();
	ASSERT_TRUE(AdminAuth::setPin("123456"));

	for (int i = 0; i < AdminAuth::kMaxFailedAttempts; ++i)
	{
		EXPECT_FALSE(AdminAuth::verifyPin("000000", "10.0.0.1"));
	}
	EXPECT_TRUE(AdminAuth::isLockedOut("10.0.0.1"));

	// Even the correct PIN is refused while the cooldown is engaged.
	EXPECT_FALSE(AdminAuth::verifyPin("123456", "10.0.0.1"));

	// ...and a different client is unaffected, which is the point of keying the
	// buckets: one guesser must not lock the real admin out of new logins.
	EXPECT_FALSE(AdminAuth::isLockedOut("10.0.0.2"));
	EXPECT_TRUE(AdminAuth::verifyPin("123456", "10.0.0.2"));

	AdminAuth::clearCredential();
}

TEST(WebHardening, GlobalBackstopBoundsSpoofedSourceAddresses)
{
	// Per-client buckets on their own would be a REGRESSION on a shared link:
	// a source address is trivially spoofable, so an attacker who rotates
	// addresses would get a fresh bucket — and a fresh escalation ladder — every
	// five guesses, which is a better position than the single global counter
	// this replaced. The aggregate backstop is what makes per-client accounting
	// safe to have: it bounds the total guess rate regardless of how many
	// identities the attacker invents.
	AdminAuth::clearCredential();
	ASSERT_TRUE(AdminAuth::setPin("123456"));

	// Burn the aggregate budget across several distinct "clients". Each one trips
	// its own bucket after kMaxFailedAttempts, so this is exactly the pattern a
	// spoofing attacker would use to sidestep per-client accounting.
	const int clients = AdminAuth::kMaxFailedAttemptsGlobal / AdminAuth::kMaxFailedAttempts;
	for (int c = 0; c < clients; ++c)
	{
		const std::string ip = "10.1.1." + std::to_string(c + 1);
		for (int i = 0; i < AdminAuth::kMaxFailedAttempts; ++i)
		{
			EXPECT_FALSE(AdminAuth::verifyPin("000000", ip));
		}
	}

	// A brand-new address, with an empty bucket of its own, is refused anyway.
	EXPECT_TRUE(AdminAuth::isLockedOut("10.9.9.9"));
	EXPECT_FALSE(AdminAuth::verifyPin("123456", "10.9.9.9"));

	AdminAuth::clearCredential();
}

TEST(WebHardening, GlobalBackstopSitsWellAboveOrdinaryTypos)
{
	// The backstop must not resurrect the D-3 self-DoS: an operator fumbling
	// their PIN a handful of times must never lock the whole device. Only that
	// one client's short cooldown may engage.
	ASSERT_GT(AdminAuth::kMaxFailedAttemptsGlobal, AdminAuth::kMaxFailedAttempts);

	AdminAuth::clearCredential();
	ASSERT_TRUE(AdminAuth::setPin("123456"));

	for (int i = 0; i < AdminAuth::kMaxFailedAttempts; ++i)
	{
		EXPECT_FALSE(AdminAuth::verifyPin("000000", "10.2.2.1"));
	}
	EXPECT_TRUE(AdminAuth::isLockedOut("10.2.2.1"));
	// Everyone else is still free to log in.
	EXPECT_FALSE(AdminAuth::isLockedOut("10.2.2.2"));
	EXPECT_TRUE(AdminAuth::verifyPin("123456", "10.2.2.2"));

	AdminAuth::clearCredential();
}

// ─────────────────────────────────────────────────────────────────────────────
// Registrar admission mode + Learn-mode extension onboarding.
//
// Regression context: RequestsHandler::setRegistrarMode() was reachable ONLY
// from unit tests. SIP digest authentication was fully implemented, fully
// tested, and completely unreachable on a shipped device, because nothing in
// production ever wrote the persisted mode. These endpoints are what close that.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	// Provision, log in, and return {cookie, csrf} on a server that has a real
	// handler attached — required because the HTTP plane goes dark the moment a
	// PIN exists, and only set-pin's grace window keeps it open.
	struct AdminSession { std::string cookie; std::string csrf; };

	AdminSession loginOn(int port)
	{
		AdminSession a;
		EXPECT_EQ(httpPostStatus(port, "/api/admin/set-pin", "pin=123456"), 200);
		std::string resp = httpPostRaw(port, "/api/admin/login", "pin=123456");
		EXPECT_EQ(statusOf(resp), 200);
		a.cookie = cookieOf(resp, "pd_session");
		a.csrf   = csrfOf(resp);
		return a;
	}
}

TEST(Registrar, ModeRoundTripsThroughTheDashboard)
{
	AdminAuth::clearCredential();
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18100, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	AdminSession a = loginOn(18100);
	ASSERT_FALSE(a.cookie.empty());

	// Ships open: POCKETDIAL_OPEN_REGISTRAR seeds the default, and until now
	// nothing could change it.
	std::string get = httpGetRaw(18100, "/api/registrar", "pd_session=" + a.cookie);
	EXPECT_EQ(statusOf(get), 200);
	EXPECT_NE(get.find("\"mode\":\"open\""), std::string::npos);
	EXPECT_NE(get.find("\"attached\":true"), std::string::npos);

	std::string set = httpPostRaw(18100, "/api/registrar", "mode=learn",
	                              "pd_session=" + a.cookie, a.csrf);
	EXPECT_EQ(statusOf(set), 200);
	EXPECT_NE(set.find("\"mode\":\"learn\""), std::string::npos);
	EXPECT_EQ(handler.getRegistrarMode(), RequestsHandler::RegistrarMode::Learn);

	// And it is readable back, not just accepted.
	get = httpGetRaw(18100, "/api/registrar", "pd_session=" + a.cookie);
	EXPECT_NE(get.find("\"mode\":\"learn\""), std::string::npos);

	AdminAuth::clearCredential();
}

TEST(Registrar, SwitchingToSecureWithNothingSecuredNeedsConfirmation)
{
	// The foot-gun this guards: `secure` digest-challenges every REGISTER. On a
	// device that has never run Learn mode, no extension has a secret, so every
	// phone would fail to register at once — and the operator would have no
	// working handset left to notice with.
	AdminAuth::clearCredential();
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18101, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	AdminSession a = loginOn(18101);

	std::string blocked = httpPostRaw(18101, "/api/registrar", "mode=secure",
	                                  "pd_session=" + a.cookie, a.csrf);
	EXPECT_EQ(statusOf(blocked), 409);
	EXPECT_EQ(handler.getRegistrarMode(), RequestsHandler::RegistrarMode::Open)
		<< "a refused switch must not have changed the mode";

	std::string forced = httpPostRaw(18101, "/api/registrar",
	                                 "mode=secure&confirm=LOCKOUT",
	                                 "pd_session=" + a.cookie, a.csrf);
	EXPECT_EQ(statusOf(forced), 200);
	EXPECT_EQ(handler.getRegistrarMode(), RequestsHandler::RegistrarMode::Secure);

	AdminAuth::clearCredential();
}

TEST(Registrar, RejectsUnknownModeAndUnknownDevice)
{
	AdminAuth::clearCredential();
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18102, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	AdminSession a = loginOn(18102);

	EXPECT_EQ(statusOf(httpPostRaw(18102, "/api/registrar", "mode=wide-open",
	                               "pd_session=" + a.cookie, a.csrf)), 400);
	EXPECT_EQ(statusOf(httpPostRaw(18102, "/api/registrar/device",
	                               "action=secure&target=aabbccddeeff",
	                               "pd_session=" + a.cookie, a.csrf)), 404);
	EXPECT_EQ(statusOf(httpPostRaw(18102, "/api/registrar/device",
	                               "action=explode&target=1001",
	                               "pd_session=" + a.cookie, a.csrf)), 400);
	EXPECT_EQ(statusOf(httpPostRaw(18102, "/api/registrar/device", "action=secure",
	                               "pd_session=" + a.cookie, a.csrf)), 400);

	AdminAuth::clearCredential();
}

TEST(Registrar, MutatingEndpointsRequireTheCsrfToken)
{
	// The registrar mode is the single most security-relevant setting exposed by
	// the dashboard: flipping it to `open` disables SIP authentication entirely.
	// It must not be reachable with a stolen cookie alone.
	AdminAuth::clearCredential();
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18103, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	AdminSession a = loginOn(18103);

	EXPECT_EQ(statusOf(httpPostRaw(18103, "/api/registrar", "mode=open",
	                               "pd_session=" + a.cookie)), 403);
	EXPECT_EQ(statusOf(httpPostRaw(18103, "/api/registrar/device",
	                               "action=forget&target=1001",
	                               "pd_session=" + a.cookie)), 403);

	AdminAuth::clearCredential();
}
