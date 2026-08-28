// SipStatus.cpp
#include "SipStatus.hpp"
#include <charconv>

namespace PocketDial {
namespace {

// cppcheck flags every scalar member below for having no constructor to
// assign it (uninitMemberVarNoCtor). False positive: StatusEntry is a
// deliberate plain aggregate -- the only instance is the constexpr
// kStatusTable below, where every element is brace-initialised with all
// three fields, so nothing is ever read uninitialised. A constructor would
// defeat the point of a constexpr ROM table.
struct StatusEntry {
    // cppcheck-suppress uninitMemberVarNoCtor
    uint16_t         code;
    std::string_view reason;
    // cppcheck-suppress uninitMemberVarNoCtor
    bool             softFail;
};

// constexpr = ROM on ESP32, no heap, no init cost
constexpr StatusEntry kStatusTable[] = {
    // 1xx Provisional
    { 100, "Trying",                          false },
    { 180, "Ringing",                         false },
    { 181, "Call Is Being Forwarded",         false },
    { 182, "Queued",                          false },
    { 183, "Session Progress",                false },

    // 2xx Success
    { 200, "OK",                              false },
    { 202, "Accepted",                        false },

    // 4xx — soft = transient/retryable, hard = structural/permanent
    { 400, "Bad Request",                     false }, // our bug
    { 401, "Unauthorized",                    true  }, // auth challenge, expected
    { 403, "Forbidden",                       false }, // permanent
    { 404, "Not Found",                       true  }, // wrong URI, log+continue
    { 405, "Method Not Allowed",              true  }, // retry sans method
    { 407, "Proxy Auth Required",             true  }, // auth challenge
    { 408, "Request Timeout",                 true  }, // transient
    { 410, "Gone",                            false }, // permanent
    { 413, "Request Entity Too Large",        true  }, // reducible
    { 415, "Unsupported Media Type",          false }, // codec mismatch, hard
    { 420, "Bad Extension",                   false }, // we sent bad headers
    { 480, "Temporarily Unavailable",         true  }, // transient
    { 481, "Call/Transaction Does Not Exist", false }, // state mismatch
    { 486, "Busy Here",                       true  }, // transient
    { 487, "Request Terminated",              true  }, // expected CANCEL ack
    { 488, "Not Acceptable Here",             false }, // media negotiation fail
};

const StatusEntry* lookupStatus(uint16_t code) noexcept {
    for (const auto& e : kStatusTable)
        if (e.code == code) return &e;
    return nullptr;
}

} // namespace

SipStatusClass sipStatusClass(uint16_t code) noexcept {
    if (code >= 100 && code < 200) return SipStatusClass::Provisional;
    if (code >= 200 && code < 300) return SipStatusClass::Success;
    if (code >= 300 && code < 400) return SipStatusClass::Redirection;
    if (code >= 400 && code < 500) return SipStatusClass::ClientError;
    if (code >= 500 && code < 600) return SipStatusClass::ServerError;
    if (code >= 600 && code < 700) return SipStatusClass::GlobalFail;
    return SipStatusClass::Unknown;
}

std::optional<SipStatusInfo> parseSipStatusLine(std::string_view line) noexcept {
    // Strip CRLF
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);

    // Must start with "SIP/2.0 "
    constexpr std::string_view kPrefix = "SIP/2.0 ";
    if (line.size() < kPrefix.size() + 3) return std::nullopt;
    if (line.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    line.remove_prefix(kPrefix.size());

    // Parse exactly 3-digit code
    uint16_t code = 0;
    auto [ptr, ec] = std::from_chars(line.data(), line.data() + 3, code);
    if (ec != std::errc{} || code < 100 || code > 699) return std::nullopt;

    // Pure parse: no I/O, no side effects. The reason phrase is intentionally not
    // stored so SipStatusInfo stays a trivially-copyable POD. Diagnostics for 4xx
    // are emitted once at the routing site (off the lock) rather than on every
    // parse() -- which runs on every construct/copy/reset/setter.
    const auto  klass = sipStatusClass(code);
    const auto* entry = lookupStatus(code);
    const bool  softFail = (klass == SipStatusClass::ClientError)
                         ? (entry ? entry->softFail : true)  // unknown 4xx: soft
                         : false;

    // Positional aggregate init (not designated) to stay C++17-compatible: MSVC
    // rejects designated initializers below /std:c++20. Field order: code, klass, softFail.
    return SipStatusInfo{ code, klass, softFail };
}

} // namespace PocketDial
