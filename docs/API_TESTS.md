# Pocket-Dial Firmware: Systematic HTTP API Test Plan

This document defines the systematic test suite and validation plan for the HTTP REST API of the **pocket-dial ESP32 firmware**. 

This plan ensures that all management endpoints function correctly under standard workloads, handle malformed parameters safely, validate boundaries, and reject malicious cross-origin requests.

---

## 🔒 1. Cross-Origin Request Security Validation (CSRF Protection)

To prevent Cross-Site Request Forgery (CSRF) attacks (where a malicious website visited in another browser tab triggers side-effecting actions on the pocket-dial AP), the firmware enforces a strict **Same-Origin check** on all state-altering `POST` endpoints.

### CSRF Mitigation Architecture
The system blocks cross-origin requests by inspecting the `Origin` header appended by modern browsers. 

```
                                [Incoming HTTP Request]
                                           │
                        ┌──────────────────┴──────────────────┐
                 Yes    ▼                                     ▼    No
           [Origin Header Present?]                  [Allow Request]
                        │                                (Direct curl / browser nav)
                        ▼
           [Does Origin Host == Host Header?]
                   ┌────┴────┐
            Yes    ▼         ▼    No
             [Allow]    [Reject 403 Forbidden]
```

### Safe Origins Matrix
1. **Direct Request (e.g., `curl` or Direct URL Nav):** No `Origin` header is sent. **Allow** (Allows manual management and scripting).
2. **Same-Origin Request (CGA Web Dashboard):** The browser attaches `Origin: http://192.168.4.1` matching `Host: 192.168.4.1`. **Allow**.
3. **Cross-Origin Request (Third-party site on the same AP):** A browser script on `http://malicious.com` POSTs to `192.168.4.1`. The browser attaches `Origin: http://malicious.com`, which does not match `Host: 192.168.4.1`. **REJECT WITH `403 Forbidden`**.

> [!WARNING]
> Access-Control-Allow-Origin (CORS) headers are intentionally omitted. Allowing wildcard CORS would bypass CSRF safety bounds.

---

## 📡 2. Endpoint Specifications & JSON Schemas

### 2.1 GET `/` or `/index.html`
Serves the CGA CRT web dashboard.
* **Request:** `GET /`
* **Response:** `200 OK`
* **Content-Type:** `text/html; charset=utf-8`

### 2.2 GET `/api/status`
Fetches a read-only snapshot of the registrar, call sessions, and system metrics.
* **Request:** `GET /api/status`
* **Response:** `200 OK`
* **Content-Type:** `application/json`
* **Schema:**
  ```json
  {
    "ip": "192.168.4.1",
    "port": 5060,
    "httpPort": 80,
    "uptime": 345,
    "packetsProcessed": 104,
    "packetsDropped": 0,
    "clients": [
      {
        "number": "101",
        "address": "192.168.4.20:5061"
      }
    ],
    "sessions": [
      {
        "caller": "101",
        "callee": "102",
        "state": "Connected",
        "duration": "02:15"
      }
    ]
  }
  ```

### 2.3 POST `/api/kill`
Administratively disconnects a registered extension and terminates its calls.
* **Request:** `POST /api/kill`
* **Content-Type:** `application/x-www-form-urlencoded`
* **Parameters:** `extension=XXXX` (e.g., `extension=101`)
* **Response (Success):** `200 OK`, `application/json`
  ```json
  {"status":"ok","disconnected":"101"}
  ```
* **Response (Missing parameter):** `400 Bad Request`, `application/json`
  ```json
  {"error":"missing extension parameter"}
  ```
* **Response (CORS Blocked):** `403 Forbidden`, `application/json`
  ```json
  {"error":"cross-origin request rejected"}
  ```

### 2.4 GET `/api/wifi/scan`
Triggers an active Wi-Fi channel scan and returns visible networks.
* **Request:** `GET /api/wifi/scan`
* **Response (Success on ESP32):** `200 OK`, `application/json`
  ```json
  {
    "networks": [
      {
        "ssid": "Office_WiFi",
        "rssi": -65,
        "encryption": "WPA2"
      },
      {
        "ssid": "Guest_AP",
        "rssi": -80,
        "encryption": "OPEN"
      }
    ]
  }
  ```
* **Response (On Desktop):** `200 OK`, `application/json`
  ```json
  {"networks":[], "note":"WiFi scan not available on desktop"}
  ```

### 2.5 POST `/api/wifi/connect`
Configures network credentials, saves them to NVS, and restarts in Station mode.
* **Request:** `POST /api/wifi/connect`
* **Content-Type:** `application/x-www-form-urlencoded`
* **Parameters:** `ssid=SSID_NAME&password=WIFI_PASSWORD`
* **Response (Success on ESP32):** `200 OK`, `application/json`
  ```json
  {"status":"ok","message":"WiFi credentials saved. Rebooting to Station Mode..."}
  ```
* **Response (Missing SSID):** `400 Bad Request`, `application/json`
  ```json
  {"error":"missing ssid parameter"}
  ```
* **Response (Desktop):** `501 Not Implemented`, `application/json`
  ```json
  {"error":"WiFi connect not available on desktop"}
  ```

### 2.6 POST `/api/wifi/mode_ap`
Sets operational mode back to Standalone AP and reboots the device.
* **Request:** `POST /api/wifi/mode_ap`
* **Response (Success on ESP32):** `200 OK`, `application/json`
  ```json
  {"status":"ok","message":"Operational mode set to Standalone AP. Rebooting..."}
  ```
* **Response (Desktop):** `501 Not Implemented`, `application/json`
  ```json
  {"error":"WiFi mode select not available on desktop"}
  ```

---

## 🧪 3. Systematic Test Matrix

QA teams should execute the following 12 test cases against a running device to verify complete compliance:

### Happy Path Tests
* **TC-HP-01 (Get Dashboard):**
  * **Action:** GET `http://192.168.4.1/`
  * **Expected:** Returns status `200 OK` with HTML matching `CGA_INDEX_HTML`.
* **TC-HP-02 (Get System Status):**
  * **Action:** GET `http://192.168.4.1/api/status`
  * **Expected:** Returns `200 OK` and a valid JSON map containing system metrics, pre-allocated client snapshots, and active sessions.
* **TC-HP-03 (Kill Active Extension):**
  * **Action:** POST `http://192.168.4.1/api/kill` with body `extension=101`.
  * **Expected:** Returns `200 OK` with JSON `{"status":"ok","disconnected":"101"}`. Active call sessions involving `101` are immediately swept.
* **TC-HP-04 (Scan WiFi):**
  * **Action:** GET `http://192.168.4.1/api/wifi/scan`
  * **Expected:** Switch mode to `APSTA`. After channel scanning, returns `200 OK` with a list of SSIDs, RSSI values, and encryption metrics.

### Edge Case & Boundary Validation Tests
* **TC-ED-01 (Payload Too Large):**
  * **Action:** POST a payload larger than 16 KB (16,384 bytes) to `/api/wifi/connect` or any endpoint.
  * **Expected:** Server immediately aborts and returns `413 Payload Too Large` with JSON `{"error":"request body exceeds 16 KB limit"}`. Connection closes.
* **TC-ED-02 (Missing Kill Parameter):**
  * **Action:** POST `http://192.168.4.1/api/kill` with an empty body.
  * **Expected:** Returns `400 Bad Request` with JSON `{"error":"missing extension parameter"}`.
* **TC-ED-03 (Missing Connect Parameters):**
  * **Action:** POST `http://192.168.4.1/api/wifi/connect` with body `password=12345678` (missing `ssid`).
  * **Expected:** Returns `400 Bad Request` with JSON `{"error":"missing ssid parameter"}`.
* **TC-ED-04 (URL-Encoded Values Parsing):**
  * **Action:** POST to `/api/wifi/connect` with SSID containing spaces and special characters: `ssid=Office+AP%21&password=pass`.
  * **Expected:** Verify saved credentials in logs or NVS correspond to correctly decoded `Office AP!` rather than encoded characters.

### CSRF Security Tests
* **TC-SEC-01 (Direct Request - No Origin Header):**
  * **Action:** POST to `/api/kill` using `curl` with no `Origin` header.
  * **Expected:** Server assumes a direct administrative request. **Returns `200 OK`**.
* **TC-SEC-02 (Same-Origin Request):**
  * **Action:** POST to `/api/kill` with headers `Host: 192.168.4.1` and `Origin: http://192.168.4.1`.
  * **Expected:** Origin matches Host. **Returns `200 OK`**.
* **TC-SEC-03 (Cross-Origin Block):**
  * **Action:** POST to `/api/kill` with headers `Host: 192.168.4.1` and `Origin: http://malicious-website.com`.
  * **Expected:** Origin mismatch. **Returns `403 Forbidden`** with JSON `{"error":"cross-origin request rejected"}`.

### Concurrent Stress Tests
* **TC-ST-01 (Rapid Dashboard Status Polling):**
  * **Action:** Fire 50 requests per second against `/api/status` for 60 seconds while an active SIP call is running on Core 1.
  * **Expected:** 
    * All HTTP status requests return successfully.
    * No connection stalls occur.
    * **Crucially:** SIP call audio remains smooth, and no signaling UDP packets are dropped on Core 1 (verifying snapshotting prevents thread blocking).
