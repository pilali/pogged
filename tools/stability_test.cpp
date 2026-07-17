// stability_test — pitch-shift STABILITY on close partials (design §15).
//
// The user's ear: the up voices sound slightly "vibrating". Cause found: the
// multi-res crossover was OUTPUT-referred while resolution lives on the INPUT
// side. A voice at `ratio` puts an input partial at f on the output at
// ratio·f, so with a fixed 250 Hz output crossover the +1 voice hands the
// short window output down to 250 Hz — input partials down to 125 Hz, which
// its 23 Hz bins cannot separate on a chord: their lobes merge, the peaks
// beat, the translation warbles. Fix: cross at XOVER×ratio for up voices
// (input-referred), which is what pogged_dsp wires. The down voices already
// satisfy the constraint at 250 (output 250 = input 500) and are unchanged.
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
    static StreamVocoderT<4096>     v4096;
    static StreamVocoderT<2048>     v2048;
    static MultiVocoder<4096, 2048> multi;
    v4096.init(SR); v2048.init(SR); multi.init(SR);

    bool ok = true;
    std::printf("══ shift stability on close partials (§15) ══\n");

    // The up voices, on the C3+E3 pair (34 Hz apart). Crossovers as wired in
    // pogged_dsp: XOVER×ratio, input-referred.
    for (float ratio : { 2.0f, 4.0f }) {
        multi.set_xover(MultiVocoder<>::XOVER * std::max(1.0f, ratio));
        const double m  = am_depth(multi, ratio, 130.81f, 164.81f);
        const double s4 = am_depth(v4096, ratio, 130.81f, 164.81f);
        const bool this_ok = m < 0.5;
        ok &= this_ok;
        std::printf("  x%g on C3+E3 (34 Hz apart): multi %.2f dB AM "
                    "(4096 floor %.2f, < 0.5)%s\n",
                    ratio, m, s4, this_ok ? "  ok" : "  ** FAIL");
    }

    // The sub, on a pair 43 Hz apart — already input-safe at 250, pinned so
    // it stays that way.
    {
        multi.set_xover(MultiVocoder<>::XOVER);
        const double m = am_depth(multi, 0.5f, 164.81f, 207.65f);
        const bool this_ok = m < 0.5;
        ok &= this_ok;
        std::printf("  x0.5 on E3+G#3 (43 Hz apart): multi %.2f dB AM (< 0.5)%s\n",
                    m, this_ok ? "  ok" : "  ** FAIL");
    }

    // The counterexamples that justify the rule, kept visible:
    multi.set_xover(MultiVocoder<>::XOVER);           // output-referred (bug)
    std::printf("  [counterexamples, report-only] x2 on C3+E3: "
                "short window alone %.1f dB AM, multi at fixed 250 %.1f dB AM\n",
                am_depth(v2048, 2.0f, 130.81f, 164.81f),
                am_depth(multi, 2.0f, 130.81f, 164.81f));

    std::printf("stability_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
