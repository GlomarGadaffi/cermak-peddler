#include "BlfSubscriptions.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "IDGen.hpp"
#include "Session.hpp"
#include "SipClient.hpp"
#include "SipMessageTypes.h"

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <lwip/sockets.h>
#elif defined(__linux__)
#include <arpa/inet.h>
#endif

std::string BlfSubscriptions::parseEventPackage(const std::string& raw)
{
	size_t pos = 0;
	while (pos < raw.size())
	{
		size_t nl = raw.find('\n', pos);
		size_t lineEnd = (nl == std::string::npos) ? raw.size() : nl;
		size_t next = (nl == std::string::npos) ? raw.size() : nl + 1;
		if (raw[pos] == '\r' || raw[pos] == '\n') break;

		size_t colon = raw.find(':', pos);
		if (colon != std::string::npos && colon < lineEnd)
		{
			std::string name = raw.substr(pos, colon - pos);
			std::transform(name.begin(), name.end(), name.begin(),
				[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
			if (name == "event" || name == "o")
			{
				size_t vBegin = colon + 1;
				size_t vEnd = lineEnd;
				std::string val = raw.substr(vBegin, vEnd - vBegin);
				size_t semi = val.find(';');
				if (semi != std::string::npos) val.erase(semi);
				size_t b = val.find_first_not_of(" \t\r");
				size_t e = val.find_last_not_of(" \t\r");
				if (b == std::string::npos) return "";
				return val.substr(b, e - b + 1);
			}
		}
		pos = next;
	}
	return "";
}

std::string BlfSubscriptions::buildDialogInfoXml(const std::string& entity, unsigned version,
	const std::string& dialogId, const std::string& state, const std::string& direction)
{
	// Minimal RFC 4235 document, always state="full": each NOTIFY carries the
	// complete (zero-or-one element) dialog set, so watchers never need to merge
	// partials. An empty `state` omits the <dialog> element — the canonical
	// "idle lamp" body that Yealink/Grandstream BLF keys expect.
	std::ostringstream xml;
	xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
	    << "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\""
	    << version << "\" state=\"full\" entity=\"" << entity << "\">\r\n";
	if (!state.empty())
	{
		xml << "<dialog id=\"" << dialogId << "\"";
		if (!direction.empty()) xml << " direction=\"" << direction << "\"";
		xml << "><state>" << state << "</state></dialog>\r\n";
	}
	xml << "</dialog-info>\r\n";
	return xml.str();
}

std::string BlfSubscriptions::computeDialogState(const std::string& targetAor,
	std::string& outDirection, std::string& outDialogId) const
{
	std::string best;
	auto rank = [](const std::string& s) -> int {
		if (s == "confirmed") return 3;
		if (s == "early")     return 2;
		if (s == "trying")    return 1;
		return 0;
	};
	for (const auto& [callID, session] : _env.sessionsView())
	{
		bool isSrc  = session->getSrc()  && session->getSrc()->getNumber()  == targetAor;
		bool isDest = session->getDest() && session->getDest()->getNumber() == targetAor;
		if (!isSrc && !isDest) continue;

		std::string state, dir;
		switch (session->getState())
		{
			case Session::State::Invited:
				state = isDest ? "early" : "trying";
				dir   = isDest ? "recipient" : "initiator";
				break;
			case Session::State::Connected:
			case Session::State::Held:
				// RFC 4235 §4: a held call is an established dialog; state stays
				// "confirmed" so the watcher's BLF lamp remains lit (#53).
				state = "confirmed";
				dir   = isSrc ? "initiator" : "recipient";
				break;
			default:
				continue;
		}
		if (rank(state) > rank(best))
		{
			best = state;
			outDirection = dir;
			outDialogId  = callID;
		}
	}
	if (best.empty()) { outDirection.clear(); outDialogId.clear(); }
	return best;
}

std::shared_ptr<SipMessage> BlfSubscriptions::buildDialogNotify(DialogSubscription& sub,
	const std::string& state, const std::string& direction, const std::string& dialogId,
	bool terminated, const char* termReason)
{
	std::string activeIp = _env.localIp();
	std::string srcIpPort = activeIp + ":" + std::to_string(_env.serverPort());
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &sub.addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(sub.addr.sin_port));
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);

	std::string entity = "sip:" + sub.targetAor + "@" + srcIpPort;
	std::string body = buildDialogInfoXml(entity, sub.version, dialogId, state, direction);

	int remaining = 0;
	if (!terminated)
	{
		auto now = std::chrono::steady_clock::now();
		remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
			sub.deadline - now).count());
		if (remaining < 0) remaining = 0;
	}
	std::string subState = terminated
		? ("terminated;reason=" + std::string(termReason))
		: ("active;expires=" + std::to_string(remaining));

	// NOTIFY runs inside the subscription dialog: our To-tag becomes the From,
	// the watcher's From becomes the To (RFC 6665 §4.4.1 — roles swap).
	std::string fromHdr = sub.subTo;
	std::string toHdr   = sub.watcherFrom;
	auto stripName = [](const std::string& h) {
		size_t c = h.find(':');
		return (c == std::string::npos) ? h : h.substr(c + 1);
	};

	std::ostringstream ss;
	ss << "NOTIFY sip:" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From:" << stripName(fromHdr) << "\r\n"
	   << "To:" << stripName(toHdr) << "\r\n"
	   << "Call-ID: " << sub.callId << "\r\n"
	   << "CSeq: " << sub.cseq++ << " NOTIFY\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Event: dialog\r\n"
	   << "Subscription-State: " << subState << "\r\n"
	   << "Contact: <sip:" << sub.targetAor << "@" << srcIpPort << ">\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: application/dialog-info+xml\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	return _env.messageFromPool(ss.str(), sub.addr);
}

void BlfSubscriptions::onSubscribe(const std::shared_ptr<SipMessage>& data)
{
	const std::string& activeIp = _env.localIp();

	// 1. Event-package gate: only the RFC 4235 "dialog" package is implemented.
	std::string pkg = parseEventPackage(data->toString());
	if (pkg != "dialog")
	{
		auto resp = _env.messageFromPool(data->toString(), data->getSource());
		resp->setHeader(SipMessageTypes::BAD_EVENT);
		resp->clearBody();
		resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		resp->addHeader("Allow-Events", "dialog");
		_env.enqueue(data->getSource(), std::move(resp));
		return;
	}

	// 2. Watched-target validation.
	std::string target(data->getToNumber());
	if (!_env.validAor(target))
	{
		auto resp = _env.messageFromPool(data->toString(), data->getSource());
		resp->setHeader(SipMessageTypes::BAD_REQUEST);
		resp->clearBody();
		resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_env.enqueue(data->getSource(), std::move(resp));
		return;
	}

	const int expires = _env.requestedExpires(data);
	std::string callId(data->getCallID());
	{
		size_t c = callId.find(':');
		if (c != std::string::npos) callId.erase(0, c + 1);
		size_t b = callId.find_first_not_of(" \t");
		size_t e = callId.find_last_not_of(" \t\r");
		callId = (b == std::string::npos) ? "" : callId.substr(b, e - b + 1);
	}

	// 3. Refresh / unsubscribe: match existing subscription by Call-ID.
	DialogSubscription* sub = nullptr;
	for (auto& s : _subscriptions)
	{
		if (s.used && s.callId == callId) { sub = &s; break; }
	}

	if (sub == nullptr && expires > 0)
	{
		for (auto& s : _subscriptions)
		{
			if (!s.used) { sub = &s; break; }
		}
		if (sub == nullptr)
		{
			auto resp = _env.messageFromPool(data->toString(), data->getSource());
			resp->setHeader("SIP/2.0 503 Service Unavailable");
			resp->clearBody();
			resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
			_env.enqueue(data->getSource(), std::move(resp));
			_env.log("BLF: subscription pool exhausted, 503 to watcher of " + target, true);
			return;
		}
		*sub = DialogSubscription{};
		sub->used        = true;
		sub->callId      = callId;
		sub->watcherFrom = std::string(data->getFrom());
		sub->subTo       = std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9);
		sub->targetAor   = target;
		_env.log("BLF: new subscription, watcher " + std::string(data->getFromNumber())
			+ " -> target " + target);
	}

	// 4. 202 Accepted (RFC 6665 §4.2.1).
	{
		auto resp = _env.messageFromPool(data->toString(), data->getSource());
		resp->setHeader(SipMessageTypes::ACCEPTED);
		resp->clearBody();
		resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		if (sub) resp->setTo(sub->subTo);
		else     resp->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
		resp->setContact(_env.contactFor(target));
		resp->addHeader("Expires", std::to_string(expires));
		_env.enqueue(data->getSource(), std::move(resp));
	}

	if (sub == nullptr) return;

	sub->addr       = data->getSource();
	sub->expiresSec = expires;
	sub->deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(expires);

	// 5. Immediate NOTIFY (RFC 6665 §4.2.1.4).
	std::string dir, dialogId;
	std::string state = computeDialogState(target, dir, dialogId);
	const bool terminating = (expires == 0);
	auto notify = buildDialogNotify(*sub, state, dir, dialogId, terminating, "noresource");
	_env.enqueue(sub->addr, std::move(notify));
	sub->lastState = state + "|" + dir + "|" + dialogId;
	sub->version++;

	if (terminating)
	{
		_env.log("BLF: unsubscribe, watcher of " + target + " released");
		*sub = DialogSubscription{};
	}
}

void BlfSubscriptions::refresh()
{
	for (auto& sub : _subscriptions)
	{
		if (!sub.used) continue;
		std::string dir, dialogId;
		std::string state = computeDialogState(sub.targetAor, dir, dialogId);
		std::string token = state + "|" + dir + "|" + dialogId;
		if (token == sub.lastState) continue;
		auto notify = buildDialogNotify(sub, state, dir, dialogId, false, "");
		_env.enqueue(sub.addr, std::move(notify));
		sub.lastState = token;
		sub.version++;
	}
}

void BlfSubscriptions::sweepExpired()
{
	auto now = std::chrono::steady_clock::now();
	for (auto& sub : _subscriptions)
	{
		if (!sub.used || now < sub.deadline) continue;
		std::string dir, dialogId;
		std::string state = computeDialogState(sub.targetAor, dir, dialogId);
		auto notify = buildDialogNotify(sub, state, dir, dialogId, true, "timeout");
		_env.enqueue(sub.addr, std::move(notify));
		_env.log("BLF: subscription to " + sub.targetAor + " expired");
		sub = DialogSubscription{};
	}
}
