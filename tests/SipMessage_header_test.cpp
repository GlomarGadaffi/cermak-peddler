#include <gtest/gtest.h>
#include "SipMessage.hpp"
#include <arpa/inet.h>

TEST(SipMessage, HeaderMatchingRobustness) {
    sockaddr_in dummy = {}; 
    std::string raw =
        "OPTIONS sip:server SIP/2.0\r\n"
        " v: SIP/2.0/UDP 1.2.3.4:5060;branch=z9h\r\n"
        "F: <sip:100@host>\r\n"
        "T: <sip:server@host>\r\n"
        "i: abc\r\n"
        "c: <sip:...>\r\n"
        "l: 0\r\n\r\n";
    SipMessage msg(raw, dummy);
    ASSERT_FALSE(std::string(msg.getVia()).empty());
    ASSERT_FALSE(std::string(msg.getFrom()).empty());
}
