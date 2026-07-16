// freeze_test — POG3 FREEZE+GLISS.
//
// "Freeze+Gliss allows you to freeze the sound you hear at the moment you move
// an expression pedal from the heel position towards the toe. When in the full
// toe position, the frozen sound sustains indefinitely, and you can play the
// dry voice over it."
//
// What is pinned:
//   1. The engine is deterministic at the heel. This does NOT prove the port is
//      inert against the pre-freeze build (that binary is gone); what proves it
//      is construction — at the heel the smoothed mix stays at 0 and the ring is
//      fed `x` itself — plus the rest of the suite measuring unchanged. What it
//      does do is underwrite every comparison below, which are all two renders
//      differing in one thing.
//   2. The octaves HOLD the frozen note after the input goes silent. Measured
//      on the +1 voice at its own pitch, not merely on "some energy": a hum or
//      a stuck DC would pass a level check.
//   3. The DRY STAYS LIVE over the freeze — the whole point of the feature.
//      Checked by freezing note A, then playing note B: the output must carry
//      A's octave AND B's dry at once.
//   4. Engaging and releasing does not click, in either direction.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float SR    = 48000.0f;
static constexpr float F_A   = 220.0f;   // the note that gets frozen
static constexpr float F_B   = 330.0f;   // played live over it afterwards
static constexpr int   BLOCK = 128;
static constexpr int   N     = (int)SR * 4;

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

static double rms(const std::vector<float>& x, double from_s, double to_s)
{
    const int a = (int)(from_s*SR), b = (int)(to_s*SR);
    double e = 0.0;
    for (int i = a; i < b; ++i) e += (double)x[i]*x[i];
    return std::sqrt(e / (b - a));
}

// in_hz: what is played, per sample (0 = silence). pedal: freeze port, per block.
static void render(std::vector<float>& l, const std::vector<float>& in,
                   const std::vector<float>& pedal, float dry)
{
    PoggedDsp* d = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.out_level = 1.0f; p.input_gain = 1.0f;
    p.lp_cutoff = 20000.0f; p.lp_q = 0.707f; p.attack_sens = 0.35f;
    p.up1_level = 1.0f; p.dry_level = dry;

    l.assign(N, 0.0f);
    std::vector<float> r(N, 0.0f);
    for (int i = 0; i < N; i += BLOCK) {
        p.freeze = pedal[(size_t)i];
        pogged_dsp_process(d, &p, in.data()+i, l.data()+i, r.data()+i,
                           std::min(BLOCK, N-i));
    }
    pogged_dsp_free(d);
}

int main()
{
    std::printf("══ freeze + gliss ══\n");
    bool ok = true;

    // A plays for 1 s, then silence. The pedal rises at 0.9 s and stays at the
    // toe — so the octave must hold A long after A itself has stopped.
    std::vector<float> in(N, 0.0f), pedal(N, 0.0f), l, l2;
    for (int i = 0; i < (int)SR; ++i)
        in[i] = 0.5f * std::sin(2.0 * M_PI * F_A * i / SR);
    for (int i = (int)(0.9f*SR); i < N; ++i) pedal[(size_t)i] = 1.0f;

    // 1. Two identical renders must agree exactly, or nothing below means
    //    anything: each of those is a pair differing in one input only.
    std::vector<float> no_pedal(N, 0.0f);
    render(l,  in, no_pedal, 0.0f);
    render(l2, in, no_pedal, 0.0f);
    double d0 = 0.0;
    for (int i = 0; i < N; ++i) d0 = std::max(d0, (double)std::abs(l[i]-l2[i]));
    // Deterministic engine: two identical renders must agree exactly, which is
    // what makes the comparisons below meaningful at all.
    std::printf("  heel (freeze=0): engine deterministic, |delta| %.1e "
                "(must be 0)  %s\n", d0, d0 == 0.0 ? "ok" : "WRONG");
    ok &= d0 == 0.0;

    // 2. The octave holds after the input stops.
    render(l, in, pedal, 0.0f);
    const double held = rms(l, 2.5, 3.5);            // 1.5 s after A stopped
    const double at_oct = goertzel_db(l, (int)(SR*2.5f), (int)(SR*3.5f), F_A*2.0);
    const double at_dc  = goertzel_db(l, (int)(SR*2.5f), (int)(SR*3.5f), 5.0);
    const bool holds = held > 0.02 && (at_oct - at_dc) > 20.0;
    std::printf("  +1 holds after input stops: rms %.4f (> 0.02), and it is the "
                "octave — %.0f Hz beats DC by %+.1f dB (>= +20)  %s\n",
                held, F_A*2.0, at_oct - at_dc, holds ? "ok" : "WRONG");
    ok &= holds;

    // 3. Released at the heel, the held sound must go away again.
    std::vector<float> pedal_rel = pedal;
    for (int i = (int)(2.0f*SR); i < N; ++i) pedal_rel[(size_t)i] = 0.0f;
    render(l2, in, pedal_rel, 0.0f);
    const double after_rel = rms(l2, 2.5, 3.5);
    const bool releases = after_rel < 0.1 * held;
    std::printf("  back to the heel: rms %.4f vs %.4f held (must collapse)  %s\n",
                after_rel, held, releases ? "ok" : "WRONG");
    ok &= releases;

    // 4. THE POINT: the dry stays live over the freeze. Freeze A, then play B.
    std::vector<float> in_ab = in;
    for (int i = (int)(1.5f*SR); i < N; ++i)
        in_ab[(size_t)i] = 0.5f * std::sin(2.0 * M_PI * F_B * i / SR);
    render(l, in_ab, pedal, 1.0f);                   // dry up
    const int a = (int)(SR*2.5f), b = (int)(SR*3.5f);
    const double octA = goertzel_db(l, a, b, F_A*2.0);   // frozen A, an octave up
    const double dryB = goertzel_db(l, a, b, F_B);       // live B, dry
    const double octB = goertzel_db(l, a, b, F_B*2.0);   // B must NOT be shifted
    const bool both = (octA - octB) > 15.0 && (dryB - octB) > 15.0;
    std::printf("  dry plays over the freeze: frozen A's octave %.0f Hz and live "
                "dry B %.0f Hz both present, while B's octave %.0f Hz stays "
                "%+.1f dB down (>= +15)  %s\n",
                F_A*2.0, F_B, F_B*2.0, std::min(octA, dryB) - octB,
                both ? "ok" : "WRONG");
    ok &= both;

    // 5. No click on engage or release. The ring is swapped between the live
    //    input and the loop, which is a hard cut without the crossfade.
    double mx = 0.0;
    for (int i = 1; i < N; ++i)
        mx = std::max(mx, (double)std::abs(l2[i] - l2[i-1]));
    double in_mx = 0.0;
    for (int i = 1; i < N; ++i)
        in_mx = std::max(in_mx, (double)std::abs(in[i] - in[i-1]));
    const bool smooth = mx <= 20.0 * in_mx;
    std::printf("  engage + release: max sample delta %.4f (limit %.4f = 20x "
                "input slope)  %s\n", mx, 20.0*in_mx, smooth ? "ok" : "WRONG");
    ok &= smooth;

    // 6. GLISSANDO: back to the heel, play B, rise again — the held octave must
    //    LEAVE A and arrive on B. And the pedal's position must set how long
    //    that takes: "the closer the pedal is to the toe the slower the rate".
    for (double pos : { 0.0, 1.0 }) {
        std::vector<float> ped(N, 0.0f), in2(N, 0.0f);
        for (int i = 0; i < (int)SR; ++i)                    // A ...
            in2[(size_t)i] = 0.5f * std::sin(2.0 * M_PI * F_A * i / SR);
        for (int i = (int)(1.5f*SR); i < (int)(2.5f*SR); ++i)  // ... then B
            in2[(size_t)i] = 0.5f * std::sin(2.0 * M_PI * F_B * i / SR);
        for (int i = (int)(0.9f*SR); i < (int)(1.5f*SR); ++i)  // freeze A
            ped[(size_t)i] = 0.35f;
        for (int i = (int)(2.4f*SR); i < N; ++i)               // heel, then B
            ped[(size_t)i] = (float)(pos * 0.99 + 0.01);
        render(l, in2, ped, 0.0f);

        // Measured EARLY in the glide (0.1-0.5 s after the capture at 2.4 s):
        // the fast glide (20 ms) is long done and sits on B, while the slow one
        // (2 s) is only 5-25% of the way over and must still be mostly A. Read
        // later the slow case closes to ~2 dB, which is too thin to trust
        // across compilers — the claim is the ORDERING, so measure where the
        // ordering is unambiguous.
        const int c = (int)(SR*2.5f), e = (int)(SR*2.9f);
        const double gA = goertzel_db(l, c, e, F_A*2.0);
        const double gB = goertzel_db(l, c, e, F_B*2.0);
        const bool want_B = pos < 0.5;
        const bool got = want_B ? (gB > gA) : (gA > gB);
        std::printf("  gliss @ pedal %.2f (%s): 0.4 s in, A's octave %+.1f dB vs "
                    "B's %+.1f dB -> %s wins  %s\n",
                    pos, want_B ? "heel: fast" : "toe: slow", gA, gB,
                    gB > gA ? "B" : "A", got ? "ok" : "WRONG");
        ok &= got;
    }

    std::printf("freeze_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
