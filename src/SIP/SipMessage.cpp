#include "SipMessage.hpp"
#include "SipMessageTypes.h"
#include <cstring>
#include <cctype>

namespace
{
	bool iequal(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (std::tolower(static_cast<unsigned char>(a[i])) !=
				std::tolower(static_cast<unsigned char>(b[i]))) return false;
		return true;
	}

	// Header name = text before the first ':', with surrounding whitespace
	// trimmed (tolerates a leading space before a compact header name, e.g.
	// " v: SIP/2.0/UDP ...", and any padding around the colon).
	std::string_view headerNameOf(std::string_view line)
	{
		size_t colonPos = line.find(':');
		if (colonPos == std::string_view::npos) return {};
		size_t nameEnd = colonPos;
		while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(line[nameEnd - 1]))) --nameEnd;
		size_t nameStart = 0;
		while (nameStart < nameEnd && std::isspace(static_cast<unsigned char>(line[nameStart]))) ++nameStart;
		return line.substr(nameStart, nameEnd - nameStart);
	}

	// Header value = text after the first ':', with leading whitespace trimmed.
	std::string_view headerValueOf(std::string_view line)
	{
		size_t colon = line.find(':');
		if (colon == std::string_view::npos) return {};
		std::string_view v = line.substr(colon + 1);
		while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.remove_prefix(1);
		return v;
	}

	// Splits a raw SIP message into its start line, header lines (verbatim, in
	// order, duplicates preserved), and body. Tolerates bare-LF line endings and
	// a bare "\n\n" header/body separator — defensive parsing of untrusted
	// network input (SEC-02), mirroring the tolerance the old buffer-scanning
	// parse() had.
	void splitMessage(const std::string& raw, std::string& startLine,
		std::vector<std::string>& headerLines, std::string& body)
	{
		startLine.clear();
		headerLines.clear();
		body.clear();

		size_t bodyStart = raw.find("\r\n\r\n");
		size_t sepLen = 4;
		if (bodyStart == std::string::npos)
		{
			bodyStart = raw.find("\n\n");
			sepLen = 2;
		}

		std::string_view headerBlock;
		if (bodyStart != std::string::npos)
		{
			headerBlock = std::string_view(raw.data(), bodyStart);
			body = raw.substr(bodyStart + sepLen);
		}
		else
		{
			headerBlock = raw;
		}

		if (headerBlock.empty())
		{
			return;
		}

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
			startLine = std::string(headerBlock);
			return;
		}

		startLine = std::string(headerBlock.substr(pos_start, pos_end - pos_start));
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

			if (!line.empty())
			{
				headerLines.emplace_back(line);
			}
		}
	}
}

SipMessage::SipMessage(std::string message, sockaddr_in src) : _src(src)
{
	_hasSdp = (message.find("application/sdp") != std::string::npos);
	splitMessage(message, _startLine, _headerLines, _body);
}

void SipMessage::reset(std::string message, sockaddr_in src)
{
	_src = src;
	_hasSdp = (message.find("application/sdp") != std::string::npos);
	// splitMessage() clear()s _headerLines rather than reassigning it, so a
	// pooled message's vector capacity survives across reset() calls.
	splitMessage(message, _startLine, _headerLines, _body);
}

// NOTE (audit #68): setType() was removed. It was dead code (zero call sites,
// grep-confirmed across src/, main/, tests/, sketches/) and its body conflated a
// _header-relative offset with a replace() length — a latent foot-gun if a future
// caller ever reused it on a non-start-line header. Rather than leave a method that
// only happens to work for the start line, the dead helper was deleted. If a
// method-token rewrite is ever needed, use setHeader() to rewrite the full line.

void SipMessage::setHeader(std::string value)
{
	_startLine = std::move(value);
}

size_t SipMessage::findHeaderIndex(std::string_view fullName, std::string_view compactName) const
{
	for (size_t i = 0; i < _headerLines.size(); ++i)
	{
		std::string_view name = headerNameOf(_headerLines[i]);
		if (iequal(name, fullName)) return i;
		if (!compactName.empty() && iequal(name, compactName)) return i;
	}
	return std::string::npos;
}

void SipMessage::insertHeaderLine(std::string value)
{
	size_t clIdx = findHeaderIndex("content-length", "l");
	if (clIdx != std::string::npos)
	{
		_headerLines.insert(_headerLines.begin() + static_cast<long>(clIdx), std::move(value));
	}
	else
	{
		_headerLines.push_back(std::move(value));
	}
}

void SipMessage::setNamedHeader(std::string_view fullName, std::string_view compactName, std::string value)
{
	size_t idx = findHeaderIndex(fullName, compactName);
	if (idx != std::string::npos)
	{
		_headerLines[idx] = std::move(value);
	}
	else
	{
		insertHeaderLine(std::move(value));
	}
}

void SipMessage::setVia(std::string value)      { setNamedHeader("via", "v", std::move(value)); }
void SipMessage::setFrom(std::string value)     { setNamedHeader("from", "f", std::move(value)); }
void SipMessage::setTo(std::string value)       { setNamedHeader("to", "t", std::move(value)); }
void SipMessage::setCallID(std::string value)   { setNamedHeader("call-id", "i", std::move(value)); }
void SipMessage::setCSeq(std::string value)     { setNamedHeader("cseq", {}, std::move(value)); }
void SipMessage::setContact(std::string value)  { setNamedHeader("contact", "m", std::move(value)); }

void SipMessage::setContentLength(std::string value)
{
	// Unlike the other named setters, this one only ever updates an EXISTING
	// Content-Length header — every message we build already has one (it's
	// cloned from the incoming request), so there has never been an insert path
	// here, and syncContentLength()'s "absent -> no-op" contract depends on that.
	size_t idx = findHeaderIndex("content-length", "l");
	if (idx != std::string::npos)
	{
		_headerLines[idx] = std::move(value);
	}
}

void SipMessage::addHeader(const std::string& name, const std::string& value)
{
	insertHeaderLine(name + ": " + value);
}

void SipMessage::enforceG711()
{
	size_t mPos = _body.find("m=audio ");
	if (mPos != std::string::npos)
	{
		size_t lineEnd = _body.find("\r\n", mPos);
		if (lineEnd == std::string::npos) lineEnd = _body.find("\n", mPos);
		if (lineEnd == std::string::npos) lineEnd = _body.size();

		std::string_view mLine(_body.data() + mPos, lineEnd - mPos);
		size_t rtpPos = mLine.find("RTP/AVP ");
		if (rtpPos != std::string_view::npos)
		{
			std::string newMLine = std::string(mLine.substr(0, rtpPos + 8)) + "0 8 101";
			_body.replace(mPos, mLine.length(), newMLine);
		}
	}
	// Rewriting the codec list changed the SDP body size; resync Content-Length
	// so the answer isn't dropped as malformed on UDP.
	syncContentLength();
}

void SipMessage::syncContentLength()
{
	size_t idx = findHeaderIndex("content-length", "l");
	if (idx == std::string::npos)
	{
		return; // no Content-Length header present to update
	}

	// Preserve whichever header-name form the message already uses.
	bool fullForm = _headerLines[idx].find("Content-Length") != std::string::npos;
	std::string newLine = (fullForm ? "Content-Length: " : "l: ") + std::to_string(_body.size());
	_headerLines[idx] = std::move(newLine);
}

void SipMessage::clearBody()
{
	_body.clear();
	syncContentLength();
}

SipMessage::SdpDirection SipMessage::getSdpDirection() const
{
	const std::string_view whole(_body);
	size_t pos = 0;
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
		if (eol == std::string_view::npos)
		{
			break;
		}
		pos = eol + 1;
	}
	return SdpDirection::None;
}

std::string_view SipMessage::getBody() const
{
	return _body;
}

void SipMessage::setBody(const std::string& body)
{
	_body = body;
	syncContentLength();   // keep Content-Length honest (the 777-bug class)
}

std::string SipMessage::toString() const
{
	std::string out;
	out.reserve(_startLine.size() + 2 + _body.size() + 64);
	out += _startLine;
	out += "\r\n";
	for (const auto& line : _headerLines)
	{
		out += line;
		out += "\r\n";
	}
	out += "\r\n";
	out += _body;
	return out;
}

bool SipMessage::isValidMessage() const
{
	// Structural validity (SEC-02): reject empty payloads and packets whose
	// start line / method-or-status token could not be parsed. A well-formed
	// SIP message always yields a non-empty start line and type token.
	if (_startLine.empty()) return false;
	if (getType().empty()) return false;
	return true;
}

std::string_view SipMessage::getType() const
{
	size_t sp = _startLine.find(' ');
	std::string_view first = (sp == std::string::npos)
		? std::string_view(_startLine)
		: std::string_view(_startLine).substr(0, sp);
	if (first == "SIP/2.0")
	{
		return _startLine;
	}
	return first;
}

std::string_view SipMessage::getHeader() const
{
	return _startLine;
}

std::string_view SipMessage::getVia() const
{
	size_t idx = findHeaderIndex("via", "v");
	return idx == std::string::npos ? std::string_view{} : std::string_view(_headerLines[idx]);
}

std::string_view SipMessage::getFrom() const
{
	size_t idx = findHeaderIndex("from", "f");
	return idx == std::string::npos ? std::string_view{} : std::string_view(_headerLines[idx]);
}

std::string_view SipMessage::getFromNumber() const
{
	return extractNumber(getFrom());
}

std::string_view SipMessage::getTo() const
{
	size_t idx = findHeaderIndex("to", "t");
	return idx == std::string::npos ? std::string_view{} : std::string_view(_headerLines[idx]);
}

std::string_view SipMessage::getToNumber() const
{
	return extractNumber(getTo());
}

std::string_view SipMessage::getCallID() const
{
	size_t idx = findHeaderIndex("call-id", "i");
	return idx == std::string::npos ? std::string_view{} : std::string_view(_headerLines[idx]);
}

std::string_view SipMessage::getCSeq() const
{
	size_t idx = findHeaderIndex("cseq");
	return idx == std::string::npos ? std::string_view{} : std::string_view(_headerLines[idx]);
}

std::string_view SipMessage::getViaBranch() const
{
	std::string_view via = getVia();
	auto pos = via.find("branch=");
	if (pos == std::string_view::npos) return {};
	pos += 7;
	auto end = via.find(';', pos);
	if (end == std::string_view::npos) end = via.size();
	return via.substr(pos, end - pos);
}

std::string_view SipMessage::getCSeqMethod() const
{
	std::string_view cSeq = getCSeq();
	auto sp = cSeq.rfind(' ');
	if (sp == std::string_view::npos) return {};
	auto method = cSeq.substr(sp + 1);
	while (!method.empty() && (method.back() == ' ' || method.back() == '\r' || method.back() == '\n'))
		method.remove_suffix(1);
	return method;
}

uint32_t SipMessage::getSessionExpiresSecs() const
{
	size_t idx = findHeaderIndex("session-expires");
	if (idx == std::string::npos) return 0;
	std::string_view v = headerValueOf(_headerLines[idx]);
	uint32_t val = 0;
	size_t i = 0;
	while (i < v.size() && v[i] >= '0' && v[i] <= '9')
		val = val * 10 + static_cast<uint32_t>(v[i++] - '0');
	return val;
}

std::string_view SipMessage::getSessionExpiresRefresher() const
{
	size_t idx = findHeaderIndex("session-expires");
	if (idx == std::string::npos) return {};
	std::string_view line = _headerLines[idx];
	size_t rp = line.find("refresher=");
	if (rp == std::string_view::npos) return {};
	size_t vs = rp + 10;
	size_t ve = line.find_first_of("; \t\r\n", vs);
	if (ve == std::string_view::npos) ve = line.size();
	return line.substr(vs, ve - vs);
}

uint32_t SipMessage::getMinSESecs() const
{
	size_t idx = findHeaderIndex("min-se");
	if (idx == std::string::npos) return 0;
	std::string_view v = headerValueOf(_headerLines[idx]);
	uint32_t val = 0;
	size_t i = 0;
	while (i < v.size() && v[i] >= '0' && v[i] <= '9')
		val = val * 10 + static_cast<uint32_t>(v[i++] - '0');
	return val;
}

std::string_view SipMessage::getContact() const
{
	size_t idx = findHeaderIndex("contact", "m");
	return idx == std::string::npos ? std::string_view{} : std::string_view(_headerLines[idx]);
}

std::string_view SipMessage::getContactNumber() const
{
	return extractNumber(getContact());
}

sockaddr_in SipMessage::getSource() const
{
	return _src;
}

std::string_view SipMessage::getContentLength() const
{
	size_t idx = findHeaderIndex("content-length", "l");
	return idx == std::string::npos ? std::string_view{} : std::string_view(_headerLines[idx]);
}

std::string_view SipMessage::getAuthorization() const
{
	size_t idx = findHeaderIndex("authorization");
	return idx == std::string::npos ? std::string_view{} : std::string_view(_headerLines[idx]);
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
