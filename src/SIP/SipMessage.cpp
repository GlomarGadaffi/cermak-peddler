#include "SipMessage.hpp"
#include "SipMessageTypes.h"
#include <cstring>
#include <cctype>

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
	_authorization = {};
	_statusInfo.reset();

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
		_statusInfo = PocketDial::parseSipStatusLine(_header);
		return;
	}

	_header = headerBlock.substr(pos_start, pos_end - pos_start);
	_type = _header.substr(0, _header.find(" "));
	if (_type == "SIP/2.0")
	{
		_type = _header;
	}
	_statusInfo = PocketDial::parseSipStatusLine(_header);

	// Helper to match headers case-insensitively and handle compact names.
	// Zero-allocation: compares characters in-place via tolower rather than
	// constructing std::string temporaries for the name and each candidate.
	auto matchHeader = [](std::string_view line, std::string_view fullHdr, std::string_view compactHdr = "") -> bool {
		size_t colonPos = line.find(':');
		if (colonPos == std::string::npos)
		{
			return false;
		}

		size_t nameEnd = colonPos;
		while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(line[nameEnd - 1]))) --nameEnd;
		size_t nameStart = 0;
		while (nameStart < nameEnd && std::isspace(static_cast<unsigned char>(line[nameStart]))) ++nameStart;
		std::string_view name = line.substr(nameStart, nameEnd - nameStart);

		auto iequal = [](std::string_view a, std::string_view b) -> bool {
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (std::tolower(static_cast<unsigned char>(a[i])) !=
					std::tolower(static_cast<unsigned char>(b[i]))) return false;
			return true;
		};

		if (iequal(name, fullHdr)) return true;
		if (!compactHdr.empty() && iequal(name, compactHdr)) return true;
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
		else if (matchHeader(line, "authorization"))
		{
			// Whole header line ("Authorization: Digest ..."); SipDigest's parser
			// tolerates the leading header name. No compact form for Authorization.
			_authorization = line;
		}
	}
}

// NOTE (audit #68): setType() was removed. It was dead code (zero call sites,
// grep-confirmed across src/, main/, tests/, sketches/) and its body conflated a
// _header-relative offset with a replace() length — a latent foot-gun if a future
// caller ever reused it on a non-start-line header. Rather than leave a method that
// only happens to work for the start line, the dead helper was deleted. If a
// method-token rewrite is ever needed, mirror setHeader()'s findHeader()+length
// pattern (replace(pos, tokenLen, value)).

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

void SipMessage::addHeader(const std::string& name, const std::string& value)
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
	// Rewriting the codec list changed the SDP body size. Refresh the cached
	// header views (the replace() above may have reallocated _messageStr), then
	// resync Content-Length so the answer isn't dropped as malformed on UDP.
	reparse();
	syncContentLength();
}

void SipMessage::syncContentLength()
{
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
		return; // no header/body separator → nothing to size
	}

	size_t bodyLen = (bodyStart <= _messageStr.size()) ? _messageStr.size() - bodyStart : 0;

	if (_contentLength.empty())
	{
		return; // no Content-Length header present to update
	}

	// Preserve whichever header-name form the message already uses.
	if (_contentLength.find("Content-Length") != std::string_view::npos)
	{
		setContentLength("Content-Length: " + std::to_string(bodyLen));
	}
	else
	{
		setContentLength("l: " + std::to_string(bodyLen));
	}
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

SipMessage::SdpDirection SipMessage::getSdpDirection() const
{
	// Locate the body (after the header/body separator). Mirrors parse()'s
	// tolerance for bare-LF messages.
	size_t bodyStart = _messageStr.find("\r\n\r\n");
	size_t sepLen = 4;
	if (bodyStart == std::string::npos)
	{
		bodyStart = _messageStr.find("\n\n");
		sepLen = 2;
	}
	if (bodyStart == std::string::npos)
	{
		return SdpDirection::None;
	}

	// Walk the body line by line; the attribute must be line-anchored ("a=..."
	// at the start of a line) so a stray substring elsewhere can't match.
	const std::string_view whole(_messageStr);
	size_t pos = bodyStart + sepLen;
	while (pos < whole.size())
	{
		size_t eol = whole.find('\n', pos);
		size_t lineEnd = (eol == std::string_view::npos) ? whole.size() : eol;
		std::string_view line = whole.substr(pos, lineEnd - pos);
		if (!line.empty() && line.back() == '\r')
		{
			line.remove_suffix(1);
		}
		if (line == "a=sendrecv") return SdpDirection::SendRecv;
		if (line == "a=sendonly") return SdpDirection::SendOnly;
		if (line == "a=recvonly") return SdpDirection::RecvOnly;
		if (line == "a=inactive") return SdpDirection::Inactive;
		if (eol == std::string::npos)
		{
			break;
		}
		pos = eol + 1;
	}
	return SdpDirection::None;
}

std::string_view SipMessage::getBody() const
{
	size_t sep = _messageStr.find("\r\n\r\n");
	size_t sepLen = 4;
	if (sep == std::string::npos)
	{
		sep = _messageStr.find("\n\n");
		sepLen = 2;
	}
	if (sep == std::string::npos)
	{
		return {};
	}
	return std::string_view(_messageStr).substr(sep + sepLen);
}

void SipMessage::setBody(const std::string& body)
{
	size_t sep = _messageStr.find("\r\n\r\n");
	size_t sepLen = 4;
	if (sep == std::string::npos)
	{
		sep = _messageStr.find("\n\n");
		sepLen = 2;
	}
	if (sep == std::string::npos)
	{
		// No header/body separator yet: append one, then the body.
		_messageStr += "\r\n\r\n";
		sep = _messageStr.size() - 4;
		sepLen = 4;
	}
	_messageStr.erase(sep + sepLen);
	_messageStr += body;
	syncContentLength();   // keep Content-Length honest (the 777-bug class)
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

std::string_view SipMessage::getViaBranch() const
{
	auto pos = _via.find("branch=");
	if (pos == std::string_view::npos) return {};
	pos += 7;
	auto end = _via.find(';', pos);
	if (end == std::string_view::npos) end = _via.size();
	return _via.substr(pos, end - pos);
}

std::string_view SipMessage::getCSeqMethod() const
{
	auto sp = _cSeq.rfind(' ');
	if (sp == std::string_view::npos) return {};
	auto method = _cSeq.substr(sp + 1);
	while (!method.empty() && (method.back() == ' ' || method.back() == '\r' || method.back() == '\n'))
		method.remove_suffix(1);
	return method;
}

uint32_t SipMessage::getSessionExpiresSecs() const
{
	size_t sep = _messageStr.find("\r\n\r\n");
	size_t limit = (sep != std::string::npos) ? sep : _messageStr.size();
	std::string_view hdrs(_messageStr.data(), limit);
	size_t pos = hdrs.find("Session-Expires:");
	if (pos == std::string_view::npos) pos = hdrs.find("session-expires:");
	if (pos == std::string_view::npos) return 0;
	pos += 16; // len("Session-Expires:")
	while (pos < limit && (hdrs[pos] == ' ' || hdrs[pos] == '\t')) ++pos;
	uint32_t v = 0;
	while (pos < limit && hdrs[pos] >= '0' && hdrs[pos] <= '9')
		v = v * 10 + static_cast<uint32_t>(hdrs[pos++] - '0');
	return v;
}

std::string_view SipMessage::getSessionExpiresRefresher() const
{
	size_t sep = _messageStr.find("\r\n\r\n");
	size_t limit = (sep != std::string::npos) ? sep : _messageStr.size();
	std::string_view hdrs(_messageStr.data(), limit);
	size_t pos = hdrs.find("Session-Expires:");
	if (pos == std::string_view::npos) pos = hdrs.find("session-expires:");
	if (pos == std::string_view::npos) return {};
	size_t eol = hdrs.find("\r\n", pos);
	if (eol == std::string_view::npos) eol = limit;
	std::string_view line = hdrs.substr(pos, eol - pos);
	size_t rp = line.find("refresher=");
	if (rp == std::string_view::npos) return {};
	size_t vs = rp + 10;
	size_t ve = line.find_first_of("; \t\r\n", vs);
	if (ve == std::string_view::npos) ve = line.size();
	return line.substr(vs, ve - vs);
}

uint32_t SipMessage::getMinSESecs() const
{
	size_t sep = _messageStr.find("\r\n\r\n");
	size_t limit = (sep != std::string::npos) ? sep : _messageStr.size();
	std::string_view hdrs(_messageStr.data(), limit);
	size_t pos = hdrs.find("Min-SE:");
	if (pos == std::string_view::npos) pos = hdrs.find("min-se:");
	if (pos == std::string_view::npos) return 0;
	pos += 7; // len("Min-SE:")
	while (pos < limit && (hdrs[pos] == ' ' || hdrs[pos] == '\t')) ++pos;
	uint32_t v = 0;
	while (pos < limit && hdrs[pos] >= '0' && hdrs[pos] <= '9')
		v = v * 10 + static_cast<uint32_t>(hdrs[pos++] - '0');
	return v;
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

std::string_view SipMessage::getAuthorization() const
{
	return _authorization;
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
