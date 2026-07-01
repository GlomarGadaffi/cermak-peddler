// Scalar reference kernels — correct, portable, and the always-on fallback.
// At <=8 narrowband ports the cost is a rounding error (~64k adds/s). Swap in
// the PIE versions only when ports x sample-rate climbs (see docs/CONFERENCE_MIXER.md §6).
#include "mix_kernels.h"
#include <cstring>

#ifndef POCKETDIAL_MIXBUS_PIE

extern "C" void mix_accumulate(int32_t* mix,
                               const int16_t frame[][MIX_FRAME],
                               const unsigned char* present, int maxPorts, int frameLen)
{
    std::memset(mix, 0, sizeof(int32_t) * static_cast<size_t>(frameLen));
    for (int p = 0; p < maxPorts; ++p)
        if (present[p])
            for (int i = 0; i < frameLen; ++i)
                mix[i] += frame[p][i];          // int16 promoted to int32 — no saturation
}

extern "C" void mix_minus_self(int16_t* out, const int32_t* mix,
                               const int16_t* self, int frameLen)
{
    for (int i = 0; i < frameLen; ++i)
    {
        int32_t v = mix[i] - static_cast<int32_t>(self[i]);
        out[i] = (v > 32767) ? 32767 : (v < -32768) ? -32768 : static_cast<int16_t>(v);
    }
}

// Portable stand-in for the .S kernel so the scalar build links and the
// small-conference path is testable without the toolchain.
extern "C" void mix_sum4_s16(int16_t* out, const int16_t* a, const int16_t* b,
                             const int16_t* c, const int16_t* d, int frameLen)
{
    for (int i = 0; i < frameLen; ++i)
    {
        int32_t v = static_cast<int32_t>(a[i]) + b[i] + c[i] + d[i];
        out[i] = (v > 32767) ? 32767 : (v < -32768) ? -32768 : static_cast<int16_t>(v);
    }
}

#endif // !POCKETDIAL_MIXBUS_PIE
