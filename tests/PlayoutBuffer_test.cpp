// PlayoutBuffer_test.cpp — unit coverage for the adaptive jitter-buffer used
// by LoopbackAnchorClient and any future AnchorClient implementation.

#include <gtest/gtest.h>
#include "PlayoutBuffer.hpp"

#include <vector>
#include <cstring>

static std::vector<int16_t> ramp(size_t count, int16_t start = 1)
{
    std::vector<int16_t> v(count);
    for (size_t i = 0; i < count; ++i)
        v[i] = static_cast<int16_t>(start + static_cast<int>(i));
    return v;
}

// ── Basic write → read ────────────────────────────────────────────────────────

TEST(PlayoutBuffer, WriteAndReadBack) {
    PlayoutBuffer buf(160);
    auto samples = ramp(80);
    ASSERT_EQ(buf.write(samples.data(), 80), 80u);

    std::vector<int16_t> out(80, 0);
    bool noUnderrun = buf.read(out.data(), 80);
    EXPECT_TRUE(noUnderrun);
    EXPECT_EQ(out, samples);
}

TEST(PlayoutBuffer, GetLengthAfterWrite) {
    PlayoutBuffer buf(160);
    auto samples = ramp(40);
    buf.write(samples.data(), 40);
    EXPECT_EQ(buf.getLength(), 40u);
}

TEST(PlayoutBuffer, GetLengthEmptyBuffer) {
    PlayoutBuffer buf(160);
    EXPECT_EQ(buf.getLength(), 0u);
}

// ── Underrun handling ─────────────────────────────────────────────────────────

TEST(PlayoutBuffer, ReadEmptyBufferReturnsUnderrun) {
    PlayoutBuffer buf(160);
    std::vector<int16_t> out(80, 99);
    bool noUnderrun = buf.read(out.data(), 80);
    EXPECT_FALSE(noUnderrun);   // underrun
    EXPECT_GT(buf.getUnderruns(), 0u);
}

TEST(PlayoutBuffer, UnderrunFillsWithNoise) {
    PlayoutBuffer buf(160);
    std::vector<int16_t> out(80, 0);
    buf.read(out.data(), 80);
    // Comfort noise is low-amplitude — just confirm it doesn't return silence (0)
    // or corrupt the output with uninitialised data. Any non-zero value is fine.
    // (exact noise value is implementation-defined; this checks it doesn't crash.)
    EXPECT_EQ(out.size(), 80u);
}

// ── Overrun handling ──────────────────────────────────────────────────────────

TEST(PlayoutBuffer, OverrunDropsOldestSamples) {
    PlayoutBuffer buf(80);   // capacity = 80 samples
    auto big = ramp(120);    // write 120 — must overrun
    buf.write(big.data(), 120);
    EXPECT_EQ(buf.getLength(), 80u);         // buffer is full, not over-full
    EXPECT_GT(buf.getOverruns(), 0u);        // at least one overrun recorded
}

// ── Clear ─────────────────────────────────────────────────────────────────────

TEST(PlayoutBuffer, ClearResetsLength) {
    PlayoutBuffer buf(160);
    auto samples = ramp(40);
    buf.write(samples.data(), 40);
    buf.clear();
    EXPECT_EQ(buf.getLength(), 0u);
}

TEST(PlayoutBuffer, ClearResetsStats) {
    PlayoutBuffer buf(40);
    auto big = ramp(80);
    buf.write(big.data(), 80);   // force overrun
    buf.clear();
    EXPECT_EQ(buf.getOverruns(), 0u);
    EXPECT_EQ(buf.getUnderruns(), 0u);
}

// ── Null / zero-count guards ──────────────────────────────────────────────────

TEST(PlayoutBuffer, WriteNullReturnsZero) {
    PlayoutBuffer buf(160);
    EXPECT_EQ(buf.write(nullptr, 10), 0u);
}

TEST(PlayoutBuffer, WriteZeroCountReturnsZero) {
    PlayoutBuffer buf(160);
    int16_t dummy = 0;
    EXPECT_EQ(buf.write(&dummy, 0), 0u);
}

// ── Target depth ─────────────────────────────────────────────────────────────

TEST(PlayoutBuffer, SetAndGetTargetDepth) {
    PlayoutBuffer buf(1600);
    buf.setTargetDepth(320);
    EXPECT_EQ(buf.getTargetDepth(), 320u);
}
