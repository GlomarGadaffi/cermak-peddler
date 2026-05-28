#include "SipClient.hpp"

SipClient::SipClient(std::string number, sockaddr_in address, int expiresSeconds)
	: _number(std::move(number)), _address(address)
{
	renew(expiresSeconds);
}

void SipClient::renew(int expiresSeconds)
{
	_expiresSeconds = expiresSeconds;
	_expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(expiresSeconds);
}

bool SipClient::isExpired(std::chrono::steady_clock::time_point now) const
{
	return now >= _expiresAt;
}

int SipClient::getExpiresSeconds() const
{
	return _expiresSeconds;
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
