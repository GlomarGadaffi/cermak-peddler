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
#include <string_view>
#include <stdexcept>

class SipMessage
{
public:

	SipMessage(std::string message, sockaddr_in src);
	virtual ~SipMessage() = default;

	void reset(std::string message, sockaddr_in src);

	SipMessage(const SipMessage& other);
	SipMessage& operator=(const SipMessage& other);

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


	std::string_view getType() const;
	std::string_view getHeader() const;
	std::string_view getVia() const;
	std::string_view getFrom() const;
	std::string_view getFromNumber() const;
	std::string_view getTo() const;
	std::string_view getToNumber() const;
	std::string_view getCallID() const;
	std::string_view getCSeq() const;
	std::string_view getContact() const;
	std::string_view getContactNumber() const;
	std::string_view getContentLength() const;
	sockaddr_in getSource() const;

	// Issue #42: virtual SDP probe replaces dynamic_cast so call setup works
	// on the Arduino ESP32 toolchain, which builds with RTTI disabled (-fno-rtti).
	virtual bool hasSdp() const { return _hasSdp; }

	std::string toString() const;
	bool isValidMessage() const;

protected:
	virtual void reparse() { parse(); }
	void parse();
	std::string_view extractNumber(std::string_view header) const;
	size_t findHeader(std::string_view field) const;

	std::string_view _type;
	std::string_view _header;
	std::string_view _via;
	std::string_view _from;
	std::string_view _fromNumber;
	std::string_view _to;
	std::string_view _toNumber;
	std::string_view _callID;
	std::string_view _cSeq;
	std::string_view _contact;
	std::string_view _contactNumber;
	std::string_view _contentLength;
	std::string _messageStr;
	bool _hasSdp = false;

	sockaddr_in _src{};
};

#endif
