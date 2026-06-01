// HttpServer.cpp: Issues #23 and #28 resolved.
#include "HttpServer.hpp"
#include "RequestsHandler.hpp"
#include "index_html.h"
#include "IPHelper.hpp"
#include <cstring>
#include <sstream>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <vector>

#if defined(ESP_PLATFORM)
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#endif

HttpServer::HttpServer(const std::string& ip, int port, RequestsHandler& handler)
	: _ip(ip), _port(port), _listenSock(-1), _handler(handler), _running(false)
{
	_startTime = currentTimeMs();

#if defined _WIN32 || defined _WIN64
	// WSAStartup may already be called by UdpServer, but calling it again is safe
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	_listenSock = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
	if (_listenSock < 0)
	{
		throw std::runtime_error("HttpServer: TCP socket creation failed");
	}

	// Allow address reuse
	int opt = 1;
#if defined _WIN32 || defined _WIN64
	setsockopt(_listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
	setsockopt(_listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(_ip.c_str());
	addr.sin_port = htons(static_cast<uint16_t>(_port));

	if (bind(_listenSock, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) < 0)
	{
		closeSocket(_listenSock);
		throw std::runtime_error("HttpServer: bind failed on port " + std::to_string(_port));
	}

	if (listen(_listenSock, 8) < 0)
	{
		closeSocket(_listenSock);
		throw std::runtime_error("HttpServer: listen failed");
	}
}

HttpServer::~HttpServer()
{
	_running = false;
	// Close the listen socket to unblock accept()
	if (_listenSock >= 0)
	{
		shutdown(_listenSock, 2);
		closeSocket(_listenSock);
	}
	if (_acceptThread.joinable())
	{
		_acceptThread.join();
	}
}

void HttpServer::start()
{
	_running = true;
	_acceptThread = std::thread(&HttpServer::acceptLoop, this);
}

void HttpServer::acceptLoop()
{
	while (_running)
	{
		sockaddr_in clientAddr{};
#if defined _WIN32 || defined _WIN64
		int addrLen = sizeof(clientAddr);
#else
		socklen_t addrLen = sizeof(clientAddr);
#endif
		int clientSock = static_cast<int>(accept(_listenSock,
			reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen));

		if (clientSock < 0)
		{
			if (!_running) break;
			continue;
		}

		// Handle each request synchronously on the accept thread.
		// For an embedded device serving a single admin, this is fine.
		handleClient(clientSock);
	}
}

void HttpServer::handleClient(int clientSock)
{
	// Issue #23 resolved: Added SO_RCVTIMEO per-client socket timeout and capped Content-Length to 16KB to prevent Accept thread DoS
#if defined _WIN32 || defined _WIN64
	DWORD tv = 5000; // 5 seconds timeout
	setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
	struct timeval tv{ .tv_sec = 5, .tv_usec = 0 };
	setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	// Heap-allocate the read buffer. On ESP32 the accept loop runs on a std::thread
	// whose default pthread stack is ~3 KB; a 4 KB stack-local buffer would overflow
	// on the first request. Using std::vector keeps the data on the heap.
	std::vector<char> buf(4096, 0);

	// Read initial data. A follow-up loop below handles POST bodies that span
	// multiple TCP segments (see Content-Length body-read completion below).
#if defined _WIN32 || defined _WIN64
	int bytesRead = recv(clientSock, buf.data(), static_cast<int>(buf.size()) - 1, 0);
#else
	int bytesRead = static_cast<int>(recv(clientSock, buf.data(), buf.size() - 1, 0));
#endif

	if (bytesRead <= 0)
	{
		closeSocket(clientSock);
		return;
	}

	// #18: ensure the complete POST body is present before parsing.
	// If the headers indicate a Content-Length larger than what arrived in the
	// first segment, keep reading until we have it all.
	std::string raw(buf.data(), static_cast<size_t>(bytesRead));
	size_t clPos = raw.find("Content-Length:");
	if (clPos == std::string::npos)
		clPos = raw.find("content-length:");
	if (clPos != std::string::npos)
	{
		size_t valStart = raw.find_first_not_of(" \t", clPos + 15);
		size_t valEnd   = raw.find_first_of("\r\n", valStart);
		if (valStart != std::string::npos && valEnd != std::string::npos)
		{
			size_t contentLength = 0;
			size_t parseIdx = valStart;
			while (parseIdx < valEnd && std::isspace(static_cast<unsigned char>(raw[parseIdx]))) ++parseIdx;
			while (parseIdx < valEnd && std::isdigit(static_cast<unsigned char>(raw[parseIdx])))
			{
				if (contentLength > 200000000)
				{
					contentLength = 200000000;
					break;
				}
				contentLength = contentLength * 10 + (raw[parseIdx] - '0');
				++parseIdx;
			}

			// Cap total body we are willing to read (16 KB is generous for wifi passwords)
			constexpr size_t MAX_BODY_BYTES = 16384;
			if (contentLength > MAX_BODY_BYTES)
			{
				sendResponse(clientSock, 413, "Payload Too Large", "application/json",
				             "{\"error\":\"request body exceeds 16 KB limit\"}");
				closeSocket(clientSock);
				return;
			}

			size_t headerEnd = raw.find("\r\n\r\n");
			if (headerEnd != std::string::npos)
			{
				size_t bodyStart  = headerEnd + 4;
				size_t bodyHave   = raw.size() > bodyStart ? raw.size() - bodyStart : 0;
				while (bodyHave < contentLength)
				{
					buf.assign(buf.size(), 0);
#if defined _WIN32 || defined _WIN64
					int n = recv(clientSock, buf.data(), static_cast<int>(buf.size()) - 1, 0);
#else
					int n = static_cast<int>(recv(clientSock, buf.data(), buf.size() - 1, 0));
#endif
					if (n <= 0) break;
					raw.append(buf.data(), static_cast<size_t>(n));
					bodyHave += static_cast<size_t>(n);
				}
			}
		}
	}

	HttpRequest req = parseRequest(raw);

#if defined(ESP_PLATFORM)
	// Captive Portal Redirect: If the request is a GET, and the Host is not our IP or is a generic captive portal test domain,
	// redirect the user to our landing page. This triggers the OS captive portal prompt.
	bool isLocalHost = (req.host.find("192.168.4.1") != std::string::npos || 
	                    req.host.find("localhost") != std::string::npos ||
	                    req.host.find("pocketdial") != std::string::npos ||
	                    _ip == "0.0.0.0" || 
	                    req.host.find(_ip) != std::string::npos);

	if (req.method == "GET" && !isLocalHost && req.path != "/api/status" && req.path != "/api/wifi/scan")
	{
		sendRedirect(clientSock, "http://192.168.4.1/");
		closeSocket(clientSock);
		return;
	}
#endif

	// Route
	if (req.method == "GET" && (req.path == "/" || req.path == "/index.html"))
	{
		sendHtml(clientSock);
	}
	else if (req.method == "GET" && req.path == "/api/status")
	{
		sendApiStatus(clientSock);
	}
	else if (req.method == "POST" && req.path == "/api/kill")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else
		{
			sendApiKill(clientSock, req.body);
		}
	}
	else if (req.method == "GET" && req.path == "/api/wifi/scan")
	{
		sendApiWifiScan(clientSock);
	}
	else if (req.method == "POST" && req.path == "/api/wifi/connect")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else
		{
			sendApiWifiConnect(clientSock, req.body);
		}
	}
	else if (req.method == "POST" && req.path == "/api/wifi/mode_ap")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else
		{
			sendApiWifiModeAp(clientSock);
		}
	}
	else
	{
		send404(clientSock);
	}

	closeSocket(clientSock);
}

HttpServer::HttpRequest HttpServer::parseRequest(const std::string& raw)
{
	HttpRequest req;

	// Parse request line: "GET /path HTTP/1.1\r\n"
	size_t methodEnd = raw.find(' ');
	if (methodEnd == std::string::npos) return req;
	req.method = raw.substr(0, methodEnd);

	size_t pathStart = methodEnd + 1;
	size_t pathEnd = raw.find(' ', pathStart);
	if (pathEnd == std::string::npos) return req;
	req.path = raw.substr(pathStart, pathEnd - pathStart);

	// Strip query string
	size_t queryPos = req.path.find('?');
	if (queryPos != std::string::npos)
	{
		req.path = req.path.substr(0, queryPos);
	}

	// Scan headers for Origin and Host
	auto extractHeader = [&](const std::string& name) -> std::string {
		std::string lower = raw;
		std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
		std::string needle = "\r\n" + name + ":";
		size_t p = lower.find(needle);
		if (p == std::string::npos) return {};
		size_t vs = raw.find_first_not_of(" \t", p + needle.size());
		size_t ve = raw.find("\r\n", vs);
		if (vs == std::string::npos) return {};
		return raw.substr(vs, ve == std::string::npos ? std::string::npos : ve - vs);
	};
	req.origin = extractHeader("origin");
	req.host   = extractHeader("host");

	// Find body (after \r\n\r\n)
	size_t bodyStart = raw.find("\r\n\r\n");
	if (bodyStart != std::string::npos)
	{
		req.body = raw.substr(bodyStart + 4);
	}

	return req;
}

void HttpServer::sendResponse(int sock, int statusCode, const std::string& statusText,
                              const std::string& contentType, const std::string& body)
{
	std::ostringstream resp;
	resp << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
	resp << "Content-Type: " << contentType << "\r\n";
	resp << "Content-Length: " << body.size() << "\r\n";
	// No Access-Control-Allow-Origin header: wildcard CORS would allow any
	// browser tab on the same AP to fire side-effecting POSTs without a preflight.
	resp << "Connection: close\r\n";
	resp << "\r\n";
	resp << body;

	std::string data = resp.str();
	const char* ptr = data.c_str();
	size_t remaining = data.size();
	while (remaining > 0)
	{
#if defined _WIN32 || defined _WIN64
		int sent = ::send(sock, ptr, static_cast<int>(remaining), 0);
#else
		int sent = static_cast<int>(::send(sock, ptr, remaining, 0));
#endif
		if (sent <= 0) break;
		ptr += sent;
		remaining -= static_cast<size_t>(sent);
	}
}

void HttpServer::sendHtml(int sock)
{
	sendResponse(sock, 200, "OK", "text/html; charset=utf-8",
	             std::string(CGA_INDEX_HTML));
}

// Helper: JSON-escape a string
static std::string jsonEscape(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s)
	{
		switch (c)
		{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:   out += c;
		}
	}
	return out;
}

void HttpServer::sendApiStatus(int sock)
{
	uint64_t uptimeMs = currentTimeMs() - _startTime;
	uint64_t uptimeSec = uptimeMs / 1000;

	auto clients = _handler.getActiveClients();
	auto sessions = _handler.getActiveSessions();
	uint64_t packets = _handler.getPacketsProcessed();
	uint64_t dropped = _handler.getPacketsDropped();   // Issue #38

	std::string displayIp = _ip;
	if (displayIp == "0.0.0.0")
	{
		displayIp = getPrimaryLocalIP();
	}

	std::ostringstream json;
	json << "{";
	json << "\"ip\":\"" << jsonEscape(displayIp) << "\",";
	json << "\"port\":" << 5060 << ",";
	json << "\"httpPort\":" << _port << ",";
	json << "\"uptime\":" << uptimeSec << ",";
	json << "\"packetsProcessed\":" << packets << ",";
	json << "\"packetsDropped\":" << dropped << ",";

	// Clients array
	json << "\"clients\":[";
	for (size_t i = 0; i < clients.size(); i++)
	{
		if (i > 0) json << ",";
		json << "{\"number\":\"" << jsonEscape(clients[i].first)
		     << "\",\"address\":\"" << jsonEscape(clients[i].second) << "\"}";
	}
	json << "],";

	// Sessions array
	json << "\"sessions\":[";
	for (size_t i = 0; i < sessions.size(); i++)
	{
		if (i > 0) json << ",";
		int durationSec = std::get<3>(sessions[i]);
		int hrs = durationSec / 3600;
		int mins = (durationSec % 3600) / 60;
		int secs = durationSec % 60;
		char durationBuf[32]{};
		if (hrs > 0)
		{
			snprintf(durationBuf, sizeof(durationBuf), "%02d:%02d:%02d", hrs, mins, secs);
		}
		else
		{
			snprintf(durationBuf, sizeof(durationBuf), "%02d:%02d", mins, secs);
		}

		json << "{\"caller\":\"" << jsonEscape(std::get<0>(sessions[i]))
		     << "\",\"callee\":\"" << jsonEscape(std::get<1>(sessions[i]))
		     << "\",\"state\":\"" << jsonEscape(std::get<2>(sessions[i]))
		     << "\",\"duration\":\"" << durationBuf << "\"}";
	}
	json << "]";

	json << "}";

	sendResponse(sock, 200, "OK", "application/json", json.str());
}

void HttpServer::sendApiKill(int sock, const std::string& body)
{
	// Parse "extension=XXXX" from the body
	std::string ext;
	std::string prefix = "extension=";
	size_t pos = body.find(prefix);
	if (pos != std::string::npos)
	{
		ext = body.substr(pos + prefix.size());
		// Trim whitespace / newlines
		while (!ext.empty() && (ext.back() == '\r' || ext.back() == '\n' || ext.back() == ' '))
			ext.pop_back();
	}

	if (ext.empty())
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"missing extension parameter\"}");
		return;
	}

	_handler.forceDisconnect(ext);
	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"disconnected\":\"" + jsonEscape(ext) + "\"}");
}

bool HttpServer::isSameOrigin(const HttpRequest& req) const
{
	// No Origin header means a direct request (browser nav, curl, etc.) — allow.
	if (req.origin.empty()) return true;

	// Strip the scheme from the Origin (e.g. "http://192.168.4.1:8080" → "192.168.4.1:8080")
	std::string originHost = req.origin;
	size_t schemeEnd = originHost.find("://");
	if (schemeEnd != std::string::npos)
		originHost = originHost.substr(schemeEnd + 3);

	// Compare against the Host header the client sent.
	return originHost == req.host;
}

void HttpServer::send404(int sock)
{
	sendResponse(sock, 404, "Not Found", "text/plain", "404 Not Found");
}

void HttpServer::closeSocket(int sock)
{
#if defined(__linux__) || defined(ESP_PLATFORM)
	close(sock);
#elif defined _WIN32 || defined _WIN64
	closesocket(sock);
#endif
}

uint64_t HttpServer::currentTimeMs() const
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count()
	);
}

// Helpers for URL decoding and parsing post/form params
static std::string urlDecode(const std::string& src)
{
	std::string ret;
	char ch = '\0';
	int ii = 0;
	for (size_t pos = 0; pos < src.length(); ++pos) {
		if (src[pos] == '+') {
			ret += ' ';
		} else if (src[pos] == '%') {
			if (pos + 2 < src.length() && 
				sscanf(src.substr(pos + 1, 2).c_str(), "%x", &ii) == 1) {
				ch = static_cast<char>(ii);
				ret += ch;
				pos += 2;
			} else {
				ret += src[pos];
			}
		} else {
			ret += src[pos];
		}
	}
	return ret;
}

static std::string getFormParam(const std::string& body, const std::string& key)
{
	std::string prefix = key + "=";
	size_t pos = body.find(prefix);
	if (pos == std::string::npos)
	{
		prefix = "&" + key + "=";
		pos = body.find(prefix);
		if (pos == std::string::npos) return "";
	}
	size_t start = pos + prefix.length();
	size_t end = body.find('&', start);
	std::string val;
	if (end == std::string::npos) {
		val = body.substr(start);
	} else {
		val = body.substr(start, end - start);
	}
	while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) {
		val.pop_back();
	}
	return urlDecode(val);
}

void HttpServer::sendApiWifiScan(int sock)
{
#if defined(ESP_PLATFORM)
	// Switch mode to AP+STA so we can scan
	wifi_mode_t current_mode;
	if (esp_wifi_get_mode(&current_mode) == ESP_OK) {
		if (current_mode == WIFI_MODE_AP) {
			esp_wifi_set_mode(WIFI_MODE_APSTA);
		}
	}

	wifi_scan_config_t scan_config = {};
	scan_config.show_hidden = true;
	
	esp_err_t err = esp_wifi_scan_start(&scan_config, true);
	if (err != ESP_OK) {
		sendResponse(sock, 500, "Internal Server Error", "application/json", 
		             "{\"error\":\"WiFi scan start failed\",\"code\":" + std::to_string(err) + "}");
		return;
	}

	uint16_t ap_count = 0;
	esp_wifi_scan_get_ap_num(&ap_count);
	
	std::vector<wifi_ap_record_t> ap_records(ap_count);
	if (ap_count > 0) {
		esp_wifi_scan_get_ap_records(&ap_count, ap_records.data());
	}

	std::ostringstream json;
	json << "{\"networks\":[";
	for (uint16_t i = 0; i < ap_count; ++i) {
		if (i > 0) json << ",";
		std::string ssid(reinterpret_cast<char*>(ap_records[i].ssid));
		int rssi = ap_records[i].rssi;
		std::string enc = "OPEN";
		switch (ap_records[i].authmode) {
			case WIFI_AUTH_WEP: enc = "WEP"; break;
			case WIFI_AUTH_WPA_PSK: enc = "WPA"; break;
			case WIFI_AUTH_WPA2_PSK: enc = "WPA2"; break;
			case WIFI_AUTH_WPA_WPA2_PSK: enc = "WPA/WPA2"; break;
			case WIFI_AUTH_WPA2_ENTERPRISE: enc = "WPA2 Enterprise"; break;
			case WIFI_AUTH_WPA3_PSK: enc = "WPA3"; break;
			case WIFI_AUTH_WPA2_WPA3_PSK: enc = "WPA2/WPA3"; break;
			default: break;
		}
		json << "{\"ssid\":\"" << jsonEscape(ssid) << "\",\"rssi\":" << rssi 
		     << ",\"encryption\":\"" << jsonEscape(enc) << "\"}";
	}
	json << "]}";

	sendResponse(sock, 200, "OK", "application/json", json.str());
#else
	sendResponse(sock, 200, "OK", "application/json", 
	             "{\"networks\":[], \"note\":\"WiFi scan not available on desktop\"}");
#endif
}

void HttpServer::sendApiWifiConnect(int sock, const std::string& body)
{
	std::string ssid = getFormParam(body, "ssid");
	std::string password = getFormParam(body, "password");

	if (ssid.empty())
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"missing ssid parameter\"}");
		return;
	}

#if defined(ESP_PLATFORM)
	nvs_handle_t nvs_handle;
	esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
	if (err == ESP_OK) {
		nvs_set_u8(nvs_handle, "wifi_mode", 1); // 1 = STATION
		nvs_set_str(nvs_handle, "wifi_ssid", ssid.c_str());
		nvs_set_str(nvs_handle, "wifi_pass", password.c_str());
		nvs_commit(nvs_handle);
		nvs_close(nvs_handle);
	}

	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"message\":\"WiFi credentials saved. Rebooting to Station Mode...\"}");

	// Create a background task to restart after 1 second
	xTaskCreate([](void*) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		esp_restart();
	}, "restart_task", 2048, NULL, 5, NULL);
#else
	(void)password;
	sendResponse(sock, 501, "Not Implemented", "application/json",
	             "{\"error\":\"WiFi connect not available on desktop\"}");
#endif
}

void HttpServer::sendApiWifiModeAp(int sock)
{
#if defined(ESP_PLATFORM)
	nvs_handle_t nvs_handle;
	esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
	if (err == ESP_OK) {
		nvs_set_u8(nvs_handle, "wifi_mode", 2); // 2 = AP (Standalone)
		nvs_commit(nvs_handle);
		nvs_close(nvs_handle);
	}

	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"message\":\"Operational mode set to Standalone AP. Rebooting...\"}");

	// Create a background task to restart after 1 second
	xTaskCreate([](void*) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		esp_restart();
	}, "restart_task", 2048, NULL, 5, NULL);
#else
	sendResponse(sock, 501, "Not Implemented", "application/json",
	             "{\"error\":\"WiFi mode select not available on desktop\"}");
#endif
}

void HttpServer::sendRedirect(int sock, const std::string& location)
{
	std::ostringstream resp;
	resp << "HTTP/1.1 302 Found\r\n";
	resp << "Location: " << location << "\r\n";
	resp << "Content-Length: 0\r\n";
	resp << "Connection: close\r\n\r\n";

	std::string data = resp.str();
	::send(sock, data.c_str(), static_cast<int>(data.size()), 0);
}
