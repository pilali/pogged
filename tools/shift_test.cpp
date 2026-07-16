// shift_test — verifies each octave voice actually produces its target pitch.
//
// A sustained 220 Hz sine runs through the core with a single voice raised
// (dry muted). Goertzel bins at the target frequency vs the input frequency
// must show the target dominating by >= 10 dB. The sub voices additionally
// get a splice-tremolo check: the windowed-RMS ripple over the steady region
// must stay <= 6 dB.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float SR    = 48000.0f;
static constexpr float FIN   = 220.0f;
static constexpr int   NSEC  = 3;
static constexpr int   N     = (int)SR * NSEC;
static constexpr int   BLOCK = 256;

static double goertzel_db(const std::vector<float>& x, int from, int to, float f)
{
    const double w  = 2.0 * M_PI * f / SR;
    const double cw = 2.0 * std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = from; i < to; ++i) {
        s0 = x[i] + cw * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - cw * s1 * s2;
    return 10.0 * std::log10(p + 1e-30);
}

static bool run_voice(const char* name, int voice_idx, float target_hz,
                      bool check_ripple)
{
    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.out_level   = 1.0f;
    p.input_gain  = 1.0f;   // 0 would clamp to 0.5: a silent 6 dB cut
    p.lp_cutoff   = 20000.0f;
    p.lp_q        = 0.707f;
    p.attack_sens = 0.35f;
    float* levels[5] = { &p.sub1_level, &p.sub2_level, &p.up1_level,
                         &p.up2_level, &p.up5_level };
    *levels[voice_idx] = 1.0f;

    // Pans default to centre, where the pedal pan law is unity on both
    // channels — so L is exactly what the mono core used to emit.
    std::vector<float> in(N), out(N), out_r(N);
    for (int i = 0; i < N; ++i)
        in[i] = 0.5f * std::sin(2.0 * M_PI * FIN * i / SR);
    for (int i = 0; i < N; i += BLOCK)
        pogged_dsp_process(dsp, &p, in.data() + i, out.data() + i,
                           out_r.data() + i, std::min(BLOCK, N - i));
    pogged_dsp_free(dsp);

    const int from = (int)SR, to = N;               // skip 1 s of settling
    const double tgt_db = goertzel_db(out, from, to, target_hz);
    const double inp_db = goertzel_db(out, from, to, FIN);
    const double sep    = tgt_db - inp_db;
    bool ok = sep >= 10.0;

    double ripple_db = 0.0;
    if (check_ripple) {
        const int win = (int)(0.030f * SR), hop = (int)(0.010f * SR);
        double rmin = 1e30, rmax = 0.0;
        for (int i = from; i + win < to; i += hop) {
            double e = 0.0;
            for (int j = 0; j < win; ++j) e += (double)out[i+j] * out[i+j];
            const double r = std::sqrt(e / win);
            rmin = std::min(rmin, r);
            rmax = std::max(rmax, r);
        }
        ripple_db = 20.0 * std::log10(rmax / (rmin + 1e-30));
        ok = ok && ripple_db <= 6.0;
    }

    if (check_ripple)
        std::printf("  %-10s target %6.1f Hz: sep %+6.1f dB (>= +10), "
                    "ripple %.1f dB (<= 6)\n", name, target_hz, sep, ripple_db);
    else
        std::printf("  %-10s target %6.1f Hz: sep %+6.1f dB (>= +10)\n",
                    name, target_hz, sep);
    return ok;
}

int main()
{
    bool ok = true;
    ok &= run_voice("sub1 (-1)",  0, FIN * 0.5f,  true);
    ok &= run_voice("sub2 (-2)",  1, FIN * 0.25f, true);
    ok &= run_voice("up1  (+1)",  2, FIN * 2.0f,  false);
    ok &= run_voice("up2  (+2)",  3, FIN * 4.0f,  false);
    // +5th: equal-tempered, 2^(7/12) — see FIFTH_RATIO in pogged_dsp.cpp.
    ok &= run_voice("up5  (+5th)", 4, FIN * 1.4983071f, false);
    std::printf("shift_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
