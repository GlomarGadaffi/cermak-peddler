#include <gtest/gtest.h>
#include "SipMessage.hpp"
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

TEST(SipMessage, BasicParse) {
    sockaddr_in s; s.sin_addr.s_addr = inet_addr("127.0.0.1");
    std::string raw = "REGISTER sip:server SIP/2.0\r\n"
                      "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=1\r\n"
                      "From: <sip:100@server>\r\n"
                      "To: <sip:100@server>\r\n"
                      "Call-ID: id\r\n"
                      "CSeq: 1 REGISTER\r\n"
                      "Content-Length: 0\r\n\r\n";
    SipMessage m(raw, s);
    ASSERT_TRUE(m.isValidMessage());
    ASSERT_EQ(std::string(m.getType()), "REGISTER");
}

// Regression: enforceG711() rewrites the SDP m= codec list, which changes the
// body size. It must resync Content-Length, otherwise the 777 echo / 999 page
// answer is dropped by the peer as truncated and the caller sits on ringback.
TEST(SipMessage, EnforceG711ResyncsContentLength) {
    sockaddr_in s; s.sin_addr.s_addr = inet_addr("127.0.0.1");
    std::string body =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=audio 5004 RTP/AVP 0 8 9 18 101\r\n"
        "a=rtpmap:0 PCMU/8000\r\n";
    std::string head =
        "INVITE sip:777@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=1\r\n"
        "From: <sip:100@server>\r\n"
        "To: <sip:777@server>\r\n"
        "Call-ID: id\r\n"
        "CSeq: 1 INVITE\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    SipMessage m(head + body, s);

    m.enforceG711();

    // Codec list collapsed to PCMU/PCMA + telephone-event.
    ASSERT_NE(m.toString().find("m=audio 5004 RTP/AVP 0 8 101\r\n"), std::string::npos);

    // Content-Length must equal the actual body byte count after the rewrite.
    const std::string out = m.toString();
    size_t sep = out.find("\r\n\r\n");
    ASSERT_NE(sep, std::string::npos);
    size_t actualBody = out.size() - (sep + 4);
    ASSERT_EQ(std::string(m.getContentLength()),
              "Content-Length: " + std::to_string(actualBody));
}

// Guard test for the SipMessage storage rewrite (owned header-line list replacing
// the shared _messageStr + string_view fields). SipMessage only ever names ~8
// headers (Via/From/To/Call-ID/CSeq/Contact/Content-Length/Authorization); every
// other header on the wire — including headers repeated more than once, like the
// triple Alert-Info the intercom auto-answer path emits (RequestsHandler.cpp) —
// must ride along untouched. A naive "N owned fields, drop the shared buffer"
// rewrite would silently lose all of these on the first mutation. This test must
// stay green across the rewrite.
TEST(SipMessage, PreservesUnknownAndRepeatedHeadersOnMutation) {
    sockaddr_in s; s.sin_addr.s_addr = inet_addr("127.0.0.1");
    std::string raw =
        "INVITE sip:999@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=1\r\n"
        "From: <sip:100@server>\r\n"
        "To: <sip:999@server>\r\n"
        "Call-ID: guard-id\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:100@127.0.0.1:5060>\r\n"
        "Call-Info: <sip:any>;answer-after=0\r\n"
        "Alert-Info: info=alert-autoanswer\r\n"
        "Alert-Info: answer-after=0\r\n"
        "Alert-Info: intercom=true\r\n"
        "P-Auto-Answer: normal\r\n"
        "WWW-Authenticate: Digest realm=\"pocketdial\", nonce=\"abc123\"\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: 0\r\n\r\n";

    SipMessage unmodified(raw, s);
    ASSERT_EQ(unmodified.toString(), raw) << "byte-identical round trip with no mutation";

    SipMessage m(raw, s);
    m.setVia("Via: SIP/2.0/UDP 127.0.0.1:5060;branch=1;received=10.0.0.1");
    const std::string out = m.toString();

    ASSERT_NE(out.find("Call-Info: <sip:any>;answer-after=0"), std::string::npos);
    ASSERT_NE(out.find("P-Auto-Answer: normal"), std::string::npos);
    ASSERT_NE(out.find("WWW-Authenticate: Digest realm=\"pocketdial\", nonce=\"abc123\""), std::string::npos);
    ASSERT_NE(out.find("Content-Type: application/sdp"), std::string::npos);

    size_t alertInfoCount = 0;
    for (size_t pos = out.find("Alert-Info:"); pos != std::string::npos;
         pos = out.find("Alert-Info:", pos + 1))
    {
        ++alertInfoCount;
    }
    ASSERT_EQ(alertInfoCount, 3u) << "all three repeated Alert-Info headers must survive a mutation";
    ASSERT_NE(out.find("Alert-Info: info=alert-autoanswer"), std::string::npos);
    ASSERT_NE(out.find("Alert-Info: answer-after=0"), std::string::npos);
    ASSERT_NE(out.find("Alert-Info: intercom=true"), std::string::npos);

    // Fields untouched by the mutation must still read correctly too.
    ASSERT_EQ(std::string(m.getCallID()), "Call-ID: guard-id");
    ASSERT_EQ(std::string(m.getContact()), "Contact: <sip:100@127.0.0.1:5060>");
}
