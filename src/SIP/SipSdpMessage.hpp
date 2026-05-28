#ifndef SIP_SDP_MESSAGE_HPP
#define SIP_SDP_MESSAGE_HPP

#include "SipMessage.hpp"

class SipSdpMessage : public SipMessage
{
public:
	SipSdpMessage(std::string message, sockaddr_in src);

	void setMedia(std::string value);

	std::string getVersion() const;
	std::string getOriginator() const;
	std::string getSessionName() const;
	std::string getConnectionInformation() const;
	std::string getTime() const;
	std::string getMedia() const;
	int getRtpPort() const;

private:
	void parseSdp();
	int extractRtpPort(const std::string& data) const;

	std::string _version;
	std::string _originator;
	std::string _sessionName;
	std::string _connectionInformation;
	std::string _time;
	std::string _media;
	int _rtpPort = 0;
};

#endif
