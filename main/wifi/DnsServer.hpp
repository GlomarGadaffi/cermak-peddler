#pragma once

#include <string>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

class DnsServer {
public:
    DnsServer();
    ~DnsServer();

    // Start the DNS server task on UDP port 53.
    // Binds to 0.0.0.0 (all interfaces) or a specific interface IP (e.g. 192.168.4.1).
    bool start(const std::string& resolvedIp = "192.168.4.1");

    // Stop the DNS server and release resources. Blocks (bounded) until dns_task has
    // actually exited, so it is safe to destroy the object immediately afterwards.
    void stop();

private:
    static void dns_task(void* pvParameters);

    std::string _resolvedIp;
    TaskHandle_t _taskHandle;
    // _socketFd and _running are touched from BOTH the caller (start/stop) and
    // dns_task on another core, so they are atomic — a plain int/bool would be a
    // data race on the SMP Xtensa. _exitSem is given by dns_task as its last act so
    // stop() can join it before the object is torn down (no use-after-free).
    std::atomic<int>  _socketFd;
    std::atomic<bool> _running;
    bool _taskStarted = false;             // a dns_task was successfully created
    SemaphoreHandle_t _exitSem = nullptr;  // dns_task gives this right before it exits
};
