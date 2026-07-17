// stability_test — pitch-shift STABILITY on close partials (§15/§20).
//
// §15 found the mechanism: with a fixed 250 Hz OUTPUT crossover, the up
// voices' short window renders input partials down to 125 Hz that its bins
// cannot separate on a chord — their lobes merge, the peaks beat, the
// translation warbles. The §15 fix (input-referred crossover, ×ratio) cures
// it but moves the up voices' low-mids onto the long window — LATENCY the
// user ruled out of spec (§20). So the shipped shape keeps the fixed 250 Hz
// crossover, the up cases are REPORT-ONLY tracked numbers (the §20 program's
// target), and the input-safe sub path stays asserted.
//
// Material: two partials 34 Hz apart (C3+E3 — an ordinary guitar voicing),
// or 43 Hz apart for the sub case. Metric: per-OUTPUT-partial amplitude
// modulation (max/min of a sliding 150 ms Hann-Goertzel) after settling; an
// ideal shift holds each partial steady, so the gate is in fractions of a dB.
// The known-bad configurations are printed as report-only counterexamples.
#include "../src/stream_vocoder.hpp"
#include "../src/stream_multivocoder.hpp"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

static constexpr float    SR  = 48000.0f;
static constexpr uint32_t RSZ = 65536, MASK = RSZ - 1;

static double band(const std::vector<float>& x, int from, int len, float f)
{
    const double w = 2.0 * M_PI * f / SR, c = 2.0 * std::cos(w);
    double s0, s1 = 0, s2 = 0, ws = 0;
    for (int i = 0; i < len; ++i) {
        const double wn = 0.5 * (1 - std::cos(2 * M_PI * i / (len - 1)));
        s0 = (double)x[from + i] * wn + c * s1 - s2;
        s2 = s1; s1 = s0; ws += wn;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, p)) / ws;
}

// Worst per-partial AM depth (dB) of `eng` shifting two partials by `ratio`.
template <class Engine>
static double am_depth(Engine& eng, float ratio, float f1, float f2)
{
    const int n = (int)(5.0f * SR);
    std::vector<float> in(n);
    for (int i = 0; i < n; ++i) {
        const double t = i / (double)SR;
        in[i] = 0.3 * (std::sin(2 * M_PI * f1 * t) + std::sin(2 * M_PI * f2 * t));
    }
    eng.set_ratio(ratio);
    eng.reset();
    std::vector<float> ring(RSZ, 0.0f), out(n);
    uint64_t wpos = RSZ;
    for (int i = 0; i < n; ++i) {
        ring[wpos & MASK] = in[i];
        ++wpos;
        out[i] = eng.process(ring.data(), MASK, wpos);
    }
    const int win = (int)(0.150f * SR), hop = (int)(0.010f * SR);
    double worst = 0.0;
    for (float f : { f1 * ratio, f2 * ratio }) {
        double lo = 1e30, hi = 0;
        for (int i = (int)SR; i + win < n; i += hop) {
            const double a = band(out, i, win, f);
            lo = std::min(lo, a); hi = std::max(hi, a);
        }
        worst = std::max(worst, 20.0 * std::log10(hi / (lo + 1e-30)));
    }
    return worst;
}

int main()
{
    // Static: each engine instance owns large buffers, keep them off the stack.
    // The shipped §20 shape: 4096+2048, FIXED 250 Hz output crossover — the
    // latency budget the user ruled usable (85/42 ms). The up voices' short
    // window therefore renders input partials down to 125 Hz that it cannot
    // always resolve: those cases are REPORT-ONLY tracked numbers (the §20
    // stability program's target), not asserts. The sub path is input-safe
    // at this crossover and stays asserted.
    static StreamVocoderT<4096>     vlong;
    static StreamVocoderT<2048>     v2048;
    static MultiVocoder<4096, 2048> multi;
    vlong.init(SR); v2048.init(SR); multi.init(SR);
    constexpr float XOUT = 250.0f;
    multi.set_xover(XOUT);
    multi.tune(0.20f, 1.0f);     // §20: long-window smoothing only, as wired

    bool ok = true;
    std::printf("══ shift stability on close partials (§15/§20) ══\n");

    // The up voices, on the C3+E3 pair (34 Hz apart) — the §20 budget trade,
    // tracked: lower is better, the long-window floor is the reference.
    for (float ratio : { 2.0f, 4.0f }) {
        const double m  = am_depth(multi, ratio, 130.81f, 164.81f);
        const double s4 = am_depth(vlong, ratio, 130.81f, 164.81f);
        std::printf("  x%g on C3+E3 (34 Hz apart): multi %.2f dB AM "
                    "(long-window floor %.2f)  [REPORT-ONLY, §20 budget]\n",
                    ratio, m, s4);
    }

    // The sub, on a pair 43 Hz apart (input 165-208 Hz -> long window).
    {
        const double m = am_depth(multi, 0.5f, 164.81f, 207.65f);
        const bool this_ok = m < 0.5;
        ok &= this_ok;
        std::printf("  x0.5 on E3+G#3 (43 Hz apart): multi %.2f dB AM (< 0.5)%s\n",
                    m, this_ok ? "  ok" : "  ** FAIL");
    }

    // The bare short window, for scale: what the up voices' 250-500 Hz
    // output region rides on under the §20 budget.
    std::printf("  [reference, report-only] x2 on C3+E3 through a bare "
                "2048 window: %.1f dB AM\n",
                am_depth(v2048, 2.0f, 130.81f, 164.81f));

    std::printf("stability_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
