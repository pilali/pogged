// reinject_test — transient reinjection (§18): the wet's FELT latency.
//
// The pitched body of the vocoder path cannot arrive before its window
// allows (85-171 ms, §16-17); what CAN arrive at once is the pick's
// broadband snap. On each onset a short high-passed burst of the input is
// summed into the wet bus. Three things are pinned:
//   1. a wet-only preset ATTACKS within ~20 ms of the pick (the burst),
//      while the tonal body still blooms at its own pace (reported);
//   2. with ATTACK engaged the burst is gated off — a click would defeat a
//      deliberate swell;
//   3. with the dry present the burst is gated off — the dry already IS the
//      zero-latency attack, and doubling it would harden every pick.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

static constexpr float SR    = 48000.0f;
static constexpr int   BLOCK = 256;
static constexpr float T_ON  = 0.5f;      // pick lands here
static constexpr float T_END = 3.0f;

// A pick: tone with a fast rise + a short broadband click, like a plectrum.
static std::vector<float> make_pick()
{
    const int n = (int)(T_END * SR), on = (int)(T_ON * SR);
    std::vector<float> in(n, 0.0f);
    unsigned seed = 20260717;
    for (int i = on; i < n; ++i) {
        const float t = (i - on) / SR;
        in[i] = 0.4f * (1.0f - std::exp(-t / 0.002f))
                     * std::sin(2.0f * float(M_PI) * 220.0f * t);
        if (t < 0.006f) {
            seed = seed * 1664525u + 1013904223u;
            in[i] += 0.35f * std::exp(-t / 0.003f)
                           * (((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f);
        }
    }
    return in;
}

static std::vector<float> render(const std::vector<float>& in,
                                 float dry, float atk_ms)
{
    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.dry_level   = dry;
    p.up1_level   = 1.0f;
    p.out_level   = 0.4f;      // below the clipper knee (§14)
    p.input_gain  = 1.0f;
    p.lp_cutoff   = 20000.0f;
    p.lp_q        = 0.707f;
    p.attack_ms   = atk_ms;
    p.attack_sens = 0.5f;
    p.focus       = 1.0f;      // vocoder: the engine whose body is late

    const int n = (int)in.size();
    std::vector<float> out(n), out_r(n);
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
    const auto in = make_pick();
    const int  on = (int)(T_ON * SR), n = (int)in.size();
    const int  w5 = (int)(0.005f * SR);
    bool ok = true;

    std::printf("══ transient reinjection (§18) ══\n");

    // 1. Wet-only: arrival = first 5 ms window over 0.15x the steady level.
    {
        const auto out = render(in, 0.0f, 0.0f);
        const double steady = rms(out, n - (int)(0.5f * SR), (int)(0.4f * SR));
        int arrive = -1;
        for (int i = on; i + w5 < n; i += (int)(0.001f * SR))
            if (rms(out, i, w5) > 0.15 * steady) { arrive = i - on; break; }
        const double ms = 1000.0 * arrive / SR;
        const bool this_ok = arrive >= 0 && ms < 20.0;
        ok &= this_ok;
        // The tonal body's own arrival, for contrast (report-only).
        int body = -1;
        for (int i = on + (int)(0.030f * SR); i + w5 < n; i += (int)(0.002f * SR))
            if (rms(out, i, w5) > 0.5 * steady) { body = i - on; break; }
        std::printf("  wet-only: attack heard at %.1f ms (< 20)%s"
                    "   [tonal body at ~%.0f ms]\n",
                    ms, this_ok ? "  ok" : "  ** FAIL", 1000.0 * body / SR);
    }

    // 2. ATTACK engaged: no click — the first 30 ms stay quiet.
    {
        const auto out = render(in, 0.0f, 500.0f);
        const double steady = rms(out, n - (int)(0.5f * SR), (int)(0.4f * SR));
        const double early  = rms(out, on, (int)(0.030f * SR));
        const bool this_ok = early < 0.1 * steady;
        ok &= this_ok;
        std::printf("  ATTACK 500 ms: first 30 ms at %.4f vs steady %.4f "
                    "(< 0.1x)%s\n", early, steady, this_ok ? "  ok" : "  ** FAIL");
    }

    // 3. Dry present: the pick window equals the dry alone — no doubled click.
    {
        const auto out = render(in, 1.0f, 0.0f);
        const double got  = rms(out, on, (int)(0.012f * SR));
        const double dry  = 0.4 * rms(in, on, (int)(0.012f * SR));   // x out_level
        const bool this_ok = got < 1.25 * dry;
        ok &= this_ok;
        std::printf("  dry on: pick window %.4f vs dry alone %.4f (< 1.25x)%s\n",
                    got, dry, this_ok ? "  ok" : "  ** FAIL");
    }

    std::printf("reinject_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
