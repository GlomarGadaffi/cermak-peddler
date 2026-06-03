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
