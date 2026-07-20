#include "SipSdpMessage.hpp"
#include <string>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <cctype>

namespace
{
	// The six SDP fields SipSdpMessage exposes, extracted in a single pass over
	// the body. Mirrors the old parseSdp() line-splitting exactly (primary
	// "\r\n" delimiter, bare "\n" fallback) so behavior on mixed/malformed line
	// endings is unchanged.
	struct SdpFields
	{
		std::string_view version, originator, sessionName, connectionInformation, time, media;
	};

	SdpFields parseSdpFields(std::string_view body)
	{
		SdpFields f;
		size_t pos_start = 0;
		while (pos_start < body.size())
		{
			size_t pos_end = body.find("\r\n", pos_start);
			size_t next_start = pos_end + 2;
			if (pos_end == std::string_view::npos)
			{
				pos_end = body.find('\n', pos_start);
				next_start = pos_end + 1;
			}

			std::string_view line;
			if (pos_end == std::string_view::npos)
			{
				line = body.substr(pos_start);
				pos_start = body.size();
			}
			else
			{
				line = body.substr(pos_start, pos_end - pos_start);
				pos_start = next_start;
			}

			if (line.empty()) continue;

			if (line.compare(0, 2, "v=") == 0)      f.version = line;
			else if (line.compare(0, 2, "o=") == 0) f.originator = line;
			else if (line.compare(0, 2, "s=") == 0) f.sessionName = line;
			else if (line.compare(0, 2, "c=") == 0) f.connectionInformation = line;
			else if (line.compare(0, 2, "t=") == 0) f.time = line;
			else if (line.compare(0, 2, "m=") == 0) f.media = line;
		}
		return f;
	}
}

SipSdpMessage::SipSdpMessage(std::string message, sockaddr_in src) : SipMessage(std::move(message), src)
{
}

void SipSdpMessage::setMedia(std::string value)
{
	// Operate on a local copy of the body so the search offset and the mutation
	// stay consistent within the same buffer (no cross-object pointer math
	// against the base class's storage).
	std::string body(getBody());
	size_t pos_start = 0;
	while (pos_start < body.size())
	{
		size_t pos_end = body.find("\r\n", pos_start);
		size_t next_start = pos_end + 2;
		if (pos_end == std::string::npos)
		{
			pos_end = body.find('\n', pos_start);
			next_start = pos_end + 1;
		}
		size_t lineLen = (pos_end == std::string::npos) ? (body.size() - pos_start) : (pos_end - pos_start);

		if (lineLen >= 2 && body.compare(pos_start, 2, "m=") == 0)
		{
			body.replace(pos_start, lineLen, value);
			setBody(body);
			return;
		}
		pos_start = (pos_end == std::string::npos) ? body.size() : next_start;
	}
	// No "m=" line found: no-op, matching the original's behavior when there
	// was no media line to replace.
}

std::string_view SipSdpMessage::getVersion() const
{
	return parseSdpFields(getBody()).version;
}

std::string_view SipSdpMessage::getOriginator() const
{
	return parseSdpFields(getBody()).originator;
}

std::string_view SipSdpMessage::getSessionName() const
{
	return parseSdpFields(getBody()).sessionName;
}

std::string_view SipSdpMessage::getConnectionInformation() const
{
	return parseSdpFields(getBody()).connectionInformation;
}

std::string_view SipSdpMessage::getTime() const
{
	return parseSdpFields(getBody()).time;
}

std::string_view SipSdpMessage::getMedia() const
{
	return parseSdpFields(getBody()).media;
}

int SipSdpMessage::getRtpPort() const
{
	return extractRtpPort(getMedia());
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
