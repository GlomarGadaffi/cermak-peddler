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

	std::memset(&_servaddr, 0, sizeof(_servaddr));
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
	_receiverThread = std::thread([=]()
		{
			char buffer[BUFFER_SIZE];
			sockaddr_in senderEndPoint;
			std::memset(&senderEndPoint, 0, sizeof(senderEndPoint));
			int len = sizeof(senderEndPoint);

			while (_keepRunning)
			{
				std::memset(&senderEndPoint, 0, sizeof(senderEndPoint));
				int bytesReceived;
#if defined(__linux__) || defined(ESP_PLATFORM)
				bytesReceived = static_cast<int>(recvfrom(_sockfd, buffer, BUFFER_SIZE, 0,
					reinterpret_cast<struct sockaddr*>(&senderEndPoint), reinterpret_cast<socklen_t*>(&len)));
#elif defined _WIN32 || defined _WIN64
				bytesReceived = recvfrom(_sockfd, buffer, BUFFER_SIZE, 0,
					reinterpret_cast<struct sockaddr*>(&senderEndPoint), &len);
#endif
				if (!_keepRunning || bytesReceived <= 0) continue;
				_onNewMessageEvent(std::string(buffer, static_cast<size_t>(bytesReceived)), senderEndPoint);
			}
		});
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
#if defined(__linux__) || defined(ESP_PLATFORM)
	close(_sockfd);
#elif defined _WIN32 || defined _WIN64
	closesocket(_sockfd);
#endif
	if (_receiverThread.joinable()) {
		_receiverThread.join();
	}
}
