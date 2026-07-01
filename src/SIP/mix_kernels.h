#pragma once
#include <cstdint>
#include <cstddef>

// Two leaf kernels behind fixed signatures. The scalar bodies (mix_kernels_scalar.cpp)
// are correct and ship today. Define POCKETDIAL_MIXBUS_PIE to swap in the hand-written
// ESP32-S3 vector versions. See docs/CONFERENCE_MIXER.md §6 for the vector path and its
// verification caveats. Each vector kernel lives in its OWN .S file (the S3
// one-function-per-.S-file gotcha).

#ifndef MIX_FRAME
#define MIX_FRAME 160
#endif

#ifdef __cplusplus
extern "C" {
#endif

// (A) mix[i] = SUM over present ports of frame[p][i].  Accumulate in int32; do NOT saturate.
void mix_accumulate(int32_t* mix,
                    const int16_t frame[][MIX_FRAME],
                    const unsigned char* present, int maxPorts, int frameLen);

// (B) out[i] = sat16( mix[i] - self[i] ).  Saturate EXACTLY ONCE, here.
void mix_minus_self(int16_t* out, const int32_t* mix, const int16_t* self, int frameLen);

// Confirmed-ISA vector primitive (pie/mix_sum4_s16.S): out[i] = sat16(a+b+c+d).
// Used directly for small (<=5-way) conferences — no int32, no minus-self. See §6.
void mix_sum4_s16(int16_t* out, const int16_t* a, const int16_t* b,
                  const int16_t* c, const int16_t* d, int frameLen);

#ifdef __cplusplus
}
#endif
