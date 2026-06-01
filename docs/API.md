# Pocket-Dial ESP32 Firmware: HTTP REST API Specification

This document provides the formal API specification for the HTTP control interface of the **pocket-dial** firmware. The API handles status reporting, client management, and Wi-Fi onboarding.

---

## 1. Global Server Settings & Connection Behavior

The HTTP server operates under strict resource constraints and security policies designed to prevent device crashes and malicious manipulation.

### Connection Limits & Socket Policies
* **Protocol**: HTTP/1.1
* **Default Port**: 80 (Overridden to custom port if configured)
* **Socket Timeout (`SO_RCVTIMEO`)**: **5 Seconds**. Connections that do not send data within 5 seconds of connection are forcefully closed.
* **Payload Limit**: **16 KB (16,384 bytes)**. Any request body larger than 16 KB (including large Wi-Fi passwords) is rejected with status `413 Payload Too Large`.

### Security Response Headers
Every API response returned by the server includes the following HTTP headers:
```http
Content-Type: application/json (or text/html for static files)
Content-Length: <byte_count>
Connection: close
```
> [!IMPORTANT]
> **CORS Restrictions**: No `Access-Control-Allow-Origin` headers are sent. Wildcard CORS is prohibited to prevent background malicious browser tabs from reading internal VoIP station mappings.

---

## 2. Security & Same-Origin Verification (CSRF Protection)

To prevent Cross-Site Request Forgery (CSRF) exploits when operating as an open Wi-Fi network, the server implements strict Same-Origin Verification on all state-mutating endpoints (`/api/kill`, `/api/wifi/connect`, `/api/wifi/mode_ap`):

1. **Origin Header Scan**: The server parses the HTTP `Origin` header.
2. **Direct/Same-Origin Allow**:
   * If `Origin` is missing (direct browser navigation, local CLI `curl` requests), the transaction is **allowed**.
   * If `Origin` is present, its host and port are extracted and compared directly against the HTTP `Host` header sent by the client.
3. **Cross-Origin Reject**: If the `Origin` host does not match the `Host` header (indicating a background request from a malicious external site), the server rejects the request with **`403 Forbidden`** and the JSON body:
   ```json
   {
     "error": "cross-origin request rejected"
   }
   ```

---

## 3. Captive Portal Redirect Mechanism

When booting into onboarding mode, the device intercepts client browser check domains (e.g. `captive.apple.com`, `connectivitycheck.gstatic.com`) to display the setup screen.

* **Trigger**: Any HTTP `GET` request where the `Host` header does not contain:
  * `192.168.4.1` (the local SoftAP gateway IP)
  * `localhost`
  * `pocketdial` (mDNS hostname)
  * The current DHCP-assigned IP address.
* **Action**: The server immediately returns a `302 Found` redirect to the captive portal landing page:
  ```http
  HTTP/1.1 302 Found
  Location: http://192.168.4.1/
  Content-Length: 0
  Connection: close
  ```

---

## 4. REST API Endpoint Catalog

| Endpoint | Method | Security Level | Auth Required | Description |
| :--- | :---: | :---: | :---: | :--- |
| [`/`](#get-) | `GET` | Low | None | Serves the web dashboard HTML interface. |
| [`/api/status`](#get-apistatus) | `GET` | Low | None | Retrieves registrar uptime, packet statistics, active extensions, and ongoing sessions. |
| [`/api/kill`](#post-apikill) | `POST` | High | Same-Origin | Forcefully disconnects and de-registers an active SIP extension. |
| [`/api/wifi/scan`](#get-apiwifiscan) | `GET` | Low | None | Triggers a scan of nearby Wi-Fi APs and returns their SSIDs and signal strengths. |
| [`/api/wifi/connect`](#post-apiwificonnect) | `POST` | High | Same-Origin | Saves Wi-Fi credentials to NVS and schedules an ESP32 system reboot into Station Mode. |
| [`/api/wifi/mode_ap`](#post-apiwifimode_ap) | `POST` | High | Same-Origin | Sets the device to Standalone Access Point Mode and schedules a system reboot. |

---

### `GET /`
Serves the retro CGA CRT web interface.

* **Request Headers**: None
* **Response Content-Type**: `text/html; charset=utf-8`
* **Response Status Codes**:
  * `200 OK`: File successfully transmitted.

---

### `GET /api/status`
Returns a detailed JSON object representing the active state of the SIP registration database and traffic statistics.

* **Request Headers**: None
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`
* **Response Payload JSON Example**:
```json
{
  "ip": "192.168.4.1",
  "port": 5060,
  "httpPort": 80,
  "uptime": 14205,
  "packetsProcessed": 10543,
  "packetsDropped": 12,
  "clients": [
    {
      "number": "1001",
      "address": "192.168.4.12:5060"
    },
    {
      "number": "1002",
      "address": "192.168.4.15:5068"
    }
  ],
  "sessions": [
    {
      "caller": "1001",
      "callee": "1002",
      "state": "Connected",
      "duration": "03:45"
    }
  ]
}
```

#### Field Schema Definitions

| Field Name | Type | Description |
| :--- | :---: | :--- |
| `ip` | String | The primary active IP address of the SIP server interface. |
| `port` | Integer | The active UDP signaling port (typically 5060). |
| `httpPort` | Integer | The active TCP HTTP port (typically 80). |
| `uptime` | Integer | Time in seconds since the HTTP server initialized. |
| `packetsProcessed` | Integer | Total UDP signaling packets processed by the state machine. |
| `packetsDropped` | Integer | Total UDP signaling packets dropped by rate-limiting or firewall rules. |
| `clients` | Array | Array of objects listing active VoIP extensions. |
| `clients[].number` | String | SIP extension number (e.g., `"1001"`). |
| `clients[].address` | String | Client's IP and port (e.g., `"192.168.4.12:5060"`). |
| `sessions` | Array | Array of active SIP communication channels. |
| `sessions[].caller` | String | Extension that initiated the call. |
| `sessions[].callee` | String | Target extension receiving the call. |
| `sessions[].state` | String | Active session state: `Invited`, `Connected`, `Busy`, `Unavailable`, `Cancel`, `Bye`. |
| `sessions[].duration` | String | Active call length formatted as `MM:SS` or `HH:MM:SS`. |

---

### `POST /api/kill`
Disconnects a specified VoIP station, removing its registration and terminating any active calls involving its extension.

* **Requires Same-Origin Check**: Yes
* **Request Content-Type**: `application/x-www-form-urlencoded`
* **Request Parameters**:
  * `extension` (Required): The registration extension number to disconnect.
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`: Target extension disconnected.
  * `400 Bad Request`: Parameter `extension` is missing or empty.
  * `403 Forbidden`: Same-Origin verification failed.

#### Request Example (Form URL-Encoded)
```http
POST /api/kill HTTP/1.1
Host: 192.168.4.1
Origin: http://192.168.4.1
Content-Type: application/x-www-form-urlencoded
Content-Length: 14

extension=1001
```

#### Response Example (200 OK)
```json
{
  "status": "ok",
  "disconnected": "1001"
}
```

#### Response Example (400 Bad Request)
```json
{
  "error": "missing extension parameter"
}
```

---

### `GET /api/wifi/scan`
Triggers an immediate background Wi-Fi network scan. On ESP32, this temporarily sets the radio to `AP+STA` mode to complete the scan.

* **Request Headers**: None
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`
  * `500 Internal Server Error`: Background scan driver failed to launch.

#### Response Example (200 OK - ESP32 Platform)
```json
{
  "networks": [
    {
      "ssid": "Office-Main-5G",
      "rssi": -65,
      "encryption": "WPA2"
    },
    {
      "ssid": "Guest-Open",
      "rssi": -82,
      "encryption": "OPEN"
    }
  ]
}
```
> [!NOTE]
> On desktop platforms (Windows/Linux development builds), the endpoint returns:
> `{"networks":[], "note":"WiFi scan not available on desktop"}`

---

### `POST /api/wifi/connect`
Configures the device to operate in **Wi-Fi Station Mode**, saving the SSID and password to flash, and triggers a system reboot.

* **Requires Same-Origin Check**: Yes
* **Request Content-Type**: `application/x-www-form-urlencoded`
* **Request Parameters**:
  * `ssid` (Required): SSID of the target network.
  * `password` (Optional): Password of the target network.
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`: Credentials stored, reboot scheduled.
  * `400 Bad Request`: Parameter `ssid` is missing.
  * `403 Forbidden`: Same-Origin verification failed.

#### Request Example
```http
POST /api/wifi/connect HTTP/1.1
Host: 192.168.4.1
Origin: http://192.168.4.1
Content-Type: application/x-www-form-urlencoded
Content-Length: 35

ssid=My-Home-WiFi&password=secure123
```

#### Response Example (200 OK)
```json
{
  "status": "ok",
  "message": "WiFi credentials saved. Rebooting to Station Mode..."
}
```

---

### `POST /api/wifi/mode_ap`
Sets the operational mode of the device back to **Standalone Access Point Mode** (`esp32-sipserver`), saving settings to NVS flash, and triggers a system reboot.

* **Requires Same-Origin Check**: Yes
* **Request Headers**: None
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`: Storage committed, reboot scheduled.
  * `403 Forbidden`: Same-Origin verification failed.

#### Response Example (200 OK)
```json
{
  "status": "ok",
  "message": "Operational mode set to Standalone AP. Rebooting..."
}
```
