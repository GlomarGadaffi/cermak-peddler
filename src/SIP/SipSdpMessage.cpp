#include "SipSdpMessage.hpp"
#include <string>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <cctype>

SipSdpMessage::SipSdpMessage(const std::string& message, sockaddr_in src) : SipMessage(message, src)
{
}

SipSdpMessage& SipSdpMessage::operator=(const SipSdpMessage& other)
{
	if (this == &other) return *this;

	SipMessage::operator=(other);   // copies the body, advances _bodyGen
	// Deliberately NOT copying _spans/_spansGen — see the header. Generation 0
	// is never a live body generation, so this forces a re-parse on next access.
	_spansGen = 0;
	return *this;
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

// Issue #101(B): one single-pass parse of the body, cached until the body
// changes. Line splitting mirrors the old per-accessor parseSdpFields() exactly
// (primary "\r\n" delimiter, bare "\n" fallback) so behavior on mixed/malformed
// line endings is unchanged, as is last-one-wins when a body repeats a field.
const SipSdpMessage::SdpSpans& SipSdpMessage::ensureParsed() const
{
	const uint64_t gen = bodyGeneration();
	if (_spansGen == gen)
	{
		return _spans;   // body has not been touched since the last parse
	}

	const std::string_view body = getBody();

	// Rebuilt from scratch rather than updated in place: a field present in the
	// PREVIOUS body and absent from this one must not survive in the cache. This
	// is the recycled-pool-slot case (reset() hands the same object back out with
	// a different call's SDP), which is precisely what makes a stale cache here
	// dangerous rather than merely wrong.
	SdpSpans spans;

	size_t pos_start = 0;
	while (pos_start < body.size())
	{
		const size_t lineStart = pos_start;
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

		const FieldSpan span{static_cast<uint32_t>(lineStart), static_cast<uint32_t>(line.size())};
		if (line.compare(0, 2, "v=") == 0)      spans.version = span;
		else if (line.compare(0, 2, "o=") == 0) spans.originator = span;
		else if (line.compare(0, 2, "s=") == 0) spans.sessionName = span;
		else if (line.compare(0, 2, "c=") == 0) spans.connectionInformation = span;
		else if (line.compare(0, 2, "t=") == 0) spans.time = span;
		else if (line.compare(0, 2, "m=") == 0) spans.media = span;
	}

	_spans    = spans;
	_spansGen = gen;
	return _spans;
}

std::string_view SipSdpMessage::viewOf(const FieldSpan& span) const
{
	if (span.len == 0) return {};   // field absent — same empty view as before

	// Belt-and-braces: ensureParsed() guarantees the span indexes the body it was
	// parsed from, so an out-of-range span means the generation counter missed a
	// mutation. Report the field absent rather than let substr() throw
	// std::out_of_range out of the middle of a SIP handler — exceptions are
	// enabled in the ESP-IDF build (CONFIG_COMPILER_CXX_EXCEPTIONS=y) but nothing
	// up the call stack catches, so the throw would take the SIP task with it.
	// The invariant is still enforced by the tests, not by this clamp.
	const std::string_view body = getBody();
	if (span.pos > body.size() || span.len > body.size() - span.pos) return {};
	return body.substr(span.pos, span.len);
}

std::string_view SipSdpMessage::getVersion() const
{
	return viewOf(ensureParsed().version);
}

std::string_view SipSdpMessage::getOriginator() const
{
	return viewOf(ensureParsed().originator);
}

std::string_view SipSdpMessage::getSessionName() const
{
	return viewOf(ensureParsed().sessionName);
}

std::string_view SipSdpMessage::getConnectionInformation() const
{
	return viewOf(ensureParsed().connectionInformation);
}

std::string_view SipSdpMessage::getTime() const
{
	return viewOf(ensureParsed().time);
}

std::string_view SipSdpMessage::getMedia() const
{
	return viewOf(ensureParsed().media);
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
