#ifndef SIP_SDP_MESSAGE_HPP
#define SIP_SDP_MESSAGE_HPP

#include "SipMessage.hpp"

class SipSdpMessage : public SipMessage
{
public:
	SipSdpMessage(std::string message, sockaddr_in src);

	// Issue #42: see SipMessage::hasSdp(). This concrete type always carries SDP.
	bool hasSdp() const override { return _hasSdp; }

	void setMedia(std::string value);

	std::string_view getVersion() const;
	std::string_view getOriginator() const;
	std::string_view getSessionName() const;
	std::string_view getConnectionInformation() const;
	std::string_view getTime() const;
	std::string_view getMedia() const;
	int getRtpPort() const;

protected:
	void reparse() override;

private:
	void parseSdp();
	int extractRtpPort(std::string_view data) const;

	std::string_view _version;
	std::string_view _originator;
	std::string_view _sessionName;
	std::string_view _connectionInformation;
	std::string_view _time;
	std::string_view _media;
	int _rtpPort = 0;
};

#endif
