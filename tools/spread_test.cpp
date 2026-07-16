// spread_test — verifies the POG3 SPREAD stereo delay.
//
// The manual: SPREAD "introduces a short delay line on the +5th, +1, and +2
// voices... The Right channel delay time is 3x longer than the Left delay time
// producing a wide stereo spectrum. Right delay time maximum is 150ms while
// the Left maximum is 50ms." and "The two suboctave voices do not go through
// the Spread effect."
//
// Both halves of that are checked:
//   1. on the +1 voice, cross-correlating L against R finds its peak at
//      exactly (150 - 50) = 100 ms of lag at full spread, and at 0 with spread
//      off (where the two channels must be bit-identical);
//   2. on the sub -1 voice, spread changes nothing at all.
//
// Noise input, so the correlation peak is unambiguous — a sine would correlate
// at every period and prove nothing.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static constexpr float SR    = 48000.0f;
static constexpr int   N     = (int)SR * 3;
static constexpr int   BLOCK = 256;

static void render(bool sub_voice, float spread,
                   std::vector<float>& l, std::vector<float>& r)
{
    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.out_level   = 1.0f;
    p.input_gain  = 1.0f;   // 0 would clamp to 0.5: a silent 6 dB cut
    p.lp_cutoff   = 20000.0f;
    p.lp_q        = 0.707f;
    p.attack_sens = 0.35f;
    p.spread      = spread;
    if (sub_voice) p.sub1_level = 1.0f; else p.up1_level = 1.0f;

    // Deterministic noise: a fixed LCG, so the test is reproducible.
    std::vector<float> in(N);
    uint32_t seed = 12345u;
    for (int i = 0; i < N; ++i) {
        seed = seed * 1664525u + 1013904223u;
        in[i] = 0.4f * ((float)(seed >> 8) / 8388608.0f - 1.0f);
    }
    l.assign(N, 0.0f); r.assign(N, 0.0f);
    for (int i = 0; i < N; i += BLOCK)
        pogged_dsp_process(dsp, &p, in.data() + i, l.data() + i, r.data() + i,
                           std::min(BLOCK, N - i));
    pogged_dsp_free(dsp);
}

// Lag (in samples) of R relative to L that maximises correlation.
static int best_lag(const std::vector<float>& l, const std::vector<float>& r,
                    int from, int to, int max_lag)
{
    int    best_d = -1;
    double best_c = -1e30;
    for (int d = 0; d <= max_lag; ++d) {
        double c = 0.0;
        for (int i = from; i < to; i += 4)          // stride: plenty of samples
            c += (double)l[i] * r[i + d];
        if (c > best_c) { best_c = c; best_d = d; }
    }
    return best_d;
}

int main()
{
    std::printf("══ spread ══\n");
    bool ok = true;
    std::vector<float> l, r;

    // 1. Spread off: the two channels must be identical (delay 0 reads the
    //    sample just written, so the delay line is transparent).
    render(false, 0.0f, l, r);
    double maxdiff = 0.0;
    for (int i = 0; i < N; ++i) maxdiff = std::max(maxdiff, (double)std::abs(l[i] - r[i]));
    const bool off_ok = maxdiff == 0.0;
    std::printf("  spread=0  : |L-R| max %.1e (must be 0 -> transparent)\n", maxdiff);
    ok &= off_ok;

    // 2. Full spread on +1: R lags L by (150-50) = 100 ms = 4800 samples.
    render(false, 1.0f, l, r);
    const int want = (int)(0.100f * SR);
    // Search window starts after the delay lines have filled and the smoothed
    // spread has settled (~30 ms one-pole; 1 s is ample).
    const int lag = best_lag(l, r, (int)SR, N - (int)(0.2f * SR), (int)(0.2f * SR));
    const bool lag_ok = std::abs(lag - want) <= (int)(0.002f * SR);   // ±2 ms
    std::printf("  spread=1  : +1 voice R lags L by %d samples (%.1f ms), "
                "want %d (%.0f ms) ±2 ms\n", lag, 1000.0 * lag / SR, want,
                1000.0 * want / SR);
    ok &= lag_ok;

    // 3. Suboctaves are excluded: spread must not touch them.
    std::vector<float> l0, r0;
    render(true, 0.0f, l0, r0);
    render(true, 1.0f, l, r);
    double subdiff = 0.0;
    for (int i = 0; i < N; ++i) subdiff = std::max(subdiff, (double)std::abs(l0[i] - l[i]));
    const bool sub_ok = subdiff == 0.0;
    std::printf("  spread=0/1: sub -1 voice unchanged, max delta %.1e (must be 0 "
                "-> suboctaves excluded)\n", subdiff);
    ok &= sub_ok;

    std::printf("spread_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
