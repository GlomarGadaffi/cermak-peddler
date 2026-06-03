#include "SipMessage.hpp"
#include "SipMessageTypes.h"
#include <cstring>
#include <cctype>
#include <algorithm>

SipMessage::SipMessage(std::string message, sockaddr_in src) : _messageStr(std::move(message)), _src(src)
{
	_hasSdp = (_messageStr.find("application/sdp") != std::string::npos);
	parse();
}

SipMessage::SipMessage(const SipMessage& other) : _messageStr(other._messageStr), _hasSdp(other._hasSdp), _src(other._src)
{
	parse();
}

SipMessage& SipMessage::operator=(const SipMessage& other)
{
	if (this != &other)
	{
		_messageStr = other._messageStr;
		_src = other._src;
		_hasSdp = other._hasSdp;
		parse();
	}
	return *this;
}

void SipMessage::reset(std::string message, sockaddr_in src)
{
	_messageStr = std::move(message);
	_src = src;
	_hasSdp = (_messageStr.find("application/sdp") != std::string::npos);
	reparse();
}

void SipMessage::parse()
{
	_type = {};
	_header = {};
	_via = {};
	_from = {};
	_fromNumber = {};
	_to = {};
	_toNumber = {};
	_callID = {};
	_cSeq = {};
	_contact = {};
	_contactNumber = {};
	_contentLength = {};

	// 1. Separate header block and body payload
	size_t bodyStart = _messageStr.find("\r\n\r\n");
	size_t headerEnd = bodyStart;
	if (bodyStart == std::string::npos)
	{
		bodyStart = _messageStr.find("\n\n");
		headerEnd = bodyStart;
	}

	std::string_view headerBlock;
	if (headerEnd != std::string::npos)
	{
		headerBlock = std::string_view(_messageStr.data(), headerEnd);
	}
	else
	{
		headerBlock = _messageStr;
	}

	if (headerBlock.empty())
	{
		return;
	}

	// 2. Parse start line (Request-Line or Status-Line)
	size_t pos_start = 0;
	size_t pos_end = headerBlock.find("\r\n");
	size_t lineDelimLen = 2;
	if (pos_end == std::string::npos)
	{
		pos_end = headerBlock.find("\n");
		lineDelimLen = 1;
	}

	if (pos_end == std::string::npos)
	{
		_header = headerBlock;
		_type = _header.substr(0, _header.find(" "));
		if (_type == "SIP/2.0")
		{
			_type = _header;
		}
		return;
	}

	_header = headerBlock.substr(pos_start, pos_end - pos_start);
	_type = _header.substr(0, _header.find(" "));
	if (_type == "SIP/2.0")
	{
		_type = _header;
	}

	// More robust header matcher: trim and lowercase the header name and compare
	auto matchHeader = [](std::string_view line, std::string_view fullHdr, std::string_view compactHdr = "") -> bool {
		size_t colonPos = line.find(':');
		if (colonPos == std::string::npos) return false;
		// Trim trailing whitespace before colon
		size_t nameEnd = colonPos;
		while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(line[nameEnd - 1]))) --nameEnd;
		std::string name(line.substr(0, nameEnd));
		// Lowercase name
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
		std::string full(fullHdr);
		std::transform(full.begin(), full.end(), full.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
		if (name == full) return true;
		if (!compactHdr.empty()) {
			std::string compact(compactHdr);
			std::transform(compact.begin(), compact.end(), compact.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
			if (name == compact) return true;
		}
		return false;
	};

	// 3. Iterate over the remaining header block lines
	pos_start = pos_end + lineDelimLen;
	while (pos_start < headerBlock.size())
	{
		pos_end = headerBlock.find("\r\n", pos_start);
		size_t next_start = pos_end + 2;
		if (pos_end == std::string::npos)
		{
			pos_end = headerBlock.find("\n", pos_start);
			next_start = pos_end + 1;
		}

		std::string_view line;
		if (pos_end == std::string::npos)
		{
			line = headerBlock.substr(pos_start);
			pos_start = headerBlock.size();
		}
		else
		{
			line = headerBlock.substr(pos_start, pos_end - pos_start);
			pos_start = next_start;
		}

		if (line.empty())
		{
			continue;
		}

		if (matchHeader(line, "via", "v"))
		{
			_via = line;
		}
		else if (matchHeader(line, "from", "f"))
		{
			_from = line;
			_fromNumber = extractNumber(line);
		}
		else if (matchHeader(line, "to", "t"))
		{
			_to = line;
			_toNumber = extractNumber(line);
		}
		else if (matchHeader(line, "call-id", "i"))
		{
			_callID = line;
		}
		else if (matchHeader(line, "cseq"))
		{
			_cSeq = line;
		}
		else if (matchHeader(line, "contact", "m"))
		{
			_contact = line;
			_contactNumber = extractNumber(line);
		}
		else if (matchHeader(line, "content-length", "l"))
		{
			_contentLength = line;
		}
	}
}
