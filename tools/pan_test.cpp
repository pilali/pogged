// pan_test — verifies the mono→stereo path and the per-voice pan law.
//
// The law is deliberately NOT constant-power: the POG3 is a pedal whose quick
// start has you "connect either the LEFT or RIGHT" output, so a centred voice
// must come out of a single jack at FULL level. Constant power would put it at
// -3 dB and quietly halve the power of every mono patch. This pins that down:
//
//   centre     -> L == R, both at full level
//   hard left  -> R silent, L unchanged from centre
//   hard right -> L silent, R unchanged from centre
//
// Checked on a wet voice (+1 octave) and on the dry path, which is panned too
// but never delayed or filtered.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float SR    = 48000.0f;
static constexpr float FIN   = 220.0f;
static constexpr int   N     = (int)SR * 2;
static constexpr int   BLOCK = 256;

static double rms(const std::vector<float>& x, int from, int to)
{
    double e = 0.0;
    for (int i = from; i < to; ++i) e += (double)x[i] * x[i];
    return std::sqrt(e / (double)(to - from));
}

// Renders one setting; reports the RMS of each channel over the steady region.
static void render(bool dry_voice, float pan, double& l_rms, double& r_rms,
                   double& max_lr_diff)
{
    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.out_level   = 1.0f;
    p.lp_cutoff   = 20000.0f;
    p.lp_q        = 0.707f;
    p.attack_sens = 0.35f;
    if (dry_voice) { p.dry_level = 1.0f; p.pan_dry = pan; }
    else           { p.up1_level = 1.0f; p.pan_up1 = pan; }

    std::vector<float> in(N), l(N), r(N);
    for (int i = 0; i < N; ++i)
        in[i] = 0.5f * std::sin(2.0 * M_PI * FIN * i / SR);
    for (int i = 0; i < N; i += BLOCK)
        pogged_dsp_process(dsp, &p, in.data() + i, l.data() + i, r.data() + i,
                           std::min(BLOCK, N - i));
    pogged_dsp_free(dsp);

    const int from = (int)SR, to = N;            // skip 1 s: pans are smoothed
    l_rms = rms(l, from, to);
    r_rms = rms(r, from, to);
    max_lr_diff = 0.0;
    for (int i = from; i < to; ++i)
        max_lr_diff = std::max(max_lr_diff, (double)std::abs(l[i] - r[i]));
}

static bool check(const char* name, bool dry_voice)
{
    double cl, cr, cdiff, ll, lr, ldiff, rl, rr, rdiff;
    render(dry_voice,  0.0f, cl, cr, cdiff);     // centre
    render(dry_voice, -1.0f, ll, lr, ldiff);     // hard left
    render(dry_voice, +1.0f, rl, rr, rdiff);     // hard right

    // Centre: identical channels, and full level on each (not -3 dB).
    // Centre must be bit-identical: both gains are a literal 1.0, no arithmetic.
    const bool centre_id = cdiff == 0.0;

    // "Silent" is a dB statement, not a bit-zero one. The pan position is a
    // smoothed float, and a one-pole stalls once gc*(target-p) falls below the
    // ULP of a float near 1.0 — so hard-over settles ~-78 dB down rather than
    // at exactly 0. Require 60 dB of channel separation, which is inaudible
    // and still ~20x tighter than anything a wrong pan law would produce.
    const double SILENT = 1e-3;
    const bool left_ok  = lr < SILENT * ll && std::abs(ll - cl) < 0.02 * cl;
    const bool right_ok = rl < SILENT * rr && std::abs(rr - cr) < 0.02 * cr;

    std::printf("  %-4s centre L=%.4f R=%.4f (|L-R| %.0e) | "
                "hard-L %.4f/%.1e | hard-R %.1e/%.4f | sep %.0f/%.0f dB\n",
                name, cl, cr, cdiff, ll, lr, rl, rr,
                20.0 * std::log10(ll / (lr + 1e-30)),
                20.0 * std::log10(rr / (rl + 1e-30)));
    if (!centre_id) std::printf("    centre channels are not identical\n");
    if (!left_ok)   std::printf("    hard-left wrong (R not silent, or L lost level)\n");
    if (!right_ok)  std::printf("    hard-right wrong (L not silent, or R lost level)\n");
    return centre_id && left_ok && right_ok;
}

int main()
{
    std::printf("══ stereo pan ══\n");
    bool ok = true;
    ok &= check("+1",  false);
    ok &= check("dry", true);
    std::printf("pan_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
