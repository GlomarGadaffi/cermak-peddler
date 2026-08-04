# Pocket-Dial ESP32 Firmware: HTTP REST API Specification

This document provides the formal API specification for the HTTP control interface of the **pocket-dial** firmware. The API handles status reporting, client management, and Wi-Fi onboarding.

> [!NOTE]
> The admin session endpoints (`/api/admin/*`) are specified in §0 above, not
> repeated in the §4 catalog below. Source of truth for every route:
> `src/Helpers/HttpServer.cpp` (`handleClient()` dispatch).

---

## 0. Reachability & Admin Session Layer (read this first)

**The HTTP server is dark by default on a provisioned device.** Once an admin PIN
exists, the TCP listener itself is closed except within a bounded open window
(default 600 s) granted by one of:

1. **DTMF trigger** — the admin extension (default `1001`), while registered, dials
   `*4887`; the SIP INFO's source IP must match the registration's bound IP.
2. **Provisioning grace** — a successful `POST /api/admin/set-pin` grants the same
   window (first-run onboarding and PIN changes).
3. **Keep-alive** — an authenticated `POST /api/admin/keepalive` extends the window
   by 1 hour.

Outside a window, connections are refused at the socket level — every endpoint in
this document is unreachable. An **unprovisioned** device listens unconditionally
(onboarding requires the web UI before any credential exists). Threat analysis:
`docs/THREAT_MODEL.md` §5.5.

**Admin session endpoints** (all JSON; mutating ones are Same-Origin checked):

| Endpoint | Method | Auth | Purpose |
| :--- | :--- | :--- | :--- |
| `/api/admin/status` | `GET` | None | `{provisioned, authenticated}` booleans for dashboard render. |
| `/api/admin/set-pin` | `POST` | None if unprovisioned; session if changing | Set/change the admin PIN (`pin=` form param, ≥4 chars, **must not begin `4887`** — reserved for the DTMF star-code). Grants the provisioning grace window. |
| `/api/admin/login` | `POST` | PIN in body | Issues the `pd_session` cookie (HttpOnly, SameSite=Strict). Rate-limited with lockout. |
| `/api/admin/logout` | `POST` | Session | Invalidates the session cookie. |
| `/api/admin/keepalive` | `POST` | Session | Extends the HTTP-open window by 3600 s. |

Once provisioned, all state-mutating endpoints (`/api/kill`, `/api/dnd`,
`/api/forward`, `/api/group`, `/api/wifi/*`, `/api/factory-reset`, OTA upload)
additionally require the `pd_session` cookie.

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
| [`/api/kill`](#post-apikill) | `POST` | High | Same-Origin (+session once provisioned) | Forcefully disconnects and de-registers an active SIP extension. |
| [`/api/cdr`](#get-apicdr) | `GET` | Low | None | Returns the in-memory Call Detail Record ring (most recent calls, newest first). |
| [`/api/pcap`](#get-apipcap) | `GET` | Medium | Session once provisioned | Downloads the last `POCKETDIAL_PCAP_RING_SIZE` SIP signaling packets as a `.pcap` (Wireshark-readable). |
| [`/api/dnd`](#post-apidnd) | `POST` | High | Same-Origin (+session once provisioned) | Sets or clears Do-Not-Disturb on an extension. |
| [`/api/forward`](#post-apiforward) | `POST` | High | Same-Origin (+session once provisioned) | Configures call forwarding (`always`/`busy`/`noanswer`) for an extension. |
| [`/api/group`](#post-apigroup) | `POST` | High | Same-Origin (+session once provisioned) | Creates, updates, or deletes a ring/hunt group. |
| [`/api/wifi/scan`](#get-apiwifiscan) | `GET` | Low | None | Triggers a scan of nearby Wi-Fi APs and returns their SSIDs and signal strengths. |
| [`/api/wifi/connect`](#post-apiwificonnect) | `POST` | High | Same-Origin (+session once provisioned) | Saves Wi-Fi credentials to NVS and schedules an ESP32 system reboot into Station Mode. |
| [`/api/wifi/mode_ap`](#post-apiwifimode_ap) | `POST` | High | Same-Origin (+session once provisioned) | Sets the device to Standalone Access Point Mode and schedules a system reboot. |
| [`/api/configuring`](#post-apiconfiguring) | `POST` | Low | None | Pauses the captive-portal auto-switch-to-Standalone decay while a user is mid-setup. |
| [`/api/factory-reset`](#post-apifactory-reset) | `POST` | High | Same-Origin (+session once provisioned) | Wipes the admin credential and Wi-Fi/mode NVS state, then reboots to captive-portal setup. ESP-only (`501` on desktop). |
| [`/api/ota/status`](#get-apiotastatus) | `GET` | Low | None | Reports the running/boot/next OTA partition labels and pending-verify flag. |
| [`/api/ota/upload`](#post-apiotaupload) | `POST` | High | Same-Origin (+session once provisioned) | Streams a firmware image into the inactive OTA slot. ESP-only (`501` on desktop). |
| [`/api/ota/reboot`](#post-apiotareboot) | `POST` | High | Same-Origin (+session once provisioned) | Reboots into the freshly staged OTA image. Simulated (`200`, no-op) on desktop. |

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
* **Requires `pd_session` cookie**: Once the device is provisioned (see §0)
* **Request Content-Type**: `application/x-www-form-urlencoded`
* **Request Parameters**:
  * `extension` (Required): The registration extension number to disconnect.
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`: Target extension disconnected.
  * `400 Bad Request`: Parameter `extension` is missing or empty.
  * `401 Unauthorized`: Device is provisioned and the request carries no valid session.
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
* **Requires `pd_session` cookie**: Once the device is provisioned (see §0)
* **Request Content-Type**: `application/x-www-form-urlencoded`
* **Request Parameters**:
  * `ssid` (Required): SSID of the target network.
  * `password` (Optional): Password of the target network.
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`: Credentials stored, reboot scheduled.
  * `400 Bad Request`: Parameter `ssid` is missing.
  * `401 Unauthorized`: Device is provisioned and the request carries no valid session.
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
* **Requires `pd_session` cookie**: Once the device is provisioned (see §0)
* **Request Headers**: None
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`: Storage committed, reboot scheduled.
  * `401 Unauthorized`: Device is provisioned and the request carries no valid session.
  * `403 Forbidden`: Same-Origin verification failed.

#### Response Example (200 OK)
```json
{
  "status": "ok",
  "message": "Operational mode set to Standalone AP. Rebooting..."
}
```

---

### `GET /api/cdr`
Returns the in-memory Call Detail Record ring (most recent calls first). Read-only, ungated — same reachability posture as `/api/status`.

* **Request Headers**: None
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`
* **Response Payload JSON Example**:
```json
[
  {
    "caller": "1001",
    "callee": "1002",
    "startMs": 1723180800000,
    "ageSec": 42,
    "duration": 37,
    "result": "completed"
  }
]
```

#### Field Schema Definitions

| Field Name | Type | Description |
| :--- | :---: | :--- |
| `caller` | String | Extension that initiated the call. |
| `callee` | String | Target extension. |
| `startMs` | Integer | Call start time on the server's steady-clock basis (not wall-clock; no RTC is guaranteed on-device). |
| `ageSec` | Integer | Seconds since the call started, derived from the same steady-clock basis as `startMs`. |
| `duration` | Integer | Call length in seconds. |
| `result` | String | Outcome of the call (e.g. `"completed"`). |

---

### `GET /api/pcap`
Downloads a classic libpcap file of the most recent SIP signaling packets, ready to open directly in Wireshark. Both directions are captured (packets the server received and packets it sent); RTP/media is never included — pocket-dial brokers RTP peer-to-peer and never relays it, so there is nothing server-side to capture for a call's media.

Captured packets are synthesized into a minimal Ethernet+IPv4+UDP frame around the exact SIP bytes (dummy MAC addresses — the server has no real link-layer information — but real source/destination IP:port), so Wireshark's SIP dissector decodes them exactly as it would a real capture. Timestamps are relative to the server's monotonic clock, not wall-clock (no RTC is guaranteed on the device — same basis as `/api/cdr`'s `startMs`).

Only packets that pass structural validation and the per-source-IP rate limiter (Issue #38) are captured — this is a signaling-research aid, not a wire-level DoS forensics tool. The ring is bounded (`POCKETDIAL_PCAP_RING_SIZE`, default 64); older packets are dropped as new ones arrive.

* **Requires `pd_session` cookie**: Once the device is provisioned (see §0). No same-origin check — this is a plain file download, not a state-mutating action, and `SameSite=Strict` on the session cookie already prevents a cross-site page from riding an admin's session to reach it.
* **Request Headers**: None
* **Response Content-Type**: `application/vnd.tcpdump.pcap`
* **Response Headers**: `Content-Disposition: attachment; filename="pocket-dial.pcap"`
* **Response Status Codes**:
  * `200 OK`: Always — an empty/never-populated ring still returns a valid (headers-only) `.pcap`.
  * `401 Unauthorized`: Device is provisioned and the request carries no valid session.

---

### `POST /api/dnd`
Sets or clears Do-Not-Disturb on a registered extension.

* **Requires Same-Origin Check**: Yes
* **Requires `pd_session` cookie**: Once the device is provisioned (see §0)
* **Request Content-Type**: `application/x-www-form-urlencoded`
* **Request Parameters**:
  * `extension` (Required): The extension to set DND on. Must not be `777` (echo) or `999` (broadcast) — DND cannot be applied to the virtual extensions.
  * `on` (Optional): `1`, `true`, or `on` enables DND; anything else (including omitted or `0`) disables it.
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`
  * `400 Bad Request`: `extension` is missing, or is `777`/`999`.
  * `401 Unauthorized` / `403 Forbidden`: as above.

#### Response Example (200 OK)
```json
{
  "status": "ok",
  "extension": "1001",
  "dnd": true
}
```

---

### `POST /api/forward`
Configures call forwarding for an extension.

* **Requires Same-Origin Check**: Yes
* **Requires `pd_session` cookie**: Once the device is provisioned (see §0)
* **Request Content-Type**: `application/x-www-form-urlencoded`
* **Request Parameters**:
  * `extension` (Required): The extension to configure. Must not be `777` or `999`.
  * `trigger` (Required): One of `always`, `busy`, `noanswer`.
  * `target` (Optional): The extension to forward to. Empty clears the rule for that trigger.
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`
  * `400 Bad Request`: `extension` or `trigger` missing, `trigger` not one of the three values, or `extension` is `777`/`999`.
  * `401 Unauthorized` / `403 Forbidden`: as above.

#### Response Example (200 OK)
```json
{
  "status": "ok",
  "extension": "1001",
  "trigger": "busy",
  "target": "1002"
}
```

---

### `POST /api/group`
Creates, updates, or deletes a ring/hunt group.

* **Requires Same-Origin Check**: Yes
* **Requires `pd_session` cookie**: Once the device is provisioned (see §0)
* **Request Content-Type**: `application/x-www-form-urlencoded`
* **Request Parameters**:
  * `extension` (Required): The group's own extension number. Must not be `777` or `999`.
  * `members` (Optional): Comma/space-separated member extensions. An empty list deletes the group.
  * `mode` (Optional): `ringall` (default) or `hunt`.
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`
  * `400 Bad Request`: `extension` missing, `extension` is `777`/`999`, or `mode` is not `ringall`/`hunt`.
  * `401 Unauthorized` / `403 Forbidden`: as above.

#### Response Example (200 OK)
```json
{
  "status": "ok",
  "extension": "700",
  "mode": "ringall",
  "members": "1001,1002,1003"
}
```

---

### `POST /api/configuring`
Tells the device a user is actively working through setup, pausing the captive-portal watchdog that would otherwise auto-switch the device back to Standalone AP mode. No same-origin gate — the action is harmless (it only extends a grace window, and cannot mutate credentials or network state).

* **Request Headers**: None
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`

#### Response Example (200 OK)
```json
{
  "status": "ok",
  "message": "Setup mode held — auto-switch to Standalone paused."
}
```

---

### `POST /api/factory-reset`
Clears the admin PIN/session store and Wi-Fi/mode NVS state, returning the device to its unprovisioned captive-portal state, then reboots. **ESP-only** — returns `501` on the desktop build (no NVS to erase, and the process must keep running for the test harness).

* **Requires Same-Origin Check**: Yes
* **Requires `pd_session` cookie**: Once the device is provisioned (see §0)
* **Request Content-Type**: `application/x-www-form-urlencoded`
* **Request Parameters**:
  * `confirm` (Required): Must be the literal string `ERASE`. Guards against an accidental/stray POST wiping the device.
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK` (ESP32): Credential and NVS state cleared, reboot scheduled.
  * `400 Bad Request`: `confirm` is missing or not `ERASE`.
  * `401 Unauthorized` / `403 Forbidden`: as above.
  * `501 Not Implemented` (desktop): Factory reset is not available off-device.

#### Response Example (200 OK, ESP32)
```json
{
  "status": "ok",
  "message": "Factory reset. Rebooting to captive-portal setup..."
}
```

---

### `GET /api/ota/status`
Read-only OTA introspection — which partition is running/booting/staged next, and whether the currently running image is still pending its post-update validation. No secrets, so it is reachable pre-auth like `/api/status`.

* **Request Headers**: None
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`
* **Response Payload JSON Example**:
```json
{
  "running": "ota_0",
  "boot": "ota_0",
  "next": "ota_1",
  "pendingVerify": false,
  "otaSupported": true,
  "error": ""
}
```

#### Field Schema Definitions

| Field Name | Type | Description |
| :--- | :---: | :--- |
| `running` | String | Partition label of the image currently executing. |
| `boot` | String | Partition label the bootloader will boot next. |
| `next` | String | Partition label a new OTA upload would target. |
| `pendingVerify` | Boolean | `true` if the running image has not yet called `markValid()` (anti-rollback window; see `docs/OTA.md`). |
| `otaSupported` | Boolean | `false` on the desktop build (no ESP32 partition table). |
| `error` | String | Reserved for a future error surface; currently always `""`. |

---

### `POST /api/ota/upload`
Streams a firmware image body directly into the inactive OTA slot. **Not routed through the normal 16 KB-buffered body path** — a firmware image is multi-megabyte, so `handleClient()` detects this path on the request line and hands off to a streaming reader before the usual `Content-Length` cap is applied. **ESP-only.**

* **Requires Same-Origin Check**: Yes
* **Requires `pd_session` cookie**: Once the device is provisioned (see §0)
* **Request Content-Type**: raw firmware binary (no particular `Content-Type` is enforced)
* **Request Headers**:
  * `Content-Length` (Required): Non-zero. Parsed without the normal 16 KB cap, clamped to a 32 MB ceiling (larger than any 16 MB flash layout the firmware supports) to bound the work the server will do for a malformed value.
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK` (ESP32): Image staged into the inactive slot; reboot required to run it.
  * `400 Bad Request`: Body was incomplete or a flash write failed mid-stream.
  * `401 Unauthorized` / `403 Forbidden`: as above.
  * `411 Length Required`: `Content-Length` missing or `0`.
  * `422 Unprocessable Entity`: The image was fully received but failed validation (e.g. bad magic byte / signature).
  * `500 Internal Server Error`: OTA slot could not be opened, or activation failed after a valid image was written.
  * `501 Not Implemented` (desktop): OTA is not available off-device. The body is still drained so the client's upload completes cleanly rather than being reset mid-stream.

#### Response Example (200 OK, ESP32)
```json
{
  "status": "ok",
  "bytes": 1548032,
  "rebootRequired": true,
  "nextPartition": "ota_1",
  "message": "image staged; POST /api/ota/reboot to boot it"
}
```

---

### `POST /api/ota/reboot`
Reboots into the image staged by a prior `/api/ota/upload`. **ESP32**: refuses if there is no pending image (the boot and running partitions already match). **Desktop**: always returns a simulated success without exiting the process, so the smoke-test harness keeps running.

* **Requires Same-Origin Check**: Yes
* **Requires `pd_session` cookie**: Once the device is provisioned (see §0)
* **Request Headers**: None
* **Response Content-Type**: `application/json`
* **Response Status Codes**:
  * `200 OK`: Reboot scheduled (ESP32) or simulated (desktop).
  * `401 Unauthorized` / `403 Forbidden`: as above.
  * `409 Conflict` (ESP32): No pending OTA image to boot into.

#### Response Example (200 OK, ESP32)
```json
{
  "status": "ok",
  "message": "rebooting into the new image..."
}
```

#### Response Example (200 OK, desktop)
```json
{
  "status": "ok",
  "simulated": true,
  "message": "reboot is a no-op on the desktop build"
}
```
