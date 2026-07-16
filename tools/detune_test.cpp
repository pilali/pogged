// detune_test — verifies the detune is a chorus (moving), not a fixed offset.
//
// The POG2 manual describes DETUNE as raising "both the depth and rate of
// detune", i.e. an LFO. A static offset would sound like fixed, lifeless
// beating, so this pins down that the detuned voice actually sweeps.
//
// A sustained 220 Hz sine runs with only the +1 voice up (dry muted) and
// detune at max. That voice is a 50/50 mix of a main tap parked at 440 Hz and
// a detuned tap. The tell is *where the detuned energy sits*:
//
//   * a STATIC +25 cent detune parks the second tap at 446.4 Hz and it never
//     visits the flat side -> energy piles up sharp (measured: +16.3 dB).
//   * an LFO sweeps symmetrically across 433.7-446.4 Hz -> no sharp-side pile
//     (measured: -12.7 dB, and FM sidebands appear at exactly the 3 Hz LFO
//     rate: 437, 434, 431 Hz).
//
// The two behaviours are 29 dB apart, so asserting "energy is not concentrated
// sharp" fails loudly if the LFO ever regresses to a static offset, with wide
// margin on both sides.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float SR    = 48000.0f;
static constexpr float FIN   = 220.0f;
static constexpr int   N     = (int)SR * 3;
static constexpr int   BLOCK = 256;

// Coherent single-bin energy over [from, to).
static double goertzel_db(const std::vector<float>& x, int from, int to, double f)
{
    const double w  = 2.0 * M_PI * f / SR;
    const double cw = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (int i = from; i < to; ++i) {
        const double s0 = x[i] + cw * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - cw * s1 * s2;
    return 10.0 * std::log10(p + 1e-30);
}

int main()
{
    std::printf("══ detune movement ══\n");

    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.up1_level    = 1.0f;         // dry stays at 0: the +1 voice in isolation
    p.out_level    = 1.0f;
    p.lp_cutoff    = 20000.0f;
    p.lp_q         = 0.707f;
    p.attack_sens  = 0.35f;
    p.detune_cents = 25.0f;        // max: depth 25 cents, LFO rate 3 Hz

    std::vector<float> in(N), out(N), out_r(N);   // pans centred: L == R
    for (int i = 0; i < N; ++i)
        in[i] = 0.5f * std::sin(2.0 * M_PI * FIN * i / SR);
    for (int i = 0; i < N; i += BLOCK)
        pogged_dsp_process(dsp, &p, in.data() + i, out.data() + i,
                           out_r.data() + i, std::min(BLOCK, N - i));
    pogged_dsp_free(dsp);

    const int from = (int)SR, to = N;                   // skip 1 s of settling
    const double f_main  = FIN * 2.0;                            // 440.0 Hz
    const double f_sharp = f_main * std::pow(2.0,  25.0 / 1200.0);  // 446.4
    const double f_flat  = f_main * std::pow(2.0, -25.0 / 1200.0);  // 433.7

    const double sharp_db = goertzel_db(out, from, to, f_sharp);
    const double flat_db  = goertzel_db(out, from, to, f_flat);
    const double asym     = sharp_db - flat_db;

    const bool ok = asym <= 3.0;
    std::printf("  +1 detuned: sharp %.1f Hz %.1f dB vs flat %.1f Hz %.1f dB\n",
                f_sharp, sharp_db, f_flat, flat_db);
    std::printf("  sharp-side concentration %+.1f dB (<= +3 -> sweeping, "
                "not parked)\n", asym);
    std::printf("detune_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
