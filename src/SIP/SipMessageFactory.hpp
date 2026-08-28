#ifndef SIP_MESSAGE_FACTORY_HPP
#define SIP_MESSAGE_FACTORY_HPP

#include <optional>
#include <memory>
#include <string_view>
#include "SipSdpMessage.hpp"

class SipMessageFactory
{
public:
	// Issue #81: zero-copy — `message` is a view (ultimately into UdpServer::
	// receiveLoop()'s stack buffer), never copied here. Issue #105: taking it by
	// view rather than by value also means the caller's original wire-received
	// bytes stay intact after this returns — SipServer::onNewMessage() needs them
	// afterwards to hand the exact received bytes to RequestsHandler::handle()
	// for inbound pcap/trace capture.
	std::optional<std::shared_ptr<SipMessage>> createMessage(std::string_view message, sockaddr_in src);

private:
	static constexpr auto SDP_CONTENT_TYPE = "application/sdp";

	bool containsSdp(const std::string& message) const;
};

#endif 
