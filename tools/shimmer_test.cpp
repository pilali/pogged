// shimmer_test — the "scintillement" on REAL polyphony, measured (§16).
//
// A real chord is where the shimmer lives: two notes' harmonics COLLIDE
// (come closer than the analysis window resolves) at every register — on a
// major third A2+C#3, h4 of A2 sits 24 Hz from h3 of C#3, h5 of A2 sits
// 4.5 Hz from h4 of C#3. An unresolved pair makes the per-peak translation
// warble at the pair's beat rate: the merged peak's frequency estimate
// swings, the phasor integrates the swing, and when the picker intermittently
// resolves the pair the overlapped lobes get chopped into regions translated
// by different offsets. A single realistic string (inharmonic, beating,
// noisy) does NOT shimmer — measured ≤0.3 dB excess on every engine.
//
// Material: A2+C#3, both rendered as realistic strings — stiffness
// inharmonicity, per-mode beats (two polarizations), noise floor. Reference:
// the SAME generator with every f0 doubled = the ideal +1 shift. Metric:
// per-harmonic EXCESS amplitude modulation (sliding Hann-Goertzel max/min)
// over the ideal's own — the material beats by design; only the artifact
// counts. Gates on the shipped shape (8192+4096 crossed at 1200 Hz input,
// mirroring PoggedVocoder); the architectures that fail are report-only
// counterexamples. The residual worst case is the 4.5 Hz pair: resolving it
// would take a ~850 ms window — the ideal shift beats there too, ours beats
// somewhat deeper.
#include "../src/stream_vocoder.hpp"
#include "../src/stream_multivocoder.hpp"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <complex>

static constexpr float    SR  = 48000.0f;
static constexpr uint32_t RSZ = 65536, MASK = RSZ - 1;
static constexpr int      NH  = 10;
static constexpr float    B_STIFF = 3e-4f;   // guitar-ish string stiffness

static double fh(double f0, int h) { return h * f0 * std::sqrt(1.0 + B_STIFF * h * h); }

// One realistic string: inharmonic partials, each beating slowly (the two
// polarizations of a real string), slow decay, broadband noise floor.
static void add_string(std::vector<float>& x, double f0, unsigned seed)
{
    const int n = (int)x.size();
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u;
                       return (seed >> 8) * (1.0 / 16777216.0); };
    for (int h = 1; h <= NH; ++h) {
        const double f  = fh(f0, h);
        const double a  = 0.35 / h;
        const double br = 0.3 + 1.2 * rnd();      // beat rate, Hz
        const double bd = 0.10 + 0.10 * rnd();    // beat depth, 10-20 %
        const double ph = 2 * M_PI * rnd();
        for (int i = 0; i < n; ++i) {
            const double t = i / (double)SR;
            const double env = std::exp(-0.025 * h * t) *
                               (1.0 + bd * std::sin(2 * M_PI * br * t + ph));
            x[i] += (float)(a * env * std::sin(2 * M_PI * f * t + ph * h));
        }
    }
    for (int i = 0; i < n; ++i)
        x[i] += (float)(0.35e-2 * (rnd() - 0.5));
}

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

static double am_db(const std::vector<float>& x, float f)
{
    const int win = (int)(0.100f * SR), hop = (int)(0.010f * SR);
    double lo = 1e30, hi = 0;
    for (int i = (int)(1.5f * SR); i + win < (int)x.size(); i += hop) {
        const double a = band(x, i, win, f);
        lo = std::min(lo, a); hi = std::max(hi, a);
    }
    return 20.0 * std::log10(hi / (lo + 1e-30));
}

// Roughness (§23): modulation-index in the 5-80 Hz band of each harmonic's
// envelope — the band the ear reads as flutter/shimmer. Our AM metric's
// 100 ms window averages that away, and twice in this project the ear
// caught what AM missed; this is the second guardrail.
static double mod_index(const std::vector<float>& x, double f)
{
    const int n = (int)x.size();
    const int i0 = (int)(1.5 * SR), i1 = n - (int)(0.2 * SR);
    const double a = 1.0 - std::exp(-2.0 * M_PI * 80.0 / SR);
    std::complex<double> lp = 0.0;
    const int DEC = 16;
    std::vector<double> env;
    env.reserve((i1 - i0) / DEC + 1);
    for (int i = i0; i < i1; ++i) {
        const double t = i / (double)SR;
        lp += a * ((double)x[i] * std::exp(std::complex<double>(0.0,
                     -2.0 * M_PI * f * t)) - lp);
        if ((i - i0) % DEC == 0) env.push_back(std::abs(lp));
    }
    const double fs_e = SR / DEC;
    double mean = 0; for (double e : env) mean += e; mean /= env.size();
    double pw = 0.0;
    for (double fm = 5.0; fm <= 80.0; fm += 2.5) {
        const double w = 2 * M_PI * fm / fs_e, c = 2 * std::cos(w);
        double s0, s1 = 0, s2 = 0;
        for (double e : env) { s0 = (e - mean) + c * s1 - s2; s2 = s1; s1 = s0; }
        pw += (s1 * s1 + s2 * s2 - c * s1 * s2) / (env.size() * (double)env.size());
    }
    return std::sqrt(pw) / (mean + 1e-30);
}

struct Excess { double worst, mean; };

template <class Engine>
static Excess excess(Engine& eng, const std::vector<float>& in,
                     const std::vector<float>& ideal)
{
    eng.set_ratio(2.0f);
    eng.reset();
    std::vector<float> ring(RSZ, 0.0f), out(in.size());
    uint64_t wpos = RSZ;
    for (size_t i = 0; i < in.size(); ++i) {
        ring[wpos & MASK] = in[i];
        ++wpos;
        out[i] = eng.process(ring.data(), MASK, wpos);
    }
    double worst = 0, sum = 0; int cnt = 0;
    for (double f0 : { 110.0, 138.59 })
        for (int h = 1; h <= NH; ++h) {
            const float f = (float)(2.0 * fh(f0, h));
            const double ex = am_db(out, f) - am_db(ideal, f);
            worst = std::max(worst, ex); sum += ex; ++cnt;
        }
    return { worst, sum / cnt };
}

int main()
{
    const int n = (int)(6.0f * SR);
    std::vector<float> in(n, 0.0f), ideal(n, 0.0f);
    add_string(in, 110.0, 111);    add_string(in, 138.59, 222);
    add_string(ideal, 220.0, 111); add_string(ideal, 277.18, 222);

    std::printf("══ shimmer on a realistic major third, x2 (§16) ══\n");

    // The shipped §20 shape: 4096+2048 at the fixed 250 Hz output crossover
    // — the latency budget the user ruled usable. Its collision shimmer is
    // the §20 program's target: the gates are a RATCHET set just above
    // today's measurement, so any stability work must move them DOWN and
    // nothing may quietly regress. The 8192 shape is kept as the purity
    // reference — what §16 measured when latency was free.
    static MultiVocoder<4096, 2048, 8> shipped;   // OS=8 as wired (§21)
    shipped.init(SR);
    shipped.set_xover(250.0f);
    shipped.tune(0.20f, 1.0f);   // §20: long-window smoothing only, as wired
    const Excess s = excess(shipped, in, ideal);
    bool ok = s.mean < 19.0 && s.worst < 73.0;
    std::printf("  shipped 4096+2048 @ xout 250: excess AM mean %+.2f dB (< 19),"
                " worst %+.1f dB (< 73)  [RATCHET, §20]%s\n",
                s.mean, s.worst, ok ? "  ok" : "  ** FAIL");

    // §23 guardrails learned from the Spike-8 post-mortem.
    // 1. Roughness (ear proxy) vs the ideal, ratcheted like the AM metric.
    // 2. Per-harmonic PARITY vs the parametric-off engine: the worst LOCAL
    //    regression the §22 engagement causes. Report-only for now (Spike 7
    //    itself trades a few harmonics), but it must be looked at on every
    //    engine change — the Spike-8 failure was shipping a mean improvement
    //    while this number quietly grew.
    {
        auto render = [&](auto& e) {
            e.set_ratio(2.0f); e.reset();
            std::vector<float> ring(RSZ, 0.0f), out(in.size());
            uint64_t wpos = RSZ;
            for (size_t i = 0; i < in.size(); ++i) {
                ring[wpos & MASK] = in[i]; ++wpos;
                out[i] = e.process(ring.data(), MASK, wpos);
            }
            return out;
        };
        const auto o_on = render(shipped);
        double rworst = -1e30, rsum = 0; int rcnt = 0;
        for (double f0 : { 110.0, 138.59 })
            for (int h = 1; h <= NH; ++h) {
                const double f = 2.0 * fh(f0, h);
                if (f > 0.45 * SR) continue;
                const double ex = 20.0 * std::log10(
                    (mod_index(o_on, f) + 1e-9) / (mod_index(ideal, f) + 1e-9));
                rworst = std::max(rworst, ex); rsum += ex; ++rcnt;
            }
        const bool rok = (rsum / rcnt) < 2.0 && rworst < 14.0;
        ok = ok && rok;
        std::printf("  roughness (5-80 Hz, ear proxy): mean %+.2f dB (< 2),"
                    " worst %+.1f dB (< 14)  [RATCHET, §23]%s\n",
                    rsum / rcnt, rworst, rok ? "  ok" : "  ** FAIL");

        static MultiVocoder<4096, 2048, 8> off_eng;
        off_eng.init(SR);
        off_eng.set_xover(250.0f);
        off_eng.tune(0.20f, 1.0f);
        off_eng.prony(false, false);
        const auto o_off = render(off_eng);
        double par = -1e30; double pf = 0; int ph_ = 0;
        for (double f0 : { 110.0, 138.59 })
            for (int h = 1; h <= NH; ++h) {
                const float f = (float)(2.0 * fh(f0, h));
                const double d = am_db(o_on, f) - am_db(o_off, f);
                if (d > par) { par = d; pf = f0; ph_ = h; }
            }
        std::printf("  [§23 parity, report-only] worst per-harmonic regression"
                    " of §22 vs OFF: %+.1f dB (%s h%d)\n",
                    par, pf > 120.0 ? "C#3" : "A2", ph_);
    }

    // The out-of-budget purity reference (§16), report-only.
    static MultiVocoder<8192, 4096> pure;
    pure.init(SR);
    pure.set_xover(1200.0f * 2.0f);
    pure.prony(false, false);   // §22 needs quasi-stationary history; at this
                                // shape (341/171 ms) it is not — keep the §16
                                // reference as §16 measured it
    const Excess o = excess(pure, in, ideal);
    std::printf("  [reference, report-only] 8192+4096 @ xin 1200 (171/85 ms): "
                "mean %+.2f dB, worst %+.1f dB\n", o.mean, o.worst);

    std::printf("shimmer_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
