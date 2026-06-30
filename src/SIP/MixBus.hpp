#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "PlayoutBuffer.hpp"   // pocket-dial's existing ring (src/SIP/PlayoutBuffer.hpp)

// ── Conference mix bus ───────────────────────────────────────────────────────
// The summing junction. Sits between the decode edge (RtpReceiver / anchor rx)
// and the encode edge (RtpSender / anchor tx). Every active port hears the sum
// of all OTHER ports ("minus-self"). The full mix is held in int32 and clipped
// EXACTLY ONCE, on the way out — the running sum is never saturated.
//
// Lifecycle is a per-port state machine driven lock-free against the mix tick:
//   Free --attach()--> Active --detach()--> Draining --(tick reclaims)--> Free
// The tick is the SOLE owner of ring teardown, so a leg leaving never corrupts
// or silences the others (the same single-active-bridge discipline MediaBridge
// already keeps for one leg, generalized here to N).
class MixBus
{
public:
    static constexpr int FRAME     = 160;  // samples/tick; MUST equal RTP ptime (20 ms @ 8 kHz)
    static constexpr int MAX_PORTS = 8;

    MixBus() = default;

    // Cold path (SIP signaling threads).
    int  attach();                 // -> portId in [0,MAX_PORTS), or -1 if full
    void detach(int port);         // non-blocking; tick reclaims at next boundary

    // Hot path (decode / encode tasks). Per-port jitter-absorbing rings.
    bool inputFrame (int port, const int16_t* pcm, size_t n);  // leg -> bus
    bool outputFrame(int port,       int16_t* pcm, size_t n);  // bus -> leg

    // Master clock. Call from exactly ONE periodic driver, every FRAME samples.
    void tick();

    int activePorts() const;

private:
    enum class State : uint8_t { Free = 0, Active = 1, Draining = 2 };

    struct Port
    {
        std::atomic<State> state{State::Free};
        PlayoutBuffer      in;     // leg -> bus  (the input direction a 1:1 MediaBridge doesn't need)
        PlayoutBuffer      out;    // bus -> leg  (same role as MediaBridge's playout buffer)
    };

    Port    _ports[MAX_PORTS];
    int32_t _mix[FRAME] = {};      // wide accumulator — clipped once, at output
};
