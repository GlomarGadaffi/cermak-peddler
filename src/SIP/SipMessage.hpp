#ifndef SIP_MESSAGE_HPP
#define SIP_MESSAGE_HPP

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <lwip/sockets.h>
#undef INADDR_NONE
#elif defined(__linux__)
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#endif

#include <iostream>
#include <string>
#include <stdexcept>

class SipMessage
{
public:

	SipMessage(std::string message, sockaddr_in src);
	virtual ~SipMessage() = default;

	void setType(std::string value);
	void setHeader(std::string value);
	void setVia(std::string value);
	void setFrom(std::string value);
	void setTo(std::string value);
	void setCallID(std::string value);
	void setCSeq(std::string value);
	void setContact(std::string value);
	void setContentLength(std::string value);
	void addHeader(std::string name, std::string value);
	void enforceG711();
	void clearBody();


	const std::string& getType() const;
	const std::string& getHeader() const;
	const std::string& getVia() const;
	const std::string& getFrom() const;
	const std::string& getFromNumber() const;
	const std::string& getTo() const;
	const std::string& getToNumber() const;
	const std::string& getCallID() const;
	const std::string& getCSeq() const;
	const std::string& getContact() const;
	const std::string& getContactNumber() const;
	const std::string& getContentLength() const;
	sockaddr_in getSource() const;

	// Issue #42: virtual SDP probe replaces dynamic_cast so call setup works
	// on the Arduino ESP32 toolchain, which builds with RTTI disabled (-fno-rtti).
	// Base messages carry no SDP; SipSdpMessage overrides this to return true.
	virtual bool hasSdp() const { return false; }

	std::string toString() const;
	bool isValidMessage() const;

protected:
	void parse();
	std::string extractNumber(const std::string& header) const;

	std::string _type;
	std::string _header;
	std::string _via;
	std::string _from;
	std::string _fromNumber;
	std::string _to;
	std::string _toNumber;
	std::string _callID;
	std::string _cSeq;
	std::string _contact;
	std::string _contactNumber;
	std::string _contentLength;
	std::string _messageStr;

	sockaddr_in _src{};
};

#endif
