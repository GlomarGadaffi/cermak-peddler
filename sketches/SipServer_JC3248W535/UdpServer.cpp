#include "UdpServer.hpp"
#include <thread>
#include <cstring>

UdpServer::UdpServer(std::string ip, int port, OnNewMessageEvent event) : _ip(std::move(ip)), _port(port), _onNewMessageEvent(event), _keepRunning(false)
{

#if defined _WIN32 || defined _WIN64
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		throw std::runtime_error("WSAStartup Failed");
	}
#endif

	if ((_sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		throw std::runtime_error("socket creation failed");
	}

#if defined _WIN32 || defined _WIN64
	DWORD timeout = 500; // 500ms timeout
	setsockopt(_sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 500000; // 500ms timeout
	setsockopt(_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	_servaddr = {};
	_servaddr.sin_family = AF_INET;
	_servaddr.sin_addr.s_addr = inet_addr(_ip.c_str());
	_servaddr.sin_port = htons(port);

	if (bind(_sockfd, reinterpret_cast<const struct sockaddr*>(&_servaddr), sizeof(_servaddr)) < 0)
	{
		throw std::runtime_error("bind failed");
	}
}

UdpServer::~UdpServer()
{
	closeServer();
}

void UdpServer::startReceive()
{
	_keepRunning = true;
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	if (_receiverExited == nullptr)
	{
		_receiverExited = xSemaphoreCreateBinary();
	}
	xTaskCreatePinnedToCore([](void* arg)
		{
			auto* self = static_cast<UdpServer*>(arg);
			self->receiveLoop();
			// Signal closeServer() that the loop has fully exited and no longer
			// touches any UdpServer members before the object is destroyed.
			if (self->_receiverExited != nullptr)
			{
				xSemaphoreGive(self->_receiverExited);
			}
			vTaskDelete(NULL);
		},
		"udp_receiver_task",
		8192, // 8KB stack size to prevent C++ SIP parser overflows
		this,
		5,    // Priority
		&_receiverTaskHandle,
		0     // Pinned to Core 0
	);
#else
	_receiverThread = std::thread([=]() { receiveLoop(); });
#endif
}

void UdpServer::receiveLoop()
{
	char buffer[BUFFER_SIZE];
	sockaddr_in senderEndPoint;
	senderEndPoint = {};
	int len = sizeof(senderEndPoint);

	while (_keepRunning)
	{
		senderEndPoint = {};
		int bytesReceived;
#if defined(__linux__) || defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
		bytesReceived = static_cast<int>(recvfrom(_sockfd, buffer, BUFFER_SIZE, 0,
			reinterpret_cast<struct sockaddr*>(&senderEndPoint), reinterpret_cast<socklen_t*>(&len)));
#elif defined _WIN32 || defined _WIN64
		bytesReceived = recvfrom(_sockfd, buffer, BUFFER_SIZE, 0,
			reinterpret_cast<struct sockaddr*>(&senderEndPoint), &len);
#endif
		if (!_keepRunning || bytesReceived <= 0) continue;
		_onNewMessageEvent(std::string(buffer, static_cast<size_t>(bytesReceived)), senderEndPoint);
	}
}

int UdpServer::send(const struct sockaddr_in& address, const std::string& buffer)
{
	return sendto(_sockfd, buffer.c_str(), buffer.size(),
		0, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address));
}

void UdpServer::closeServer()
{
	_keepRunning = false;
	shutdown(_sockfd, 2);
#if defined(__linux__) || defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	close(_sockfd);
#elif defined _WIN32 || defined _WIN64
	closesocket(_sockfd);
#endif

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	// Block until receiveLoop() has fully exited before returning, so the
	// UdpServer object is not destroyed out from under the running task.
	// Closing the socket above unblocks recvfrom() immediately; the timeout is
	// a backstop in case the task was never started.
	if (_receiverExited != nullptr)
	{
		xSemaphoreTake(_receiverExited, pdMS_TO_TICKS(2000));
		vSemaphoreDelete(_receiverExited);
		_receiverExited = nullptr;
	}
#else
	if (_receiverThread.joinable()) {
		_receiverThread.join();
	}
#endif
}
