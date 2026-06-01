# Code Quality Review & Refactoring Recommendations
**Project:** pocket-dial (ESP32 VoIP PBX firmware)  
**Date:** June 1, 2026  
**Auditor:** AgencyAuditSpecialist  
**Status:** Complete — Post-Refactor Review  

---

## Executive Quality Review

This code review details a comprehensive pass over the post-refactor **pocket-dial** firmware codebase (`GlomarGadaffi/pocket-dial`). The focus was on evaluating style compliance, naming conventions, include file dependencies, modularity, memory footprints, and general code cleanliness.

### Codebase Health Assessment
Overall, the codebase is in **excellent health**. The division of labor between `HttpServer`, `UdpServer`, `RequestsHandler`, and the individual models (`SipClient`, `Session`, `SipMessage`) is highly logical and modular. The C++ code is written clearly, utilizing RAII principles (e.g., `std::lock_guard` for mutexes, `std::vector` for heap management).

> [!NOTE]
> **Key Improvement:** The refactoring team has successfully removed many high-maintenance structures. The code compiles without RTTI support (ideal for microcontrollers) and keeps standard dependencies minimal. However, there are several areas where optimizations and security fixes can easily be integrated to reduce CPU usage and clean up redundant abstractions.

---

## Style, Naming, and Namespace Consistency

### 1. Consistent Casing
* **Class Names:** PascalCase is strictly and correctly used (e.g., `RequestsHandler`, `HttpServer`, `UdpServer`).
* **Member Methods:** `RequestsHandler` uses a mix of camelCase (e.g., `onRegister`, `getActiveClients`) and lower snake_case (e.g., `ip_bytes` in DnsServer). Standardizing all member methods to camelCase would improve consistency.
* **Member Variables:** Private class member variables consistently and correctly utilize the leading underscore prefix (e.g., `_clients`, `_mutex`, `_port`). This avoids parameter shadowing and is a highly readable convention.

### 2. Missing Namespaces
Almost all core classes (`HttpServer`, `RequestsHandler`, `Session`, `SipClient`) are defined in the global namespace. This can cause naming collisions if other libraries are linked in the future.
* **Actionable Suggestion:** Wrap all files inside a `PocketDial` namespace.
```cpp
namespace PocketDial {
    class RequestsHandler { ... };
}
```

---

## Include Analysis & Cleanliness

The platform-specific header inclusions (such as selecting between `<lwip/sockets.h>` for ESP32 and `<WinSock2.h>` for Windows) are duplicated across several header files:
* `UdpServer.hpp`
* `HttpServer.hpp`
* `RequestsHandler.hpp`
* `SipClient.hpp`

This duplication makes the code harder to maintain and prone to compilation errors if compile flags change.

### Recommendation
Extract these system selections into a single, unified header named `src/Helpers/PlatformSockets.h` and include it across the project:

```cpp
// src/Helpers/PlatformSockets.h
#ifndef PLATFORM_SOCKETS_H
#define PLATFORM_SOCKETS_H

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#elif defined(__linux__)
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#endif
```

---

## Actionable Code Quality Observations

### Observation 1: Inefficient String Transformations in `HttpServer::parseRequest`
In `HttpServer.cpp`, extracting custom headers copies and transforms the *entire* raw request string to lowercase on **every** header extraction call (e.g., twice per connection: once for `host`, once for `origin`):

```cpp
auto extractHeader = [&](const std::string& name) -> std::string {
    std::string lower = raw; // Duplicates up to 16 KB string
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::string needle = "\r\n" + name + ":";
    size_t p = lower.find(needle);
    // ...
```

#### Actionable Refactoring
Scan the request string line-by-line without copying or modifying the whole payload. This drastically reduces the heap allocation pressure on the ESP32:

```diff
-	// Scan headers for Origin and Host
-	auto extractHeader = [&](const std::string& name) -> std::string {
-		std::string lower = raw;
-		std::transform(lower.begin(), lower.end(), lower.begin(),
-			[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
-		std::string needle = "\r\n" + name + ":";
-		size_t p = lower.find(needle);
-		if (p == std::string::npos) return {};
-		size_t vs = raw.find_first_not_of(" \t", p + needle.size());
-		size_t ve = raw.find("\r\n", vs);
-		if (vs == std::string::npos) return {};
-		return raw.substr(vs, ve == std::string::npos ? std::string::npos : ve - vs);
-	};
-	req.origin = extractHeader("origin");
-	req.host   = extractHeader("host");

+	// Efficient, low-overhead line-by-line header scanner
+	size_t pos = raw.find("\r\n");
+	if (pos != std::string::npos) {
+		pos += 2; // Skip request line
+		while (pos < raw.size()) {
+			size_t lineEnd = raw.find("\r\n", pos);
+			if (lineEnd == std::string::npos) break;
+			if (lineEnd == pos) break; // Reached header-body boundary
+
+			std::string line = raw.substr(pos, lineEnd - pos);
+			size_t colon = line.find(':');
+			if (colon != std::string::npos) {
+				std::string hName = line.substr(0, colon);
+				std::transform(hName.begin(), hName.end(), hName.begin(), ::tolower);
+				
+				// Strip trailing whitespaces from name
+				while (!hName.empty() && std::isspace(hName.back())) hName.pop_back();
+
+				if (hName == "origin" || hName == "host") {
+					size_t valStart = colon + 1;
+					while (valStart < line.size() && std::isspace(static_cast<unsigned char>(line[valStart]))) valStart++;
+					std::string hVal = line.substr(valStart);
+					if (hName == "origin") req.origin = hVal;
+					else if (hName == "host") req.host = hVal;
+				}
+			}
+			pos = lineEnd + 2;
+		}
+	}
```

---

### Observation 2: Missing Rate Limiting Implementation in `RequestsHandler`
The "Per-Source-IP Rate Limiting & Optional Allowlist (#38)" feature is detailed in `CHANGELOG.md` and declared in `RequestsHandler.hpp` (lines 88-91, 130-139), but the actual implementations of `ipAllowed` and `allowPacket` are **completely missing** from `RequestsHandler.cpp`. As a result, the rate limiter does not function and packets are never dropped.

#### Actionable Refactoring
Provide the missing definitions in `RequestsHandler.cpp` and call them at the start of `handle()`:

```cpp
// Add to the very top of RequestsHandler::handle() in RequestsHandler.cpp:
void RequestsHandler::handle(std::shared_ptr<SipMessage> request)
{
    // Check if IP is in the CIDR allowlist and token bucket allows the packet
    if (!ipAllowed(request->getSource()) || !allowPacket(request->getSource()))
    {
        _packetsDropped.fetch_add(1, std::memory_order_relaxed);
        return; // Drop packet immediately
    }
    
    _packetsProcessed.fetch_add(1, std::memory_order_relaxed);
    // ... rest of handle logic ...
}

// Add these helper implementations at the bottom of RequestsHandler.cpp:
bool RequestsHandler::ipAllowed(const sockaddr_in& src) const
{
    if (_allowMask == 0) return true; // No allowlist configured
    uint32_t ip = ntohl(src.sin_addr.s_addr);
    return (ip & _allowMask) == _allowNet;
}

bool RequestsHandler::allowPacket(const sockaddr_in& src)
{
    auto now = std::chrono::steady_clock::now();
    uint32_t ip = src.sin_addr.s_addr; // Key by raw network-byte-order IP

    auto it = _rateBuckets.find(ip);
    if (it == _rateBuckets.end())
    {
        // New bucket: burst 40, sustained 20 pkt/s
        _rateBuckets[ip] = { 40.0, now };
        return true;
    }

    auto& bucket = it->second;
    double elapsedSec = std::chrono::duration<double>(now - bucket.last).count();
    bucket.last = now;

    // Replenish tokens (sustained rate = 20 tokens/sec)
    bucket.tokens = std::min(40.0, bucket.tokens + elapsedSec * 20.0);

    if (bucket.tokens >= 1.0)
    {
        bucket.tokens -= 1.0;
        return true;
    }

    return false; // Denied (Rate limit exceeded)
}
```

---

### Observation 3: Missing Header Empty-Needle Validation
In `SipMessage::setVia`, `setFrom`, `setTo`, `setCallID`, and `setCSeq`, string manipulation is performed using `.find()` on member variables. If any of these fields are empty in the incoming packet, `.find("")` returns `0`, which inserts the new header value at index `0`, corrupting the start of the message:

```cpp
void SipMessage::setVia(std::string value) {
    auto viaPos = _messageStr.find(_via); // If _via is "", find("") returns 0
    if (viaPos != std::string::npos) {
        _messageStr.replace(viaPos, _via.length(), value); // Corrupts index 0
    }
    _via = std::move(value);
}
```

#### Actionable Refactoring
Add safe guards against empty needles, matching the protection added to `setContact`:

```diff
 void SipMessage::setVia(std::string value) {
-	auto viaPos = _messageStr.find(_via);
-	if (viaPos != std::string::npos) {
-		_messageStr.replace(viaPos, _via.length(), value);
-	}
+	if (!_via.empty()) {
+		auto viaPos = _messageStr.find(_via);
+		if (viaPos != std::string::npos) {
+			_messageStr.replace(viaPos, _via.length(), value);
+		}
+	} else {
+		size_t clPos = _contentLength.empty() ? std::string::npos : _messageStr.find(_contentLength);
+		if (clPos != std::string::npos) {
+			_messageStr.insert(clPos, value + "\r\n");
+		} else {
+			_messageStr += value + "\r\n";
+		}
+	}
 	_via = std::move(value);
 }
```

---

### Observation 4: Unified Compiler Warning Flags
Hardening the compiler options prevents regression bugs on multiple target configurations (such as different cross-compilers for ESP32 and Waveshare Ethernet boards).

The root `CMakeLists.txt` is missing critical compiler warnings. We recommend expanding the compiler flag checks to ensure complete type and variable safety:

```cmake
# Expand compile warnings in CMakeLists.txt
if (MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(
        -Wall 
        -Wextra 
        -Wpedantic 
        -Wshadow 
        -Wconversion 
        -Wdouble-promotion 
        -Wformat=2
    )
endif()
```

---

## Conclusion

By implementing the actionable observations listed above, the pocket-dial firmware will benefit from **decreased memory fragmentation**, **reduced CPU usage**, and a **stronger security posture** against denial of service and packet manipulation. The overall architecture is robust, and these refinements will elevate the firmware to commercial-grade stability.
