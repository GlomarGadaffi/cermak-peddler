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
#include "PbxConfig.hpp"
#include "RtpSender.hpp"

class RequestsHandler
{
public:

	using OnHandledEvent = std::function<void(const sockaddr_in&, std::shared_ptr<SipMessage>)>;

	RequestsHandler(std::string serverIp, int serverPort,
		OnHandledEvent onHandledEvent);

	static std::shared_ptr<SipMessage> getMessageFromPool(std::string message, sockaddr_in src);

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
	// Default "101". Loaded from NVS namespace "pbxcfg", key "admin_ext" at boot.
	std::string getAdminExt() const;

	// ── Registrar mode (STAGE 2) ──────────────────────────────────────────────────
	// Runtime registrar policy, replacing the compile-time POCKETDIAL_OPEN_REGISTRAR
	// gate. NVS-persisted (namespace "pbxcfg", key "reg_mode"); the compile-time
	// symbol now only seeds the DEFAULT at boot. Thread-safe (mirror setDnd): setter
	// takes _mutex + refreshes the snapshot; getter reads an atomic so the SIP hot
	// path (onRegister) never locks just to branch on the mode.
	enum class RegistrarMode : uint8_t
	{
		Open   = 0,   // standalone: accept every REGISTER, no challenge (legacy)
		Learn  = 1,   // TOFU + MAC-lock: adopt unknown devices, enforce secured ones
		Secure = 2,   // require digest auth for every provisioned extension
	};
	void setRegistrarMode(RegistrarMode mode);
	RegistrarMode getRegistrarMode() const;

	// ── Device registry (STAGE 2: Learn-mode adoption) ────────────────────────────
	// Adopted-device lifecycle for the TUI. A device is keyed by its 12-hex MAC and
	// remembers the extension it registered as and whether it has been promoted from
	// first-seen (Learned) to digest-enforced (Secured). All accessors are
	// thread-safe (snapshot mutex) and NVS-persisted, mirroring forwards/groups.
	enum class DeviceState : uint8_t
	{
		Learned = 0,   // TOFU: seen + accepted, not yet locked to digest auth
		Secured = 1,   // promoted: MAC-locked + digest-enforced for its extension
	};
	struct AdoptedDevice
	{
		std::string mac;         // 12 lowercase hex chars
		std::string extension;   // the AOR it last registered as
		DeviceState state = DeviceState::Learned;
		bool online = false;     // currently has a live registration binding
	};
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

	// onSubscribe: Event-package gate (489 on anything but "dialog"), AOR validation
	// (404 for unregistered targets), fixed-slot allocation (503 on exhaustion),
	// 202 Accepted + an immediate full-state NOTIFY. Called from handle() — caller holds _mutex.
	void onSubscribe(std::shared_ptr<SipMessage> data);

	// One BLF watcher dialog. Fixed-size record in a std::array — no heap growth.
	struct DialogSubscription
	{
		bool        used = false;
		std::string callId;        // subscription dialog id (refresh/unsubscribe key)
		std::string watcherFrom;   // subscriber's full From header (incl. its tag)
		std::string subTo;         // our full To header (incl. the tag we minted)
		std::string targetAor;     // the extension being watched (the To user-part)
		std::string lastState;     // last NOTIFYed state token (change detection)
		unsigned    version = 0;   // dialog-info version counter (monotonic)
		unsigned    cseq = 1;      // NOTIFY CSeq within the subscription dialog
		int         expiresSec = 0;
		sockaddr_in addr{};        // where NOTIFYs go (the SUBSCRIBE source)
		std::chrono::steady_clock::time_point deadline{};
	};
	std::array<DialogSubscription, POCKETDIAL_MAX_SUBSCRIPTIONS> _subscriptions;

	// Compute the current RFC 4235 dialog state of `targetAor` from the registrar +
	// session tables: ""=idle, else trying/early/confirmed plus direction and dialog id.
	// Caller holds _mutex.
	std::string computeDialogState(const std::string& targetAor,
		std::string& outDirection, std::string& outDialogId) const;

	// Build one NOTIFY for a subscription slot carrying a dialog-info+xml body.
	// `terminated` selects Subscription-State: terminated;reason=<termReason>.
	// Caller holds _mutex.
	std::shared_ptr<SipMessage> buildDialogNotify(DialogSubscription& sub,
		const std::string& state, const std::string& direction, const std::string& dialogId,
		bool terminated, const char* termReason);

	// Recompute every watched target's state and NOTIFY slots whose state changed.
	// Called at the end of handle() and from tick() (inside _mutex). Caller holds _mutex.
	void refreshSubscriptions();

	// Expire overdue subscriptions: terminal NOTIFY (reason=timeout) + slot free.
	// Called from tick(); caller holds _mutex.
	void sweepSubscriptions();

	// ── DTMF SIP INFO handler (Task 2C) ──────────────────────────────────────────
	// Invoked from handle() when a SIP INFO arrives carrying
	// Content-Type: application/dtmf-relay. Parses the Signal= digit, updates the
	// per-Call-ID accumulator, and dispatches CLASS feature code actions.
	// Called from the single-threaded SIP handler path — no additional mutex needed.
	void onDtmfInfo(std::shared_ptr<SipMessage> data);

	// ── Register beep (signaling-only intercom tone) ─────────────────────────────
	// On a NEW registration, send the registering phone a brief auto-answer INVITE so
	// it plays its own intercom tone (the "beep"), then tear the call straight back
	// down. NO RTP is ever sourced — the tone is the phone's local intercom alert. The
	// outbound UAC dialog is tracked in a small bounded ring (_beepDialogs) keyed by
	// Call-ID so onOk() can drive ACK→BYE and tick() can time it out / CANCEL it. All
	// of these assume the caller already holds _mutex (non-recursive).
	//
	// State machine (per beep dialog):
	//   sendRegisterBeep() : allocate a slot, send INVITE (auto-answer headers), arm
	//                        a deadline; if no slot free, skip the beep (it's cosmetic).
	//   onOk() INVITE 200  : send ACK, then BYE, advance to AwaitingByeOk.
	//   onOk() BYE 200     : free the slot.
	//   tick() deadline    : if still AwaitingInviteOk, CANCEL and free; if
	//                        AwaitingByeOk, just free (best-effort BYE already sent).
	void sendRegisterBeep(const std::shared_ptr<SipClient>& phone);
	std::shared_ptr<SipMessage> buildBeepAck(const std::shared_ptr<SipMessage>& ok);
	std::shared_ptr<SipMessage> buildBeepBye(const std::shared_ptr<SipMessage>& ok);
	std::shared_ptr<SipMessage> buildBeepCancel(std::size_t slot);

	// AwaitingCancelDone: CANCEL sent, lingering until the 487 final response is ACKed
	// (or a bounded deadline frees the slot). Added for #90 — see beep teardown notes.
	enum class BeepState { Free, AwaitingInviteOk, AwaitingByeOk, AwaitingCancelDone };
	struct BeepDialog
	{
		BeepState state = BeepState::Free;
		std::string callID;        // fresh per beep; how onOk()/tick() find this slot
		std::string branch;        // Via branch (reused for INVITE/CANCEL)
		std::string fromTag;       // our (server) From tag
		std::string ext;           // target extension (phone number)
		sockaddr_in addr{};        // phone's contact address
		std::chrono::steady_clock::time_point deadline{};
	};
	// Bounded outbound-UAC dialog table. Tiny fixed footprint; if all slots are busy a
	// new registration just skips its beep. Guarded by _mutex.
	std::array<BeepDialog, POCKETDIAL_MAX_BEEPS> _beepDialogs;
	// Find the beep slot owning a Call-ID, or nullptr. Caller holds _mutex.
	BeepDialog* findBeepByCallID(std::string_view callID);

	// ── RFC 3261 §17 INVITE client transaction layer ──────────────────────────
	// One slot per outgoing INVITE fork.  Retransmit interval (Timer A) doubles
	// from T1=500 ms each tick until a provisional response advances the state to
	// Proceeding (no more retransmits) or Timer B (32 s) fires.
	// Pool exhaustion → message sent once with no retransmit (graceful degradation).
	// All fields guarded by _mutex.
	struct SipTransaction
	{
		enum class Type  : uint8_t { None, InviteClient };
		enum class State : uint8_t { Calling, Proceeding, Completed, Accepted };

		Type  type  = Type::None;
		State state = State::Calling;

		sockaddr_in peer{};
		char msg[1500]{};       // serialized bytes ready for retransmit (Ethernet MTU safe)
		size_t msgLen       = 0;
		bool   msgTruncated = false;

		char callId[128]{};    // Call-ID for freeTxsForCallId() lifecycle linkage
		char viaBranch[72]{};  // z9hG4bK… branch param (primary matching key)
		char cseqMethod[12]{}; // "INVITE" etc. — disambiguates CANCEL sharing the branch

		std::chrono::steady_clock::time_point nextRetransmit{};     // next Timer A fire
		std::chrono::steady_clock::time_point transactionTimeout{}; // Timer B (32 s)
		std::chrono::steady_clock::time_point absorbDeadline{};     // Timer L (RFC 6026, 2xx)

		uint32_t retransmitCount   = 0;
		uint32_t currentIntervalMs = 500; // Timer A: starts at T1, doubles each retransmit
	};
	std::array<SipTransaction, POCKETDIAL_MAX_TRANSACTIONS> _txPool{};

	// RFC 3261 §17 transaction helpers (all callers hold _mutex).
	static SipTransaction::Type classifyTxType(const std::shared_ptr<SipMessage>& msg);
	void registerTx(SipTransaction::Type type, const sockaddr_in& peer,
		const std::shared_ptr<SipMessage>& msg);
	bool matchAndAdvanceTx(const std::shared_ptr<SipMessage>& msg);
	void sweepTransactions(std::chrono::steady_clock::time_point now);
	void freeTxsForCallId(std::string_view callId);

	// RFC 4028 session timer helpers. Caller holds _mutex.
	void armSessionTimer(Session* session, const std::shared_ptr<SipMessage>& ok200);
	void sweepSessionTimers(std::chrono::steady_clock::time_point now);

	// ── Call parking / park-orbit ──────────────────────────────────────────────
	// Virtual orbit extensions 700..70(N-1). An INVITE to a FREE orbit parks the
	// caller's leg there; an INVITE to an OCCUPIED orbit retrieves it: the retriever
	// is answered with the parked party's SDP and the parked party is re-INVITEd so
	// media renegotiates peer-to-peer. tick() sweeps timed-out parks: ring back the
	// parker (Referred-By) when registered, else tear down with BYE. All helpers
	// assume the caller holds _mutex and only enqueue to _outbox.
	enum class ParkState : uint8_t { Free, Parked, RingingBack, Retrieving };
	struct ParkSlot
	{
		ParkState state = ParkState::Free;
		std::string orbit;              // "700".."70N"
		std::string callID;             // Call-ID of the parked dialog
		std::string parkedExt;          // the parked party's extension
		sockaddr_in parkedAddr{};       // the parked party's signaling address
		bool        parked = false;     // slot has a captured parked dialog
		std::string parkedSdp;          // parked party's SDP (for retrieve / ring-back offers)
		std::string parkedFromTag;      // parked party's From-tag (re-INVITE / BYE To-tag)
		std::string localTag;           // our UAS To-tag on the parked dialog
		std::string parker;             // ring-back target on timeout
		std::chrono::steady_clock::time_point parkedAt{};
		// Ring-back UAC dialog toward the parker (server-minted, fresh Call-ID).
		std::string rbCallID;
		std::string rbFromTag;
		std::string rbBranch;
		sockaddr_in rbAddr{};
		std::chrono::steady_clock::time_point deadline{};
	};
	std::array<ParkSlot, POCKETDIAL_PARK_SLOTS> _parkSlots;
	std::chrono::seconds _parkTimeout{POCKETDIAL_PARK_TIMEOUT_SEC};
	std::vector<std::string> _parkPendingAcks;    // park re-INVITE ACKs pending
	std::vector<std::string> _transferPendingAcks; // attended-transfer re-INVITE ACKs pending

	int parkOrbitIndex(std::string_view ext) const;
	void onParkInvite(std::shared_ptr<SipMessage> data,
		const std::shared_ptr<SipClient>& caller, int orbitIdx);
	void sendParkReinvite(ParkSlot& slot, const std::string& sdp);
	void byeParkedParty(const ParkSlot& slot);
	void startParkRingback(ParkSlot& slot, const std::shared_ptr<SipClient>& parker,
		std::chrono::steady_clock::time_point now);
	bool handleParkOk(const std::shared_ptr<SipMessage>& data);
	bool handleTransferOk(const std::shared_ptr<SipMessage>& data);
	void parkSweep(std::chrono::steady_clock::time_point now);
	void freeParkSlot(std::string_view callID);
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
	void buildInviteFork(const std::shared_ptr<SipMessage>& invite,
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

	std::string _serverIp;
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
		// Adopted devices (STAGE 2): {mac, ext, state, online}. Mirrored from _devices
		// under _mutex; copied out for the TUI under _snapshotMutex.
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

	// ── Registrar mode (STAGE 2) ──────────────────────────────────────────────────
	// Atomic so onRegister() can read the policy without taking _mutex (it already
	// holds _mutex via handle(), but keeping this atomic also lets getRegistrarMode()
	// be lock-free for the dashboard). Seeded from POCKETDIAL_OPEN_REGISTRAR at boot,
	// then overridden by the persisted NVS value if present.
#ifdef POCKETDIAL_OPEN_REGISTRAR
	std::atomic<RegistrarMode> _registrarMode{RegistrarMode::Open};
#else
	std::atomic<RegistrarMode> _registrarMode{RegistrarMode::Secure};
#endif
	void loadRegistrarMode();             // boot-time reload from NVS; caller holds _mutex
	void persistRegistrarMode();          // write-through after a mode change; holds _mutex

	// ── Device registry (STAGE 2: Learn-mode adoption) ────────────────────────────
	// Adopted devices keyed by 12-hex MAC. Bounded by POCKETDIAL_MAX_CLIENTS (a flood
	// of distinct MACs cannot grow the heap without limit; new keys past the cap are
	// dropped, mirroring _dnd/_forwards). Guarded by _mutex; mirrored into the
	// snapshot and persisted to NVS (namespace "pbxcfg", key "devices").
	struct DeviceRecord
	{
		std::string extension;
		DeviceState state = DeviceState::Learned;
	};
	std::unordered_map<std::string, DeviceRecord> _devices;   // mac -> record
	void loadDevices();                   // boot-time reload; caller holds _mutex
	void persistDevices();                // write-through after a mutation; holds _mutex
	void refreshDeviceSnapshot();         // mirror _devices into _snapshot; holds both mutexes

	// Learn-mode REGISTER admission. Caller holds _mutex. Resolves the source MAC,
	// applies TOFU + MAC-lock, and returns the digest decision. On a first-packet
	// ARP miss it returns Accept (deferring the lock to the next REGISTER). May set
	// `outRejectReason` when returning Reject. Records/updates the adoption entry.
	enum class AuthDecision { Accept, Challenge, Reject };
	AuthDecision admitLearn(const std::shared_ptr<SipMessage>& data,
		const std::string& ext, std::string& outRejectReason);
	// Secure-mode REGISTER admission: challenge + verify digest for `ext`. Caller
	// holds _mutex. Enqueues the 401 (with WWW-Authenticate) itself when challenging.
	AuthDecision admitSecure(const std::shared_ptr<SipMessage>& data,
		const std::string& ext, std::string& outRejectReason);
	// Emit a 401 Unauthorized with a fresh WWW-Authenticate challenge for `data`
	// into _outbox. `stale` answers an expired-but-valid nonce. Caller holds _mutex.
	void sendChallenge(const std::shared_ptr<SipMessage>& data, bool stale);
	// Emit a 403 Forbidden with a reason phrase into _outbox. Caller holds _mutex.
	void sendForbidden(const std::shared_ptr<SipMessage>& data, const std::string& reason);
	// Mark a device record online/offline in the snapshot after a (de)registration.
	// Caller holds _mutex.
	void markDeviceOnline(const std::string& mac, bool online);

	// NVS persistence for _forwards / _ringGroups / _pageZones. No-ops on host (the
	// maps are the store); on ESP they read/write the "pbxcfg" NVS namespace.
	// Caller holds _mutex.
	void loadPbxConfig();                 // boot-time reload into the maps
	void persistForwards();               // write-through after a setForward mutation
	void persistRingGroups();             // write-through after a setRingGroup mutation
	void persistPageZones();              // write-through after a setPageZone mutation
	bool _pbxConfigLoaded = false;

	// BLF event-package parsing helper (pure / static / host-testable).
	// Returns the canonical package name (e.g. "dialog") or "unknown".
	static std::string parseEventPackage(const std::string& raw);

	// RFC 4235 dialog-info XML builder (pure). Called from buildDialogNotify().
	static std::string buildDialogInfoXml(const std::string& entity, unsigned version,
		const std::string& dialogId, const std::string& state, const std::string& direction);

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
	// Default "101". Loaded from NVS namespace "pbxcfg", key "admin_ext" at boot.
	std::string _adminExt{"101"};
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
#else
		uint32_t    lastTick{0};     // monotonic ms counter on host
#endif
		static constexpr uint32_t TIMEOUT_MS = 5000;
	};
	std::unordered_map<std::string, DtmfAccum> _dtmfState;
};

#endif
