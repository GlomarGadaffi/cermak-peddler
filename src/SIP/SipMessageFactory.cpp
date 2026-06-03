#include "SipMessageFactory.hpp"
#include "RequestsHandler.hpp"

std::optional<std::shared_ptr<SipMessage>> SipMessageFactory::createMessage(std::string message, sockaddr_in src)
{
	auto msg = RequestsHandler::getMessageFromPool(std::move(message), src);
	if (!msg)
	{
		return std::nullopt;
	}
	return msg;
}

bool SipMessageFactory::containsSdp(const std::string& message) const
{
	return message.find(SDP_CONTENT_TYPE) != std::string::npos;
}
