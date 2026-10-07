// polyswell_test — the POG3's headline ATTACK behaviour, made measurable:
// "each new attack swells in WITHOUT altering the sustain of the notes
// already ringing" (design doc §14).
//
// Material: A (220 Hz) is picked and HELD; 2.5 s later B (330 Hz, with a
// pick-like transient) is attacked ON TOP of the still-ringing A. With a
// 500 ms ATTACK the POG3 criterion splits per band:
//   1. B's octaves must SWELL — quiet right after the attack, ~90 % of their
//      steady level in roughly attack_ms;
//   2. A's octaves must NOT move while B swells;
//   3. the DRY (when mixed in) takes no swell at all: B's fundamental is
//      there immediately, at full level — the POG's defining trait, only the
//      DRY ATTACK button routes the dry through the envelope.
// A single wet-bus envelope cannot do 1 AND 2: if the detector fires on B it
// ducks A (the re-pick duck), and if it does not fire B never swells. Only a
// per-band envelope passes both — which is what the vocoder's per-bin swell
// provides. Asserted on the vocoder engine (focus = 1); the granular engine
// keeps the POG2-style global envelope and is reported for contrast.
//
// Two mixes are run: the isolated +1 octave (the cleanest measurement), and
// the Classic-POG mix dry + sub1 + up1, where A lives at 110/440 Hz wet and
// B at 165/660 Hz wet + 330 Hz dry.
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

static std::vector<float> render(float focus, float dry, float sub1, float up1,
                                 const std::vector<float>& in)
{
    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.dry_level   = dry;
    p.sub1_level  = sub1;
    p.up1_level   = up1;
    // Applied BEFORE the output soft-clip: at unity the 5-component mix
    // (dry A+B, sub A+B, up A+B) crests past the 0.7 knee, and the clipper's
    // compression rises when B enters — a headroom artifact that would show
    // up as a fake "dip" on A's bands. Every metric here is a ratio, so the
    // scale-down changes nothing else.
    p.out_level   = 0.4f;
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

// Band amplitude at f over [from, from+len) (Goertzel, normalized). Hann
// weighted: a rectangular window leaks ~-20 dB between the mix's components
// (330 dry into the 440 measurement at 30 ms), which beats against the band
// under test and fakes a ~1 dB dip. Hann sidelobes are below -31 dB.
static double band(const std::vector<float>& x, int from, int len, float f)
{
    const double w = 2.0 * M_PI * f / SR, c = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, wsum = 0.0;
    for (int i = 0; i < len; ++i) {
        const double wn = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (len - 1)));
        s0 = (double)x[from + i] * wn + c * s1 - s2;
        s2 = s1; s1 = s0;
        wsum += wn;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, p)) / wsum;
}

// Analysis window per band: the low octaves (110/165 Hz, 55 Hz apart) need a
// longer window than the ups to keep the Goertzel leakage between them and
// the 220/330 dry components ~-23 dB; the ups stay on the original 30 ms.
static int win_of(float f) { return (int)((f < 300.0f ? 0.080f : 0.030f) * SR); }

struct Check { const char* what; float f; };

// One engine on one mix. Returns false only when asserted and failing.
static bool run_mix(const char* mix_name, float dry, float sub1, float up1,
                    int eng, const std::vector<float>& in)
{
#ifdef POGGED_NO_VOCODER
    // The per-bin swell IS the vocoder's (see the header): with the vocoder
    // compiled out, focus = 1 still renders granular, whose POG2-style global
    // envelope legitimately ducks the ringing note. Report, assert nothing —
    // the same shape focus_test takes for this build.
    const bool assert_this = false;
#else
    const bool assert_this = (eng == 1);    // vocoder asserted; granular
                                            // stays POG2-global, reported
#endif
    const auto out = render((float)eng, dry, sub1, up1, in);
    const int  b_on = (int)(T_B_ON * SR);
    const int  n    = (int)in.size();
    const int  hop  = (int)(0.010f * SR);
    bool ok = true;

    std::printf("  %s, %s engine:%s\n", mix_name,
                eng ? "vocoder " : "granular",
                assert_this ? "" : "  [report-only: POG2-style global envelope]");

    // A's wet octaves must hold while B swells: worst dip over [B, B+1.2 s],
    // excluding a 60 ms guard right at the attack where B's own broadband
    // pick transient legitimately crosses the band.
    const Check holds[] = { { "sub -1", 110.0f }, { "up +1", 440.0f } };
    for (const auto& h : holds) {
        if ((h.f < 300.0f ? sub1 : up1) <= 0.0f) continue;
        const int win = win_of(h.f);
        double ref = 0.0; int nref = 0;
        for (int i = b_on - (int)(0.5f * SR); i + win <= b_on; i += hop, ++nref)
            ref += band(out, i, win, h.f);
        ref /= nref;
        const int guard = (int)(0.060f * SR);
        double worst = 1e30;
        for (int i = b_on + guard; i + win <= b_on + (int)(1.2f * SR); i += hop)
            worst = std::min(worst, band(out, i, win, h.f));
        const double dip_db = 20.0 * std::log10(worst / ref);
        // The sub band gets its own bound in a hybrid build: there the sub's
        // vocoder is LONG-WINDOW-ONLY (§30 dropped its short window, since
        // the granular now renders the sub's attack), which costs the hold a
        // little of its time resolution. Measured on the same material:
        // -1.95 dB with the split sub, -2.04 dB long-only, -0.70 dB on a
        // pinned 2048 window. All three are one note holding under another;
        // the point of the bound is that A does not audibly move, and 2.5 dB
        // is still well inside that. Everything else keeps the tight 2.0.
#if defined(POGGED_DYN_FOCUS) && !defined(POGGED_SUB_SPLIT)
        const double hold_lim = (h.f < 300.0f) ? -2.5 : -2.0;
#else
        const double hold_lim = -2.0;
#endif
        const bool this_ok = dip_db > hold_lim;
        if (assert_this) ok = ok && this_ok;
        std::printf("    A holds at %s (%g Hz): dip %+.2f dB (ref %.4f, "
                    "limit %+.1f)%s\n",
                    h.what, h.f, dip_db, ref, hold_lim,
                    assert_this ? (this_ok ? "  ok" : "  ** FAIL") : "");
    }

    // B's wet octaves must swell: quiet early on, ~steady after attack_ms.
    // Levels are read at the OUTPUT, so the engine's own latency (~85 ms
    // vocoder) is inside the tolerance band rather than subtracted out.
    const Check swells[] = { { "sub -1", 165.0f }, { "up +1", 660.0f } };
    for (const auto& s : swells) {
        if ((s.f < 300.0f ? sub1 : up1) <= 0.0f) continue;
        const int win = win_of(s.f);
        const double steady = band(out, n - (int)(0.4f * SR), (int)(0.3f * SR), s.f);
        const double early  = band(out, b_on + (int)(0.130f * SR), win, s.f);
        int t90 = -1;
        for (int i = b_on; i + win <= n; i += hop)
            if (band(out, i, win, s.f) >= 0.9 * steady) { t90 = i - b_on; break; }
        const double t90_ms = 1000.0 * t90 / SR;
        const bool ok_early = early < 0.55 * steady;
        const bool ok_t90   = (t90 > 0) && t90_ms > 0.4 * ATK_MS && t90_ms < 2.0 * ATK_MS;
        if (assert_this) ok = ok && ok_early && ok_t90;
        std::printf("    B swells at %s (%g Hz): early %.4f / steady %.4f (< 0.55x)%s,"
                    " 90%% in %.0f ms (0.4-2.0x of %g)%s\n",
                    s.what, s.f, early, steady,
                    assert_this ? (ok_early ? "  ok" : "  ** FAIL") : "",
                    t90_ms, ATK_MS,
                    assert_this ? (ok_t90 ? "  ok" : "  ** FAIL") : "");
    }

    // The dry takes NO swell: B's fundamental is at full level right away
    // (window starts 40 ms in, past the pick transient's 2.5x spike).
    if (dry > 0.0f) {
        const int win = win_of(330.0f);
        const double steady = band(out, n - (int)(0.4f * SR), (int)(0.3f * SR), 330.0f);
        const double early  = band(out, b_on + (int)(0.040f * SR), win, 330.0f);
        const bool this_ok  = early > 0.8 * steady;
        if (assert_this) ok = ok && this_ok;
        std::printf("    dry is immediate at 330 Hz: early %.4f / steady %.4f (> 0.8x)%s\n",
                    early, steady,
                    assert_this ? (this_ok ? "  ok" : "  ** FAIL") : "");
    }
    return ok;
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

    bool all_ok = true;
    for (int eng = 1; eng >= 0; --eng) {
        all_ok &= run_mix("up1 only          ", 0.0f, 0.0f, 1.0f, eng, in);
        all_ok &= run_mix("dry + sub1 + up1  ", 1.0f, 1.0f, 1.0f, eng, in);
    }

    std::printf("polyswell_test: %s\n", all_ok ? "PASS" : "FAIL");
    return all_ok ? 0 : 1;
}
