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

	// Helper to match headers case-insensitively and handle compact names
	auto matchHeader = [](std::string_view line, std::string_view fullHdr, std::string_view compactHdr = "") -> bool {
		size_t colonPos = line.find(':');
		if (colonPos == std::string::npos)
		{
			return false;
		}

		size_t nameEnd = colonPos;
		while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(line[nameEnd - 1])))
		{
			nameEnd--;
		}

		std::string_view name = line.substr(0, nameEnd);
		if (name.length() != fullHdr.length() && (compactHdr.empty() || name.length() != compactHdr.length()))
		{
			return false;
		}

		auto charToLowerEquals = [](char c1, char c2) {
			return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2));
		};

		bool matchFull = (name.length() == fullHdr.length());
		if (matchFull)
		{
			for (size_t i = 0; i < name.length(); ++i)
			{
				if (!charToLowerEquals(name[i], fullHdr[i]))
				{
					matchFull = false;
					break;
				}
			}
		}
		if (matchFull) return true;

		if (!compactHdr.empty() && name.length() == compactHdr.length())
		{
			bool matchCompact = true;
			for (size_t i = 0; i < name.length(); ++i)
			{
				if (!charToLowerEquals(name[i], compactHdr[i]))
				{
					matchCompact = false;
					break;
				}
			}
			if (matchCompact) return true;
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

void SipMessage::setType(std::string value)
{
	if (!_header.empty())
	{
		size_t spacePos = _header.find(' ');
		if (spacePos != std::string_view::npos)
		{
			size_t headerOffset = _header.data() - _messageStr.data();
			_messageStr.replace(headerOffset, spacePos, value);
		}
	}
	reparse();
}

void SipMessage::setHeader(std::string value)
{
	size_t pos = findHeader(_header);
	if (pos != std::string::npos)
	{
		_messageStr.replace(pos, _header.length(), value);
	}
	reparse();
}

void SipMessage::setVia(std::string value)
{
	size_t pos = findHeader(_via);
	if (pos != std::string::npos)
	{
		_messageStr.replace(pos, _via.length(), value);
	}
	else
	{
		size_t clPos = findHeader(_contentLength);
		if (clPos != std::string::npos)
		{
			_messageStr.insert(clPos, value + "\r\n");
		}
		else
		{
			_messageStr += value + "\r\n";
		}
	}
	reparse();
}

void SipMessage::setFrom(std::string value)
{
	size_t pos = findHeader(_from);
	if (pos != std::string::npos)
	{
		_messageStr.replace(pos, _from.length(), value);
	}
	else
	{
		size_t clPos = findHeader(_contentLength);
		if (clPos != std::string::npos)
		{
			_messageStr.insert(clPos, value + "\r\n");
		}
		else
		{
			_messageStr += value + "\r\n";
		}
	}
	reparse();
}

void SipMessage::setTo(std::string value)
{
	size_t pos = findHeader(_to);
	if (pos != std::string::npos)
	{
		_messageStr.replace(pos, _to.length(), value);
	}
	else
	{
		size_t clPos = findHeader(_contentLength);
		if (clPos != std::string::npos)
		{
			_messageStr.insert(clPos, value + "\r\n");
		}
		else
		{
			_messageStr += value + "\r\n";
		}
	}
	reparse();
}

void SipMessage::setCallID(std::string value)
{
	size_t pos = findHeader(_callID);
	if (pos != std::string::npos)
	{
		_messageStr.replace(pos, _callID.length(), value);
	}
	else
	{
		size_t clPos = findHeader(_contentLength);
		if (clPos != std::string::npos)
		{
			_messageStr.insert(clPos, value + "\r\n");
		}
		else
		{
			_messageStr += value + "\r\n";
		}
	}
	reparse();
}

void SipMessage::setCSeq(std::string value)
{
	size_t pos = findHeader(_cSeq);
	if (pos != std::string::npos)
	{
		_messageStr.replace(pos, _cSeq.length(), value);
	}
	else
	{
		size_t clPos = findHeader(_contentLength);
		if (clPos != std::string::npos)
		{
			_messageStr.insert(clPos, value + "\r\n");
		}
		else
		{
			_messageStr += value + "\r\n";
		}
	}
	reparse();
}

void SipMessage::setContact(std::string value)
{
	size_t pos = findHeader(_contact);
	if (pos != std::string::npos)
	{
		_messageStr.replace(pos, _contact.length(), value);
	}
	else
	{
		size_t clPos = findHeader(_contentLength);
		if (clPos != std::string::npos)
		{
			_messageStr.insert(clPos, value + "\r\n");
		}
		else
		{
			_messageStr += value + "\r\n";
		}
	}
	reparse();
}

void SipMessage::setContentLength(std::string value)
{
	size_t pos = findHeader(_contentLength);
	if (pos != std::string::npos)
	{
		_messageStr.replace(pos, _contentLength.length(), value);
	}
	reparse();
}

void SipMessage::addHeader(std::string name, std::string value)
{
	size_t clPos = findHeader(_contentLength);
	if (clPos != std::string::npos)
	{
		_messageStr.insert(clPos, name + ": " + value + "\r\n");
	}
	else
	{
		size_t bodyBoundary = _messageStr.find("\r\n\r\n");
		if (bodyBoundary != std::string::npos)
		{
			_messageStr.insert(bodyBoundary + 2, name + ": " + value + "\r\n");
		}
		else
		{
			_messageStr += name + ": " + value + "\r\n";
		}
	}
	reparse();
}

void SipMessage::enforceG711()
{
	size_t mPos = _messageStr.find("m=audio ");
	if (mPos != std::string::npos)
	{
		size_t lineEnd = _messageStr.find("\r\n", mPos);
		if (lineEnd == std::string::npos) lineEnd = _messageStr.find("\n", mPos);
		if (lineEnd != std::string::npos)
		{
			std::string_view mLine = std::string_view(_messageStr.data() + mPos, lineEnd - mPos);
			size_t rtpPos = mLine.find("RTP/AVP ");
			if (rtpPos != std::string_view::npos)
			{
				std::string newMLine = std::string(mLine.substr(0, rtpPos + 8)) + "0 8 101";
				_messageStr.replace(mPos, mLine.length(), newMLine);
			}
		}
	}
	reparse();
}

void SipMessage::clearBody()
{
	size_t bodyStart = _messageStr.find("\r\n\r\n");
	if (bodyStart != std::string::npos)
	{
		_messageStr.erase(bodyStart + 4);
	}
	else
	{
		bodyStart = _messageStr.find("\n\n");
		if (bodyStart != std::string::npos)
		{
			_messageStr.erase(bodyStart + 2);
		}
	}

	if (!_contentLength.empty())
	{
		if (_contentLength.find("Content-Length:") != std::string_view::npos)
		{
			setContentLength("Content-Length: 0");
		}
		else
		{
			setContentLength("l: 0");
		}
	}
	reparse();
}

std::string SipMessage::toString() const
{
	return _messageStr;
}

bool SipMessage::isValidMessage() const
{
	// Structural validity (SEC-02): reject empty payloads and packets whose
	// start line / message type could not be parsed. A well-formed SIP message
	// always yields a non-empty start line and a method/status token.
	if (_messageStr.empty()) return false;
	if (_header.empty()) return false;
	if (_type.empty()) return false;
	return true;
}

std::string_view SipMessage::getType() const
{
	return _type;
}

std::string_view SipMessage::getHeader() const
{
	return _header;
}

std::string_view SipMessage::getVia() const
{
	return _via;
}

std::string_view SipMessage::getFrom() const
{
	return _from;
}

std::string_view SipMessage::getFromNumber() const
{
	return _fromNumber;
}

std::string_view SipMessage::getTo() const
{
	return _to;
}

std::string_view SipMessage::getToNumber() const
{
	return _toNumber;
}

std::string_view SipMessage::getCallID() const
{
	return _callID;
}

std::string_view SipMessage::getCSeq() const
{
	return _cSeq;
}

std::string_view SipMessage::getContact() const
{
	return _contact;
}

std::string_view SipMessage::getContactNumber() const
{
	return _contactNumber;
}

sockaddr_in SipMessage::getSource() const
{
	return _src;
}

std::string_view SipMessage::getContentLength() const
{
	return _contentLength;
}

std::string_view SipMessage::extractNumber(std::string_view header) const
{
	auto sipPos = header.find("sip:");
	if (sipPos == std::string_view::npos)
		return {};

	auto start = sipPos + 4;
	auto atPos = header.find('@', start);
	if (atPos == std::string_view::npos)
		return {};

	return header.substr(start, atPos - start);
}

size_t SipMessage::findHeader(std::string_view field) const
{
	if (field.empty()) return std::string::npos;
	size_t pos = _messageStr.find(field);
	if (pos != std::string::npos)
	{
		size_t bodyStart = _messageStr.find("\r\n\r\n");
		if (bodyStart == std::string::npos) bodyStart = _messageStr.find("\n\n");
		size_t headerLimit = (bodyStart != std::string::npos) ? bodyStart : _messageStr.size();
		if (pos < headerLimit) return pos;
	}
	return std::string::npos;
}
