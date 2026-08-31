# Phone Compatibility & Registration

How to register SIP clients — softphones and hardware IP phones — against **pocket-dial**.
Every client uses the same core settings; this document gives per-client field mappings,
documents known interoperability quirks, and provides a "tested configuration" table you
can fill in for your own fleet.

For the end-to-end first call, see [SETUP_GUIDE.md](SETUP_GUIDE.md). For board choice, see
[HARDWARE_SELECTION.md](HARDWARE_SELECTION.md).

---

## 1. Universal settings (every client)

| Field | Value |
| :--- | :--- |
| SIP server / registrar / domain / proxy | `192.168.4.1` (SoftAP) — or the device's LAN IP on wired builds; `pocketdial.local` where mDNS resolves |
| Port | `5060` |
| Transport | **UDP only** — the engine does not speak TCP or TLS |
| Username / Auth ID / extension | your choice, e.g. `1001` (avoid `777` and `999`) |
| Password | any value or blank — there is **no SIP digest auth today** (see [THREAT_MODEL.md](THREAT_MODEL.md) S-3) |
| Audio codec | **G.711** µ-law (PCMU, `0`) / a-law (PCMA, `8`) always; **G.722** (`9`) between two phones that both offer it (peer-to-peer legs only — the echo test, register beep and any server-terminated media stay G.711). DTMF telephone-event (any payload number, `101` by convention) passes through |
| Registration expiry | ≤ `3600` s (the registrar caps higher values to 3600) |
| NAT / STUN / ICE / rport | **off** — media is peer-to-peer on one L2 segment; NAT traversal only adds latency and failure modes |

> [!IMPORTANT]
> **Codec is still the most important setting, but it now fails loudly.** pocket-dial does
> not transcode. On peer-to-peer legs it relays each phone's own offer/answer with the
> phone's preference order intact, dropping only payloads it won't carry (anything but
> PCMU, PCMA, G.722 and telephone-event) via `SipMessage::filterAudioCodecs()`. Two
> G.722-capable phones therefore negotiate wideband on their own; a phone left on
> Opus/G.729-only gets a clean **488 Not Acceptable Here** instead of the old rewritten
> answer that advertised payloads it never offered (signalling up, media dead). Keep
> G.711 enabled on every client so calls to the echo test, the register beep and any
> server-terminated leg — which stay pinned to `0 8 101` via `enforceG711()` — still work.

> [!NOTE]
> **Reserved extensions:** `777` (echo test) and `999` (all-page broadcast) are virtual
> extensions handled by the server. Never assign them to a real phone.

---

## 2. Softphones

### Linphone (desktop / mobile)

1. Settings → Account → add a SIP account manually.
2. SIP address: `sip:1001@192.168.4.1`.
3. SIP proxy / transport: `192.168.4.1:5060`, **UDP**.
4. Disable "outbound proxy" and any ICE/STUN/TURN under network settings.
5. Audio codecs: enable **PCMU** and **PCMA**, disable Opus/G.722/speex.

### Zoiper

1. Add account → manual configuration → SIP.
2. Domain / host: `192.168.4.1`, username `1001`, transport **UDP**.
3. Under Codecs, keep only **G.711 u-law / a-law**.
4. Disable STUN/rport in network settings.

### MicroSIP (Windows)

1. Menu → Add Account.
2. SIP server: `192.168.4.1`, Username `1001`, Domain `192.168.4.1`, transport **UDP**.
3. Codecs: select **PCMU** and **PCMA** only.

### Groundwire / Acrobits

1. Add SIP account (generic SIP).
2. Server `192.168.4.1`, port `5060`, username `1001`, transport **UDP**.
3. In advanced/codec settings, restrict to **G.711**; turn ICE/STUN **off**.

---

## 3. Hardware IP phones

Hardware desk phones can be configured manually through their web UI (server, port,
codec, transport as above). Note that automated zero-touch HTTP provisioning is a
**design specification only — not yet implemented** (see
[PROVISIONING.md](PROVISIONING.md), which is marked "No code merged yet"). Configure
hardware phones manually for now.

### Yealink (e.g. T2x / T4x series)

- Account → Register: Server Host `192.168.4.1`, Port `5060`, Transport **UDP**,
  Register Name / User Name `1001`.
- Codec: move only **PCMU** and **PCMA** to the enabled list; remove Opus/G.722/G.729.
- NAT: set NAT Traversal = Disabled, rport = Disabled, STUN = Disabled.

> [!IMPORTANT]
> **180 Ringing SDP strip (Yealink quirk, handled server-side).** Strict professional
> terminals like Yealink can hang on ringing or loop on early media when a provisional
> response carries SDP. pocket-dial **strips the SDP body from every `180 Ringing`** it
> forwards (`ringing->clearBody()` — the body is removed and `Content-Length` rewritten),
> which resolves the ringing hang on Yealink phones. This is automatic; you do not need to
> configure anything (README "VoIP Interoperability & SDP Stripping").

### Grandstream (e.g. GXP / GRP series)

- Account → General: SIP Server `192.168.4.1`, SIP Transport **UDP**, SIP User ID `1001`,
  Authenticate ID `1001`.
- Audio codecs: preferred vocoder list = **PCMU** then **PCMA** only; clear the rest.
- Disable STUN and "Use NAT IP".

### Cisco (SPA / MPP series)

- Ext 1 → Proxy and Registration: Proxy `192.168.4.1`, Transport **UDP**.
- Subscriber Information: User ID `1001`.
- Audio Configuration: preferred codec **G711u** / **G711a**; disable others; turn off
  ICE/STUN.

### Polycom / Poly

- Configure the SIP registrar address `192.168.4.1:5060`, transport **UDPOnly**, line
  address/auth user `1001`.
- Codec preferences: enable **G711_Mu** and **G711_A**, disable G722/Opus.

---

## 4. Behavior to expect after registration

- The registrar pings each client with a SIP `OPTIONS` keepalive **every 5 seconds** and
  prunes a client after **~15 seconds** of silence (`RequestsHandler.cpp`). Leave each
  phone's "answer OPTIONS / keep-alive" behavior at its default (on).
- A registered extension appears in the `clients` array of
  [`GET /api/status`](API.md#get-apistatus); an active call appears in `sessions`.
- If the client pool is full, a new REGISTER may receive `503 Service Unavailable`; the
  phone retries on its next refresh ([SCALING.md §4](SCALING.md)).
- Call audio (RTP) flows **directly phone-to-phone**, not through the device.

---

## 5. Quick verification per client

1. Register the client; confirm it shows "registered".
2. Confirm the extension appears on the dashboard (`/api/status` → `clients`).
3. Dial **`777`** — you should hear your own voice (echo). Confirms RTP + codec.
4. With a second extension registered, dial it directly — confirm two-way audio.
5. Dial **`999`** — confirm other registered phones are paged.

If any step fails, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md) (registration timeouts,
one-way/no audio, call drops).

---

## 6. Tested configuration table (fill in for your fleet)

Record what you have actually validated against your build. The settings below are the
known-good baseline; mark Pass/Fail and note firmware versions.

| Client | Type | Firmware/Version | Server:Port | Transport | Extension | Codecs enabled | 777 echo | 999 page | Direct call | Notes |
| :--- | :--- | :--- | :--- | :---: | :--- | :--- | :---: | :---: | :---: | :--- |
| Linphone | Softphone | | `192.168.4.1:5060` | UDP | | PCMU, PCMA | | | | |
| Zoiper | Softphone | | `192.168.4.1:5060` | UDP | | PCMU, PCMA | | | | |
| MicroSIP | Softphone | | `192.168.4.1:5060` | UDP | | PCMU, PCMA | | | | |
| Groundwire | Softphone | | `192.168.4.1:5060` | UDP | | PCMU, PCMA | | | | |
| Yealink | IP phone | | `192.168.4.1:5060` | UDP | | PCMU, PCMA | | | | 180-Ringing SDP stripped server-side |
| Grandstream | IP phone | | `192.168.4.1:5060` | UDP | | PCMU, PCMA | | | | |
| Cisco | IP phone | | `192.168.4.1:5060` | UDP | | G711u, G711a | | | | |
| Polycom/Poly | IP phone | | `192.168.4.1:5060` | UDP | | G711_Mu, G711_A | | | | UDPOnly transport |

**Related:** [SETUP_GUIDE.md](SETUP_GUIDE.md) · [HARDWARE_SELECTION.md](HARDWARE_SELECTION.md) ·
[TROUBLESHOOTING.md](TROUBLESHOOTING.md) · [API.md](API.md)
