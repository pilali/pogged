// warp_test — POG3 WARP: "whammy style pitch bend on all voices except DRY".
//
// `warp` is where the expression pedal is; heel and toe carry an interval each,
// so the bend runs heel -> toe. What is pinned here:
//
//   1. warp = 0 with heel = 0 is EXACTLY identity — a patch saved before these
//      ports existed must sound bit-for-bit as it did. Every host fills the
//      missing ports with their defaults, so this is the compatibility test.
//   2. Each voice lands on the pitch the bend asks for, in BOTH directions.
//   3. The dry is spared, as the manual says, even at full bend.
//   4. The lag budget follows the bend. This is the one that bites: a tap
//      reading at ratio r eats (r-1)*grain of lag over a grain, so the +2 voice
//      warped up an octave (ratio 8) needs ~7 grains of budget where its
//      nominal ratio 4 needed 3. Sized for the nominal ratio it overtakes the
//      write head and reads samples that have not been written yet — which
//      shows up as a wrong//noisy pitch, not as a crash. Checked at the
//      extreme: +2 warped a full octave up.
//   5. Sweeping the pedal does not click. The bend moves the respawn anchor
//      (unavoidable — the budget has to grow), so this is not free by
//      construction.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float SR    = 48000.0f;
static constexpr float FIN   = 220.0f;
static constexpr int   N     = (int)SR * 3;
static constexpr int   BLOCK = 256;

static double goertzel_db(const std::vector<float>& x, int from, int to, double f)
{
    const double w  = 2.0 * M_PI * f / SR;
    const double cw = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (int i = from; i < to; ++i) {
        const double s0 = x[i] + cw * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return 10.0 * std::log10(s1*s1 + s2*s2 - cw*s1*s2 + 1e-30);
}

static void base(PoggedParams& p)
{
    p.out_level   = 1.0f;
    p.input_gain  = 1.0f;   // 0 would clamp to 0.5: a silent 6 dB cut
    p.lp_cutoff   = 20000.0f;
    p.lp_q        = 0.707f;
    p.attack_sens = 0.35f;
}

// Renders one voice at a fixed pedal position.
static void render(std::vector<float>& l, float PoggedParams::*voice,
                   float warp, float heel, float toe)
{
    PoggedDsp* d = pogged_dsp_new(SR);
    PoggedParams p = {};
    base(p);
    p.*voice = 1.0f;
    p.warp = warp; p.warp_heel = heel; p.warp_toe = toe;

    std::vector<float> in(N), r(N);
    for (int i = 0; i < N; ++i) in[i] = 0.5f * std::sin(2.0 * M_PI * FIN * i / SR);
    l.assign(N, 0.0f);
    for (int i = 0; i < N; i += BLOCK)
        pogged_dsp_process(d, &p, in.data()+i, l.data()+i, r.data()+i,
                           std::min(BLOCK, N-i));
    pogged_dsp_free(d);
}

// Does the voice sit on `want_hz` rather than on its unbent pitch `nominal_hz`?
// min_sep defaults to a loose 10 dB — enough to prove the bend happened at all.
// The lag-budget case passes a much tighter one; see its call site for why.
static bool lands_on(const char* what, std::vector<float>& l,
                     double want_hz, double nominal_hz, double min_sep = 10.0)
{
    const int from = (int)(SR * 1.5f), to = N;     // past the smoothed sweep
    const double want_db = goertzel_db(l, from, to, want_hz);
    const double nom_db  = goertzel_db(l, from, to, nominal_hz);
    const double sep     = want_db - nom_db;
    const bool ok = sep >= min_sep;
    std::printf("  %-38s %7.1f Hz beats its unbent %7.1f Hz by %+6.1f dB "
                "(>= %+.0f)  %s\n", what, want_hz, nominal_hz, sep, min_sep,
                ok ? "ok" : "WRONG");
    return ok;
}

int main()
{
    std::printf("══ warp (whammy bend) ══\n");
    bool ok = true;
    std::vector<float> a, b;

    // 1. Identity. warp=0 & heel=0 must reproduce the un-warped render exactly:
    //    same samples, not merely the same pitch. Anything else means old
    //    patches changed sound when these ports appeared.
    render(a, &PoggedParams::up1_level, 0.0f, 0.0f, 0.0f);   // no bend at all
    render(b, &PoggedParams::up1_level, 0.0f, 0.0f, 12.0f);  // toe set, pedal at heel
    double maxdiff = 0.0;
    for (int i = 0; i < N; ++i) maxdiff = std::max(maxdiff, (double)std::abs(a[i]-b[i]));
    const bool id_ok = maxdiff == 0.0;
    std::printf("  pedal at heel (warp=0, heel=0): |delta| vs no-warp %.1e "
                "(must be 0 -> saved patches unchanged)  %s\n",
                maxdiff, id_ok ? "ok" : "WRONG");
    ok &= id_ok;

    // 2. Bend up: +1 voice (440 Hz) warped +12 must sing an octave higher.
    render(a, &PoggedParams::up1_level, 1.0f, 0.0f, 12.0f);
    ok &= lands_on("+1 warped +12 st (toe)", a, FIN*2.0*2.0, FIN*2.0);

    // 3. Bend down, and the heel>toe sweep direction: -1 voice (110 Hz)
    //    warped -12 must sing an octave lower.
    render(a, &PoggedParams::sub1_level, 1.0f, 0.0f, -12.0f);
    ok &= lands_on("-1 warped -12 st (toe)", a, FIN*0.5*0.5, FIN*0.5);

    // 4. Mid-sweep lands on a real interval, not just at the ends: a fifth
    //    above the +1 octave. Note toe stays inside [-12, 12] and the pedal
    //    does the 7/12 — asking for it with toe = 14 instead would be silently
    //    clamped back to 12 and quietly measure the wrong bin.
    render(a, &PoggedParams::up1_level, 7.0f/12.0f, 0.0f, 12.0f);
    ok &= lands_on("+1 warped +7 st (pedal mid-sweep)", a,
                   FIN*2.0*std::pow(2.0, 7.0/12.0), FIN*2.0);

    // 5. THE LAG BUDGET. +2 (880 Hz) warped +12 reads at ratio 8 — double its
    //    nominal 4. If the budget still covered only ratio 4 the tap would
    //    overtake the write head and read samples not yet written.
    //
    //    The threshold is 45 dB, not the 10 dB above, and that is the whole
    //    point of this line: with the budget frozen at the nominal ratio this
    //    case still measures +35 dB and would sail past a 10 dB bar while
    //    quietly reading garbage. Measured 58 dB with the budget, 35 without —
    //    45 sits between them with 13 dB of margin either side.
    render(a, &PoggedParams::up2_level, 1.0f, 0.0f, 12.0f);
    ok &= lands_on("+2 warped +12 st (ratio 8: budget)", a, FIN*4.0*2.0,
                   FIN*4.0, 45.0);

    // 6. The dry is spared: "on all voices except DRY".
    render(a, &PoggedParams::dry_level, 1.0f, 0.0f, 12.0f);
    const int from = (int)(SR*1.5f);
    const double dry_at_in = goertzel_db(a, from, N, FIN);
    const double dry_bent  = goertzel_db(a, from, N, FIN*2.0);
    const bool dry_ok = dry_at_in - dry_bent >= 20.0;
    std::printf("  %-38s stays at %.0f Hz, %+.1f dB over the bent %.0f Hz "
                "(>= +20)  %s\n", "dry at full warp", FIN,
                dry_at_in - dry_bent, FIN*2.0, dry_ok ? "ok" : "WRONG");
    ok &= dry_ok;

    // 7. Sweeping the pedal must not click. The bend grows the lag budget, so
    //    it MOVES the respawn anchor — this is not free by construction.
    {
        PoggedDsp* d = pogged_dsp_new(SR);
        PoggedParams p = {};
        base(p);
        p.up1_level = 1.0f; p.warp_heel = 0.0f; p.warp_toe = 12.0f;
        std::vector<float> in(N), l(N), r(N);
        for (int i = 0; i < N; ++i) in[i] = 0.5f*std::sin(2.0*M_PI*FIN*i/SR);
        for (int i = 0; i < N; i += BLOCK) {
            p.warp = (float)i / (float)N;             // heel -> toe over 3 s
            pogged_dsp_process(d, &p, in.data()+i, l.data()+i, r.data()+i,
                               std::min(BLOCK, N-i));
        }
        pogged_dsp_free(d);
        double mx = 0.0;
        for (int i = (int)SR; i < N; ++i)
            mx = std::max(mx, (double)std::abs(l[i]-l[i-1]));
        double in_mx = 0.0;
        for (int i = 1; i < N; ++i)
            in_mx = std::max(in_mx, (double)std::abs(in[i]-in[i-1]));
        // The voice legitimately reaches 4x the input frequency at full bend
        // (+1 octave, bent another), so its slope legitimately reaches ~4x the
        // input's. 8x is twice that physical bound — loose enough to be safe,
        // tight enough to bite: measured 0.057 (4x) here, but 0.251 (18x) with
        // the lag budget frozen, which a 20x bar would have waved through.
        const bool smooth = mx <= 8.0 * in_mx;
        std::printf("  %-38s max sample delta %.4f (limit %.4f = 8x input "
                    "slope, voice reaches 4x)  %s\n", "pedal swept heel->toe",
                    mx, 8.0*in_mx, smooth ? "ok" : "WRONG");
        ok &= smooth;
    }

    std::printf("warp_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
