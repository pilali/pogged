// swell_test — verifies the POG2-style attack behaviour:
//   1. after an onset the wet reaches 90 % of its steady level in roughly
//      attack_ms (the Envelope's time constant is defined as 90 % coverage),
//   2. a re-pick after a pause ducks the wet and re-swells it.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

static constexpr float SR    = 48000.0f;
static constexpr int   BLOCK = 256;

static std::vector<float> render(float attack_ms, const std::vector<float>& in)
{
    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.up1_level   = 1.0f;
    p.out_level   = 1.0f;
    p.input_gain  = 1.0f;   // 0 would clamp to 0.5: a silent 6 dB cut
    p.lp_cutoff   = 20000.0f;
    p.lp_q        = 0.707f;
    p.attack_ms   = attack_ms;
    p.attack_sens = 0.35f;

    const int n = (int)in.size();
    std::vector<float> out(n), out_r(n);          // pans centred: L == R
    for (int i = 0; i < n; i += BLOCK)
        pogged_dsp_process(dsp, &p, in.data() + i, out.data() + i, out_r.data() + i,
                           std::min(BLOCK, n - i));
    pogged_dsp_free(dsp);
    return out;
}

static double rms(const std::vector<float>& x, int from, int len)
{
    double e = 0.0;
    for (int i = from; i < from + len; ++i) e += (double)x[i] * x[i];
    return std::sqrt(e / len);
}

int main()
{
    const float ATK = 500.0f;                    // ms
    const int   sil0  = (int)(0.5f * SR);        // 0.5 s silence
    const int   tone1 = (int)(2.0f * SR);        // 2.0 s tone
    const int   gap   = (int)(0.4f * SR);        // 0.4 s pause
    const int   tone2 = (int)(1.5f * SR);        // 1.5 s tone (re-pick)
    const int   n     = sil0 + tone1 + gap + tone2;

    std::vector<float> in(n, 0.0f);
    for (int i = 0; i < tone1; ++i)
        in[sil0 + i] = 0.5f * std::sin(2.0 * M_PI * 220.0 * i / SR);
    for (int i = 0; i < tone2; ++i)
        in[sil0 + tone1 + gap + i] = 0.5f * std::sin(2.0 * M_PI * 220.0 * i / SR);

    const auto out = render(ATK, in);

    const int win = (int)(0.030f * SR);
    // Steady wet level: late in the first tone.
    const double steady = rms(out, sil0 + tone1 - 10 * win, 10 * win);

    // 1. Time to reach 90 % of steady after the first onset.
    int t90 = -1;
    for (int i = sil0; i < sil0 + tone1 - win; i += win / 4) {
        if (rms(out, i, win) >= 0.9 * steady) { t90 = i - sil0; break; }
    }
    const double t90_ms = 1000.0 * t90 / SR;
    // Envelope covers 90 % in attack_ms; allow detector latency + window
    // granularity on top, and fail if it's way too fast (no swell at all).
    const bool ok_atk = (t90 > 0) && t90_ms > 0.4 * ATK && t90_ms < 1.8 * ATK;

    // 2. Re-pick: right after the second onset the wet must be well below
    // steady (ducked/restarted), then recover.
    const int rp = sil0 + tone1 + gap;
    const double early2 = rms(out, rp + (int)(0.020f * SR), win);
    const double late2  = rms(out, rp + tone2 - 10 * win, 5 * win);
    const bool ok_rp = early2 < 0.5 * steady && late2 > 0.7 * steady;

    std::printf("  attack %g ms -> 90%% in %.0f ms (0.4-1.8x)%s\n",
                ATK, t90_ms, ok_atk ? "" : "  ** FAIL");
    std::printf("  re-pick: early %.3f / steady %.3f (< 0.5), late %.3f (> 0.7)%s\n",
                early2, steady, late2, ok_rp ? "" : "  ** FAIL");

    const bool ok = ok_atk && ok_rp;
    std::printf("swell_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
