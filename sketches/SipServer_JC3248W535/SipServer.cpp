#include "SipServer.hpp"
#include "SipMessageTypes.h"

#if defined(ARDUINO)
#include <ESPmDNS.h>
#elif defined(ESP_PLATFORM)
#include <mdns.h>
#endif

SipServer::SipServer(std::string ip, int port, int httpPort) :
	_socket(ip, port, std::bind(&SipServer::onNewMessage, this, std::placeholders::_1, std::placeholders::_2)),
	_handler(ip, port, std::bind(&SipServer::onHandled, this, std::placeholders::_1, std::placeholders::_2))
{
	(void)httpPort;
	_socket.startReceive();

	// ── Multicast DNS (mDNS) responder broadcast ─────────────────────
#if defined(ARDUINO)
	if (MDNS.begin("pocketdial")) {
		MDNS.addService("sip", "udp", port);
		MDNS.addService("http", "tcp", httpPort);
		std::cout << "[mDNS] Broadcast active: pocketdial.local\n";
	} else {
		std::cerr << "[mDNS] Failed to start responder\n";
	}
#elif defined(ESP_PLATFORM)
	esp_err_t err = mdns_init();
	if (err == ESP_OK) {
		mdns_hostname_set("pocketdial");
		mdns_instance_name_set("Pocket Dial SIP Server");
		mdns_service_add(NULL, "_sip", "_udp", port, NULL, 0);
		mdns_service_add(NULL, "_http", "_tcp", httpPort, NULL, 0);
		std::cout << "[mDNS] Broadcast active: pocketdial.local\n";
	} else {
		std::cerr << "[mDNS] Failed to initialize: " << err << "\n";
	}
#endif

	// ── Desktop Background Tick Thread ───────────────────────────────
#if !defined(ESP_PLATFORM) && !defined(ARDUINO)
	_tickRunning = true;
	_tickThread = std::thread(&SipServer::tickLoop, this);
#endif
}

SipServer::~SipServer()
{
#if !defined(ESP_PLATFORM) && !defined(ARDUINO)
	if (_tickRunning)
	{
		_tickRunning = false;
		if (_tickThread.joinable())
		{
			_tickThread.join();
		}
	}
#endif
}

void SipServer::onNewMessage(std::string data, sockaddr_in src)
{
	auto message = _messagesFactory.createMessage(std::move(data), std::move(src));
	if (message.has_value())
	{
		_handler.handle(std::move(message.value()));
	}
}

void SipServer::onHandled(const sockaddr_in& dest, std::shared_ptr<SipMessage> message)
{
	_socket.send(dest, message->toString());
}

#if !defined(ESP_PLATFORM) && !defined(ARDUINO)
void SipServer::tickLoop()
{
	while (_tickRunning)
	{
		_handler.tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
}
#endif
