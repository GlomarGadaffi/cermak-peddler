// MixBus_test.cpp — verifies the two correctness-critical properties of the
// conference mix bus: (1) minus-self (a talker never hears itself), and
// (2) the running mix is NOT pre-saturated (loud legs don't cancel via
// clipping). Also covers the attach/detach/reclaim port lifecycle and the
// small-conference mix_sum4_s16 primitive. See docs/CONFERENCE_MIXER.md.

#include <gtest/gtest.h>
#include "MixBus.hpp"
#include "mix_kernels.h"

#include <vector>

namespace
{
	std::vector<int16_t> constFrame(int16_t v)
	{
		return std::vector<int16_t>(MixBus::FRAME, v);
	}
}

TEST(MixBus, MinusSelfMixesCorrectly) {
	MixBus bus;

	// Three legs.
	int A = bus.attach();
	int B = bus.attach();
	int C = bus.attach();
	ASSERT_EQ(A, 0);
	ASSERT_EQ(B, 1);
	ASSERT_EQ(C, 2);
	ASSERT_EQ(bus.activePorts(), 3);

	// Each leg pushes a DC frame: A=+1000, B=+2000, C=+4000.
	auto a = constFrame(1000), b = constFrame(2000), c = constFrame(4000);
	bus.inputFrame(A, a.data(), a.size());
	bus.inputFrame(B, b.data(), b.size());
	bus.inputFrame(C, c.data(), c.size());

	bus.tick();   // mix = 7000; A->6000, B->5000, C->3000

	int16_t oa[MixBus::FRAME], ob[MixBus::FRAME], oc[MixBus::FRAME];
	EXPECT_TRUE(bus.outputFrame(A, oa, MixBus::FRAME));
	EXPECT_TRUE(bus.outputFrame(B, ob, MixBus::FRAME));
	EXPECT_TRUE(bus.outputFrame(C, oc, MixBus::FRAME));
	EXPECT_EQ(oa[0], 6000);
	EXPECT_EQ(ob[0], 5000);
	EXPECT_EQ(oc[0], 3000);
}

TEST(MixBus, NoOverSaturationOnLoudLegs) {
	// Four loud legs at +30000. Naive int16 mix would clip the running sum to
	// 32767, then a leg would hear 32767-30000=2767 (WRONG). Correct int32 mix
	// = 120000; a leg hears 120000-30000 = 90000 -> sat16 -> 32767.
	MixBus bus;
	int p0 = bus.attach(), p1 = bus.attach(), p2 = bus.attach(), p3 = bus.attach();
	auto loud = constFrame(30000);
	bus.inputFrame(p0, loud.data(), loud.size());
	bus.inputFrame(p1, loud.data(), loud.size());
	bus.inputFrame(p2, loud.data(), loud.size());
	bus.inputFrame(p3, loud.data(), loud.size());
	bus.tick();

	int16_t o0[MixBus::FRAME];
	EXPECT_TRUE(bus.outputFrame(p0, o0, MixBus::FRAME));
	EXPECT_EQ(o0[0], 32767);
}

TEST(MixBus, DetachReclaimsSlotOnNextTick) {
	MixBus bus;
	int A = bus.attach();
	int B = bus.attach();
	(void)A;

	bus.detach(B);
	EXPECT_EQ(bus.activePorts(), 1);   // B is Draining, not Active, immediately

	bus.tick();                        // reclaims B -> Free
	EXPECT_EQ(bus.activePorts(), 1);

	int D = bus.attach();              // should reuse the freed slot index
	EXPECT_EQ(D, B);
}

TEST(MixBus, AttachFailsWhenFull) {
	MixBus bus;
	for (int i = 0; i < MixBus::MAX_PORTS; ++i)
	{
		EXPECT_GE(bus.attach(), 0);
	}
	EXPECT_EQ(bus.attach(), -1);
}

TEST(MixBus, InputOutputFrameRejectInactivePort) {
	MixBus bus;
	int16_t buf[MixBus::FRAME] = {};
	EXPECT_FALSE(bus.inputFrame(0, buf, MixBus::FRAME));   // never attached
	EXPECT_FALSE(bus.outputFrame(0, buf, MixBus::FRAME));
}

TEST(MixBus, LateLegContributesSilenceNotStaleAudio) {
	MixBus bus;
	int A = bus.attach();
	int B = bus.attach();

	auto a = constFrame(1000);
	bus.inputFrame(A, a.data(), a.size());
	// B never pushes a frame this tick.
	bus.tick();

	int16_t oa[MixBus::FRAME];
	ASSERT_TRUE(bus.outputFrame(A, oa, MixBus::FRAME));
	EXPECT_EQ(oa[0], 0);   // A hears mix-minus-self = (1000 + 0) - 1000 = 0
}

// ── Small-conference confirmed-ISA primitive ──────────────────────────────────

TEST(MixBus, MixSum4S16AddsFourLegs) {
	auto a = constFrame(1000), b = constFrame(2000), c = constFrame(4000), z = constFrame(0);
	int16_t out[MixBus::FRAME];
	mix_sum4_s16(out, a.data(), b.data(), c.data(), z.data(), MixBus::FRAME);
	EXPECT_EQ(out[0], 7000);   // 1000+2000+4000+0
}

TEST(MixBus, MixSum4S16Saturates) {
	auto loud = constFrame(30000);
	int16_t out[MixBus::FRAME];
	mix_sum4_s16(out, loud.data(), loud.data(), loud.data(), loud.data(), MixBus::FRAME);
	EXPECT_EQ(out[0], 32767);   // 120000 saturates to int16 max
}
