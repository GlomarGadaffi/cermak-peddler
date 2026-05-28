#include "SipSdpMessage.hpp"
#include "SipMessageHeaders.h"
#include <string>
#include <cstring>
#include <stdexcept>

SipSdpMessage::SipSdpMessage(std::string message, sockaddr_in src) : SipMessage(std::move(message), src)
{
	parseSdp();
}

void SipSdpMessage::setMedia(std::string value)
{
	auto mPos = _messageStr.find(_media);
	if (mPos != std::string::npos)
	{
		_messageStr.replace(mPos, _media.length(), value);
	}
	_media = std::move(value);
}

void SipSdpMessage::setRtpPort(int port)
{
	std::string currentRtpPort = std::to_string(_rtpPort);
	std::string copyM = _media;
	auto portPos = _media.find(currentRtpPort);
	if (portPos != std::string::npos)
	{
		copyM.replace(portPos, currentRtpPort.length(), std::to_string(port));
	}
	_rtpPort = port;
	setMedia(std::move(copyM));
}

std::string SipSdpMessage::getVersion() const
{
	return _version;
}

std::string SipSdpMessage::getOriginator() const
{
	return _originator;
}

std::string SipSdpMessage::getSessionName() const
{
	return _sessionName;
}

std::string SipSdpMessage::getConnectionInformation() const
{
	return _connectionInformation;
}

std::string SipSdpMessage::getTime() const
{
	return _time;
}

std::string SipSdpMessage::getMedia() const
{
	return _media;
}

int SipSdpMessage::getRtpPort() const
{
	return _rtpPort;
}

void SipSdpMessage::parseSdp()
{
	size_t pos_start = _messageStr.find("v=");
	if (pos_start == std::string::npos) {
		throw std::runtime_error("SDP body missing version line");
	}

	const size_t delimLen = std::strlen(SipMessageHeaders::HEADERS_DELIMETER);
	size_t pos_end;

	while ((pos_end = _messageStr.find(SipMessageHeaders::HEADERS_DELIMETER, pos_start)) != std::string::npos)
	{
		if (pos_start == pos_end) {
			break;
		}

		std::string line = _messageStr.substr(pos_start, pos_end - pos_start);

		if (line.compare(0, 2, "v=") == 0)
		{
			_version = std::move(line);
		}
		else if (line.compare(0, 2, "o=") == 0)
		{
			_originator = std::move(line);
		}
		else if (line.compare(0, 2, "s=") == 0)
		{
			_sessionName = std::move(line);
		}
		else if (line.compare(0, 2, "c=") == 0)
		{
			_connectionInformation = std::move(line);
		}
		else if (line.compare(0, 2, "t=") == 0)
		{
			_time = std::move(line);
		}
		else if (line.compare(0, 2, "m=") == 0)
		{
			_media = line;
			_rtpPort = extractRtpPort(line);
		}

		pos_start = pos_end + delimLen;
	}
}

int SipSdpMessage::extractRtpPort(const std::string& data) const
{
	try
	{
		// m=<type> <port> <proto> <fmt> — skip the first token to reach the port
		auto spacePos = data.find(' ');
		if (spacePos == std::string::npos)
			return 0;
		std::string portStr = data.substr(spacePos + 1, data.find(' ', spacePos + 1) - spacePos - 1);
		return std::stoi(portStr);
	}
	catch (const std::exception& e)
	{
		std::cerr << "SipSdpMessage: failed to parse RTP port: " << e.what() << '\n';
		return 0;
	}
}
