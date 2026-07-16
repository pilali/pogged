// dry_test — POG3 input gain and the three DRY routing buttons.
//
// The dry path is the POG's signature: untouched and undelayed. The DRY
// buttons deliberately break that, one effect at a time, so each one is
// checked for doing exactly what it claims and nothing more.
//
//   input_gain  — "the level of the signal seen at the input" (0.5-3x), so it
//                 must scale everything downstream, dry included.
//   dry_attack  — the swell reaches the dry.
//   dry_filter  — the filter reaches the dry.
//   dry_detune  — the chorus reaches the dry, AND with it SPREAD, which the
//                 manual gates on this same button.
//
// All four are checked with ONLY the dry up (every voice muted), so anything
// measured is the dry path and not a shifted voice leaking in.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float SR    = 48000.0f;
static constexpr float FIN   = 440.0f;
static constexpr int   N     = (int)SR * 2;
static constexpr int   BLOCK = 256;

struct Setup {
    float gain = 1.0f, atk_ms = 0.0f, cutoff = 20000.0f;
    float d_atk = 0.0f, d_filt = 0.0f, d_det = 0.0f;
    float detune = 0.0f, spread = 0.0f;
};

static void render(const Setup& s, std::vector<float>& l, std::vector<float>& r)
{
    PoggedDsp* d = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.dry_level   = 1.0f;          // dry only: every voice stays at zero
    p.out_level   = 1.0f;
    p.input_gain  = s.gain;
    p.lp_cutoff   = s.cutoff;
    p.lp_q        = 0.707f;
    p.attack_ms   = s.atk_ms;
    p.attack_sens = 0.35f;
    p.filter_sens = 0.5f;
    p.dry_attack  = s.d_atk;
    p.dry_filter  = s.d_filt;
    p.dry_detune  = s.d_det;
    p.detune_cents = s.detune;
    p.spread      = s.spread;

    std::vector<float> in(N, 0.0f);
    for (int i = (int)(0.2f*SR); i < N; ++i)
        in[i] = 0.4f * std::sin(2.0 * M_PI * FIN * i / SR);
    l.assign(N, 0.0f); r.assign(N, 0.0f);
    for (int i = 0; i < N; i += BLOCK)
        pogged_dsp_process(d, &p, in.data()+i, l.data()+i, r.data()+i,
                           std::min(BLOCK, N-i));
    pogged_dsp_free(d);
}

static double rms(const std::vector<float>& x, float from_s, float to_s)
{
    const int a = (int)(from_s*SR), b = (int)(to_s*SR);
    double e = 0.0;
    for (int i = a; i < b; ++i) e += (double)x[i]*x[i];
    return std::sqrt(e / (b - a));
}

int main()
{
    std::printf("══ input gain + DRY routing ══\n");
    bool ok = true;
    std::vector<float> l, r, l2, r2;

    // 1. INPUT GAIN scales the dry. 2x in must be ~2x out (well under the
    //    soft clipper's 0.7 knee, or the clipper would hide the gain).
    Setup g1; g1.gain = 1.0f;
    Setup g2; g2.gain = 2.0f;
    render(g1, l, r); const double a = rms(l, 1.0f, 2.0f);
    render(g2, l2, r2); const double b = rms(l2, 1.0f, 2.0f);
    const double ratio = b / (a + 1e-30);
    const bool gain_ok = std::abs(ratio - 2.0) < 0.1;
    std::printf("  input gain 1x -> 2x: output ratio %.2f (want 2.00)  %s\n",
                ratio, gain_ok ? "ok" : "WRONG");
    ok &= gain_ok;

    // 2. DRY ATTACK. With a 500 ms swell the dry must start quiet and grow;
    //    with the button off it is full from the first sample.
    Setup s_off; s_off.atk_ms = 500.0f; s_off.d_atk = 0.0f;
    Setup s_on;  s_on.atk_ms  = 500.0f; s_on.d_atk  = 1.0f;
    render(s_off, l, r);  const double off_early = rms(l, 0.22f, 0.30f);
    render(s_on,  l2, r2);
    const double on_early = rms(l2, 0.22f, 0.30f);
    const double on_late  = rms(l2, 1.2f, 2.0f);
    const bool atk_ok = on_early < 0.5 * off_early && on_late > 0.8 * off_early;
    std::printf("  dry attack: off early %.4f | on early %.4f (swelling), "
                "on late %.4f  %s\n",
                off_early, on_early, on_late, atk_ok ? "ok" : "WRONG");
    ok &= atk_ok;

    // 3. DRY FILTER. A 200 Hz low-pass must cut the 440 Hz dry only when the
    //    button is lit; off, the dry bypasses the filter entirely.
    Setup f_off; f_off.cutoff = 200.0f; f_off.d_filt = 0.0f;
    Setup f_on;  f_on.cutoff  = 200.0f; f_on.d_filt  = 1.0f;
    render(f_off, l, r);  const double unfiltered = rms(l, 1.0f, 2.0f);
    render(f_on,  l2, r2); const double filtered  = rms(l2, 1.0f, 2.0f);
    const double cut_db = 20.0 * std::log10(filtered / (unfiltered + 1e-30));
    const bool filt_ok = cut_db < -12.0;
    std::printf("  dry filter: LP 200 Hz on a 440 Hz dry -> %+.1f dB when lit "
                "(< -12; off = untouched)  %s\n", cut_db, filt_ok ? "ok" : "WRONG");
    ok &= filt_ok;

    // 4. DRY DETUNE gates SPREAD onto the dry — the manual ties the two. With
    //    the button off, SPREAD must leave the dry perfectly mono (L == R);
    //    with it on, the two channels must diverge.
    Setup sp_off; sp_off.spread = 1.0f; sp_off.detune = 20.0f; sp_off.d_det = 0.0f;
    Setup sp_on;  sp_on.spread  = 1.0f; sp_on.detune  = 20.0f; sp_on.d_det  = 1.0f;
    render(sp_off, l, r);
    double diff_off = 0.0;
    for (int i = (int)SR; i < N; ++i) diff_off = std::max(diff_off, (double)std::abs(l[i]-r[i]));
    render(sp_on, l2, r2);
    double diff_on = 0.0;
    for (int i = (int)SR; i < N; ++i) diff_on = std::max(diff_on, (double)std::abs(l2[i]-r2[i]));
    const bool spread_ok = diff_off == 0.0 && diff_on > 0.01;
    std::printf("  dry detune gates spread: |L-R| off %.1e (must be 0) | "
                "on %.4f (must diverge)  %s\n",
                diff_off, diff_on, spread_ok ? "ok" : "WRONG");
    ok &= spread_ok;

    std::printf("dry_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
