# Zero-Touch Phone Auto-Provisioning — Design Specification

**Issue:** #35 (Zero-Touch Phone Auto-Provisioning, HTTP)
**Status:** Phase-1 design (build-ready). No code merged yet.
**Target:** ESP32-S3, ESP-IDF v5.3, C++17. Self-contained SIP registrar/PBX.
**Audience:** The engineer who will implement this. Architecture decisions below are
final unless the "Open Questions" section flags them otherwise — implement directly.

---

## 0. TL;DR for implementers

A desk phone (Yealink, Grandstream, Polycom, Cisco) boots, learns a *provisioning
URL*, fetches a per-MAC config file over HTTP from pocket-dial, and self-configures
its SIP account. pocket-dial generates that config on the fly from a MAC→extension
mapping stored in NVS, forcing the same constraints the SIP engine already enforces
(G.711 only, NAT off, server-side registration).

* **New HTTP routes** (in `HttpServer.cpp`): `GET /provision/{mac}.cfg`,
  `GET /provision/{mac}.xml`, `GET /provision/{mac}.boot`, plus admin CRUD under
  `/api/provision/*`.
* **New storage:** an NVS namespace `prov` holding MAC→(extension, secret) records.
* **Hook into SIP engine:** provisioning *pre-registers* an extension's expected
  secret so that the about-to-be-added SIP digest-auth layer (SEC-04) can validate
  the phone's REGISTER. Provisioning does **not** call into `RequestsHandler`'s hot
  path; it only reads/writes the NVS map. See §7.
* **MVP scope:** manual URL + Yealink only + admin-mapped MAC→extension. Everything
  else (DHCP option 66, multi-vendor, UI editor) is phased in later.

---

## 1. Discovery — how a phone finds pocket-dial

A factory-fresh SIP phone has no idea pocket-dial exists. There are three standard
mechanisms for it to learn a provisioning server URL. We evaluate each against the
ESP32-S3 / ESP-IDF v5.3 reality, then recommend the MVP path.

### 1.1 DHCP Option 66 / Option 43 (the "real" zero-touch path)

Enterprise phones request a provisioning URL via DHCP:

* **Option 66** (`TFTP server name`, RFC 2132) — historically a TFTP host, but every
  major vendor accepts an `http://host/path` string here. Yealink/Grandstream/Polycom
  all parse Option 66 as a provisioning URL when it begins with `http://`.
* **Option 43** (`Vendor-Specific Information`) — sub-option encoded, vendor-specific.
  Cisco/Polycom use it; encoding differs per vendor (TLV sub-options). More fragile.
* **Option 160** — Polycom/Poly's dedicated provisioning-URL option (a cleaner Polycom
  path than 66/43, single string).

**ESP-IDF feasibility constraint (the blocker).** pocket-dial's SoftAP runs the
bundled ESP-IDF `dhcpserver` component (via `esp_netif_create_default_wifi_ap()` in
`main/esp_main.cpp`). On IDF v5.3 the *server-side* option API,
`esp_netif_dhcps_option(esp_netif_t*, esp_netif_dhcp_option_mode_t,
esp_netif_dhcp_option_id_t, void*, uint32_t)`, only exposes a **fixed, small enum** of
option IDs:

```
ESP_NETIF_SUBNET_MASK            (1)
ESP_NETIF_DOMAIN_NAME_SERVER     (6)
ESP_NETIF_ROUTER_SOLICITATION_ADDRESS
ESP_NETIF_REQUESTED_IP_ADDRESS   (50)
ESP_NETIF_IP_ADDRESS_LEASE_TIME  (51)
ESP_NETIF_IP_REQUEST_RETRY_TIME
ESP_NETIF_VENDOR_CLASS_IDENTIFIER (60)
ESP_NETIF_VENDOR_SPECIFIC_INFO   (43)
```

So:

* **Option 66 is NOT settable through the public API.** There is no
  `ESP_NETIF_*` enum for code 66. To serve it you must either (a) patch/fork the
  `dhcpserver` component to append a code-66 option to the OFFER/ACK (`dhcps_option`
  table in `dhcpserver.c`), or (b) **disable the built-in DHCP server entirely and
  ship our own minimal DHCP responder** (we already ship a hand-rolled DNS server in
  `main/wifi/DnsServer.cpp`, so the pattern exists — a DHCP equivalent is ~200 LOC).
* **Option 43 IS settable** via `ESP_NETIF_VENDOR_SPECIFIC_INFO`, but the *payload*
  must be the raw vendor TLV blob, and each vendor decodes it differently. Getting
  one Option-43 blob that satisfies Yealink *and* Grandstream *and* Cisco
  simultaneously is brittle and per-firmware-version sensitive.
* **Option 160** is likewise not in the enum (same blocker as 66).

> **Decision:** DHCP-based discovery is *deferred to v2* and, when built, is done by
> **forking the bundled `dhcpserver` to inject Option 66** (single clean string,
> widest vendor support), NOT by abusing Option 43. The fork is the smaller, lower-risk
> change versus a full custom DHCP server, and Option 66 has the broadest vendor
> coverage. Document the fork as a maintained patch in `main/wifi/`.

### 1.2 mDNS (already half-built, but phones rarely use it)

pocket-dial already advertises mDNS (see `src/SIP/SipServer.cpp` and
`main/esp_main_display.cpp`):

```c
mdns_hostname_set("pocketdial");                 // -> pocketdial.local
mdns_service_add(NULL, "_sip",  "_udp", 5060, NULL, 0);
mdns_service_add(NULL, "_http", "_tcp", 80,   NULL, 0);
```

mDNS is useful for two things here, **neither of which is the primary discovery path**:

1. The phone's admin can type `http://pocketdial.local/provision/<mac>.cfg` instead of
   memorizing the IP (`pocketdial.local` already resolves; the captive-portal mDNS
   responder answers it).
2. We can advertise a **provisioning TXT record** so a future companion app or a
   mDNS-aware phone can auto-locate the URL. The `mdns_service_add()` calls currently
   pass `NULL, 0` for the TXT slot — we add one TXT key on the existing `_http._tcp`
   service:

   ```c
   mdns_txt_item_t prov_txt[] = { {"provurl", "/provision/"} };
   mdns_service_add(NULL, "_http", "_tcp", 80, prov_txt, 1);
   ```

   This is cheap (a few bytes in an already-advertised record) and worth doing even in
   MVP, but **commercial desk phones do not auto-provision from mDNS** — treat it as a
   convenience/forward-compat hook, not a discovery mechanism.

### 1.3 Manual URL (works on every phone, today, with zero firmware change)

Every phone's web UI has an "auto-provisioning / config server URL" field. An installer
enters `http://192.168.4.1/provision/<MAC>.cfg` (or `http://pocketdial.local/...`) and
the phone fetches on next reboot / "Auto Provision Now". This requires **only the HTTP
endpoint** on our side — no DHCP fork, no firmware-stack changes.

### 1.4 Recommendation

| Mechanism        | Phone-side effort | pocket-dial effort        | MVP?            |
| ---------------- | ----------------- | ------------------------- | --------------- |
| **Manual URL**   | Type one URL      | HTTP endpoint only        | **YES (MVP)**   |
| mDNS TXT hint    | None (informational) | 1-line TXT add         | Opportunistic   |
| DHCP Option 66   | None (true ZTP)   | Fork `dhcpserver` (~v2)   | No (v2)         |
| DHCP Option 43   | None              | Brittle per-vendor TLV    | No (rejected)   |

**MVP = manual URL.** It delivers the full provisioning *value* (phone self-configures
its SIP account, codec, NAT, expiry from one file) with the smallest, lowest-risk
change, and it is the only path that needs no modification to the ESP-IDF network
stack. DHCP Option 66 is the "true zero-touch" experience and is the headline v2
feature, gated behind the `dhcpserver` fork.

---

## 2. Endpoints

### 2.1 URL scheme

```
GET /provision/{mac}.cfg     # Yealink   (key=value)
GET /provision/{mac}.xml     # Grandstream / Polycom (XML; vendor disambiguated, see below)
GET /provision/{mac}.boot    # Polycom master bootstrap (points at {mac}.cfg)
GET /provision/{mac}.cisco   # Cisco SPA/MPP (v3; reserved, not in MVP)
```

* `{mac}` is the phone's 12-hex-digit MAC, **lowercase, no separators**
  (e.g. `805ec0a1b2c3`). Phones substitute their own MAC into the URL template; e.g.
  Yealink fetches `$MAC.cfg`, Grandstream `cfg$MAC.xml`, Polycom `$MAC.cfg`. We accept
  the MAC in the path and normalize it.
* **Vendor is selected by file extension**, not by `User-Agent`. Extension-based
  dispatch is deterministic, trivially testable, and matches what each vendor's
  firmware actually requests. (`User-Agent` sniffing is unreliable across firmware
  revisions and is **not** used for routing. We *log* it for diagnostics only.)
* Both `.xml` variants (Grandstream and Polycom) share an extension; disambiguate by a
  required query hint `?v=grandstream|polycom`, defaulting to Grandstream when absent.
  Rationale: Grandstream requests `cfg<mac>.xml`; Polycom requests `<mac>.cfg` +
  optionally a site `.xml`. The query string is stripped before routing today (see
  `HttpServer::parseRequest`), so the router must be extended to *read* it for this one
  case — see §7.1.

### 2.2 MAC parsing & router matching (critical implementation note)

The current router (`HttpServer::handleClient`) is a **flat exact-match if/else chain**
on `req.path`. Provisioning paths contain a variable MAC segment, so they cannot be
exact-matched. The router gains **one prefix branch**:

```cpp
// pseudo — see §7.1 for the real diff
if (req.method == "GET" && startsWith(req.path, "/provision/")) {
    sendProvisionConfig(clientSock, req);   // parses MAC + extension internally
}
```

`sendProvisionConfig` must:

1. Strip the `/provision/` prefix and the file extension.
2. **Normalize and validate the MAC**: lowercase; strip `:`/`-`/`.`; require exactly
   12 hex chars. Reject anything else with `404 Not Found` (NOT 400 — we do not want to
   leak that the route exists; see §4.4). Reuse the same whitelist discipline as
   `RequestsHandler::isValidAor` (hex-only here).
3. Look up the MAC in the NVS `prov` map (§5). Unknown MAC → `404`.
4. Render the per-vendor template (§2.3) and return it.

### 2.3 What every config pushes (vendor-agnostic policy)

Regardless of vendor, the generated config **must** force these, because the SIP engine
already enforces or assumes them server-side:

| Setting              | Value                              | Why                                                                 |
| -------------------- | ---------------------------------- | ------------------------------------------------------------------- |
| SIP server / proxy   | `192.168.4.1` (active AP IP) : `5060` | The registrar address (`_serverIp`/`_serverPort`).               |
| Transport            | UDP                                | Engine only speaks UDP (see `buildContact`: `;transport=UDP`).      |
| Extension (auth user)| assigned, e.g. `1001`              | Matches the AOR the engine will register (`_clients` key).          |
| Auth password        | per-MAC secret from NVS            | Consumed by the incoming SIP digest-auth layer (SEC-04 remediation).|
| Codec                | **G.711 only: PCMU(0), PCMA(8), telephone-event(101)** | The engine rewrites every answer SDP to `0 8 101` via `SipMessage::enforceG711()`. Phones offering only G.711 avoid a codec mismatch surprise. |
| NAT / STUN / ICE     | **OFF**                            | Media is peer-to-peer on one L2 segment; no NAT traversal. NAT keepalive/rport handling on the phone just adds latency and failure modes. |
| Registration expiry  | `3600` s                           | Matches `DEFAULT_EXPIRES`/`MAX_EXPIRES` in `RequestsHandler.cpp`; the engine caps anything higher to 3600 anyway. |
| Keep-alive           | OPTIONS-friendly                   | Engine pings each client with `OPTIONS` every 5 s and prunes after 15 s of silence (`sweepExpired`). Phones should answer OPTIONS (default on). |

> The codec lock is the single most important interop setting. pocket-dial does **not**
> transcode (no DSP budget); it only rewrites the SDP media line to `0 8 101`. If a
> phone is left on Opus/G.722-only, the rewritten answer advertises payloads the phone
> never offered and the call has no common codec. Provisioning fixes this at the source.

### 2.4 CONCRETE example — Yealink (`.cfg`, key/value)

**Request** (phone → pocket-dial, on boot / "Auto Provision Now"):

```http
GET /provision/805ec0a1b2c3.cfg HTTP/1.1
Host: 192.168.4.1
User-Agent: Yealink SIP-T46S 66.86.0.15
Accept: */*
Connection: close
```

**Response** (pocket-dial → phone). MAC `805ec0a1b2c3` is mapped to extension `1001`,
secret `Kf3pQz9mWx`:

```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Content-Length: 612
Connection: close

#!version:1.0.0.1
## pocket-dial auto-provision for 805ec0a1b2c3 (ext 1001)
account.1.enable = 1
account.1.label = 1001
account.1.display_name = 1001
account.1.auth_name = 1001
account.1.user_name = 1001
account.1.password = Kf3pQz9mWx
account.1.sip_server.1.address = 192.168.4.1
account.1.sip_server.1.port = 5060
account.1.sip_server.1.transport = 0
account.1.sip_server.1.expires = 3600
account.1.nat.nat_traversal = 0
account.1.nat.rport = 0
account.1.stun.enable = 0
account.1.codec.g711u.enable = 1
account.1.codec.g711a.enable = 1
account.1.codec.opus.enable = 0
account.1.codec.g722.enable = 0
account.1.codec.g729.enable = 0
account.1.codec.g726_32.enable = 0
```

Notes:
* `transport = 0` is Yealink's enum for UDP.
* The `#!version` line is required by Yealink firmware as the first line.
* Disabling every wideband/narrowband codec except G.711 (`g711u`, `g711a`) is what
  guarantees SDP compatibility with `enforceG711()`. `telephone-event` (DTMF, payload
  101) is implied by Yealink's default RFC2833 setting; no key needed.

### 2.5 CONCRETE example — Grandstream (`.xml`)

**Request:**

```http
GET /provision/cfg000b82aabbcc.xml HTTP/1.1
Host: 192.168.4.1
User-Agent: Grandstream GXP2170 1.0.11.46
Connection: close
```

(pocket-dial strips the `cfg` prefix and `.xml` suffix → MAC `000b82aabbcc`.)

**Response** — MAC `000b82aabbcc` → extension `1002`, secret `7tHn2bV5sR`. Grandstream
uses numeric **P-value** config keys inside `<gs_provision>`:

```http
HTTP/1.1 200 OK
Content-Type: text/xml; charset=utf-8
Content-Length: 689
Connection: close

<?xml version="1.0" encoding="UTF-8"?>
<gs_provision version="1">
  <config version="1">
    <!-- Account 1 active -->
    <P271>1</P271>
    <!-- SIP server address / port -->
    <P47>192.168.4.1</P47>
    <P40>5060</P40>
    <!-- SIP user / auth ID / auth password / display name -->
    <P35>1002</P35>
    <P36>1002</P36>
    <P34>7tHn2bV5sR</P34>
    <P3>1002</P3>
    <!-- Registration expiry (seconds) -->
    <P32>3600</P32>
    <!-- Transport: 0 = UDP -->
    <P130>0</P130>
    <!-- NAT traversal off (0 = No) ; STUN server cleared -->
    <P52>0</P52>
    <P76></P76>
    <!-- Preferred vocoder list: 0=PCMU, 8=PCMA only -->
    <P57>0</P57>
    <P58>8</P58>
    <P59></P59>
    <P60></P60>
    <P46>101</P46>
  </config>
</gs_provision>
```

Notes:
* P-codes are stable across the GXP/GRP families used for desk phones. Document the
  mapping table in code comments so the next engineer does not have to re-derive it.
* `P57..P60` are the ordered vocoder choices; leaving 3 and 4 empty after PCMU/PCMA
  means "no other codec offered."
* `P46 = 101` pins the `telephone-event` (DTMF/RFC2833) payload type to match
  `enforceG711()`'s `... 101`.

### 2.6 Polycom (`.xml` + `.boot`) — v2, spec only

Polycom fetches a master `000000000000.cfg`-style bootstrap then per-MAC overrides.
For pocket-dial we serve a minimal `{mac}.boot` master list that points at one
application XML, and a `{mac}.xml` with `<reg>` parameters:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<reg>
  <reg.1.address>1003</reg.1.address>
  <reg.1.auth.userId>1003</reg.1.auth.userId>
  <reg.1.auth.password>q8Lw1cN4dE</reg.1.auth.password>
  <reg.1.server.1.address>192.168.4.1</reg.1.server.1.address>
  <reg.1.server.1.port>5060</reg.1.server.1.port>
  <reg.1.server.1.transport>UDPOnly</reg.1.server.1.transport>
  <reg.1.server.1.expires>3600</reg.1.server.1.expires>
  <nat.signalPort>5060</nat.signalPort>
  <voIpProt.SIP.CSTA>0</voIpProt.SIP.CSTA>
  <voice.codecPref.G711_Mu>1</voice.codecPref.G711_Mu>
  <voice.codecPref.G711_A>2</voice.codecPref.G711_A>
  <voice.codecPref.G722>0</voice.codecPref.G722>
  <voice.codecPref.Opus>0</voice.codecPref.Opus>
</reg>
```

(`codecPref = 0` disables a codec; `1`/`2` set priority. UDPOnly forces transport.)

---

## 3. Extension assignment

### 3.1 Two modes

1. **Admin-mapped MAC→extension (MVP, default).** An installer registers each phone's
   MAC against a chosen extension before/at deployment, via `POST /api/provision/map`
   (§7.2) or, in v3, the dashboard editor. Deterministic, auditable, and the only mode
   appropriate when provisioning carries credentials (you know exactly which MAC gets
   which line).
2. **Sequential auto-assign (v2, opt-in).** The first time an *unknown* MAC requests a
   config, allocate the next free extension from a configured range (e.g. `1001..1032`)
   and persist the mapping. Convenient for bulk rollout, but it means *any* device on
   the AP that guesses the URL can claim a line — only enable it behind the short
   provisioning window + AP isolation (§4.4). Auto-assign is **off by default**.

### 3.2 Where mappings live

In NVS, namespace `prov` (§5). This is separate from the existing `storage` namespace
used for Wi-Fi creds, so a Wi-Fi factory-reset (`sendApiFactoryReset` erases
`wifi_*`/`decayed`) does **not** wipe the phone fleet mapping, and vice-versa. A
dedicated `POST /api/provision/reset` clears `prov`.

### 3.3 Interaction with the static SIP client pool (READ THIS)

The SIP engine pre-allocates a **fixed** client pool. In
`RequestsHandler` the constructor currently hardcodes the loop bounds:

```cpp
for (int i = 0; i < 32; ++i)  _clientPool.push_back(std::make_shared<SipClient>()); // 32 clients
for (int i = 0; i < 8;  ++i)  _sessionPool.push_back(std::make_shared<Session>());  //  8 sessions
```

`docs/SCALING.md` (authored in parallel) documents these as the device's *hard*
concurrency limits, and `src/SIP/PoolConfig.hpp` defines the intended knobs
`POCKETDIAL_MAX_CLIENTS` (default 32) / `POCKETDIAL_MAX_SESSIONS` (default 8) /
`POCKETDIAL_MSG_POOL`. **Implementation hazard to flag now:** the constructor uses the
literals `32`/`8`, not the `POOL_CONFIG` macros — so today the macros are *defined but
unused*. Provisioning must treat **`POCKETDIAL_MAX_CLIENTS` as the authoritative cap**
and the parallel pool-sizing/SCALING work should reconcile the constructor to use the
macros. (Filed as a cross-cutting note for the pool-sizing agent; provisioning does not
edit `RequestsHandler.cpp`.)

Consequences for provisioning:

* **The provisioning mapping table can be larger than the SIP pool, and that's fine.**
  A MAC→extension record is just bytes in NVS; it does not consume a `SipClient` slot
  until that phone actually sends a REGISTER. You can pre-map 50 phones against a
  32-slot pool.
* **But only `POCKETDIAL_MAX_CLIENTS` phones can be *simultaneously registered*.** The
  33rd concurrent REGISTER already gets `503 Service Unavailable` from
  `allocateClient()` (after trying to evict an expired slot). Provisioning does not
  change that ceiling and must not pretend to.
* **Guard rail:** `POST /api/provision/map` should **warn (not block)** when the number
  of active mappings exceeds `POCKETDIAL_MAX_CLIENTS`, returning a
  `{"warning":"mapped_count exceeds registrar capacity N; excess phones will get 503"}`
  field. Blocking would break the legitimate "more desks than concurrent calls" case;
  warning sets the right expectation. The dashboard (v3) should surface
  `mappedCount` vs `POCKETDIAL_MAX_CLIENTS` so the installer sees the headroom.
* **Extension namespace must stay valid AORs.** Assigned extensions are validated with
  the same rules as `RequestsHandler::isValidAor` (alphanumererics + `. - _ +`) and
  must avoid the reserved virtual extensions **`777`** (echo test) and **`999`**
  (all-page broadcast). Auto-assign and the map endpoint reject those.

---

## 4. Security

Provisioning configs carry **live SIP credentials** in cleartext inside the response
body. This is the central tension: the file must be readable by an unauthenticated
phone, yet it is the most sensitive payload the device serves.

### 4.1 HTTP vs HTTPS on a LAN appliance (reality)

* The dashboard and all APIs are **HTTP only** today (`HttpServer` is a plain TCP
  socket; no TLS). The SoftAP is **open** (`WIFI_AUTH_OPEN` in `wifi_init_softap`).
* TLS on the provisioning endpoint is **not viable for MVP**: (a) it needs a cert the
  phones will trust, and desk phones ship with their own CA stores and frequently fail
  on self-signed certs without a manual trust step — which defeats "zero-touch"; (b)
  mbedTLS server sockets add RAM/flash and CPU the S3 can spare only grudgingly while
  also running LVGL + SIP; (c) the threat is a *local L2 sniffer on an open AP*, which
  TLS would address — but the same sniffer can also see the REGISTER digest exchange
  and the RTP, so TLS on just the config fetch is a partial mitigation, not a fix.
* **Decision:** Provisioning is served over **HTTP**, and the real mitigation is
  link-layer + scoping (§4.4), not transport crypto. We *document* HTTPS-for-provisioning
  as a v3+ hardening item tied to enabling **WPA2 on the SoftAP** (a far higher-leverage
  change: closing the open AP removes the passive sniffer entirely). See §4.5.
* **Update:** that link-layer mitigation now **exists**. WPA2 on the SoftAP is
  implemented (NVS `ap_secure`, dashboard toggle, or set at flash time from the browser
  flasher) with a per-device generated passphrase — see
  [THREAT_MODEL.md §6](THREAT_MODEL.md) and
  [SETUP_GUIDE.md](SETUP_GUIDE.md#turning-on-access-point-security-wpa2). It defaults to
  **off** for fleet compatibility, so the passive-sniffer risk below is only actually
  closed on deployments that turn it on. **Provision over a secured link.**

### 4.2 Per-MAC allowlist

Provisioning serves a config **only for a MAC that already exists in the `prov` map**.
There is no "render a config for any MAC" path. Unknown MAC → `404`. In admin-mapped
mode (MVP default) this *is* the allowlist: a credential file exists only for explicitly
enrolled hardware. This is the primary access control.

### 4.3 Authenticating the provisioning fetch — can we?

Desk phones provisioning from a bare URL generally cannot present an admin session
cookie or a bearer token (the credential bootstrap problem: the thing being provisioned
has no credentials yet). Options considered:

* **HTTP Basic on the provisioning URL** — vendors *do* support a username/password in
  the provisioning URL (`http://user:pass@host/...`). But that shared secret must be
  typed into every phone, which erodes the zero-touch goal and is itself sniffable on an
  open AP. Rejected for MVP; available as an opt-in (`prov.basic_user/basic_pass` in
  NVS) for installers who want defense-in-depth.
* **Per-MAC URL token** — extend the path to `/provision/{mac}-{token}.cfg`, where
  `token` is a short per-MAC random nonce stored in the map. A scanner that doesn't know
  the token gets `404`. This raises the bar against URL-guessing without requiring any
  phone-side secret entry (the installer just uses the tokenized URL). **Recommended as
  the MVP hardening knob** — it is cheap, needs no phone feature, and composes with the
  manual-URL flow (the installer pastes the full tokenized URL).
* **Session-cookie auth** — appropriate for the *admin* CRUD endpoints (§4.6), NOT for
  the phone fetch.

> **Decision:** The phone-facing `GET /provision/...` endpoints are **unauthenticated**
> by necessity, bounded by (1) per-MAC allowlist, (2) optional per-MAC URL token
> (recommended on), (3) the provisioning window, and (4) AP isolation — §4.4.

### 4.4 Bounding the risk of an unauthenticated credential endpoint

Layered controls, in priority order:

1. **MAC allowlist (always on).** No mapping ⇒ `404`. The attack surface is exactly the
   set of enrolled MACs.
2. **Indistinguishable 404s.** Bad MAC, unknown MAC, wrong/missing token, and a
   disabled window all return the **same** `404 Not Found` body. Never return `400`/`403`
   that confirm "the route exists but you got the MAC/token wrong." Do not log the
   secret. (Mirrors the existing posture of not leaking internal state via error codes.)
3. **Provisioning window (recommended on for credential safety).** A boolean+deadline in
   NVS (`prov.window_until`, epoch seconds): outside the window, *all* `/provision/*`
   return `404`. The installer opens a window (e.g. 15 min) from the dashboard, powers on
   the phones, they provision, the window auto-closes. After that, the credential files
   are simply not served. Default window length: **15 minutes**; default state: **open at
   first boot of a freshly mapped device, else closed** (final value is an open question —
   see §8). Re-opening is an authenticated admin action.
4. **Per-MAC URL token (§4.3).** Defeats blind URL enumeration even inside the window.
5. **AP isolation / client isolation.** Enable SoftAP **client isolation** so stations
   cannot talk to each other at L2 — a rogue laptop on the AP can still reach
   `192.168.4.1` (the gateway) but cannot sniff another phone's unicast traffic. This
   does not stop a station from fetching the gateway's HTTP, so it complements (not
   replaces) the allowlist/token. *Caveat:* peer-to-peer **RTP between phones requires
   station-to-station traffic**, so full client isolation would break calls. The correct
   setting is therefore **not** blanket isolation; it is documented as a deployment
   trade-off and is **out of scope for the provisioning change itself** (it lives in
   `wifi_init_softap`). Flagged here so it is not assumed.
6. **Rate limiting.** The HTTP path has no token-bucket today (only the SIP UDP path
   does, via `RequestsHandler::allowPacket`). Provisioning enumeration over HTTP is
   bounded mainly by the `404`+token design; an HTTP-side limiter is a nice-to-have, not
   a blocker, and is noted for the HTTP-hardening backlog.

### 4.5 Relationship to SEC-03 / SEC-04 (security audit)

* **SEC-04 (no admin auth)** is being closed in parallel by the admin-auth layer
  (`POST /api/admin/login` + session cookie; mutating endpoints gated when an admin PIN
  is set). Provisioning's **admin CRUD** endpoints (§4.6) must be gated by exactly that
  layer.
* **SEC-04 (no SIP auth)** remediation = SIP digest auth in `onRegister`/`onInvite`.
  Provisioning is the **other half** of that feature: digest auth is useless without a
  way to put the right password on the phone, and provisioning is useless if the phone's
  REGISTER isn't actually checked against that password. The per-MAC secret in the `prov`
  map is the shared source of truth for both. See §7.3 for the seam.
* **SEC-03 (cleartext NVS)** now applies to SIP secrets too. The same remediation
  (enable flash encryption; or AES-CTR the secret with a MAC-derived key before
  `nvs_set_str`) should cover `prov` secrets. At minimum, **store the per-MAC secret
  encrypted-at-rest if flash encryption is enabled**, and never log it.

### 4.6 Admin CRUD endpoints — gated

`POST /api/provision/map`, `DELETE /api/provision/map`, `POST /api/provision/window`,
`POST /api/provision/reset`, and `GET /api/provision/list` are **mutating/sensitive admin
operations**. They MUST:

* Pass the existing `isSameOrigin()` CSRF check (same as `/api/kill`, etc.).
* Be gated by the new admin-auth session when an admin PIN is set (same rule the
  parallel SEC-04 work applies to other mutating endpoints).
* `GET /api/provision/list` returns mappings but **MUST redact secrets**
  (`"secret":"********"`), since it renders in the dashboard and to `/api/status`-style
  consumers.

---

## 5. Data model (NVS schema)

### 5.1 Namespace and record layout

NVS namespace: **`prov`** (distinct from `storage`). NVS is a flat key→value store, so
each mapping is encoded as a small set of keys derived from the MAC. Two layouts are
viable; we pick **(A)** for MVP simplicity.

**(A) Per-field keys (MVP).** For MAC `m` (12 lowercase hex), store:

| Key (≤15 chars)      | Type   | Example          | Notes                                   |
| -------------------- | ------ | ---------------- | --------------------------------------- |
| `e_<mac8>`           | str    | `1001`           | assigned extension (AOR)                |
| `s_<mac8>`           | blob/str | `Kf3pQz9mWx`   | per-MAC SIP secret (encrypt if FE on)   |
| `t_<mac8>`           | str    | `9f3a1c`         | optional per-MAC URL token (§4.3)       |
| `v_<mac8>`           | u8     | `0`              | vendor hint (0=auto,1=yealink,2=gs,3=poly) |

> **NVS key-length constraint:** NVS keys are capped at **15 characters**. A full 12-hex
> MAC + prefix (`e_805ec0a1b2c3` = 14 chars) fits, but to stay safely under 15 across all
> prefixes use the **last 8 hex of the MAC** (`mac8`) as the key suffix and store the full
> MAC in the value-side record for collision detection. (MAC-suffix collisions across a
> single small LAN are vanishingly unlikely, but the full MAC is validated on lookup so a
> collision yields a `404` rather than the wrong config.) Final encoding is the
> implementer's call; the constraint is the load-bearing fact.

Global control keys (not per-MAC):

| Key             | Type | Meaning                                              |
| --------------- | ---- | ---------------------------------------------------- |
| `auto_assign`   | u8   | 0=off (default), 1=sequential auto-assign            |
| `range_lo`      | str  | low end of auto-assign range, e.g. `1001`            |
| `range_hi`      | str  | high end, e.g. `1032`                                |
| `next_ext`      | str  | next extension to hand out in auto-assign            |
| `window_until`  | u32  | epoch seconds; provisioning window deadline (0=closed)|
| `count`         | u16  | number of active mappings (for capacity warnings)    |
| `basic_user`    | str  | optional HTTP Basic user for the fetch (off by default)|
| `basic_pass`    | str  | optional HTTP Basic pass                             |

**(B) Single JSON blob (considered, deferred).** Store the whole table as one JSON blob
under `prov/table`. Simpler enumeration, but a single `nvs_set_blob` rewrite per edit and
a parse on every fetch; and the blob grows unbounded. Rejected for MVP in favor of (A)'s
O(1) per-MAC reads on the hot fetch path. Revisit if the table editor (v3) wants atomic
bulk import.

### 5.2 Capacity vs pool size

* **Mapping capacity** is bounded by NVS free space, not by the SIP pool. Each record is
  ~4 keys × (~20–40 bytes incl. NVS entry overhead) ≈ **under 200 bytes/phone**. A
  default NVS partition (often a few × 4 KB sectors, commonly ~24 KB usable) holds **well
  over a hundred** mappings even sharing the namespace with Wi-Fi creds — far beyond any
  realistic single-AP phone count.
* **Practical cap** = `POCKETDIAL_MAX_CLIENTS` (32 by default) for *concurrent
  registration*, per §3.3. Recommend the dashboard cap the mapping editor at, or warn
  beyond, this number. There is no reason to map thousands of phones to a 32-slot
  registrar; if a deployment needs that, it needs a bigger pool (see `docs/SCALING.md`)
  or multiple units.

---

## 6. Phasing

### MVP — "manual URL, Yealink only"
* Routes: `GET /provision/{mac}.cfg` (Yealink) only.
* Admin-mapped MAC→extension via `POST /api/provision/map` (CSRF + admin-auth gated).
* NVS `prov` namespace, layout (A); per-MAC secret stored.
* `404`-on-anything-unknown; per-MAC URL token supported; provisioning window supported.
* mDNS TXT `provurl` hint added (1 line) — opportunistic, cheap.
* No DHCP changes. No TLS.
* Deliverable: an installer pastes one URL into a Yealink phone and it comes up as a
  working extension with G.711-locked, NAT-off, 3600 s registration.

### v2 — "true zero-touch + multi-vendor"
* Fork the bundled `dhcpserver` to inject **Option 66** = `http://192.168.4.1/provision/`
  in OFFER/ACK (documented patch under `main/wifi/`). Phones auto-discover; no typed URL.
* Add Grandstream (`.xml`) and Polycom (`.xml`/`.boot`) renderers.
* Optional **sequential auto-assign** (off by default) with range + `next_ext`.
* HTTP-side rate limiting for `/provision/*`.

### v3 — "dashboard UI + hardening"
* CGA dashboard MAC→extension **mapping editor** (list/add/remove, capacity meter vs
  `POCKETDIAL_MAX_CLIENTS`, window open/close button, regenerate token). Backed by
  `/api/provision/*`.
* Cisco SPA/MPP renderer.
* HTTPS-for-provisioning tied to enabling **WPA2 on the SoftAP** (the higher-leverage
  fix), and flash-encrypted `prov` secrets (SEC-03 closure).

---

## 7. Implementation sketch

### 7.1 Router changes — `HttpServer.cpp` / `HttpServer.hpp`

The flat if/else in `handleClient()` gains a prefix branch and the new admin routes.
Because provisioning paths carry a variable MAC, this branch is `startsWith`, not `==`:

```cpp
// in HttpServer::handleClient(), GET section:
else if (req.method == "GET" && req.path.rfind("/provision/", 0) == 0) {
    sendProvisionConfig(clientSock, req);          // §2.2 normalize+lookup+render
}
// admin CRUD (mutating -> isSameOrigin() + admin-auth session, like /api/kill):
else if (req.method == "POST"   && req.path == "/api/provision/map")    { /* gate */ sendProvisionMap(clientSock, req); }
else if (req.method == "DELETE" && req.path == "/api/provision/map")    { /* gate */ sendProvisionUnmap(clientSock, req); }
else if (req.method == "POST"   && req.path == "/api/provision/window") { /* gate */ sendProvisionWindow(clientSock, req); }
else if (req.method == "POST"   && req.path == "/api/provision/reset")  { /* gate */ sendProvisionReset(clientSock, req); }
else if (req.method == "GET"    && req.path == "/api/provision/list")   { /* gate */ sendProvisionList(clientSock); }  // secrets redacted
```

New private members in `HttpServer.hpp` (mirrors existing `sendApi*` style):
`sendProvisionConfig`, `sendProvisionMap`, `sendProvisionUnmap`, `sendProvisionWindow`,
`sendProvisionReset`, `sendProvisionList`, plus helpers `normalizeMac()`,
`renderYealinkCfg()`, `renderGrandstreamXml()` (v2), `renderPolycomXml()` (v2),
`provLookup(mac)` / `provStore(...)` wrapping NVS.

**One required parser tweak:** `parseRequest()` currently *discards* the query string
(`req.path = req.path.substr(0, queryPos)`). The `?v=grandstream|polycom` disambiguator
(§2.1) needs it. Add a `std::string query;` field to `HttpRequest` and capture it before
stripping (do **not** change the existing `path` semantics other code relies on). The
DELETE method also must be accepted by `parseRequest` (it already parses the method token
generically, so this is free).

**Content types:** `.cfg` → `text/plain; charset=utf-8`; `.xml` → `text/xml; charset=utf-8`.
Reuse the existing `sendResponse()` for everything; it already sets `Content-Length` and
`Connection: close` and omits CORS headers (correct — we never want a browser reading a
config cross-origin).

### 7.2 Admin map endpoint contract

```http
POST /api/provision/map HTTP/1.1
Host: 192.168.4.1
Origin: http://192.168.4.1
Content-Type: application/x-www-form-urlencoded

mac=805ec0a1b2c3&extension=1001&vendor=yealink
```
* Validates MAC (12 hex, normalized), extension (valid AOR, not `777`/`999`).
* Generates a per-MAC secret (use the existing `IDGen::GenerateID(...)` used for SIP
  tags/branches — already in the tree — for a URL-safe random secret/token) unless one
  is supplied.
* Persists to NVS `prov`; bumps `count`.
* Response: `{"status":"ok","mac":"805ec0a1b2c3","extension":"1001",
  "url":"http://192.168.4.1/provision/805ec0a1b2c3-9f3a1c.cfg"}` (note: tokenized URL
  returned so the installer can paste it; secret itself is **not** echoed).
* If `count > POCKETDIAL_MAX_CLIENTS`, add `"warning": "..."` (§3.3).

### 7.3 Seam into `RequestsHandler` (extension assignment ↔ SIP auth)

Provisioning **does not touch the hot signaling path** and does not add a lock. The
coupling is the **shared per-MAC secret**:

* Provisioning writes `(extension, secret)` to NVS `prov`.
* When SIP digest auth lands (SEC-04 remediation in `onRegister`/`onInvite`), the engine
  needs to resolve `extension → expected secret`. The clean seam is a **read-only lookup
  callback** injected into `RequestsHandler` at construction (same pattern as the
  existing `OnHandledEvent` functor), e.g.
  `using CredentialLookup = std::function<std::optional<std::string>(std::string_view ext)>;`
  resolved from the `prov` NVS map. The registrar calls it inside its already-held
  `_mutex` section to fetch the expected secret, then verifies the digest. No new mutable
  shared state, no provisioning code inside the registrar.
* **Important ordering:** until that callback + digest verification exist, the registrar
  is in `POCKETDIAL_OPEN_REGISTRAR` mode and will accept the provisioned extension's
  REGISTER **without** checking the secret. So MVP provisioning *configures* a password
  that *nothing verifies yet*. That is acceptable and honest for MVP (the value is
  auto-config, not auth), but the spec must say so: **provisioned credentials become
  load-bearing only once the SEC-04 SIP-auth work lands and the registrar runs in closed
  mode.** Do not market MVP provisioning as a security feature.

### 7.4 Estimated flash / RAM cost

* **Flash (code):** MVP (Yealink renderer + 6 handlers + NVS helpers + MAC normalize) is
  string-assembly and `nvs_*` calls — no new heavy dependency. Estimate **~4–8 KB** of
  `.text`. Adding Grandstream+Polycom renderers in v2 adds **~2–4 KB** more (mostly
  static format strings). The DHCP Option-66 fork adds a few hundred bytes.
* **RAM (static):** negligible. No new pools. The NVS handle is opened per-request and
  closed (matching the Wi-Fi handlers), so there is no persistent buffer. Per-request
  peak is one rendered config string on the worker thread's heap (~0.6–1 KB),
  comfortably inside the existing 4 KB `std::vector` read-buffer budget already used in
  `handleClient`. No change to the SIP pools (§3.3).
* **No new task / no new socket:** provisioning rides the existing `http_server_task`
  accept loop and detached-worker model. No core-affinity changes.

### 7.5 ASCII sequence diagram — boot → DHCP → fetch cfg → REGISTER → call

```
 PHONE (Yealink)          ESP32-S3 pocket-dial (192.168.4.1)
   T46S @ MAC                SoftAP + DHCP + HTTP(:80) + SIP(:5060/UDP)
      |                                  |
      |  (1) Associate to open SoftAP    |
      |--------------------------------->|
      |                                  |
      |  (2) DHCP DISCOVER               |
      |--------------------------------->|
      |       DHCP OFFER/ACK             |  v1(MVP): standard options only.
      |       [v2: +Option 66 =          |  v2: forked dhcpserver injects
      |        http://192.168.4.1/       |       Option 66 provisioning URL.
      |        provision/ ]              |
      |<---------------------------------|
      |   lease 192.168.4.x              |
      |                                  |
      |  (3) GET /provision/<mac>.cfg    |  MVP: URL typed by installer (manual).
      |      (HTTP, Connection: close)   |  v2: URL learned from Option 66.
      |--------------------------------->|
      |                                  |--+ normalizeMac(); validate hex
      |                                  |  | provLookup(mac) in NVS `prov`
      |                                  |  | window open? token ok?  else 404
      |                                  |  | renderYealinkCfg(ext, secret, ...)
      |                                  |<-+ enforce: server=192.168.4.1:5060,
      |   200 OK  text/plain             |        G.711(0,8,101), NAT off,
      |   account.1.* = ...              |        expires=3600, transport=UDP
      |<---------------------------------|
      |                                  |
      |  (4) (phone applies cfg, may reboot once)
      |                                  |
      |  (5) REGISTER sip:1001@...:5060  |
      |--------------------------------->|  onRegister(): isValidAor ok
      |                                  |  [closed mode + SEC-04: 401 digest
      |   [401 challenge if SIP-auth]    |   challenge -> verify secret via
      |<-- - - - - - - - - - - - - - - --|   CredentialLookup(ext) from `prov`]
      |  (5b) REGISTER + Authorization   |
      |--------------------------------->|  allocateClient() -> _clientPool slot
      |   200 OK (expires=3600)          |  (503 if pool full, > MAX_CLIENTS)
      |<---------------------------------|  registered in _clients["1001"]
      |                                  |
      |  ...OPTIONS keepalive every 5s ->|  (engine pings; prune after 15s quiet)
      |<- - - 200 OK - - - - - - - - - --|
      |                                  |
      |  (6) INVITE sip:1002@...  (call) |  onInvite(): caller registered? callee?
      |--------------------------------->|  allocate Session in _sessionPool
      |   100/180, then 200 OK w/ SDP    |  enforceG711() rewrites m=audio 0 8 101
      |<---------------------------------|  (signalling via server)
      |  (7) ACK                         |
      |--------------------------------->|
      |                                  |
      |  (8) RTP audio  <==== peer-to-peer, phone<->phone, NOT via ESP32 ====>
      |                                  |
      |  (9) BYE / 200 OK                |  endCall(); Session slot released
      |<-------------------------------->|
```

---

## 8. Report

### Recommended MVP scope
**Manual-URL provisioning, Yealink `.cfg` only, admin-mapped MAC→extension.** Add the
phone-facing `GET /provision/{mac}.cfg` route plus admin CRUD (`/api/provision/map`,
`/list`, `/window`, `/reset`) behind the existing same-origin gate and the new admin-auth
session. Back it with an NVS `prov` namespace (per-field layout). Every generated config
hard-forces SIP server `192.168.4.1:5060`, **G.711 only (`0 8 101`)**, NAT off, and
`expires=3600` — matching what `SipMessage::enforceG711()` and `RequestsHandler` already
do. Bound the unauthenticated fetch with: MAC allowlist, uniform `404`s, an optional
per-MAC URL token (recommend on), and a time-boxed provisioning window. **No DHCP fork,
no TLS, no auto-assign in MVP.** This ships the full self-configure value with the
smallest, lowest-risk change and zero modification to the ESP-IDF network stack or the
SIP hot path.

### Top 2 risks
1. **Unauthenticated endpoint serves cleartext SIP credentials over an open AP.** The
   config body contains a live password and the SoftAP is `WIFI_AUTH_OPEN` with no TLS.
   The layered mitigations (allowlist + uniform 404 + per-MAC token + provisioning
   window) raise the bar but do **not** eliminate a local passive sniffer. The durable
   fix is closing the open AP (WPA2) and/or flash-encrypting NVS. **WPA2 on the SoftAP
   now ships** (opt-in, default off — §4.1); flash encryption is still tracked as
   SEC-03/v3. Turning WPA2 on is the difference between "layered mitigations that raise
   the bar" and "no passive sniffer on the link at all", so provision over a secured
   link. Provisioning must be shipped *with* the window defaulting to a safe state and
   must never be described as a security feature on its own.
2. **Static client-pool ceiling vs. fleet size mismatch.** The registrar pre-allocates a
   fixed pool (32 clients today, hardcoded in the `RequestsHandler` constructor rather
   than via the `POCKETDIAL_MAX_CLIENTS` macro that `PoolConfig.hpp`/`SCALING.md`
   advertise). Provisioning can enroll more MACs than the pool can concurrently register,
   so the 33rd phone silently gets `503` at REGISTER. We mitigate with a capacity
   *warning* on map, but the constructor/macro inconsistency needs reconciling by the
   pool-sizing work so the advertised cap is the real cap.

### Single biggest open question for product
**What is the default state and lifetime of the provisioning window — and is a typed URL
acceptable for MVP, or does the headline feature require true DHCP Option-66 zero-touch on
day one?** Concretely: should `/provision/*` be *open by default* (best out-of-box
experience, weakest security) or *closed until an admin explicitly opens a 15-minute
window* (safest, but requires a dashboard action before phones can provision)? This single
choice drives the MVP's security posture, the dashboard UX, and whether the `dhcpserver`
fork (a non-trivial, IDF-version-sensitive change) must be pulled forward from v2 into MVP.
