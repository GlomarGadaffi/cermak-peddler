#ifndef DEVICE_CONFIG_HPP
#define DEVICE_CONFIG_HPP

// DeviceConfig: SoftAP security settings + the flash-time configuration seed.
//
// Two responsibilities, kept together because they write the same NVS keys:
//
//   1. **SoftAP security** (`ap_secure`, `ap_psk`). docs/THREAT_MODEL.md §6 and
//      docs/FEATURE_ROADMAP.md name WPA2 on the SoftAP as the single
//      highest-leverage hardening in the project: it encrypts the dashboard,
//      SIP signalling AND RTP media in one change, gates association, needs no
//      per-device certificates, and produces no browser warning — everything
//      self-signed HTTPS would not do (§6 rejects TLS as the primary control).
//
//      `ap_secure` DEFAULTS TO FALSE. Turning WPA2 on is a breaking change for
//      an already-deployed fleet — every phone must be re-associated with the
//      new passphrase — so an existing device keeps its open AP across a
//      firmware update and is switched over deliberately, from the dashboard or
//      at flash time (see 2). `ap_psk` is generated on first access regardless,
//      so the toggle always has a real passphrase to display.
//
//   2. **The flash-time configuration seed** (`cfgseed`). The browser flasher
//      (docs/flasher/index.html) writes a small fixed-layout blob to the
//      `cfgseed` partition so a board can be configured at install time. This
//      matters most on the headless `esp32s3-wifi` / `esp32s3-eth` variants,
//      where there is no screen to read a generated passphrase off.
//
//      A seed blob, NOT an NVS image, deliberately: generating a valid NVS
//      partition in JavaScript means reimplementing page headers, entry-state
//      bitmaps, span entries and CRCs, and writing it at 0x9000 would destroy
//      the saved WiFi credentials and admin PIN. A raw fixed-layout partition
//      follows the pattern partitions.csv already documents for `prompts`.
//
//      The firmware NEVER WRITES the seed partition — it only reads it — so the
//      erase-before-write discipline documented in partitions.csv for raw
//      (subtype 0x40/0x41) partitions does not apply here.
//
// Persistence: NVS namespace "storage" (the same one AdminAuth and the
// transports use), keys `ap_secure` (u8), `ap_psk` (str), `cfgseed_gen` (u32).
// All three are erased by /api/factory-reset, so a factory reset returns the
// board to its as-flashed configuration rather than to a hardcoded default.
//
// Host builds: this file compiles on the desktop/CI simulator, where there is no
// NVS and no `cfgseed` partition. Settings live in process memory for a single
// run (the AdminAuth.cpp pattern) and applyFlashSeed() is a no-op. This keeps
// the host test suite able to exercise the dashboard endpoints.

#include <string>
#include <cstdint>
#include <cstddef>

namespace DeviceConfig
{
	// ---------------------------------------------------------------------
	// Tunables
	// ---------------------------------------------------------------------

	// Generated passphrase length, in characters, from kPskAlphabet below.
	// 20 chars over a 32-symbol alphabet is 100 bits — far above what WPA2-PSK
	// offline handshake cracking can reach, and still readable off a screen.
	constexpr size_t kGeneratedPskChars = 20;

	// WPA2-PSK passphrase bounds, from IEEE 802.11i. Enforced on BOTH the
	// dashboard path and the seed path; esp_wifi rejects anything outside it.
	constexpr size_t kMinPskChars = 8;
	constexpr size_t kMaxPskChars = 63;

	// Crockford-style alphabet: no 0/O, no 1/I/L, no U. A passphrase read off a
	// small LCD or a serial log and retyped into a phone must not be ambiguous.
	constexpr const char* kPskAlphabet = "23456789ABCDEFGHJKMNPQRSTVWXYZ";

	// ---------------------------------------------------------------------
	// SoftAP security
	// ---------------------------------------------------------------------

	// True iff the standalone SoftAP should come up WPA2-PSK rather than open.
	// Defaults to FALSE on a device that has never been told otherwise — see the
	// fleet-compatibility note at the top of this file.
	bool isApSecure();

	// Persist the WPA2-on/off choice. Returns false if persistence failed.
	bool setApSecure(bool secure);

	// The AP passphrase. Generated (kGeneratedPskChars from kPskAlphabet, via the
	// hardware CSPRNG on device) and persisted on first access if none is stored,
	// so this never returns a string too short for WPA2. Callers may display it:
	// it is meant to be shown on the LVGL onboarding screen and logged to serial
	// on headless builds.
	std::string getApPsk();

	// Replace the stored passphrase with a freshly generated one and return it.
	// Existing associations break at the next AP restart — that is the point.
	std::string regenerateApPsk();

	// Overwrite the stored passphrase. Returns false (leaving the stored value
	// untouched) if `psk` is outside [kMinPskChars, kMaxPskChars] or contains a
	// byte outside printable ASCII, which esp_wifi would reject anyway.
	bool setApPsk(const std::string& psk);

	// ---------------------------------------------------------------------
	// Flash-time configuration seed
	// ---------------------------------------------------------------------

	// ==== WIRE FORMAT — keep in lockstep with docs/flasher/index.html ====
	//
	// A single 256-byte little-endian record at offset 0 of the `cfgseed`
	// partition. Every multi-byte field is little-endian; every string field is
	// NUL-padded to its full width and need not be NUL-terminated if it exactly
	// fills the field.
	//
	//   off  size  field
	//   ---  ----  -----------------------------------------------------------
	//     0     4  magic     kSeedMagic ("PDCS" as a LE u32)
	//     4     2  version   kSeedVersion
	//     6     2  flags     kSeedHas* bitmask below
	//     8     4  gen       generation counter; the flasher writes a UNIX
	//                        timestamp so every opt-in write is a new value
	//    12     1  wifiMode  0 = captive-portal default, 1 = STATION, 2 = AP
	//                        (matches the existing NVS key "wifi_mode")
	//    13     1  regMode   SIP registrar admission mode: 0 = open, 1 = learn,
	//                        2 = secure. Matches Registrar::Mode and the existing
	//                        NVS key "reg_mode" (u8) byte-for-byte.
	//    14     2  --        reserved, zero
	//    16    64  apPsk     SoftAP WPA2 passphrase
	//    80    33  staSsid   upstream WiFi SSID (STATION mode)
	//   113     3  --        reserved, zero
	//   116    64  staPass   upstream WiFi passphrase
	//   180    72  --        reserved, zero
	//   252     4  crc32     CRC-32 (IEEE 802.3, reflected poly 0xEDB88320,
	//                        init 0xFFFFFFFF, final xor 0xFFFFFFFF) over
	//                        bytes [0, 252)
	//
	// A record whose magic, version, size or CRC does not check out is IGNORED
	// silently — an unwritten partition reads as 0xFF and must not be an error.
	//
	// `regMode` was added after the first release of this format and deliberately
	// did NOT bump kSeedVersion. Each field is gated by its own "Has" flag and
	// every reader ignores flags it does not recognise, so the two directions are
	// both safe: older firmware reading a newer blob skips bit 5 and applies the
	// rest; newer firmware reading an older blob sees bit 5 clear and leaves
	// `reg_mode` alone. Bumping the version would instead have made older
	// firmware reject the whole record. Keep that property — add fields in the
	// reserved space with a new flag bit, never by repurposing an existing one.

	constexpr uint32_t kSeedMagic   = 0x53434450u;  // 'P','D','C','S' little-endian
	constexpr uint16_t kSeedVersion = 1;
	constexpr size_t   kSeedSize    = 256;

	// Where the seed's `regMode` must be written. The registrar keeps its
	// admission mode in its OWN NVS namespace, NOT the "storage" one every other
	// field of the seed lands in: Registrar::loadMode() opens
	// pbxpersist::kNvsNamespace (src/SIP/PbxPersist.hpp).
	//
	// The literal is duplicated here rather than included so this header stays
	// free of any src/SIP dependency — it is pulled into the host test suite and
	// into the pure-Ethernet transports, neither of which should drag the
	// registrar in. tests/DeviceConfig_test.cpp static_asserts the two against
	// each other, so the duplication cannot silently drift.
	//
	// Getting this wrong was issue #151: the write went to "storage", the read
	// came from "pbxcfg", and flash-time registrar mode did nothing at all on
	// every release that shipped it.
	constexpr const char* kRegistrarNvsNamespace = "pbxcfg";

	// `flags` bits. Each "Has" bit says "this field is meaningful"; a seed may
	// carry any subset, so the flasher can write only what the user changed.
	constexpr uint16_t kSeedHasApSecure = 1u << 0;  // apply the bit below
	constexpr uint16_t kSeedApSecureOn  = 1u << 1;  // the value: 1 = WPA2, 0 = open
	constexpr uint16_t kSeedHasApPsk    = 1u << 2;
	constexpr uint16_t kSeedHasWifiMode = 1u << 3;
	// staSsid and staPass are written as a pair. An SSID paired with the wrong
	// password is a device that never associates, so a bad half discards both.
	// An EMPTY staPass is valid and means an open upstream network -- the SSID
	// is what is required, not the password.
	constexpr uint16_t kSeedHasStaCreds = 1u << 4;
	// Set the SIP registrar's admission mode at flash time. This is the only
	// operator-facing way to reach `secure`/`learn` on a headless board: digest
	// auth has always been implemented, but nothing outside the tests ever wrote
	// `reg_mode`, so a device came up in the compiled-in default and stayed there.
	constexpr uint16_t kSeedHasRegMode  = 1u << 5;

	// Read the `cfgseed` partition and, if it holds a valid record whose `gen`
	// differs from the stored `cfgseed_gen`, apply the flagged settings to NVS
	// and record the new generation.
	//
	// Call once at boot, right after nvs_flash_init() and BEFORE the WiFi
	// bringup that reads these keys.
	//
	// Silently does nothing when:
	//   * there is no `cfgseed` partition — an OTA update does not rewrite the
	//     partition table, so firmware that expects `cfgseed` WILL run on boards
	//     flashed before it existed, and on the 4 MB
	//     sdkconfig.defaults.esp32_constrained layout, which has no such
	//     partition. This is the normal case, not an error;
	//   * the record fails validation (blank/erased flash);
	//   * `gen` matches the stored `cfgseed_gen` — the seed was already applied,
	//     so a plain reboot never re-clobbers settings changed since.
	//
	// Returns true iff settings were applied this call. Host build: no-op,
	// returns false.
	bool applyFlashSeed();

	// Erase ap_secure / ap_psk / cfgseed_gen. Called by /api/factory-reset.
	// Dropping cfgseed_gen is deliberate: the next boot re-applies the flash-time
	// seed, so "factory" means "as flashed", not "as hardcoded".
	void clearAll();
}

#endif // DEVICE_CONFIG_HPP
