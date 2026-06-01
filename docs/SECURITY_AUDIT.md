# Firmware Security Audit & Threat Model Report
**Project:** pocket-dial (ESP32 VoIP PBX firmware)  
**Date:** June 1, 2026  
**Auditor:** AgencyAuditSpecialist  
**Status:** Complete — Post-Refactor Review  

---

## Executive Summary

This security audit and threat model report evaluates the network-facing and local attack surfaces of the **pocket-dial** ESP32 firmware codebase. The goal of this audit is to identify potential entry points for attackers, analyze parsing robustly, review existing access control measures, and classify security vulnerabilities using industry-standard severity scoring (CVE-class vulnerabilities and CVSS v3.1 vectors).

> [!WARNING]
> **Summary of Findings:** While the firmware has successfully mitigated several high-risk vectors (including stack buffer overflows and wildcard CORS), significant security gaps remain. The most critical is a **Host/Origin Validation Bypass via DNS Rebinding (SEC-01, High Severity)**, followed by **Missing Packet Validation in SIP Parsing (SEC-02, Medium Severity)**, **Cleartext WiFi Credentials Storage in Flash (SEC-03, Low/Medium Severity)**, and a **Lack of Authentication on Admin HTTP and SIP Interfaces (SEC-04, Medium Severity)**.

---

## System Attack Surface Map

The pocket-dial PBX exposes multiple network-facing UDP/TCP ports and utilizes non-volatile flash storage for persistence.

```
                              [ ATTACK VECTORS ]
                                      │
         ┌────────────────────────────┼────────────────────────────┐
         ▼                            ▼                            ▼
   [ Port 53 UDP ]              [ Port 5060 UDP ]            [ Port 80 TCP ]
     DnsServer                   UdpServer (SIP)               HttpServer
  (DNS Redirection)            (Signaling Parser)         (CGA Admin Dashboard)
         │                            │                            │
         ▼                            ▼                            ▼
┌──────────────────┐         ┌──────────────────┐         ┌──────────────────┐
│ Captive Portal   │         │ RequestsHandler  │         │ Detached HTTP    │
│ Host Redirection │         │ Pre-Allocated    │         │ Client Threads   │
└────────┬─────────┘         └────────┬─────────┘         └────────┬─────────┘
         │                            │                            │
         │                            ▼                            │
         └───────────────────► [ NVS Storage ] ◄───────────────────┘
                                SSID & Password
                               (Cleartext Flash)
```

1. **HttpServer (Port 80/TCP):** Exposes index dashboard, network configuration scan, session termination APIs, and captive portal redirections. Run as detached threads.
2. **SIP UDP Server (Port 5060/UDP):** Listens for registration, call invites, session control signaling, and OPTIONS pings.
3. **DNS Redirect Server (Port 53/UDP):** Active in captive portal onboarding mode. Resolves all domain queries to local IP.
4. **NVS Storage (Flash Partition):** Non-volatile flash partition containing static WiFi credentials and boot configurations.
5. **BLE (Bluetooth Low Energy):** *Not implemented*. The current firmware has zero BLE footprint. (Recommendations for secure implementation are detailed below).

---

## Detailed Vulnerability Findings

### SEC-01: Host/Origin Header Validation Bypass via DNS Rebinding
* **Classification:** CWE-346: Origin Validation Error
* **Severity:** 🔴 **High (CVSS v3.1 Score: 8.1)**
* **Vector:** `CVSS:3.1/AV:N/AC:H/PR:N/UI:R/S:U/C:H/I:H/A:H`
* **Vulnerable Component:** `HttpServer::isSameOrigin(const HttpRequest& req)` in [HttpServer.cpp#L499-L512](../src/Helpers/HttpServer.cpp#L499-L512)

#### Description
The `isSameOrigin()` helper validates mutating POST requests (`/api/kill`, `/api/wifi/connect`, `/api/wifi/mode_ap`) by ensuring that the incoming `Origin` header matches the `Host` header. 

```cpp
bool HttpServer::isSameOrigin(const HttpRequest& req) const {
    if (req.origin.empty()) return true;
    std::string originHost = req.origin;
    size_t schemeEnd = originHost.find("://");
    if (schemeEnd != std::string::npos)
        originHost = originHost.substr(schemeEnd + 3);
    return originHost == req.host;
}
```

However, the server does **not** validate that the `Host` header is an authorized local address (such as `192.168.4.1` or `pocketdial.local`). This exposes the device to a **DNS Rebinding Attack**.

An attacker can register a malicious domain (e.g., `attacker.com`) and configure their DNS server to temporarily resolve this domain to the ESP32’s local IP address (`192.168.4.1`). When a victim connected to the pocket-dial AP visits `attacker.com`, their browser executes malicious JavaScript. Because the browser sends `Host: attacker.com` and `Origin: http://attacker.com`, the `isSameOrigin()` check returns `true`. The attacker’s scripts can now silently issue POST requests to disconnect calls or reprogram the WiFi config.

#### Proof of Concept (Attack Scenario)
1. Victim connects their PC to the `esp32-sipserver` AP and receives IP `192.168.4.2`.
2. Victim visits a malicious or compromised web page `http://rebinding.malicious.xyz`.
3. The malicious DNS server shifts the TTL of `rebinding.malicious.xyz` to `192.168.4.1`.
4. The script on the page makes a fetch request to `http://rebinding.malicious.xyz/api/kill` with body `"extension=101"`.
5. The browser issues the request. The HTTP payload contains:
   * `Host: rebinding.malicious.xyz`
   * `Origin: http://rebinding.malicious.xyz`
6. `isSameOrigin()` returns `true` (since `rebinding.malicious.xyz == rebinding.malicious.xyz`).
7. The call session for extension `101` is terminated silently.

#### Remediation
Strictly validate the incoming `Host` header against the device's actual IP address or configured mDNS hostname:
```cpp
bool HttpServer::isSameOrigin(const HttpRequest& req) const {
    if (req.origin.empty()) return true;
    
    // Strip scheme from Origin
    std::string originHost = req.origin;
    size_t schemeEnd = originHost.find("://");
    if (schemeEnd != std::string::npos)
        originHost = originHost.substr(schemeEnd + 3);

    // Host header must be our local IP or local mDNS hostname
    std::string activeIp = (_ip == "0.0.0.0") ? getPrimaryLocalIP() : _ip;
    bool hostValid = (req.host == activeIp || 
                      req.host == "192.168.4.1" || 
                      req.host == "pocketdial.local" ||
                      req.host.find("localhost") != std::string::npos);

    return hostValid && (originHost == req.host);
}
```

---

### SEC-02: Missing Request Parsing Validation on SIP Signaling
* **Classification:** CWE-20: Improper Input Validation
* **Severity:** 🟡 **Medium (CVSS v3.1 Score: 5.3)**
* **Vector:** `CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:N/I:L/A:N`
* **Vulnerable Component:** `RequestsHandler::handle` in [RequestsHandler.cpp#L59-L88](../src/SIP/RequestsHandler.cpp#L59-L88)

#### Description
`SipMessage` implements a helper method `isValidMessage()` to check if a parsed packet contains mandatory SIP fields:
```cpp
bool SipMessage::isValidMessage() const {
    if (_via.empty() || _to.empty() || _from.empty() || _callID.empty() || _cSeq.empty()) {
        return false;
    }
    // ...
}
```
However, **neither `SipMessageFactory` nor `RequestsHandler::handle` ever calls `isValidMessage()`**. 

When a malformed UDP packet is received, `SipMessage::parse()` returns early, leaving all parsed headers as empty strings. The handler proceeds to dispatch this empty message to the standard handlers (like `onRegister` or `onInvite`).

If a header (like `_via` or `_contact`) is empty, calling subsequent setters (like `setVia`) triggers a find-and-replace operation:
```cpp
void SipMessage::setVia(std::string value) {
    auto viaPos = _messageStr.find(_via); // _via is "" -> find("") returns 0
    if (viaPos != std::string::npos) {
        _messageStr.replace(viaPos, _via.length(), value); // replaces index 0 of length 0!
    }
    _via = std::move(value);
}
```
Because finding an empty string `""` in C++ returns index `0`, the setter prepends the header at the very beginning of the raw packet payload, corrupting the SIP Request-Line.

#### Proof of Concept (Attack Scenario)
An attacker floods the server on Port 5060 with single-line UDP packets containing just `"REGISTER sip:192.168.4.1 SIP/2.0\r\n\r\n"`.
1. `SipMessage::parse` populates `_header` and `_type` but leaves `_via`, `_to`, and `_from` completely empty.
2. `RequestsHandler::handle` dispatches it to `onRegister`.
3. `onRegister` clones the message and calls `response->setVia(data->getVia() + ";received=192.168.4.1")`.
4. Because `data->getVia()` is empty, `response->setVia(";received=192.168.4.1")` is called.
5. In `setVia`, `find("")` returns `0`, prepending the string at index `0`.
6. The resulting packet is sent out malformed, leading to logic bugs and processing errors.

#### Remediation
Enforce validation checks at the entry point of the signaling handler:
```cpp
void RequestsHandler::handle(std::shared_ptr<SipMessage> request) {
    if (!request || !request->isValidMessage()) {
        _packetsDropped.fetch_add(1, std::memory_order_relaxed);
        return; // Drop malformed packets instantly
    }
    _packetsProcessed.fetch_add(1, std::memory_order_relaxed);
    // ...
}
```

---

### SEC-03: Cleartext WiFi Credentials Storage in NVS Flash
* **Classification:** CWE-312: Cleartext Storage of Sensitive Information
* **Severity:** 🟡 **Low/Medium (CVSS v3.1 Score: 4.6)**
* **Vector:** `CVSS:3.1/AV:P/AC:L/PR:N/UI:N/S:U/C:H/I:N/A:N`
* **Vulnerable Component:** `HttpServer::sendApiWifiConnect` in [HttpServer.cpp#L660](../src/Helpers/HttpServer.cpp#L660) and [esp_main_display.cpp#L432](../main/esp_main_display.cpp#L432)

#### Description
When the user connects to the captive portal and inputs WiFi STA credentials, they are committed directly to Espressif NVS Flash under the `"storage"` namespace key `"wifi_pass"` in plain text:
```cpp
nvs_set_str(nvs_handle, "wifi_pass", password.c_str());
```
Similarly, the boot path retrieves and passes them to the network stack in plain text. By default, ESP32 does not enable flash encryption. Anyone with physical access to the device can hook up to the UART programming pins and dump the raw SPI flash contents to retrieve the WiFi network password.

#### Proof of Concept (Attack Scenario)
1. An attacker gains physical access to the pocket-dial unit.
2. They attach a standard USB-to-UART bridge to the TX/RX/GND pins.
3. They execute standard Espressif tooling:
   ```bash
   esptool.py --port COM3 --baud 921600 read_flash 0 0x1000000 flash_dump.bin
   ```
4. Running a string search on the bin dump immediately exposes the STA password:
   ```bash
   strings flash_dump.bin | grep -A 2 wifi_ssid
   ```

#### Remediation
1. **Flash Encryption:** Mandate Espressif Hardware Flash Encryption (which encrypts the entire partition table, NVS, and app partitions using an AES-256 key burned into the eFuse blocks) in the product configuration guide.
2. **Credential Obfuscation:** Before saving to NVS, encrypt the password using an AES key derived from a device-unique MAC address combined with compile-time salts.

---

### SEC-04: Lack of Authentication / Access Control on HTTP APIs and SIP Server
* **Classification:** CWE-306: Missing Authentication for Critical Function
* **Severity:** 🟡 **Medium (CVSS v3.1 Score: 6.5)**
* **Vector:** `CVSS:3.1/AV:A/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:N`
* **Vulnerable Component:** `HttpServer` routing & `RequestsHandler` registrations

#### Description
1. **HTTP Dashboard:** The admin dashboard does not require any credentials. Any device connected to the Access Point can access `/api/status` to view active calls/registered extension numbers, or make POST requests to `/api/kill` to forcibly disconnect active calls.
2. **SIP Registrar:** The registrar contains zero authentication challenges. Any SIP softphone can register arbitrary extensions (e.g., dialing `100` or `101`) without entering a password. This allows an attacker on the same local network to register as an existing extension, taking over other users' incoming and outgoing voice calls.

#### Proof of Concept (Attack Scenario)
1. An attacker joins the open `esp32-sipserver` AP.
2. They launch a softphone client (such as Linphone or MicroSIP) on their laptop.
3. They configure the client to register as extension `100` targeting SIP proxy `192.168.4.1`.
4. Because the PBX does not execute a `401 Unauthorized` Digest Authentication challenge, the registration succeeds immediately, evicting the legitimate user's client from the lookup table and allowing the attacker to intercept calls.

#### Remediation
1. **HTTP Basic Auth:** Add simple HTTP Basic Authentication or token-based cookies for the dashboard admin panel.
2. **SIP Digest Auth:** Implement a simplified SIP MD5 Digest Authentication (RFC 3261 §22) inside `onRegister` and `onInvite` handlers to challenge registrations and outbound invites against a static, pre-configured list of passwords.

---

## Architectural & BLE Security Hardening

### Bluetooth Low Energy (BLE) Threat Model
While the current firmware does not utilize Bluetooth, if BLE is added in future revisions for seamless smartphone-based onboarding/provisioning, the following threats and security mitigations must be addressed:

```
[ Attack Vector: BLE Sniffing ] ──────► ( Over-the-Air Capture ) ──────► Unencrypted Credentials Leak
[ Attack Vector: Just-Works MITM ] ───► ( Bypass Pairing Verification ) ──► Unauthorized WiFi Config Write
```

1. **Just-Works Pairing Vulnerability:** By default, BLE "Just Works" pairing uses an unauthenticated protocol exchange, leaving it completely vulnerable to active Man-in-the-Middle (MITM) hijacking.
   * *Hardening:* Enforce **BLE Passkey Entry** or **Numeric Comparison** pairing mechanisms using Out-of-Band (OOB) QR codes rendered on the 3.5" LCD screen.
2. **Eavesdropping of Custom GATT Characteristics:** Provisioning clients writing passwords to standard unencrypted GATT characteristics expose credentials to OTA sniffing.
   * *Hardening:* Require secure connections (**LE Secure Connections** with AES-CCM encryption) and restrict read/write permissions on custom SSID/Password characteristics to authenticated, bonded links.
3. **Storage of Bonding Keys:** Bonding keys must be stored securely in an encrypted NVS partition to prevent extraction.
