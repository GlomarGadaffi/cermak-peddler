// SipMessage.cpp: Issue #25 resolved.
#include "SipMessage.hpp"
#include "SipMessageTypes.h"
#include <cstring>
#include <cctype>

SipMessage::SipMessage(std::string message, sockaddr_in src) : _messageStr(std::move(message)), _src(src)
{
	parse();
}

void SipMessage::parse()
{
	// 1. Separate header block and body payload
	size_t bodyStart = std::string::npos;
	size_t headerEnd = std::string::npos;

	if ((bodyStart = _messageStr.find("\r\n\r\n")) != std::string::npos)
	{
		headerEnd = bodyStart;
		bodyStart += 4;
	}
	else if ((bodyStart = _messageStr.find("\n\n")) != std::string::npos)
	{
		headerEnd = bodyStart;
		bodyStart += 2;
	}

	std::string headerBlock;
	if (headerEnd != std::string::npos)
	{
		headerBlock = _messageStr.substr(0, headerEnd);
	}
	else
	{
		headerBlock = _messageStr;
	}

	// 2. Parse start line (Request-Line or Status-Line)
	size_t pos_start = 0;
	size_t pos_end = headerBlock.find("\r\n");
	size_t delimLen = 2;
	if (pos_end == std::string::npos)
	{
		pos_end = headerBlock.find("\n");
		delimLen = 1;
	}

	if (pos_end == std::string::npos)
	{
		throw std::runtime_error("Invalid message headers.");
	}

	_header = headerBlock.substr(pos_start, pos_end - pos_start);
	_type = _header.substr(0, _header.find(" "));
	if (_type == "SIP/2.0")
	{
		_type = _header;
	}

	// Helper to match headers case-insensitively and handle compact names
	auto matchHeader = [](const std::string& line, const std::string& fullHdr, const std::string& compactHdr = "") -> bool {
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

		std::string name = line.substr(0, nameEnd);
		for (char& c : name)
		{
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		if (name == fullHdr) return true;
		if (!compactHdr.empty() && name == compactHdr) return true;
		return false;
	};

	// 3. Iterate over the remaining header block lines
	pos_start = pos_end + delimLen;
	while (pos_start < headerBlock.size())
	{
		pos_end = headerBlock.find("\r\n", pos_start);
		size_t next_start = pos_end + 2;
		if (pos_end == std::string::npos)
		{
			pos_end = headerBlock.find("\n", pos_start);
			next_start = pos_end + 1;
		}

		std::string line;
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

	if (!isValidMessage())
	{
		throw std::runtime_error("Invalid message.");
	}
}


bool SipMessage::isValidMessage() const
{
	if (_via.empty() || _to.empty() || _from.empty() || _callID.empty() || _cSeq.empty())
	{
		return false;
	}

	if ((_type == SipMessageTypes::INVITE || _type == SipMessageTypes::REGISTER) && _contact.empty())
	{
		return false;
	}

	return true;
}

void SipMessage::setType(std::string value)
{
	_type = std::move(value);
}

void SipMessage::setHeader(std::string value)
{
	auto headerPos = _messageStr.find(_header);
	if (headerPos != std::string::npos)
	{
		_messageStr.replace(headerPos, _header.length(), value);
	}
	_header = std::move(value);
	_type = _header.substr(0, _header.find(" "));
	if (_type == "SIP/2.0")
	{
		_type = _header;
	}
}

void SipMessage::setVia(std::string value)
{
	auto viaPos = _messageStr.find(_via);
	if (viaPos != std::string::npos)
	{
		_messageStr.replace(viaPos, _via.length(), value);
	}
	_via = std::move(value);
}

void SipMessage::setFrom(std::string value)
{
	auto fromPos = _messageStr.find(_from);
	if (fromPos != std::string::npos)
	{
		_messageStr.replace(fromPos, _from.length(), value);
	}
	_fromNumber = extractNumber(value);
	_from = std::move(value);
}

void SipMessage::setTo(std::string value)
{
	auto toPos = _messageStr.find(_to);
	if (toPos != std::string::npos)
	{
		_messageStr.replace(toPos, _to.length(), value);
	}
	_toNumber = extractNumber(value);
	_to = std::move(value);
}

void SipMessage::setCallID(std::string value)
{
	auto callIdPos = _messageStr.find(_callID);
	if (callIdPos != std::string::npos)
	{
		_messageStr.replace(callIdPos, _callID.length(), value);
	}
	_callID = std::move(value);
}

void SipMessage::setCSeq(std::string value)
{
	auto cSeqPos = _messageStr.find(_cSeq);
	if (cSeqPos != std::string::npos)
	{
		_messageStr.replace(cSeqPos, _cSeq.length(), value);
	}
	_cSeq = std::move(value);
}

void SipMessage::setContact(std::string value)
{
	if (!_contact.empty()) {
		auto contactPos = _messageStr.find(_contact);
		if (contactPos != std::string::npos) {
			_messageStr.replace(contactPos, _contact.length(), value);
		}
	} else {
		// Guard against empty _contentLength: std::string::find("") returns 0, not npos,
		// which would insert before the start-line and corrupt the entire message.
		size_t clPos = _contentLength.empty()
		               ? std::string::npos
		               : _messageStr.find(_contentLength);
		if (clPos != std::string::npos) {
			_messageStr.insert(clPos, value + "\r\n");
		} else {
			_messageStr += value + "\r\n";
		}
	}
	_contact = std::move(value);
}

void SipMessage::setContentLength(std::string value)
{
	auto contentLengthPos = _messageStr.find(_contentLength);
	if (contentLengthPos != std::string::npos)
	{
		_messageStr.replace(contentLengthPos, _contentLength.length(), value);
	}
	_contentLength = std::move(value);
}

std::string SipMessage::toString() const
{
	return _messageStr;
}

const std::string& SipMessage::getType() const
{
	return _type;
}

const std::string& SipMessage::getHeader() const
{
	return _header;
}

const std::string& SipMessage::getVia() const
{
	return _via;
}

const std::string& SipMessage::getFrom() const
{
	return _from;
}

const std::string& SipMessage::getFromNumber() const
{
	return _fromNumber;
}

const std::string& SipMessage::getTo() const
{
	return _to;
}

const std::string& SipMessage::getToNumber() const
{
	return _toNumber;
}

const std::string& SipMessage::getCallID() const
{
	return _callID;
}

const std::string& SipMessage::getCSeq() const
{
	return _cSeq;
}

const std::string& SipMessage::getContact() const
{
	return _contact;
}

const std::string& SipMessage::getContactNumber() const
{
	return _contactNumber;
}

sockaddr_in SipMessage::getSource() const
{
	return _src;
}

const std::string& SipMessage::getContentLength() const
{
	return _contentLength;
}

std::string SipMessage::extractNumber(const std::string& header) const
{
	auto sipPos = header.find("sip:");
	if (sipPos == std::string::npos)
		return {};

	auto start = sipPos + 4;
	auto atPos = header.find('@', start);
	if (atPos == std::string::npos)
		return {};

	return header.substr(start, atPos - start);
}
