// polyswell_test — the POG3's headline ATTACK behaviour, made measurable:
// "each new attack swells in WITHOUT altering the sustain of the notes
// already ringing" (design doc §14).
//
// Material: A (220 Hz) is picked and HELD; 2.5 s later B (330 Hz, with a
// pick-like transient) is attacked ON TOP of the still-ringing A. The +1
// octave voice is the only one active, so the wet carries A at 440 Hz and B
// at 660 Hz. With a 500 ms ATTACK the POG3 criterion splits in two:
//   1. B's octave (660 Hz) must SWELL — quiet right after the attack, ~90 %
//      of its steady level in roughly attack_ms;
//   2. A's octave (440 Hz) must NOT move while B swells.
// A single wet-bus envelope cannot do both: if the detector fires on B it
// ducks A (the re-pick duck), and if it does not fire B never swells. Only a
// per-band envelope passes 1 AND 2 — which is what the vocoder's per-bin
// swell provides. Asserted on the vocoder engine (focus = 1); the granular
// engine keeps the POG2-style global envelope and is reported for contrast.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

static constexpr float SR    = 48000.0f;
static constexpr int   BLOCK = 256;

static constexpr float ATK_MS  = 500.0f;
static constexpr float T_SIL   = 0.5f;    // lead-in silence
static constexpr float T_B_ON  = 3.0f;    // B attacked here; A rings on
static constexpr float T_END   = 5.5f;

static std::vector<float> render(float focus, const std::vector<float>& in)
{
    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.up1_level   = 1.0f;
    p.out_level   = 1.0f;
    p.input_gain  = 1.0f;
    p.lp_cutoff   = 20000.0f;
    p.lp_q        = 0.707f;
    p.attack_ms   = ATK_MS;
    p.attack_sens = 0.9f;     // high: B only adds ~+6 dB over the ringing A
    p.focus       = focus;

    const int n = (int)in.size();
    std::vector<float> out(n), out_r(n);
    for (int i = 0; i < n; i += BLOCK)
        pogged_dsp_process(dsp, &p, in.data() + i, out.data() + i, out_r.data() + i,
                           std::min(BLOCK, n - i));
    pogged_dsp_free(dsp);
    return out;
}

// Band amplitude at f over [from, from+len) (Goertzel, normalized).
static double band(const std::vector<float>& x, int from, int len, float f)
{
    const double w = 2.0 * M_PI * f / SR, c = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (int i = from; i < from + len; ++i) {
        s0 = (double)x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, p)) / len;
}

int main()
{
    const int sil  = (int)(T_SIL  * SR);
    const int b_on = (int)(T_B_ON * SR);
    const int n    = (int)(T_END  * SR);

    std::vector<float> in(n, 0.0f);
    for (int i = sil; i < n; ++i)
        in[i] = 0.4f * std::sin(2.0 * M_PI * 220.0 * (i - sil) / SR);
    for (int i = b_on; i < n; ++i) {
        // Pick-like transient: 2.5x for ~8 ms, so the onset detector sees the
        // attack the way it would on the pedal.
        const float t     = (i - b_on) / SR;
        const float spike = 1.0f + 1.5f * std::exp(-t / 0.008f);
        in[i] += 0.4f * spike * std::sin(2.0 * M_PI * 330.0 * t);
    }

    const int win = (int)(0.030f * SR);         // 30 ms analysis window
    const int hop = (int)(0.010f * SR);

    bool all_ok = true;
    for (int eng = 1; eng >= 0; --eng) {
        const bool assert_this = (eng == 1);    // vocoder asserted; granular
                                                // stays POG2-global, reported
        const auto out = render((float)eng, in);

        // A's steady octave level, right before B (0.5 s average).
        const int ref_from = b_on - (int)(0.5f * SR);
        double ref = 0.0; int nref = 0;
        for (int i = ref_from; i + win <= b_on; i += hop, ++nref)
            ref += band(out, i, win, 440.0f);
        ref /= nref;

        // 1. A must hold while B swells: worst dip of the 440 Hz band over
        // [B, B + 1.2 s], excluding a 60 ms guard right at the attack where
        // B's own broadband pick transient legitimately crosses the bin.
        const int guard = (int)(0.060f * SR);
        double worst = 1e30;
        for (int i = b_on + guard; i + win <= b_on + (int)(1.2f * SR); i += hop)
            worst = std::min(worst, band(out, i, win, 440.0f));
        const double dip_db = 20.0 * std::log10(worst / ref);

        // 2. B must swell: quiet early on, ~steady after attack_ms. Levels are
        // read at the OUTPUT, so the engine's own latency (~85 ms vocoder) is
        // inside the tolerance band rather than subtracted out.
        const double steady_b = band(out, n - (int)(0.4f * SR), (int)(0.3f * SR), 660.0f);
        const double early_b  = band(out, b_on + (int)(0.130f * SR), win, 660.0f);
        int t90 = -1;
        for (int i = b_on; i + win <= n; i += hop)
            if (band(out, i, win, 660.0f) >= 0.9 * steady_b) { t90 = i - b_on; break; }
        const double t90_ms = 1000.0 * t90 / SR;

        const bool ok_hold  = dip_db > -2.0;
        const bool ok_early = early_b < 0.55 * steady_b;
        const bool ok_t90   = (t90 > 0) && t90_ms > 0.4 * ATK_MS && t90_ms < 2.0 * ATK_MS;

        std::printf("  %s engine:\n", eng ? "vocoder " : "granular");
        std::printf("    A (440 Hz) while B swells: dip %+.2f dB (ref %.4f)%s\n",
                    dip_db, ref,
                    assert_this ? (ok_hold ? "  (> -2)  ok" : "  (> -2)  ** FAIL")
                                : "  [report-only: POG2-style global envelope]");
        std::printf("    B (660 Hz) swell: early %.4f / steady %.4f (< 0.55x)%s\n",
                    early_b, steady_b,
                    assert_this ? (ok_early ? "  ok" : "  ** FAIL") : "");
        std::printf("    B reaches 90%% in %.0f ms (0.4-2.0x of %g)%s\n",
                    t90_ms, ATK_MS,
                    assert_this ? (ok_t90 ? "  ok" : "  ** FAIL") : "");

        if (assert_this) all_ok = ok_hold && ok_early && ok_t90;
    }

    std::printf("polyswell_test: %s\n", all_ok ? "PASS" : "FAIL");
    return all_ok ? 0 : 1;
}
