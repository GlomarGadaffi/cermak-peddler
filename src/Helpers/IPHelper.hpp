#ifndef IP_HELPER_HPP
#define IP_HELPER_HPP

#include <string>

#if defined _WIN32 || defined _WIN64
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

inline std::string getPrimaryLocalIP()
{
#if defined _WIN32 || defined _WIN64
	// Ensure Winsock is active for our query
	WSADATA wsa;
	bool wsaStarted = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif

	int sock = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
	std::string ip = "127.0.0.1";

	if (sock >= 0)
	{
		sockaddr_in loopback{};
		loopback.sin_family = AF_INET;
		loopback.sin_addr.s_addr = inet_addr("8.8.8.8");
		loopback.sin_port = htons(53);

		if (connect(sock, reinterpret_cast<const sockaddr*>(&loopback), sizeof(loopback)) == 0)
		{
			sockaddr_in localAddr{};
#if defined _WIN32 || defined _WIN64
			int addrLen = sizeof(localAddr);
#else
			socklen_t addrLen = sizeof(localAddr);
#endif
			if (getsockname(sock, reinterpret_cast<sockaddr*>(&localAddr), &addrLen) == 0)
			{
				char ipstr[INET_ADDRSTRLEN];
				if (inet_ntop(AF_INET, &(localAddr.sin_addr), ipstr, sizeof(ipstr)))
				{
					ip = ipstr;
				}
			}
		}

#if defined _WIN32 || defined _WIN64
		closesocket(sock);
#else
		close(sock);
#endif
	}

#if defined _WIN32 || defined _WIN64
	if (wsaStarted)
	{
		WSACleanup();
	}
#endif

	return ip;
}

#endif
