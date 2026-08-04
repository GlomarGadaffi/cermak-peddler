#ifndef HTTP_SERVER_HPP
#define HTTP_SERVER_HPP

#if defined(__linux__) || defined(ESP_PLATFORM)
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>

// Forward declaration — the HttpServer queries the SIP engine via this
class RequestsHandler;

class HttpServer
{
public:
	HttpServer(const std::string& ip, int port, RequestsHandler* handler = nullptr);
	~HttpServer();

	void start();

	// Late-bind the live SIP registrar. The dashboard must be able to start
	// BEFORE the SIP stack exists — an unprovisioned device holds SIP dark
	// until an admin credential is committed *via this web UI*, so HttpServer
	// cannot wait for the handler (that ordering deadlocks onboarding on the
	// headless eth/wifi builds). All endpoints null-check, so until this is
	// called they serve empty datasets. Same idiom as SshServer::attachHandler.
	void attachHandler(RequestsHandler* h)
	{
		_handler.store(h, std::memory_order_release);
	}

private:
	// PLAN_ADMIN_HTTP_ONLY.md Phase 2: idempotent socket lifecycle, called only
	// from this class's own thread (the constructor, before acceptLoop starts;
	// or acceptLoop itself once running) — never from another thread, so
	// _listenSock needs no separate lock (invariant I2).
	bool openListenSocket();                    // no-op + true if already open
	void closeListenSocket();                   // no-op if already closed; only
	                                             // stops accepting NEW connections —
	                                             // it does NOT terminate already-accepted
	                                             // connections, but the dark-by-default
	                                             // invariant holds anyway because
	                                             // handleClient serves exactly one
	                                             // request/response then closes (no
	                                             // keep-alive), each on its own detached
	                                             // thread. Keep that property if you ever
	                                             // add keep-alive, or this gate leaks.

	void acceptLoop();
	void handleClient(int clientSock);

	// HTTP request parsing
	struct HttpRequest {
		std::string method;
		std::string path;
		std::string body;
		std::string origin;  // value of the Origin: header, if present
		std::string host;    // value of the Host: header, if present
		std::string cookie;  // value of the Cookie: header, if present
	};
	HttpRequest parseRequest(const std::string& raw);

	// Response builders
	void sendResponse(int sock, int statusCode, const std::string& statusText,
	                   const std::string& contentType, const std::string& body);
	// Same as sendResponse, but injects an extra raw header line (e.g.
	// "Set-Cookie: pd_session=...; HttpOnly; Path=/; SameSite=Strict"). The
	// extraHeader must NOT include the trailing CRLF.
	void sendResponseWithHeader(int sock, int statusCode, const std::string& statusText,
	                   const std::string& contentType, const std::string& body,
	                   const std::string& extraHeader);
	void sendRedirect(int sock, const std::string& location);
	void sendHtml(int sock);
	void sendApiStatus(int sock);
	void sendApiKill(int sock, const std::string& body);
	// Phase 2: read-only Call Detail Records (newest first). Ungated like /api/status.
	void sendApiCdr(int sock);
	// Issue #33: downloads the last POCKETDIAL_PCAP_RING_SIZE SIP signaling
	// packets as a .pcap. Session-gated by the caller in handleClient() (see the
	// dispatch entry) — unlike /api/cdr, this carries full message bytes
	// (Contact URIs, User-Agent, Authorization digests), not just call metadata.
	void sendApiPcap(int sock);
	// Issue #32: the same capture ring as JSON, for the dashboard's polling live
	// tracer. Session-gated by the caller, same as sendApiPcap.
	void sendApiTrace(int sock);
	// Phase 2: set per-extension Do Not Disturb. Mutating (same-origin + auth gated).
	void sendApiDnd(int sock, const std::string& body);
	// Class A sweep: set per-extension call forwarding (always/busy/noanswer) and
	// configure ring/hunt groups. Mutating (same-origin + auth gated), mirroring DND.
	void sendApiForward(int sock, const std::string& body);
	void sendApiGroup(int sock, const std::string& body);
	void sendApiWifiScan(int sock);
	void sendApiWifiConnect(int sock, const std::string& body);
	void sendApiWifiModeAp(int sock);
	void sendApiConfiguring(int sock);
	void sendApiFactoryReset(int sock, const std::string& body);
	void send404(int sock);

	// --- Admin auth endpoints (PIN-gated session layer; see AdminAuth.hpp) ---
	void sendApiAdminStatus(int sock, const HttpRequest& req);
	void sendApiAdminSetPin(int sock, const HttpRequest& req);
	void sendApiAdminLogin(int sock, const HttpRequest& req);
	void sendApiAdminLogout(int sock, const HttpRequest& req);
	// Authenticated keep-alive: extends the admin HTTP-open window by a fixed
	// 1 hour from an already-logged-in session, so an operator doing extended
	// configuration work doesn't get cut off by the DTMF trigger's shorter TTL.
	void sendApiAdminKeepAlive(int sock, const HttpRequest& req);

	// --- OTA firmware-update endpoints (see OtaUpdater.hpp + docs/OTA.md) ---
	// Streams the request body straight into the inactive OTA slot. This MUST
	// bypass the 16 KB buffered path in handleClient(); see handleOtaUpload().
	// On host this is a stub that drains the body and replies 501.
	// `headerBytesRead` is the already-recv'd buffer (request line + headers and
	// possibly the first body bytes); `contentLength` is the parsed body size.
	void handleOtaUpload(int sock, const std::string& alreadyRead,
	                     size_t bodyStart, size_t contentLength);
	// Read-only JSON: running / boot / next partition, pending flag, last error.
	void sendApiOtaStatus(int sock);
	// Reboots into a staged image (device) or simulates it (host).
	void sendApiOtaReboot(int sock);

	// Streaming helper for the OTA upload: drains exactly `contentLength` bytes
	// from `sock`, feeding `chunkSink(ptr, len)` for each chunk. `prefix`/
	// `prefixLen` are body bytes already present in the initial header recv.
	// Returns true iff the full body was consumed. Used so the same draining
	// logic backs both the device write path and the host discard path.
	bool streamBody(int sock, const char* prefix, size_t prefixLen,
	                size_t contentLength,
	                const std::function<bool(const uint8_t*, size_t)>& chunkSink);

	// Returns true if the request has no Origin header (direct browser nav / curl)
	// or if the Origin host matches the Host header (same-origin). Blocks CSRF from
	// third-party pages on the same AP.
	bool isSameOrigin(const HttpRequest& req) const;

	// Extract the value of a named cookie (e.g. "pd_session") from the request's
	// Cookie header, or "" if absent.
	static std::string cookieValue(const HttpRequest& req, const std::string& name);

	// True iff the request carries a valid (live) pd_session cookie. Used to gate
	// the state-changing endpoints once a PIN has been provisioned.
	bool isAuthed(const HttpRequest& req) const;

	// Close a client socket portably
	void closeSocket(int sock);

	std::string _ip;
	int _port;
	int _listenSock;
	// Written by attachHandler() from the SIP task once the registrar exists;
	// read by the HTTP accept/worker threads. nullptr until then.
	std::atomic<RequestsHandler*> _handler;
	std::atomic<bool> _running;
	std::thread _acceptThread;

	// Track server uptime
	uint64_t _startTime;
	uint64_t currentTimeMs() const;
};

#endif
