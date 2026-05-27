#include "HttpServer.hpp"
#include "RequestsHandler.hpp"
#include "index_html.h"
#include "IPHelper.hpp"
#include <cstring>
#include <sstream>
#include <iostream>
#include <chrono>
#include <algorithm>

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
	// Read the request (up to 4KB is plenty for our simple API)
	char buf[4096];
	std::memset(buf, 0, sizeof(buf));

#if defined _WIN32 || defined _WIN64
	int bytesRead = recv(clientSock, buf, sizeof(buf) - 1, 0);
#else
	int bytesRead = static_cast<int>(recv(clientSock, buf, sizeof(buf) - 1, 0));
#endif

	if (bytesRead <= 0)
	{
		closeSocket(clientSock);
		return;
	}

	std::string raw(buf, static_cast<size_t>(bytesRead));
	HttpRequest req = parseRequest(raw);

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
		sendApiKill(clientSock, req.body);
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
	resp << "Access-Control-Allow-Origin: *\r\n";
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
		json << "{\"caller\":\"" << jsonEscape(std::get<0>(sessions[i]))
		     << "\",\"callee\":\"" << jsonEscape(std::get<1>(sessions[i]))
		     << "\",\"state\":\"" << jsonEscape(std::get<2>(sessions[i])) << "\"}";
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
