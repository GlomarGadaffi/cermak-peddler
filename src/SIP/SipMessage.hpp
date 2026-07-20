#ifndef SIP_MESSAGE_HPP
#define SIP_MESSAGE_HPP

#if defined(ESP_PLATFORM) || defined(ESP32)
#include <lwip/sockets.h>
#undef INADDR_NONE
#elif defined(__linux__)
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#endif

#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <optional>
#include <vector>
#include <cstdint>

#include "SipStatus.hpp"

class SipMessage
{
public:

	SipMessage(std::string message, sockaddr_in src);
	virtual ~SipMessage() = default;

	void reset(std::string message, sockaddr_in src);

	// No shared buffer means no string_view to fix up after a copy — plain
	// member-wise copy of the owned start line / header lines / body is already
	// correct.
	SipMessage(const SipMessage& other) = default;
	SipMessage& operator=(const SipMessage& other) = default;

	// setType() removed (audit #68): dead, and conflated a header-relative offset
	// with a replace() length. Use setHeader() to rewrite the full start line.
	void setHeader(std::string value);
	void setVia(std::string value);
	void setFrom(std::string value);
	void setTo(std::string value);
	void setCallID(std::string value);
	void setCSeq(std::string value);
	void setContact(std::string value);
	void setContentLength(std::string value);
	void addHeader(const std::string& name, const std::string& value);
	void enforceG711();
	void clearBody();

	// The message body — everything after the header/body separator (the SDP for
	// an INVITE/200 OK), or empty when there is none. The view is valid until the
	// next mutation of this message. setBody() replaces the body and resyncs
	// Content-Length; used by the call-park retrieve path to swap each leg's SDP
	// onto the opposite dialog so media renegotiates peer-to-peer.
	std::string_view getBody() const;
	void setBody(const std::string& body);

	// Recompute the Content-Length header from the actual body byte count and
	// rewrite it in place (preserving the full/compact header-name form). Call
	// after any edit that changes the body length; an out-of-sync Content-Length
	// causes UDP peers to discard the message as truncated/malformed.
	void syncContentLength();


	std::string_view getType() const;
	std::string_view getHeader() const;
	std::string_view getVia() const;
	std::string_view getFrom() const;
	std::string_view getFromNumber() const;
	std::string_view getTo() const;
	std::string_view getToNumber() const;
	std::string_view getCallID() const;
	std::string_view getCSeq() const;
	std::string_view getViaBranch() const;   // branch= param extracted from Via header
	std::string_view getCSeqMethod() const;  // method token extracted from CSeq header
	// RFC 4028 session timer headers (0 / empty when header absent).
	uint32_t         getSessionExpiresSecs() const;
	std::string_view getSessionExpiresRefresher() const; // "uac", "uas", or empty
	uint32_t         getMinSESecs() const;
	std::string_view getContact() const;
	std::string_view getContactNumber() const;
	std::string_view getContentLength() const;
	// Full `Authorization:` request-header line (or empty if absent). The value
	// is fed to SipDigest::parseAuthorization, which tolerates the header name.
	std::string_view getAuthorization() const;
	sockaddr_in getSource() const;
	std::optional<PocketDial::SipStatusInfo> getStatusInfo() const
	{
		return PocketDial::parseSipStatusLine(_startLine);
	}

	// SDP media-direction attribute (RFC 4566 / RFC 3264): the line-anchored
	// a=sendrecv / a=sendonly / a=recvonly / a=inactive attribute in the message
	// body. Returns None when there is no body or no direction attribute (RFC
	// 3264: an absent attribute implies sendrecv — the caller decides; we only
	// report what is on the wire). Pure string scan, host-compilable.
	enum class SdpDirection { None, SendRecv, SendOnly, RecvOnly, Inactive };
	SdpDirection getSdpDirection() const;

	// Issue #42: virtual SDP probe replaces dynamic_cast so call setup works
	// on the Arduino ESP32 toolchain, which builds with RTTI disabled (-fno-rtti).
	virtual bool hasSdp() const { return _hasSdp; }

	std::string toString() const;
	bool isValidMessage() const;

protected:
	std::string_view extractNumber(std::string_view header) const;

	bool _hasSdp = false;

private:
	// Every getter/setter below resolves against these three owned pieces —
	// no shared buffer, so mutating one header cannot shift or invalidate any
	// other. There is deliberately no cached "parsed field" state left to keep
	// in sync: named getters look their header up by name on every call (a
	// handful of short string compares against _headerLines, not a rescan of
	// the whole message), so nothing needs a reparse step after a mutation.
	std::string              _startLine;
	std::vector<std::string> _headerLines;
	std::string              _body;

	sockaddr_in _src{};

	size_t findHeaderIndex(std::string_view fullName, std::string_view compactName = {}) const;
	// Inserts a new header line just before Content-Length (matching the wire
	// position addHeader() has always used), or at the end of the header block
	// if there is no Content-Length header.
	void insertHeaderLine(std::string value);
	// Shared body for the six setters that only ever replace-or-insert a single
	// named header line (setVia/setFrom/setTo/setCallID/setCSeq/setContact).
	void setNamedHeader(std::string_view fullName, std::string_view compactName, std::string value);
};

#endif
