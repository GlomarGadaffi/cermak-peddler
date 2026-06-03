#include "SipSdpMessage.hpp"
#include <string>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <cctype>

SipSdpMessage::SipSdpMessage(std::string message, sockaddr_in src) : SipMessage(std::move(message), src)
{
	parseSdp();
}

void SipSdpMessage::reparse()
{
	SipMessage::reparse();
	if (_hasSdp)
	{
		parseSdp();
	}
	else
	{
		_version = {};
		_originator = {};
		_sessionName = {};
		_connectionInformation = {};
		_time = {};
		_media = {};
		_rtpPort = 0;
	}
}

void SipSdpMessage::setMedia(std::string value)
{
	size_t mPos = findHeader(_media);
	if (mPos != std::string::npos)
	{
		_messageStr.replace(mPos, _media.length(), value);
	}
	reparse();
}

std::string_view SipSdpMessage::getVersion() const
{
	return _version;
}

std::string_view SipSdpMessage::getOriginator() const
{
	return _originator;
}

std::string_view SipSdpMessage::getSessionName() const
{
	return _sessionName;
}

std::string_view SipSdpMessage::getConnectionInformation() const
{
	return _connectionInformation;
}

std::string_view SipSdpMessage::getTime() const
{
	return _time;
}

std::string_view SipSdpMessage::getMedia() const
{
	return _media;
}

int SipSdpMessage::getRtpPort() const
{
	return _rtpPort;
}

void SipSdpMessage::parseSdp()
{
	_version = {};
	_originator = {};
	_sessionName = {};
	_connectionInformation = {};
	_time = {};
	_media = {};
	_rtpPort = 0;

	size_t bodyStart = _messageStr.find("\r\n\r\n");
	if (bodyStart != std::string::npos)
	{
		bodyStart += 4;
	}
	else if ((bodyStart = _messageStr.find("\n\n")) != std::string::npos)
	{
		bodyStart += 2;
	}
	else
	{
		bodyStart = 0;
	}

	size_t pos_start = _messageStr.find("v=", bodyStart);
	if (pos_start == std::string::npos)
	{
		return;
	}

	size_t pos_end;
	while (pos_start < _messageStr.size())
	{
		pos_end = _messageStr.find("\r\n", pos_start);
		size_t next_start = pos_end + 2;
		if (pos_end == std::string::npos)
		{
			pos_end = _messageStr.find("\n", pos_start);
			next_start = pos_end + 1;
		}

		std::string_view line;
		if (pos_end == std::string::npos)
		{
			line = std::string_view(_messageStr.data() + pos_start, _messageStr.size() - pos_start);
			pos_start = _messageStr.size();
		}
		else
		{
			line = std::string_view(_messageStr.data() + pos_start, pos_end - pos_start);
			pos_start = next_start;
		}

		if (line.empty())
		{
			continue;
		}

		if (line.compare(0, 2, "v=") == 0)
		{
			_version = line;
		}
		else if (line.compare(0, 2, "o=") == 0)
		{
			_originator = line;
		}
		else if (line.compare(0, 2, "s=") == 0)
		{
			_sessionName = line;
		}
		else if (line.compare(0, 2, "c=") == 0)
		{
			_connectionInformation = line;
		}
		else if (line.compare(0, 2, "t=") == 0)
		{
			_time = line;
		}
		else if (line.compare(0, 2, "m=") == 0)
		{
			_media = line;
			_rtpPort = extractRtpPort(line);
		}
	}
}

int SipSdpMessage::extractRtpPort(std::string_view data) const
{
	auto spacePos = data.find(' ');
	if (spacePos == std::string_view::npos)
		return 0;
	size_t portStart = spacePos + 1;
	while (portStart < data.size() && std::isspace(static_cast<unsigned char>(data[portStart]))) ++portStart;
	size_t portEnd = portStart;
	while (portEnd < data.size() && std::isdigit(static_cast<unsigned char>(data[portEnd]))) ++portEnd;
	if (portEnd == portStart)
		return 0;
	int val = 0;
	for (size_t i = portStart; i < portEnd; ++i)
	{
		if (val > 200000000) return 200000000;
		val = val * 10 + (data[i] - '0');
	}
	return val;
}
