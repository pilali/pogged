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
//   1. Baritone mode measurably cleans up a single low B (its whole point).
//   2. The chord ripple. The audit used to check ripple on a *sine* — 0.6 dB,
//      green, while a real chord pulses at ~15 dB and ~12 Hz, right in the
//      tremolo band. That blindness is why the defect shipped. The bound below
//      is set to today's measured reality, NOT to the 6 dB a sine passes: it
//      documents the debt instead of hiding it, and tightens when the phase
//      vocoder lands. Grain tuning alone will not get there — measured, longer
//      grains make chords *worse*, not better.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float SR = 48000.0f;
static constexpr int   N  = (int)SR * 3, BLOCK = 256;

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

    const int win = (int)(0.020f*SR), hop = (int)(0.005f*SR);
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
    const bool better = b < g - 0.4;
    std::printf("  low B1, sub -1: guitar mode %.1f dB -> baritone mode %.1f dB "
                "(%+.1f dB)  %s\n", g, b, b - g, better ? "ok" : "WRONG");
    ok &= better;

    // 2. Polyphonic ripple — the known defect, bounded so it cannot silently
    //    get worse. A sine reads 0.6 dB here; a chord is the honest test.
    const double c = ripple_db(chord, 0.0f, 0);
    const bool bounded = c <= 17.0;
    std::printf("  E major chord, sub -1: %.1f dB ripple (<= 17 today; a sine "
                "reads 0.6)\n", c);
    std::printf("    ^ known: granular splices cannot align a chord's "
                "incommensurable periods.\n"
                "      Bound tightens to ~6 dB when the phase vocoder lands.\n");
    ok &= bounded;

    std::printf("range_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
