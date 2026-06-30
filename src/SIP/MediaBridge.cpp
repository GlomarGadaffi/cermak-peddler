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

void MediaBridge::init(RtpReceiver* receiver, RtpSender* sender, AnchorClient* anchor)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_receiver = receiver;
	_sender   = sender;
	_anchor   = anchor;
}

bool MediaBridge::startBridge(const std::string& handsetIp, uint16_t handsetPort, const std::string& callID, const std::string& participantId)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (_active.load(std::memory_order_acquire))
	{
		return false; // single active bridge allowed
	}

	if (!_receiver || !_sender || !_anchor)
	{
		return false;
	}

	_playoutBuffer.clear();
	_callID = callID;
	_participantId = participantId;
	_active.store(true, std::memory_order_release);

	// The anchor's inbound audio is delivered through RequestsHandler's single rx callback,
	// which fans out to feedRx() on the bridge owning the participant — so N bridges each
	// receive only THEIR call's audio. This bridge no longer registers the anchor callback
	// itself (doing so per-bridge would let the last bridge to start steal every call's audio).

	// Start the LAN RTP receiver (dynamic ephemeral port)
	bool receiverStarted = _receiver->start(0, [this](const uint8_t* mulaw, size_t n, uint32_t /*timestamp*/, uint16_t /*seq*/) {
		if (!_active.load(std::memory_order_acquire))
		{
			return;
		}

		// Decode incoming LAN handset µ-law audio to PCM16
		int16_t decoded[320];
		size_t toDecode = (n > 320) ? 320 : n;
		size_t decodedCount = RtpReceiver::mulawDecodeBuffer(mulaw, toDecode, decoded);

		// Hand the PCM16 samples to the anchor
		if (decodedCount > 0 && _anchor)
		{
			_anchor->writeAudio(_participantId, decoded, decodedCount);
		}
	});

	if (!receiverStarted)
	{
		_callID.clear();
		_participantId.clear();
		_active.store(false, std::memory_order_release);
		return false;
	}

	// Start the LAN RTP sender to stream to the handset
	bool senderStarted = _sender->start(handsetIp, handsetPort, callID, [this](uint8_t* outUlaw, size_t count) {
		if (!_active.load(std::memory_order_acquire))
		{
			return false;
		}

		// Read linear PCM16 samples from the playout buffer
		int16_t pcmSamples[320];
		size_t toRead = (count > 320) ? 320 : count;

		_playoutBuffer.read(pcmSamples, toRead);

		// Encode linear PCM16 samples to µ-law
		RtpSender::ulawEncodeBuffer(pcmSamples, toRead, outUlaw);

		// Returns true even if underrun occurred so that the RtpSender does not wipe the G.711 comfort noise samples.
		return true;
	});

	if (!senderStarted)
	{
		_receiver->stop();
		_callID.clear();
		_participantId.clear();
		_active.store(false, std::memory_order_release);
		return false;
	}

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

void MediaBridge::stopBridge()
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (!_active.load(std::memory_order_acquire))
	{
		return;
	}

	_active.store(false, std::memory_order_release);

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
