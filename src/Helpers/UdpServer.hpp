#ifndef UDP_SERVER_HPP
#define UDP_SERVER_HPP

#if defined(__linux__) || defined(ESP_PLATFORM)
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
#include <thread>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

class UdpServer
{
public:
	using OnNewMessageEvent = std::function<void(std::string, sockaddr_in)>;
	static constexpr int BUFFER_SIZE = 2048;

	UdpServer(std::string ip, int port, OnNewMessageEvent event);
	~UdpServer();

	void startReceive();
	int send(const struct sockaddr_in& address, const std::string& buffer);

private:
	void closeServer();
	void receiveLoop();

	std::string _ip;
	int _port;
	int _sockfd;
	sockaddr_in _servaddr;
	OnNewMessageEvent _onNewMessageEvent;
	std::atomic<bool> _keepRunning;

#if defined(ESP_PLATFORM)
	TaskHandle_t _receiverTaskHandle;
#else
	std::thread _receiverThread;
#endif
};

#endif
