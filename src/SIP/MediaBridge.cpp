#include "MediaBridge.hpp"

MediaBridge::MediaBridge() = default;

MediaBridge::~MediaBridge()
{
	stopBridge();
}

int MediaBridge::receiverPort() const
{
	return _receiver ? _receiver->localPort() : 0;
}

void MediaBridge::init(RtpReceiver* receiver, RtpSender* sender, AnchorClient* anchor, MixBus* bus)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_receiver = receiver;
	_sender   = sender;
	_anchor   = anchor;
	_bus      = bus;
}

bool MediaBridge::startBridge(const std::string& handsetIp, uint16_t handsetPort, const std::string& callID, const std::string& participantId)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (_active.load(std::memory_order_acquire))
	{
		return false; // single active bridge allowed
	}

	// A bridge needs sockets plus SOMEWHERE to put the audio: an anchor (1:1 leg) or a
	// MixBus (conference leg). Having neither is a misconfiguration — fail the start
	// rather than answer a call whose audio goes nowhere.
	if (!_receiver || !_sender || (!_anchor && !_bus))
	{
		return false;
	}

	// BUS mode: claim a port before any socket is opened, so a full bus fails the
	// start cleanly with nothing to unwind. The tick guarantees a Free slot has empty
	// rings, so there is nothing to clear here.
	int busPortId = -1;
	if (_bus)
	{
		busPortId = _bus->attach();
		if (busPortId < 0)
		{
			return false;   // bus full — caller answers 486/503
		}
	}

	_playoutBuffer.clear();
	_callID = callID;
	_participantId = participantId;
	_busPort.store(busPortId, std::memory_order_release);
	_active.store(true, std::memory_order_release);

	// ANCHOR mode: the anchor's inbound audio is delivered through RequestsHandler's
	// single rx callback, which fans out to feedRx() on the bridge owning the
	// participant — so N bridges each receive only THEIR call's audio. This bridge no
	// longer registers the anchor callback itself (doing so per-bridge would let the
	// last bridge to start steal every call's audio).

	// Start the LAN RTP receiver (dynamic ephemeral port)
	bool receiverStarted = _receiver->start(0, [this](const uint8_t* mulaw, size_t n, uint32_t /*timestamp*/, uint16_t /*seq*/) {
		onHandsetRtp(mulaw, n);
	});

	if (!receiverStarted)
	{
		releaseBusPortLocked();
		_callID.clear();
		_participantId.clear();
		_active.store(false, std::memory_order_release);
		return false;
	}

	// Start the LAN RTP sender to stream to the handset
	bool senderStarted = _sender->start(handsetIp, handsetPort, callID, [this](uint8_t* outUlaw, size_t count) {
		return fillHandsetTx(outUlaw, count);
	});

	if (!senderStarted)
	{
		_receiver->stop();
		releaseBusPortLocked();
		_callID.clear();
		_participantId.clear();
		_active.store(false, std::memory_order_release);
		return false;
	}

	return true;
}

// ── The two media callbacks ──────────────────────────────────────────────────
// Named members rather than inline lambdas so the host suite can drive them
// directly: on host RtpReceiver/RtpSender::start() are no-op stubs that store the
// callback and never call it, so wiring covered only by the lambdas would be
// untestable off-device.

void MediaBridge::onHandsetRtp(const uint8_t* mulaw, size_t n)
{
	if (!_active.load(std::memory_order_acquire))
	{
		return;
	}

	// Decode incoming LAN handset µ-law audio to PCM16
	int16_t decoded[MAX_FRAME_SAMPLES];
	size_t toDecode = (n > MAX_FRAME_SAMPLES) ? MAX_FRAME_SAMPLES : n;
	size_t decodedCount = RtpReceiver::mulawDecodeBuffer(mulaw, toDecode, decoded);
	if (decodedCount == 0)
	{
		return;
	}

	// BUS mode: this leg's contribution to the conference. The bus's per-port input
	// ring absorbs the leg's RTP jitter; the mix tick drains exactly one frame per
	// port in lockstep (docs/CONFERENCE_MIXER.md §3).
	const int port = _busPort.load(std::memory_order_acquire);
	if (_bus && port >= 0)
	{
		_bus->inputFrame(port, decoded, decodedCount);
		return;
	}

	// ANCHOR mode: hand the PCM16 samples to the anchor
	if (_anchor)
	{
		_anchor->writeAudio(_participantId, decoded, decodedCount);
	}
}

bool MediaBridge::fillHandsetTx(uint8_t* outUlaw, size_t count)
{
	if (!_active.load(std::memory_order_acquire))
	{
		return false;
	}

	// Zero-initialized: MixBus::outputFrame() leaves the buffer UNTOUCHED when the
	// port is no longer Active (a detach racing this frame), unlike PlayoutBuffer::read()
	// which always fills. Without this we would µ-law-encode stack garbage on teardown.
	int16_t pcmSamples[MAX_FRAME_SAMPLES] = {};
	size_t toRead = (count > MAX_FRAME_SAMPLES) ? MAX_FRAME_SAMPLES : count;

	const int port = _busPort.load(std::memory_order_acquire);
	if (_bus && port >= 0)
	{
		// The saturated sum of every OTHER port — never this leg's own audio, or the
		// talker hears themselves one frame late (docs/CONFERENCE_MIXER.md §1).
		_bus->outputFrame(port, pcmSamples, toRead);
	}
	else
	{
		// Read linear PCM16 samples from the playout buffer
		_playoutBuffer.read(pcmSamples, toRead);
	}

	// Encode linear PCM16 samples to µ-law
	RtpSender::ulawEncodeBuffer(pcmSamples, toRead, outUlaw);

	// Returns true even if underrun occurred so that the RtpSender does not wipe the G.711 comfort noise samples.
	return true;
}

bool MediaBridge::feedRx(const std::string& participantId, const int16_t* samples, size_t count)
{
	// Hot path (anchor rx task). The lock is uncontended except against start/stopBridge;
	// PlayoutBuffer is itself internally synchronized for the sender's reads.
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_active.load(std::memory_order_acquire) || _participantId != participantId)
	{
		return false;
	}
	// BUS mode: refuse rather than write into a buffer nothing reads. See the header's
	// feedRx() comment — the anchor leg needs its OWN MixBus port, which is follow-up
	// work; silently swallowing the chunk here would look like working audio.
	if (_bus)
	{
		return false;
	}
	_playoutBuffer.write(samples, count);
	return true;
}

bool MediaBridge::isFor(const std::string& participantId) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _active.load(std::memory_order_acquire) && !participantId.empty() && _participantId == participantId;
}

bool MediaBridge::isForCallId(const std::string& callID) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _active.load(std::memory_order_acquire) && !callID.empty() && _callID == callID;
}

std::string MediaBridge::participantId() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _active.load(std::memory_order_acquire) ? _participantId : std::string();
}

std::string MediaBridge::callId() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _active.load(std::memory_order_acquire) ? _callID : std::string();
}

void MediaBridge::releaseBusPortLocked()
{
	const int port = _busPort.exchange(-1, std::memory_order_acq_rel);
	if (_bus && port >= 0)
	{
		// Non-blocking: the port goes Draining and the mix tick reclaims it (clears
		// both rings, publishes Free) at the next frame boundary. The tick is the sole
		// ring-clearer, so tearing this leg down never touches state a concurrent tick
		// is reading for the OTHER legs (docs/CONFERENCE_MIXER.md §5).
		_bus->detach(port);
	}
}

void MediaBridge::stopBridge()
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (!_active.load(std::memory_order_acquire))
	{
		return;
	}

	_active.store(false, std::memory_order_release);

	// Leave the bus BEFORE the sockets stop (docs/CONFERENCE_MIXER.md §7): the port is
	// marked Draining first, so any frame still in flight from the receive task is
	// rejected by inputFrame()'s Active gate instead of landing in a ring that is about
	// to be reclaimed.
	releaseBusPortLocked();

	if (_receiver)
	{
		_receiver->stop();
	}
	if (_sender)
	{
		_sender->stop(_callID);
	}
	// The anchor rx callback is owned by RequestsHandler (one for all bridges) — a bridge
	// must NOT clear it on teardown, or it would silence every other live call's inbound audio.

	_playoutBuffer.clear();
	_callID.clear();
	_participantId.clear();
}
