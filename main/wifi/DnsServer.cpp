#include "DnsServer.hpp"
#include <cstring>
#include <sys/param.h>
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_log.h"

static const char* TAG = "dns_server";

#pragma pack(push, 1)
struct DnsHeader {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
};

struct DnsAnswerHeader {
    uint16_t name_ptr;
    uint16_t type;
    uint16_t class_type;
    uint32_t ttl;
    uint16_t rdlength;
};
#pragma pack(pop)

DnsServer::DnsServer() : _resolvedIp("192.168.4.1"), _taskHandle(nullptr), _socketFd(-1), _running(false) {}

DnsServer::~DnsServer() {
    stop();
}

bool DnsServer::start(const std::string& resolvedIp) {
    if (_running) return true;
    
    _resolvedIp = resolvedIp;
    _running = true;
    
    BaseType_t ret = xTaskCreatePinnedToCore(&DnsServer::dns_task, "dns_task", 4096, this, 5, &_taskHandle, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DNS task");
        _running = false;
        return false;
    }
    
    return true;
}

void DnsServer::stop() {
    if (!_running) return;
    
    _running = false;
    if (_socketFd != -1) {
        close(_socketFd);
        _socketFd = -1;
    }
    
    // DNS server task will terminate on its own when _running is false or socket is closed.
    // Clean up task handle if still valid.
    _taskHandle = nullptr;
    ESP_LOGI(TAG, "DNS server stopped.");
}

void DnsServer::dns_task(void* pvParameters) {
    DnsServer* self = static_cast<DnsServer*>(pvParameters);
    ESP_LOGI(TAG, "DNS server task started resolving to %s", self->_resolvedIp.c_str());

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53); // Standard DNS port

    self->_socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (self->_socketFd < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        self->_running = false;
        vTaskDelete(NULL);
        return;
    }

    int err = bind(self->_socketFd, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(self->_socketFd);
        self->_socketFd = -1;
        self->_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx_buffer[512];
    uint8_t tx_buffer[512];

    // Setup client address struct
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    // Parse the target IP address string once
    uint8_t ip_bytes[4] = {192, 168, 4, 1};
    int parsed_ip[4];
    if (sscanf(self->_resolvedIp.c_str(), "%d.%d.%d.%d", &parsed_ip[0], &parsed_ip[1], &parsed_ip[2], &parsed_ip[3]) == 4) {
        for (int i = 0; i < 4; i++) {
            ip_bytes[i] = static_cast<uint8_t>(parsed_ip[i]);
        }
    }

    while (self->_running) {
        int len = recvfrom(self->_socketFd, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            if (self->_running) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            }
            break;
        }

        // Process only if it looks like a valid DNS query (at least a header)
        if (len < sizeof(DnsHeader)) {
            continue;
        }

        DnsHeader* rx_header = reinterpret_cast<DnsHeader*>(rx_buffer);
        uint16_t flags = ntohs(rx_header->flags);
        uint16_t qdcount = ntohs(rx_header->qdcount);

        // If it's a query (not a response) and has at least one question
        if ((flags & 0x8000) == 0 && qdcount > 0) {
            // Replicate header for response
            DnsHeader* tx_header = reinterpret_cast<DnsHeader*>(tx_buffer);
            tx_header->id = rx_header->id;
            tx_header->flags = htons(0x8400); // Standard response, Auth, No error
            tx_header->qdcount = htons(qdcount);
            tx_header->ancount = htons(1); // One answer response (simplification: respond to first query only)
            tx_header->nscount = 0;
            tx_header->arcount = 0;

            // Copy queries section from rx to tx
            // Finding the end of queries section
            int query_offset = sizeof(DnsHeader);
            
            // Re-read questions section to find boundaries safely
            bool valid_query = true;
            for (int q = 0; q < qdcount; q++) {
                // Parse labels until null byte (0)
                while (query_offset < len && rx_buffer[query_offset] != 0) {
                    uint8_t label_len = rx_buffer[query_offset];
                    // Label offset sanity check
                    if (query_offset + 1 + label_len >= len) {
                        valid_query = false;
                        break;
                    }
                    query_offset += 1 + label_len;
                }
                
                if (!valid_query) break;
                query_offset += 1; // skip null byte

                // Skip QTYPE (2 bytes) and QCLASS (2 bytes)
                if (query_offset + 4 > len) {
                    valid_query = false;
                    break;
                }
                query_offset += 4;
            }

            if (!valid_query || query_offset > sizeof(tx_buffer)) {
                continue;
            }

            // Copy whole queries block to output buffer
            memcpy(tx_buffer + sizeof(DnsHeader), rx_buffer + sizeof(DnsHeader), query_offset - sizeof(DnsHeader));

            // Append standard A answer record mapping back to queried name
            DnsAnswerHeader answer;
            answer.name_ptr = htons(0xC00C); // pointer to domain name in query (offset 12)
            answer.type = htons(1);          // A record
            answer.class_type = htons(1);    // IN class
            answer.ttl = htonl(10);          // 10s lease
            answer.rdlength = htons(4);      // IPv4 length (4 bytes)

            int tx_len = query_offset;
            if (tx_len + sizeof(DnsAnswerHeader) + 4 <= sizeof(tx_buffer)) {
                memcpy(tx_buffer + tx_len, &answer, sizeof(DnsAnswerHeader));
                tx_len += sizeof(DnsAnswerHeader);
                memcpy(tx_buffer + tx_len, ip_bytes, 4);
                tx_len += 4;

                // Send back reply to source address
-                int sent = sendto(self->_socketFd, tx_buffer, tx_len, 0, (struct sockaddr *)&source_addr, socklen);
+                int sent = sendto(self->_socketFd, tx_buffer, tx_len, 0,
+                                  reinterpret_cast<struct sockaddr *>(&source_addr),
+                                  socklen);
                 if (sent < 0) {
                     ESP_LOGE(TAG, "DNS sendto failed: errno %d", errno);
                 }
             }
         }
     }

     if (self->_socketFd != -1) {
         close(self->_socketFd);
         self->_socketFd = -1;
     }
     self->_running = false;
     self->_taskHandle = nullptr;
     ESP_LOGI(TAG, "DNS server task completed.");
     vTaskDelete(NULL);
 }
