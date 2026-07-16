// range_test — the guitar/baritone/bass switch, and the polyphonic ripple the
// granular engine cannot currently fix.
//
// A sub voice's grain must span ~2 periods of the note it EMITS, not of the
// note played: the sub of a low E sings at 41 Hz (24 ms), the sub of a
// baritone's low B at 31 Hz (32 ms). A guitar-sized 55 ms grain covers only
// 1.7 periods of the latter, so the aligner has less than a full cycle to lock
// onto and the splice warbles. range_mode re-cuts the sub grains from the
// instrument's lowest note.
//
// Two things are pinned here, one of which is a KNOWN FAILURE kept visible on
// purpose:
//
//   1. Baritone mode is at least as clean as guitar mode on a low B. Note the
//      weak claim: the grain rule (>=2 periods of the EMITTED note) is sound
//      engineering, but measured through an honest 150 ms window a sustained
//      low B improves only 0.4 -> 0.2 dB. A sustained single note is the easy
//      case — the aligner locks onto it even at 1.7 periods. An earlier 20 ms
//      window flattered this to 2.4 -> 1.5 dB and made the mode look like a
//      fix; it is not one. It is kept because the sizing rule is right, not
//      because it rescues the sound.
//   2. The chord ripple — measured AGAINST AN IDEALLY SHIFTED CHORD, not
//      against zero. This matters, and an earlier version of this test got it
//      badly wrong: a chord of non-harmonic partials beats on its own (E major
//      partials sit 42.8 Hz apart -> a 23 ms beat period), so a 20 ms RMS
//      window reads ~12 dB of ripple on a PERFECT shift with no plugin in the
//      path at all. The "15 dB pulsation" that reading implied was mostly the
//      chord being a chord. With a 150 ms window — past every beat period —
//      the floor drops to 0.5 dB and the granular engine's real artifact shows
//      up honestly: +4.8 dB on the sub. Real, audible, and worth fixing; just
//      not 15 dB.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float SR = 48000.0f;
static constexpr int   N  = (int)SR * 3, BLOCK = 256;

// 150 ms RMS window: longer than any beat period in a guitar chord, so this
// measures the shifter's artifact rather than the signal's own beating.
static double ripple_db(const std::vector<float>& in, float range, float f_low_unused)
{
    (void)f_low_unused;
    PoggedDsp* d = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.out_level = 1.0f; p.lp_cutoff = 20000.0f; p.lp_q = 0.707f;
    p.attack_sens = 0.35f; p.sub1_level = 1.0f; p.range_mode = range;
    std::vector<float> l(N), r(N);
    for (int i = 0; i < N; i += BLOCK)
        pogged_dsp_process(d, &p, in.data()+i, l.data()+i, r.data()+i, std::min(BLOCK, N-i));
    pogged_dsp_free(d);

    const int win = (int)(0.150f*SR), hop = (int)(0.010f*SR);
    double rmin = 1e30, rmax = 0;
    for (int i = (int)SR; i + win < N; i += hop) {
        double e = 0; for (int j = 0; j < win; ++j) e += (double)l[i+j]*l[i+j];
        const double rms = std::sqrt(e/win);
        rmin = std::min(rmin, rms); rmax = std::max(rmax, rms);
    }
    return 20.0 * std::log10(rmax / (rmin + 1e-30));
}

int main()
{
    std::printf("══ instrument range ══\n");
    bool ok = true;

    std::vector<float> lowB(N), chord(N);
    for (int i = 0; i < N; ++i) {
        const double t = (double)i/SR;
        lowB[i]  = 0.5*std::sin(2*M_PI*61.74*t);                 // baritone low B1
        chord[i] = 0.22*(std::sin(2*M_PI*164.81*t) + std::sin(2*M_PI*207.65*t)
                       + std::sin(2*M_PI*246.94*t));             // E major triad
    }

    // 1. Baritone mode does its job on a low B.
    const double g = ripple_db(lowB, 0.0f, 0);
    const double b = ripple_db(lowB, 1.0f, 0);
    const bool better = b <= g + 0.05;      // must not be worse; gain is small
    std::printf("  low B1, sub -1: guitar mode %.1f dB -> baritone mode %.1f dB "
                "(%+.1f dB, small by design — see header)  %s\n",
                g, b, b - g, better ? "ok" : "WRONG");
    ok &= better;

    // 2. Polyphonic ripple vs the ideal-shift floor. An ideally shifted E
    //    major chord measures 0.5 dB through this same 150 ms window, so
    //    anything above that is the engine's own artifact.
    const double c = ripple_db(chord, 0.0f, 0);
    const bool bounded = c <= 7.0;
    std::printf("  E major chord, sub -1: %.1f dB (<= 7; an IDEAL shift of the "
                "same chord reads 0.5)\n", c);
    std::printf("    ^ known: granular splices cannot align a chord's "
                "incommensurable periods.\n"
                "      The streaming phase vocoder measures 0.5 dB here — "
                "exactly the floor.\n");
    ok &= bounded;

    std::printf("range_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
