#ifndef UDP_SERVER_HPP
#define UDP_SERVER_HPP

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <lwip/sockets.h>
#elif defined(__linux__)
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#if defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#endif

#include <iostream>
#include <stdexcept>
#include <atomic>
#include <functional>
#include <string_view>
#include <thread>

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#endif

class UdpServer
{
public:
	// Issue #81: a view into receiveLoop()'s own stack buffer for the duration of
	// the call — NOT into anything static/shared, so nothing here changes once
	// concurrent UdpServer instances or overlapping deliveries are involved. Every
	// registered handler runs synchronously and to completion before the next
	// recvfrom() overwrites that buffer, so the view is valid for exactly as long
	// as this event call is on the stack. A handler that needs the bytes to
	// outlive this call must copy them before returning — see SipServer::
	// onNewMessage() and RequestsHandler::handle()'s pcap capture for the two
	// places that actually do.
	using OnNewMessageEvent = std::function<void(std::string_view, sockaddr_in)>;
	static constexpr int BUFFER_SIZE = 2048;

	// Back-off constants for socket/bind retry (ESP builds only).
	// Initial delay 500 ms, doubles each attempt, hard cap at 30 s.
	static constexpr uint32_t kBackoffInitialMs = 500;
	static constexpr uint32_t kBackoffMaxMs     = 30000;

	UdpServer(std::string ip, int port, OnNewMessageEvent event);
	~UdpServer();

	void startReceive();
	int send(const struct sockaddr_in& address, const std::string& buffer);

private:
	void closeServer();
	void receiveLoop();

	// Opens and binds the UDP socket.
	// ESP builds: retries with exponential back-off until successful — never
	// throws and never calls esp_restart().
	// Other platforms: throws std::runtime_error on failure (original behaviour).
	bool openSocket();

	std::string _ip;
	int _port;
	int _sockfd = -1;
	sockaddr_in _servaddr;
	OnNewMessageEvent _onNewMessageEvent;
	std::atomic<bool> _keepRunning;

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	TaskHandle_t _receiverTaskHandle = nullptr;
	SemaphoreHandle_t _receiverExited = nullptr;
#else
	std::thread _receiverThread;
#endif
};

#endif
