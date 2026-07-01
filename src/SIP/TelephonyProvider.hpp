#ifndef TELEPHONY_PROVIDER_HPP
#define TELEPHONY_PROVIDER_HPP

// ── Telephony provider abstraction ────────────────────────────────────────────
// `AnchorClient` (AnchorClient.hpp) IS the provider-agnostic telephony interface:
// it carries the provider-neutral concerns only — credential init / token
// lifecycle (init/start/stop), call control (makeCall/answerCall/dropCall),
// bidirectional PCM16 8 kHz audio framing (writeAudio/registerAudioRxCallback),
// and event callbacks (Ringing/Answered/Dropped/Dtmf/Incoming). Nothing in it is
// specific to one external system. This header gives that role an explicit name
// and adds the fixed-size registry/factory that maps a configured provider TYPE
// to the concrete implementation linked into the firmware.
//
// Concrete implementations today:
//   LoopbackAnchorClient  — on-box mock (no external system), the safe default
//                           and the only one this project ships.
//   StubTelephonyProvider — honest compile-time scaffolding for a provider type
//                           that is declared in the enum but NOT implemented.
//
// pocket-dial intentionally ships no other AnchorClient implementation — bring
// your own to bridge a real external system (SIP trunk, recording server, AI
// pipeline, ...). Add a TelephonyProviderType enumerator for it in a fork and
// register it the same way Loopback is registered.
//
// Invariants:
//   * Provider objects are constructed ONCE at boot (they are members of
//     RequestsHandler / static storage) — the registry stores raw pointers and
//     never allocates.
//   * No heap allocation in any per-packet/per-frame media path: writeAudio /
//     the audio RX callback operate on caller-owned PCM16 buffers.

#include "AnchorClient.hpp"
#include <cstdint>
#include <cstddef>

// The provider-agnostic interface, by its proper name. AnchorClient is kept as
// the primary identifier so the existing engine/tests stay untouched.
using ITelephonyProvider = AnchorClient;

// Provider types selectable from config (NVS u8 on ESP, config file on host).
// The numeric values are PERSISTED — never reorder or reuse them. pocket-dial
// ships only Loopback; this enum is the extension point for your own provider.
enum class TelephonyProviderType : uint8_t
{
	Loopback = 0,  // on-box mock anchor (no external system) — implemented
	Count          // sentinel — keep last
};

// Display/log name for a provider type ("?" for out-of-range).
const char* telephonyProviderName(TelephonyProviderType t);

// True only for providers with a real, working implementation. Stubs return
// false so config/UI surfaces can be honest about what actually dials.
bool telephonyProviderImplemented(TelephonyProviderType t);

// ── Honest stub provider ─────────────────────────────────────────────────────
// Compile-time scaffolding for a declared-but-unimplemented provider. Every
// operation fails cleanly: start() returns false, isConnected() is always
// false, makeCall/answerCall/dropCall/writeAudio return false. It never fakes
// connectivity, never fires events, and never touches the network.
class StubTelephonyProvider : public AnchorClient
{
public:
	explicit StubTelephonyProvider(TelephonyProviderType type) : _type(type) {}

	bool init(const std::string&, const std::string&,
	          const std::string&, const std::string&) override { return true; }
	bool start() override { return false; }            // not implemented yet
	void stop() override {}
	bool isConnected() const override { return false; }
	bool makeCall(const std::string&, std::string* ownLegOut = nullptr) override { if (ownLegOut) ownLegOut->clear(); return false; }
	bool answerCall(const std::string&) override { return false; }
	bool dropCall(const std::string&) override { return false; }
	void setEventCallback(EventCallback) override {}   // no events will ever fire
	bool writeAudio(const std::string&, const int16_t*, size_t) override { return false; }
	void registerAudioRxCallback(AudioRxCallback) override {}
	void tick() override {}                            // no periodic maintenance

	TelephonyProviderType type() const { return _type; }

private:
	TelephonyProviderType _type;
};

// ── Fixed-size provider registry / factory ───────────────────────────────────
// Maps TelephonyProviderType → a boot-constructed provider instance. Static
// table, no dynamic plugin machinery: registration happens once during
// RequestsHandler construction, selection thereafter is a bounds-checked array
// read. select() returns nullptr for unregistered types — callers fall back to
// the loopback provider (and say so in the log).
class TelephonyProviderRegistry
{
public:
	static constexpr size_t kMaxProviders = static_cast<size_t>(TelephonyProviderType::Count);

	// false if the type is out of range, the slot is already taken, or
	// `provider` is null. Idempotent re-registration of the SAME pointer is ok.
	bool registerProvider(TelephonyProviderType t, AnchorClient* provider);

	// The provider for `t`, or nullptr if none registered / out of range.
	AnchorClient* select(TelephonyProviderType t) const;

private:
	AnchorClient* _providers[kMaxProviders] = {};
};

#endif // TELEPHONY_PROVIDER_HPP
