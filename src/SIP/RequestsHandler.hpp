#ifndef REQUESTS_HANDLER_HPP
#define REQUESTS_HANDLER_HPP

// Gated open mode (Issue #56). Undefine/disable to run in closed/restricted mode.
#define POCKETDIAL_OPEN_REGISTRAR

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#elif defined(__linux__)
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#endif

#include <functional>
#include <unordered_map>
#include <string>
#include <mutex>
#include <optional>
#include <vector>
#include <tuple>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <array>
#include "SipMessage.hpp"
#include "SipClient.hpp"
#include "Session.hpp"
#include "CallDetailRecord.hpp"
#include "PcapCapture.hpp"
#include "PbxConfig.hpp"
#include "RtpSender.hpp"
#include "PbxEnv.hpp"
#include "TransactionLayer.hpp"
#include "Registrar.hpp"
#include "RegisterBeeper.hpp"
#include "ParkOrbit.hpp"
#include "BlfSubscriptions.hpp"

class RequestsHandler : private PbxEnv
{
public:

	using OnHandledEvent = std::function<void(const sockaddr_in&, std::shared_ptr<SipMessage>)>;

	RequestsHandler(std::string serverIp, int serverPort,
		OnHandledEvent onHandledEvent);

	// RETURNS NULL under sustained pressure — check it. The pool is bounded and
	// its heap fallback is now bounded too (Issue #101(A)); once both are spent
	// this refuses rather than allocating without limit. The contract for a
	// caller that gets nullptr is to DROP: it cannot answer 503, because building
	// the 503 would need a message out of the same empty pool. SIP over UDP
	// retransmits, so a dropped packet costs latency, not the call.
	static std::shared_ptr<SipMessage> getMessageFromPool(std::string message, sockaddr_in src);
	// Clones an already-parsed message into a free pool slot via a direct field
	// copy (SipMessage's copy assignment is a plain owned-string/vector copy —
	// no shared buffer to fix up). Used by every response-building call site that
	// used to go through getMessageFromPool(source->toString(), source->getSource()),
	// which paid a full serialize + reparse just to duplicate a message we had
	// already parsed once (issue #76).
	static std::shared_ptr<SipMessage> getMessageFromPool(const SipMessage& source);

private:
	// Hands out an uninitialized message the caller exclusively owns: a free
	// (use_count()==1) pool slot, else a bounded heap fallback, else nullptr once
	// POCKETDIAL_MSG_HEAP_FALLBACK_MAX are already alive (Issue #101(A)).
	// Internally synchronized on a leaf mutex — the pool is reachable from the
	// UDP receive task and the tick task at once. See the definition.
	static std::shared_ptr<SipMessage> acquirePooledMessage();

public:
	// ── Media beachhead static helpers (pure; host-unit-tested) ──────────────────
	// Build the server's own SDP body for the 440 answer (server media: PCMU on the
	// server's RTP port). Pure formatter — exposed so tests can assert its body and
	// the resulting Content-Length correctness (the 777-bug class).
	static std::string buildMediaSdp(const std::string& serverIp, int rtpPort);

	// Parse the caller's RTP destination from an INVITE: the SDP c= line IP (falling
	// back to the INVITE source IP) + the m=audio port via getRtpPort(). Returns false
	// if no usable port is found. Pure/host-testable.
	static bool parseCallerRtp(const std::shared_ptr<SipMessage>& invite,
		std::string& outIp, uint16_t& outPort);

	void handle(std::shared_ptr<SipMessage> request);
	void tick();

	std::optional<std::shared_ptr<Session>> getSession(std::string_view callID);

	// ── Dashboard query API (thread-safe) ────────────────────────────
	std::vector<std::pair<std::string, std::string>> getActiveClients();
	std::vector<std::tuple<std::string, std::string, std::string, int>> getActiveSessions();
	void forceDisconnect(const std::string& extension);
	uint64_t getPacketsProcessed() const;
	uint64_t getPacketsDropped() const;   // Issue #38: rate-limited/blocked packets
	size_t getClientCount();
	size_t getSessionCount();

	// Call Detail Records (CDR): a thread-safe snapshot of the recent-call ring,
	// newest first. Copied out under _snapshotMutex like the client/session views.
	std::vector<CallDetailRecord> getCallDetailRecords();

	// Issue #33: a classic libpcap file of the last POCKETDIAL_PCAP_RING_SIZE SIP
	// signaling packets (both directions), ready to write straight to a .pcap and
	// open in Wireshark. Takes _mutex (the capture ring is populated from inside
	// handle()/drainOutbox(), both already under it).
	std::string getPcapCapture();

	// Issue #32: the same capture ring, structured for the dashboard's polling
	// live tracer (GET /api/trace) instead of serialized to a .pcap file.
	std::vector<PcapCapture::TraceRecord> getTraceRecords();

	// Issue #35: zero-touch phone provisioning. Looks `mac` up in the adopted-
	// device registry; `authRequired` tells the caller whether this device
	// needs a SIP password on the wire (Secure mode, or an individually
	// Registrar::secure()'d device) — see ProvisioningConfig.hpp for why the
	// server can never supply that password itself.
	struct ProvisioningInfo
	{
		std::string extension;
		// cppcheck flags this as uninitMemberVarNoCtor. False positive:
		// ProvisioningInfo's only construction site (findProvisioningInfo's
		// return) always brace-initialises both fields.
		// cppcheck-suppress uninitMemberVarNoCtor
		bool authRequired;
	};
	std::optional<ProvisioningInfo> findProvisioningInfo(const std::string& mac);

	// Do Not Disturb (DND): set/query a per-extension flag. setDnd is the mutating
	// path behind POST /api/dnd (thread-safe; takes _mutex). getDndExtensions
	// returns the set of extensions currently in DND from the dashboard snapshot
	// (thread-safe; takes _snapshotMutex). Both are safe to call off the SIP thread.
	void setDnd(const std::string& extension, bool on);
	std::vector<std::string> getDndExtensions();

	// Call forwarding (CFU/CFB/CFNA). setForward mutates one trigger ("always",
	// "busy" or "noanswer") for an extension; an empty target clears it (and the
	// whole entry once all three are empty). Both are thread-safe (take _mutex /
	// _snapshotMutex) and NVS-persisted, mirroring setDnd/getDndExtensions. The
	// getter returns {extension, always, busy, noAnswer} tuples for the dashboard.
	void setForward(const std::string& extension, const std::string& trigger, const std::string& target);
	std::vector<std::tuple<std::string, std::string, std::string, std::string>> getForwards();

	// Ring/hunt groups. setRingGroup replaces a group's membership + mode; an empty
	// member list deletes the group. Thread-safe and NVS-persisted. The getter
	// returns {groupExt, "ringall"|"hunt", "m1,m2,..."} for the dashboard.
	void setRingGroup(const std::string& groupExt, const std::string& members, const std::string& mode);
	std::vector<std::tuple<std::string, std::string, std::string>> getRingGroups();

	// Parked calls snapshot for the TUI: {orbit, parkedExt, parker, secondsParked}.
	std::vector<std::tuple<std::string, std::string, std::string, int>> getParkedCalls();

	// Paging zones (980–989). setPageZone replaces a zone's membership; an empty
	// member list deletes the zone. Thread-safe and NVS-persisted. The getter
	// returns {zoneExt, "m1,m2,..."} pairs for the dashboard.
	void setPageZone(const std::string& zoneExt, const std::string& members);
	std::vector<std::pair<std::string, std::string>> getPageZones();

	// ── Admin extension (Task 2B) ─────────────────────────────────────────────────
	// NVS-persisted extension identity for the administrative endpoint.
	// Default "1001". Loaded from NVS namespace "pbxcfg", key "admin_ext" at boot.
	// cppcheck suggests returning `const std::string&` here (returnByReference).
	// Deliberately not applied: _adminExt is mutated by saveAdminExt()/
	// loadAdminExt() from other call paths with no lock of its own (callers of
	// this getter are not required to hold _mutex — dashboard/HTTP reads go
	// through here off the SIP thread). Returning by value at least keeps the
	// caller's copy independent once this call returns; a reference would
	// additionally dangle/tear if a concurrent save reallocates the string
	// while the caller still holds it.
	// cppcheck-suppress returnByReference
	std::string getAdminExt() const;

	// ── Admin HTTP-open deadline (PLAN_ADMIN_HTTP_ONLY.md Phase 2) ────────────────
	// Epoch-ms deadline until which HttpServer's accept-loop should accept
	// connections on a provisioned device; 0 means no open window. Written only by
	// onDtmfInfo's DTMF trigger branch (Phase 3), which already holds _mutex; read
	// lock-free here so HttpServer's own accept-loop thread never takes _mutex
	// (invariant I2 — it is the only thread that touches the listen socket).
	uint64_t getAdminHttpOpenUntilMs() const
	{
		return _adminHttpOpenUntilMs.load(std::memory_order_acquire);
	}

	// Called by HttpServer's /api/admin/set-pin handler on a successful PIN
	// set/change. Without this, the operator who just used the web UI to
	// provision the device would lose HTTP access on the very next accept-loop
	// tick (up to ~250ms later) — before they can finish onboarding (WiFi,
	// extensions, ...) — since setting the PIN is exactly what flips
	// AdminAuth::isProvisioned() to true and the dark-by-default gate on.
	// Grants the same TTL window a DTMF trigger would; lock-free (atomic store
	// only), safe to call from the HTTP worker thread.
	void grantAdminHttpGraceWindow()
	{
		uint64_t untilMs = nowEpochMs() +
			static_cast<uint64_t>(_adminHttpTtlSec.load()) * 1000ULL;
		_adminHttpOpenUntilMs.store(untilMs, std::memory_order_release);
	}

	// Called by HttpServer's authenticated /api/admin/keepalive endpoint. A
	// fixed 1-hour window, deliberately independent of _adminHttpTtlSec (the
	// DTMF trigger's shorter default) — an operator already logged in via a
	// valid session can push the window out further for extended
	// configuration work without needing to re-trigger from a handset.
	void extendAdminHttpWindowOneHour()
	{
		uint64_t untilMs = nowEpochMs() + 3600ULL * 1000ULL;
		_adminHttpOpenUntilMs.store(untilMs, std::memory_order_release);
	}

#if !defined(ESP_PLATFORM) && !defined(ESP32) && !defined(ARDUINO)
	// Test-only: drive the deadline directly without a real DTMF trigger. Not
	// compiled into device firmware.
	void setAdminHttpOpenUntilMsForTest(uint64_t ms)
	{
		_adminHttpOpenUntilMs.store(ms, std::memory_order_release);
	}
#endif

	// ── Registrar mode (STAGE 2) ──────────────────────────────────────────────────
	// Runtime registrar policy, replacing the compile-time POCKETDIAL_OPEN_REGISTRAR
	// gate. The policy machine itself lives in Registrar (see Registrar.hpp); these
	// aliases + wrappers keep the public API stable for the dashboard/TUI. Setter
	// takes _mutex; getter reads an atomic so the SIP hot path never locks.
	using RegistrarMode = Registrar::Mode;
	void setRegistrarMode(RegistrarMode mode);
	RegistrarMode getRegistrarMode() const;

	// ── Device registry (STAGE 2: Learn-mode adoption) ────────────────────────────
	// Adopted-device lifecycle for the TUI, owned by the Registrar machine. A device
	// is keyed by its 12-hex MAC and remembers the extension it registered as and
	// whether it has been promoted from first-seen (Learned) to digest-enforced
	// (Secured). All accessors are thread-safe (snapshot mutex) and NVS-persisted.
	using DeviceState = Registrar::DeviceState;
	using AdoptedDevice = Registrar::AdoptedDevice;
	// Snapshot of all adopted devices for the dashboard/TUI (thread-safe).
	std::vector<AdoptedDevice> getAdoptedDevices();
	// Promote a device to Secured (MAC-locked + digest-enforced). Accepts either a
	// 12-hex MAC or an extension (resolved to the device currently bound to it).
	// Returns false if no such device is known. Thread-safe + persisted.
	bool secureDevice(const std::string& macOrExt);
	// Forget a device entirely (drops the adoption record; a later REGISTER re-learns
	// it in Learn mode). Accepts a MAC or an extension. Thread-safe + persisted.
	bool forgetDevice(const std::string& macOrExt);

private:
	void initHandlers();

	// SIP request handlers (camelCase to match C++ convention)
	void onRegister(std::shared_ptr<SipMessage> data);
	void onOptions(std::shared_ptr<SipMessage> data);
	void onCancel(std::shared_ptr<SipMessage> data);
	void onReqTerminated(std::shared_ptr<SipMessage> data);
	void onInvite(std::shared_ptr<SipMessage> data);
	void onTrying(std::shared_ptr<SipMessage> data);
	void onRinging(std::shared_ptr<SipMessage> data);
	void onBusy(std::shared_ptr<SipMessage> data);
	void onUnavailable(std::shared_ptr<SipMessage> data);
	void onBye(std::shared_ptr<SipMessage> data);
	void onOk(std::shared_ptr<SipMessage> data);
	void onAck(std::shared_ptr<SipMessage> data);
	void onRefer(std::shared_ptr<SipMessage> data);   // blind transfer (RFC 3515)
	void onMessage(std::shared_ptr<SipMessage> data); // inbound MESSAGE (RFC 3428): ack 200 OK
	void onReinvite(std::shared_ptr<SipMessage> data);  // mid-dialog re-INVITE (hold/resume, RFC 3261 §14)
	void onUpdate(std::shared_ptr<SipMessage> data);    // RFC 3311 mid-dialog UPDATE

	// onSubscribe: thin dispatch-table shim into the BlfSubscriptions machine
	// (see BlfSubscriptions.hpp). Called from handle() — caller holds _mutex.
	void onSubscribe(std::shared_ptr<SipMessage> data);

	// BLF presence (RFC 6665 / RFC 4235) watcher-dialog FSM. Guarded by _mutex.
	BlfSubscriptions _blf{*this};

	// ── DTMF SIP INFO handler (Task 2C) ──────────────────────────────────────────
	// Invoked from handle() when a SIP INFO arrives carrying
	// Content-Type: application/dtmf-relay. Parses the Signal= digit, updates the
	// per-Call-ID accumulator, and dispatches CLASS feature code actions.
	// Called from the single-threaded SIP handler path — no additional mutex needed.
	void onDtmfInfo(std::shared_ptr<SipMessage> data);

	// Register beep (signaling-only intercom tone): the outbound UAC dialog FSM
	// lives in RegisterBeeper (see RegisterBeeper.hpp). Guarded by _mutex.
	RegisterBeeper _beeper{*this};

	// ── PbxEnv: shared-infrastructure surface for the extracted machines ───────
	// RequestsHandler is the PbxEnv implementation each decomposed state machine
	// (TransactionLayer, ...) talks back through. All three assume the caller
	// holds _mutex, same as the direct members they forward to.
	void enqueue(const sockaddr_in& to, std::shared_ptr<SipMessage> msg) override
	{
		// A null here means messageFromPool() refused (pool + bounded heap
		// fallback both spent, Issue #101(A)). Dropping it centrally keeps the
		// decomposed machines' `enqueue(addr, messageFromPool(...))` one-liners
		// safe without a check at each, and guarantees drainOutbox() — which
		// dereferences every entry — never sees a null.
		if (!msg) return;
		_outbox.emplace_back(to, std::move(msg));
	}
	std::shared_ptr<SipMessage> messageFromPool(std::string raw, sockaddr_in src) override
	{
		return getMessageFromPool(std::move(raw), src);
	}
	void log(std::string msg, bool isError = false) override
	{
		queueLog(std::move(msg), isError);
	}
	const std::string& localIp() const override { return _localIp; }
	int serverPort() const override { return _serverPort; }
	std::shared_ptr<SipClient> findRegistered(std::string_view number) override
	{
		auto c = findClient(number);
		return c.has_value() ? c.value() : nullptr;
	}
	std::shared_ptr<SipClient> allocVirtualPeer(std::string number, const sockaddr_in& addr) override
	{
		return allocateVirtualPeer(std::move(number), addr);
	}
	std::shared_ptr<Session> allocSession(const std::string& callID,
		const std::shared_ptr<SipClient>& src) override
	{
		return allocateSession(callID, src);
	}
	void insertSession(const std::string& callID, const std::shared_ptr<Session>& session) override
	{
		_sessions.emplace(callID, session);
	}
	std::shared_ptr<Session> findSession(std::string_view callID) override
	{
		auto s = getSession(callID);
		return s.has_value() ? s.value() : nullptr;
	}
	std::string contactFor(std::string_view number) const override
	{
		return buildContact(number);
	}
	std::shared_ptr<SipMessage> serverBye(const std::string& destExt,
		const sockaddr_in& destAddr, const std::string& callId,
		const std::string& fromHeader, const std::string& toHeader) override
	{
		return buildServerBye(destExt, destAddr, callId, fromHeader, toHeader);
	}
	void forEachSessionInvolving(std::string_view aor,
		const std::function<void(const std::string&, const Session&, DialogRole)>& fn) const override
	{
		for (const auto& [callID, session] : _sessions)
		{
			if (!session) continue;
			if (session->getSrc() && session->getSrc()->getNumber() == aor)
				fn(callID, *session, DialogRole::Caller);
			if (session->getDest() && session->getDest()->getNumber() == aor)
				fn(callID, *session, DialogRole::Callee);
		}
	}
	bool validAor(std::string_view s) const override
	{
		return isValidAor(s);
	}
	int requestedExpires(const std::shared_ptr<SipMessage>& msg) const override
	{
		return parseRequestedExpires(msg);
	}

	// RFC 3261 §17 INVITE client transactions (Timer A/B/L). Guarded by _mutex.
	TransactionLayer _txLayer{*this};

	// REGISTER admission policy + adopted-device registry (STAGE 2). Guarded by
	// _mutex except the lock-free mode atomic. The compile-time
	// POCKETDIAL_OPEN_REGISTRAR symbol only seeds the DEFAULT mode at boot; the
	// NVS-persisted value (loaded in the constructor) overrides it.
#ifdef POCKETDIAL_OPEN_REGISTRAR
	Registrar _registrar{*this, Registrar::Mode::Open};
#else
	Registrar _registrar{*this, Registrar::Mode::Secure};
#endif

	// RFC 4028 session timer helpers. Caller holds _mutex.
	void armSessionTimer(Session* session, const std::shared_ptr<SipMessage>& ok200);
	void sweepSessionTimers(std::chrono::steady_clock::time_point now);

	// Call parking / park-orbit: the orbit FSM lives in ParkOrbit (see
	// ParkOrbit.hpp). Guarded by _mutex.
	ParkOrbit _park{*this};

	// Mirror the park orbits into the dashboard snapshot. Caller holds _mutex;
	// takes _snapshotMutex internally.
	void refreshParkSnapshot();

	bool setCallState(std::string_view callID, Session::State state);
	void endCall(std::string_view callID, std::string_view srcNumber, std::string_view destNumber, std::string_view reason = "");

	// CDR: write one record into the ring as a call ends. Caller must hold _mutex.
	// `session` (may be null) supplies the start time / final state used to derive
	// duration and result; src/dest provide the parties when the session lookup
	// can't (e.g. the virtual 777/999 extensions reuse a shared dummy client).
	void recordCdr(const std::shared_ptr<Session>& session,
		std::string_view srcNumber, std::string_view destNumber);
	uint64_t nowEpochMs() const;

	// Internal DND lookup used by onInvite(). Caller MUST already hold _mutex
	// (std::mutex is non-recursive); does a bounded map lookup, no locking.
	bool isDndEnabled(const std::string& extension);

	// Lock-already-held mutation cores for setDnd()/setForward() (Issue #77).
	// _mutex is non-recursive, and onDtmfInfo() runs inside handle()'s _mutex
	// lock, so it cannot call the public setDnd()/setForward() without
	// deadlocking. These do the actual map mutation + dashboard-snapshot
	// refresh (queueLog only — no _mutex/_logQueue handling of their own) so
	// both the public setters (after taking _mutex) and onDtmfInfo's CLASS
	// code handling (already inside it) share one code path and one snapshot
	// refresh, instead of onDtmfInfo writing _dnd/_forwards directly and
	// leaving the dashboard snapshot stale until an unrelated HTTP-side call
	// happens to touch the same extension.
	void setDndLocked(const std::string& extension, bool on);
	void setForwardLocked(const std::string& extension, const std::string& trigger, const std::string& target);

	// Internal forward/group/zone lookups used by onInvite()/onBusy()/tick(). Caller
	// MUST already hold _mutex (non-recursive) — bounded map lookups, no locking.
	// getForwardTarget returns "" when no forward of that trigger is configured.
	std::string getForwardTarget(const std::string& extension, const std::string& trigger) const;
	const pbx::RingGroup* findRingGroup(const std::string& extension) const;
	const pbx::PageZone* findPageZone(const std::string& extension) const;
	bool isPageZoneDialog(const std::string& extension) const;

	// Fan an INVITE out to a set of targets (the reusable core extracted from the
	// 999 all-page path). `targets` are pre-selected registered clients; `intercom`
	// adds the 999 auto-answer headers (true for 999, false for a ring group so it
	// rings normally). Builds the broadcast Session, the 180 Ringing to the caller,
	// and one forked INVITE per target. Caller holds _mutex.
	void startBroadcastFork(std::shared_ptr<SipMessage> invite,
		std::shared_ptr<SipClient> caller,
		const std::vector<std::shared_ptr<SipClient>>& targets,
		bool intercom);

	// Build and queue a single INVITE fork toward one target, re-pointing the
	// request line / To at that target. `intercom` toggles the auto-answer headers.
	// Caller holds _mutex.
	// false when the message pool refused: no INVITE was sent, so the caller must
	// not report success on its behalf (#101A).
	bool buildInviteFork(const std::shared_ptr<SipMessage>& invite,
		const std::shared_ptr<SipClient>& caller,
		const std::shared_ptr<SipClient>& target,
		bool intercom);

	// Drive the next leg of a sequential hunt group (ring one member, arm timeout).
	// Returns false when the member list is exhausted. Caller holds _mutex.
	bool huntRingNext(const std::shared_ptr<Session>& session);

	// Re-target an INVITE at `target` and (re)send it as a fresh call leg — the
	// engine behind blind-transfer and call-forward "redirect" paths. Caller holds
	// _mutex. Returns false if the target is not registered.
	bool redirectInvite(const std::shared_ptr<SipMessage>& invite,
		const std::shared_ptr<SipClient>& caller,
		const std::string& target);

	// Build a NOTIFY (Event: refer) carrying a message/sipfrag body reporting the
	// transfer result back to the transferor. Caller holds _mutex.
	std::shared_ptr<SipMessage> buildReferNotify(const std::shared_ptr<SipMessage>& refer,
		const std::shared_ptr<SipClient>& transferor,
		const std::string& sipfrag,
		bool terminated);

	// ── Media beachhead: virtual extension 440 (server-sourced RTP tone) ─────────
	// onInvite() routes a dial of 440 here. The server answers 200 OK advertising its
	// OWN media (server IP:port, m=audio <svrport> RTP/AVP 0, PCMU) and starts the
	// one-way RTP tone stream to the caller's RTP address. ONE concurrent stream: a
	// 2nd dial while busy is rejected 486 Busy Here. Caller holds _mutex.
	void onMediaInvite(std::shared_ptr<SipMessage> data, const std::shared_ptr<SipClient>& caller);

	void unregisterClient(std::string_view number);

	// Registration-lease handling (RFC 3261 §10.2.1)
	int parseRequestedExpires(const std::shared_ptr<SipMessage>& data) const;
	void sweepExpired();   // evict expired bindings; caller must hold _mutex
	void maybeSweep();     // throttled sweep; caller must hold _mutex

	std::optional<std::shared_ptr<SipClient>> findClient(std::string_view number);
	std::optional<std::shared_ptr<SipClient>> findClientByAddress(const sockaddr_in& addr);
	std::shared_ptr<SipMessage> buildOptionsPing(const std::shared_ptr<SipClient>& client);

	// ── Outbound SIP MESSAGE (STAGE 2) ────────────────────────────────────────────
	// Originate a one-shot SIP MESSAGE (RFC 3428, Content-Type text/plain) to a
	// registered extension — e.g. to notify a phone/operator of a freshly assigned
	// secret. Mirrors the register-beep UAC enqueue (build + _outbox), best-effort:
	// returns false (no enqueue) if `ext` is not currently registered. The body is
	// length-bounded to keep the message in a single UDP datagram. Thread-safe: takes
	// _mutex. Safe to call off the SIP thread (the TUI/admin path).
	bool sendMessageTo(const std::string& ext, const std::string& text);

	// Broadcast / all-page extension (Issue #37). All assume the caller holds _mutex.
	void startPaging(std::shared_ptr<SipMessage> invite, std::shared_ptr<SipClient> caller);
	void handlePagingAnswer(const std::shared_ptr<Session>& session, std::shared_ptr<SipMessage> data);
	std::shared_ptr<SipMessage> buildCancel(const std::shared_ptr<SipMessage>& invite,
		const std::shared_ptr<SipClient>& target);
	std::shared_ptr<SipMessage> buildPagingBye(const std::shared_ptr<SipMessage>& ok,
		const std::shared_ptr<SipClient>& answerer);

	// Issue #38: per-source-IP token bucket + optional allowlist. Both helpers
	// assume the caller already holds _mutex.
	bool ipAllowed(const sockaddr_in& src) const;
	bool allowPacket(const sockaddr_in& src);

	void endHandle(std::string_view destNumber, std::shared_ptr<SipMessage> message);
	std::string buildContact(std::string_view number) const;

	bool isValidAor(std::string_view s) const;
	void queueLog(std::string msg, bool isError = false);

	std::shared_ptr<SipClient> allocateClient(std::string number, sockaddr_in address, int expiresSeconds);
	std::shared_ptr<Session> allocateSession(std::string callID, std::shared_ptr<SipClient> src);
	// Draw a transient virtual-peer SipClient (777/440/park leg) from the fixed pool
	// instead of make_shared'ing one in the packet handler. Falls back to heap on
	// exhaustion (graceful, never a crash). Caller holds _mutex.
	std::shared_ptr<SipClient> allocateVirtualPeer(std::string number, sockaddr_in address, int expiresSeconds = 3600);

	// Build a 200 OK with an SDP body for an INVITE (used by 777, park, onReinvite).
	std::shared_ptr<SipMessage> buildOkWithSdp(const std::shared_ptr<SipMessage>& inviteMsg,
		const std::string& activeIp, const std::string& toTag, const std::string& sdpBody);
	// Build a server-initiated in-dialog BYE. From/To must include tags because the
	// dialog role differs per call path (beep = server UAC; park = server UAS).
	std::shared_ptr<SipMessage> buildServerBye(const std::string& destExt,
		const sockaddr_in& destAddr, const std::string& callId,
		const std::string& fromHeader, const std::string& toHeader);

	// Verify that the in-dialog request comes from a peer recorded at dialog setup
	// (source IP match). Returns false → respond 403 Forbidden. Caller holds _mutex.
	bool isDialogSourceAuthorized(const std::shared_ptr<Session>& session,
		const sockaddr_in& source) const;

	// Server-side RTP media source (the 440 tone stream). One concurrent stream; the
	// ESP-only UDP socket + 20 ms pacing task live inside it, guarded for host builds.
	RtpSender _rtpSender;

	// RequestsHandler.hpp: Issues #24 and #28 resolved.
	std::unordered_map<std::string, std::function<void(std::shared_ptr<SipMessage> request)>> _handlers;
	std::unordered_map<std::string, std::shared_ptr<Session>>   _sessions;

	std::mutex _mutex;
	OnHandledEvent _onHandled;
	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> _outbox;
	// Take everything queued this pass, registering outgoing INVITEs for
	// retransmit on the way out. The one place messages leave _outbox — see the
	// #70 ordering note on the definition. Caller holds _mutex.
	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> drainOutbox();

	std::string _serverIp;
	std::string _localIp;   // resolved once at construction; avoids getPrimaryLocalIP() under _mutex
	int         _serverPort;

	std::atomic<uint64_t> _packetsProcessed{0};
	std::atomic<uint64_t> _packetsDropped{0};

	struct RegistrarSnapshot
	{
		std::vector<std::pair<std::string, std::string>> clients;
		std::vector<std::tuple<std::string, std::string, std::string, int>> sessions;
		std::vector<CallDetailRecord> cdr;   // newest first
		std::vector<std::string> dnd;        // extensions currently in DND
		// Call-forward config: {extension, always, busy, noAnswer}.
		std::vector<std::tuple<std::string, std::string, std::string, std::string>> forwards;
		// Ring/hunt groups: {groupExt, "ringall"|"hunt", "m1,m2,..."}.
		std::vector<std::tuple<std::string, std::string, std::string>> ringGroups;
		// Parked calls: {orbit, parkedExt, parker, secondsParked}.
		std::vector<std::tuple<std::string, std::string, std::string, int>> parkedCalls;
		// Paging zones: {zoneExt, "m1,m2,..."}.
		std::vector<std::pair<std::string, std::string>> pageZones;
		// Adopted devices (STAGE 2): {mac, ext, state, online}. Mirrored from the
		// Registrar's registry under _mutex; copied out under _snapshotMutex.
		std::vector<AdoptedDevice> devices;
		uint64_t packetsProcessed = 0;
		uint64_t packetsDropped = 0;
	};
	RegistrarSnapshot _snapshot;
	std::mutex _snapshotMutex;

	// CDR ring buffer (Phase 2). Fixed capacity, no heap growth: writes wrap and
	// overwrite the oldest slot. All access is under _mutex. _cdrHead is the index
	// of the NEXT slot to write; _cdrCount caps at POCKETDIAL_CDR_RECORDS.
	std::array<CallDetailRecord, POCKETDIAL_CDR_RECORDS> _cdrRing;
	size_t _cdrHead = 0;
	size_t _cdrCount = 0;

	// Issue #33: /api/pcap ring. Populated from handle() (inbound) and
	// drainOutbox() (outbound), both already under _mutex.
	PcapCapture _pcapCapture;

	// DND state, keyed by extension. Bounded by the client-pool depth: an entry is
	// only created when DND is turned ON, and turning it OFF erases the entry, so
	// the map can never hold more than POCKETDIAL_MAX_CLIENTS live extensions.
	// Guarded by _mutex. (A std::shared_ptr<SipClient> flag would be lost across
	// re-REGISTER / pool eviction; keying by extension keeps DND sticky.)
	std::unordered_map<std::string, bool> _dnd;

	// Call-forwarding config, keyed by extension (Class A sweep). Same bounding /
	// stickiness rationale as _dnd: an entry exists only while at least one trigger
	// is set, and is bounded by POCKETDIAL_MAX_CLIENTS. Guarded by _mutex; mirrored
	// into the dashboard snapshot and persisted to NVS.
	std::unordered_map<std::string, pbx::ForwardConfig> _forwards;

	// Ring/hunt groups, keyed by the group extension (e.g. 6xx). Bounded by
	// POCKETDIAL_MAX_CLIENTS groups; each member list is bounded by splitMembers().
	// Guarded by _mutex; mirrored into the snapshot and persisted to NVS.
	std::unordered_map<std::string, pbx::RingGroup> _ringGroups;

	// Paging zones, keyed by the zone extension (980–989). Bounded by
	// POCKETDIAL_MAX_PAGE_ZONES; member lists bounded by splitZoneMembers().
	// Guarded by _mutex; mirrored into the snapshot and persisted to NVS.
	std::unordered_map<std::string, pbx::PageZone> _pageZones;

	// How long to wait between OPTIONS keepalive cycles, in minutes. Atomic so the
	// TUI can read without taking _mutex. Persisted to NVS ("pbxcfg"/"rewarm_min").
	std::atomic<uint16_t> _rewarmMinutes{60};

	// ── Admin HTTP-open TTL (PLAN_ADMIN_HTTP_ONLY.md Phase 2) ─────────────────────
	// How long a DTMF trigger keeps the HTTP admin plane reachable, in seconds.
	// Atomic (mirrors _registrarMode) so HttpServer's accept-loop can read it
	// lock-free. Read-only from firmware: loadAdminHttpTtl() honors a value
	// provisioned out-of-band in NVS (namespace "pbxcfg", key "admin_http_ttl"),
	// but no code path changes it at runtime, so there is no write-through.
	std::atomic<uint64_t> _adminHttpOpenUntilMs{0};
	std::atomic<uint16_t> _adminHttpTtlSec{600};
	void loadAdminHttpTtl();              // boot-time reload from NVS; caller holds _mutex

	// Mirror the Registrar's adopted-device registry into the dashboard snapshot.
	// Caller holds _mutex; takes _snapshotMutex internally.
	void refreshDeviceSnapshot();
	// Mirror a Registrar registry change into the dashboard snapshot, taking the
	// cheap in-place path for an online-flag-only change. Caller holds _mutex.
	void applyDeviceChange(Registrar::Change change);

	// NVS persistence for _forwards / _ringGroups / _pageZones. No-ops on host (the
	// maps are the store); on ESP they read/write the "pbxcfg" NVS namespace.
	// Caller holds _mutex.
	void loadPbxConfig();                 // boot-time reload into the maps
	void persistForwards();               // write-through after a setForward mutation
	void persistRingGroups();             // write-through after a setRingGroup mutation
	void persistPageZones();              // write-through after a setPageZone mutation
	bool _pbxConfigLoaded = false;

	// Persistent CDR (Class A sweep). The CDR ring is flushed to the "cdrlog" NVS
	// namespace on teardown (write-through) and reloaded on boot, so records survive
	// reboot. No-ops on host. Caller holds _mutex.
	void loadCdrRing();                   // boot-time reload of the ring
	void persistCdrRing();                // flush the whole ring (bounded, fixed size)

	// Pre-allocated static memory pools (Issue #53)
	std::vector<std::shared_ptr<SipClient>> _clientPool;
	std::vector<std::shared_ptr<Session>> _sessionPool;
	static std::vector<std::shared_ptr<SipMessage>> _messagePool;
	// Virtual-peer pool: transient SipClient slots for 777/440/park legs (Issue #70).
	std::vector<std::shared_ptr<SipClient>> _virtualPeerPool;

	// Issue #38: token bucket keyed by source IPv4 (network-order s_addr).
	struct RateBucket
	{
		// cppcheck flags this as uninitMemberVarNoCtor. False positive: every
		// RateBucket is created via `_rateBuckets[ip] = { 40.0, now }` (below),
		// so `tokens` is overwritten immediately and is never read from the
		// briefly-default-constructed instance operator[] creates first.
		// cppcheck-suppress uninitMemberVarNoCtor
		double tokens;
		std::chrono::steady_clock::time_point last;
	};
	std::unordered_map<uint32_t, RateBucket> _rateBuckets;
	// Optional CIDR allowlist (host order). _allowMask == 0 means "no allowlist".
	uint32_t _allowNet  = 0;
	uint32_t _allowMask = 0;
	// Dedicated lock for the rate-limit state (_rateBuckets / allowlist), held
	// for the per-packet admission check BEFORE the big handler _mutex so a flood
	// from blocked IPs is dropped without ever serializing on _mutex against
	// legitimate signaling. Lock order: never acquire _mutex while holding this;
	// tick() may nest this inside _mutex (the only place both are held), so the
	// one ordering is _mutex → _rateMutex and handle() holds them disjointly.
	std::mutex _rateMutex;

	std::chrono::steady_clock::time_point _lastSweep{};
	std::chrono::steady_clock::time_point _lastTick{};

	std::vector<std::pair<bool, std::string>> _logQueue;

	// ── Admin extension (Task 2B) ─────────────────────────────────────────────────
	// NVS-persisted extension identity for the administrative endpoint.
	// Default "1001". Loaded from NVS namespace "pbxcfg", key "admin_ext" at boot.
	std::string _adminExt{"1001"};
	void loadAdminExt();
	void saveAdminExt(const std::string& ext);

	// ── DTMF digit-collection state machine (Task 2C) ────────────────────────────
	// Per-Call-ID accumulator. Accessed only from the single-threaded UDP receiver
	// task (the same path that calls handle()), so no additional mutex is needed.
	struct DtmfAccum
	{
		std::string digits;          // accumulated digit string
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
		TickType_t  lastTick{0};     // xTaskGetTickCount() of last digit
		TickType_t  starCodeFiredAtTick{0};
#else
		uint32_t    lastTick{0};     // monotonic ms counter on host
		uint32_t    starCodeFiredAtTick{0};
#endif
		// Set when the *4887 HTTP-open star-code just matched for this dialog
		// (0 = not pending). The star-code clears `digits` the instant the
		// sequence equals "*4887", which can land before the admin finishes
		// dialing *PIN#code if their PIN happens to begin with those four
		// digits — see the Issue #93 detection in onDtmfInfo(). Cleared on the
		// next digit (one warning per incident) or on the normal DTMF timeout.
		static constexpr uint32_t TIMEOUT_MS = 5000;
	};
	std::unordered_map<std::string, DtmfAccum> _dtmfState;
};

#endif
