#ifndef SIP_CLIENT_HPP
#define SIP_CLIENT_HPP

#if defined(__linux__) || defined(ESP_PLATFORM)
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#endif

#include <iostream>
#include <string>

class SipClient
{
public:
	SipClient(std::string number, sockaddr_in address);

	bool operator==(const SipClient& other) const;

	const std::string& getNumber() const;
	const sockaddr_in& getAddress() const;

private:
	std::string _number;
	sockaddr_in _address;
};

#endif
