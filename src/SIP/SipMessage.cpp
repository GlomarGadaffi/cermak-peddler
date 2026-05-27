#include "SipMessage.hpp"
#include "SipMessageTypes.h"
#include "SipMessageHeaders.h"
#include <cstring>

SipMessage::SipMessage(std::string message, sockaddr_in src) : _messageStr(std::move(message)), _src(src)
{
	parse();
}

void SipMessage::parse()
{
	size_t pos_start = 0;
	size_t pos_end = _messageStr.find(SipMessageHeaders::HEADERS_DELIMETER);
	
	if (pos_end == std::string::npos) {
		throw std::runtime_error("Invalid message headers.");
	}

	_header = _messageStr.substr(pos_start, pos_end - pos_start);
	
	_type = _header.substr(0, _header.find(" "));
	if (_type == "SIP/2.0")
	{
		_type = _header;
	}

	pos_start = pos_end + std::strlen(SipMessageHeaders::HEADERS_DELIMETER);

	auto startsWith = [](const std::string& line, const char* hdr) {
		size_t len = std::strlen(hdr);
		return line.size() > len && line.compare(0, len, hdr) == 0
			&& (line[len] == ':' || line[len] == ' ');
	};

	while ((pos_end = _messageStr.find(SipMessageHeaders::HEADERS_DELIMETER, pos_start)) != std::string::npos) {
		if (pos_start == pos_end) {
			break;
		}

		std::string line = _messageStr.substr(pos_start, pos_end - pos_start);

		if (startsWith(line, SipMessageHeaders::VIA))
		{
			_via = line;
		}
		else if (startsWith(line, SipMessageHeaders::FROM))
		{
			_from = line;
			_fromNumber = extractNumber(line);
		}
		else if (startsWith(line, SipMessageHeaders::TO))
		{
			_to = line;
			_toNumber = extractNumber(line);
		}
		else if (startsWith(line, SipMessageHeaders::CALL_ID))
		{
			_callID = line;
		}
		else if (startsWith(line, SipMessageHeaders::CSEQ))
		{
			_cSeq = line;
		}
		else if (startsWith(line, SipMessageHeaders::CONTACT))
		{
			_contact = line;
			_contactNumber = extractNumber(line);
		}
		else if (startsWith(line, SipMessageHeaders::CONTENT_LENGTH))
		{
			_contentLength = line;
		}

		pos_start = pos_end + std::strlen(SipMessageHeaders::HEADERS_DELIMETER);
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
	_messageStr.replace(headerPos, _header.length(), value);
	_header = std::move(value);
}

void SipMessage::setVia(std::string value)
{
	auto viaPos = _messageStr.find(_via);
	_messageStr.replace(viaPos, _via.length(), value);
	_via = std::move(value);
}

void SipMessage::setFrom(std::string value)
{
	auto fromPos = _messageStr.find(_from);
	_messageStr.replace(fromPos, _from.length(), value);
	_fromNumber = extractNumber(value);
	_from = std::move(value);
}

void SipMessage::setTo(std::string value)
{
	auto toPos = _messageStr.find(_to);
	_messageStr.replace(toPos, _to.length(), value);
	_toNumber = extractNumber(value);
	_to = std::move(value);
}

void SipMessage::setCallID(std::string value)
{
	auto callIdPos = _messageStr.find(_callID);
	_messageStr.replace(callIdPos, _callID.length(), value);
	_callID = std::move(value);
}

void SipMessage::setCSeq(std::string value)
{
	auto cSeqPos = _messageStr.find(_cSeq);
	_messageStr.replace(cSeqPos, _cSeq.length(), value);
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
		auto clPos = _messageStr.find(_contentLength);
		if (clPos != std::string::npos) {
			_messageStr.insert(clPos, value + "\r\n");
		} else {
			// fallback at end
			_messageStr += value + "\r\n";
		}
	}
	_contact = std::move(value);
}

void SipMessage::setContentLength(std::string value)
{
	auto contentLengthPos = _messageStr.find(_contentLength);
	_messageStr.replace(contentLengthPos, _contentLength.length(), value);
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
