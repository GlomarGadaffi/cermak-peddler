#pragma once

#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class DnsServer {
public:
    DnsServer();
    ~DnsServer();

    // Start the DNS server task on UDP port 53.
    // Binds to 0.0.0.0 (all interfaces) or a specific interface IP (e.g. 192.168.4.1).
    bool start(const std::string& resolvedIp = "192.168.4.1");
    
    // Stop the DNS server and release resources.
    void stop();

private:
    static void dns_task(void* pvParameters);

    std::string _resolvedIp;
    TaskHandle_t _taskHandle;
    int _socketFd;
    bool _running;
};
