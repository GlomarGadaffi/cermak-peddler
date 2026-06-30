// MediaBridge_test.cpp — unit coverage for MediaBridge, the RTP<->AnchorClient
// glue. On host, RtpReceiver/RtpSender::start() are no-op stubs (no real socket
// or pacing task), so these tests exercise the parts that are real on host: the
// lifecycle/identity bookkeeping and the feedRx -> PlayoutBuffer path, including
// an end-to-end run through a real LoopbackAnchorClient wired the way
// RequestsHandler would wire any AnchorClient's single rx callback.

#include <gtest/gtest.h>
#include "MediaBridge.hpp"
#include "RtpReceiver.hpp"
#include "RtpSender.hpp"
#include "LoopbackAnchorClient.hpp"

#include <vector>

namespace
{
	struct Fixture
	{
		RtpReceiver receiver;
		RtpSender sender;
		LoopbackAnchorClient anchor;
		MediaBridge bridge;

		Fixture()
		{
			anchor.init("", "", "", "100");
			anchor.start();
			bridge.init(&receiver, &sender, &anchor);
		}
	};
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

TEST(MediaBridge, NotActiveInitially) {
	Fixture f;
	EXPECT_FALSE(f.bridge.isActive());
	EXPECT_EQ(f.bridge.participantId(), "");
	EXPECT_EQ(f.bridge.callId(), "");
}

TEST(MediaBridge, StartBridgeFailsWithoutInit) {
	RtpReceiver receiver;
	RtpSender sender;
	LoopbackAnchorClient anchor;
	MediaBridge bridge;   // init() never called -> all deps null

	EXPECT_FALSE(bridge.startBridge("127.0.0.1", 5004, "call-1", "part-1"));
}

TEST(MediaBridge, StartBridgeActivatesAndRecordsIdentity) {
	Fixture f;
	EXPECT_TRUE(f.bridge.startBridge("127.0.0.1", 5004, "call-1", "part-1"));
	EXPECT_TRUE(f.bridge.isActive());
	EXPECT_TRUE(f.bridge.isFor("part-1"));
	EXPECT_TRUE(f.bridge.isForCallId("call-1"));
	EXPECT_EQ(f.bridge.participantId(), "part-1");
	EXPECT_EQ(f.bridge.callId(), "call-1");
}

TEST(MediaBridge, IsForRejectsWrongParticipantOrCall) {
	Fixture f;
	f.bridge.startBridge("127.0.0.1", 5004, "call-1", "part-1");
	EXPECT_FALSE(f.bridge.isFor("part-2"));
	EXPECT_FALSE(f.bridge.isForCallId("call-2"));
}

TEST(MediaBridge, SecondStartBridgeFailsWhileActive) {
	Fixture f;
	EXPECT_TRUE(f.bridge.startBridge("127.0.0.1", 5004, "call-1", "part-1"));
	EXPECT_FALSE(f.bridge.startBridge("127.0.0.1", 5005, "call-2", "part-2"));
	// The original bridge is untouched by the rejected second start.
	EXPECT_TRUE(f.bridge.isFor("part-1"));
}

TEST(MediaBridge, StopBridgeClearsState) {
	Fixture f;
	f.bridge.startBridge("127.0.0.1", 5004, "call-1", "part-1");
	f.bridge.stopBridge();
	EXPECT_FALSE(f.bridge.isActive());
	EXPECT_EQ(f.bridge.participantId(), "");
	EXPECT_EQ(f.bridge.callId(), "");
}

TEST(MediaBridge, StopBridgeIsIdempotentWhenIdle) {
	Fixture f;
	f.bridge.stopBridge();   // never started
	EXPECT_FALSE(f.bridge.isActive());
}

// ── feedRx -> PlayoutBuffer ────────────────────────────────────────────────────

TEST(MediaBridge, FeedRxRejectedWhileIdle) {
	Fixture f;
	const int16_t samples[] = {1, 2, 3};
	EXPECT_FALSE(f.bridge.feedRx("part-1", samples, 3));
}

TEST(MediaBridge, FeedRxRejectedForWrongParticipant) {
	Fixture f;
	f.bridge.startBridge("127.0.0.1", 5004, "call-1", "part-1");
	const int16_t samples[] = {1, 2, 3};
	EXPECT_FALSE(f.bridge.feedRx("someone-else", samples, 3));
}

TEST(MediaBridge, FeedRxWritesIntoPlayoutBuffer) {
	Fixture f;
	f.bridge.startBridge("127.0.0.1", 5004, "call-1", "part-1");

	const int16_t in[] = {111, 222, 333};
	EXPECT_TRUE(f.bridge.feedRx("part-1", in, 3));
	EXPECT_EQ(f.bridge.getPlayoutBuffer().getLength(), 3u);

	int16_t out[3] = {};
	EXPECT_TRUE(f.bridge.getPlayoutBuffer().read(out, 3));
	EXPECT_EQ(out[0], 111);
	EXPECT_EQ(out[1], 222);
	EXPECT_EQ(out[2], 333);
}

// ── End-to-end through a real AnchorClient ────────────────────────────────────
// Mirrors the wiring a caller (e.g. RequestsHandler) would do: register the
// anchor's single rx callback once, fan out to feedRx() on the bridge owning
// the participant. Proves the AnchorClient -> MediaBridge -> PlayoutBuffer
// chain end-to-end without any real socket.

TEST(MediaBridge, AnchorAudioReachesPlayoutBufferThroughRxFanout) {
	Fixture f;
	f.bridge.startBridge("127.0.0.1", 5004, "call-1", "part-1");

	f.anchor.registerAudioRxCallback([&f](const std::string& participantId,
	                                       const int16_t* samples, size_t count) {
		if (f.bridge.isFor(participantId))
		{
			f.bridge.feedRx(participantId, samples, count);
		}
	});

	const int16_t input[] = {10, 20, 30, 40};
	// LoopbackAnchorClient::writeAudio echoes synchronously through the
	// registered AudioRxCallback (see AnchorClient_test.cpp).
	EXPECT_TRUE(f.anchor.writeAudio("part-1", input, 4));

	EXPECT_EQ(f.bridge.getPlayoutBuffer().getLength(), 4u);
	int16_t out[4] = {};
	EXPECT_TRUE(f.bridge.getPlayoutBuffer().read(out, 4));
	std::vector<int16_t> got(out, out + 4);
	EXPECT_EQ(got, (std::vector<int16_t>{10, 20, 30, 40}));
}
