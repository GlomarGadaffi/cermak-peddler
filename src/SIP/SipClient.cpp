#include "SipClient.hpp"

SipClient::SipClient(std::string number, sockaddr_in address) : _number(std::move(number)), _address(address)
{
}

bool SipClient::operator==(const SipClient& other) const
{
	return _number == other._number;
}

const std::string& SipClient::getNumber() const
{
	return _number;
}

const sockaddr_in& SipClient::getAddress() const
{
	return _address;
}
