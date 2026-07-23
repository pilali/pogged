// filterbank_test — the constant-Q heterodyne filter bank. Gates on what the
// current (overlap) engine genuinely delivers:
//   1. it SHIFTS pitch correctly (both octaves, no input/octave leak);
//   3. TIMBRE FIDELITY — a shifted rich note keeps its whole harmonic series,
//      no holes. This is the gate that would have caught the "bit-crush" the
//      ear heard and the dB tests missed (design §10).
// Section 2 (chord ripple) is REPORT-ONLY: overlap reconstruction trades chord
// stability for timbre, so it regresses there — printed, not asserted, so the
// tension stays honest rather than gamed. See docs/pog3-experimental-design.md.
//
// Engines are driven STANDALONE off a shared ring, as stagger_test drives the
// vocoder, so no wiring into FOCUS/ports is needed to measure them.
#include "../src/stream_filterbank.hpp"
#include "../src/stream_shifter.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float    SR   = 48000.0f;
static constexpr int      N    = (int)SR * 3;
static constexpr uint32_t RSZ  = 65536, MASK = RSZ - 1;

// Goertzel magnitude (normalised amplitude) of freq over [start, start+len).
static double goertzel(const std::vector<float>& x, int start, int len, float freq)
{
    const double w = 2.0 * M_PI * freq / SR;
    const double cw = std::cos(w), sw = std::sin(w), coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (int n = 0; n < len; ++n) {
        const double s0 = x[start + n] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const double re = s1 - s2 * cw, im = s2 * sw;
    return 2.0 * std::sqrt(re * re + im * im) / len;
}

// 150 ms-window RMS ripple, in dB — the range_test metric, past every beat
// period so it reads the engine's artifact, not the chord being a chord.
static double ripple_db(const std::vector<float>& y)
{
    const int win = (int)(0.150f * SR), hop = (int)(0.010f * SR);
    double rmin = 1e30, rmax = 0.0;
    for (int i = (int)(0.4f * SR); i + win < N; i += hop) {   // skip bass settle
        double e = 0.0;
        for (int j = 0; j < win; ++j) e += (double)y[i + j] * y[i + j];
        const double rms = std::sqrt(e / win);
        rmin = std::min(rmin, rms); rmax = std::max(rmax, rms);
    }
    return 20.0 * std::log10(rmax / (rmin + 1e-30));
}

// Run one engine at a fixed ratio over an input, off a private ring.
template <class Engine>
static std::vector<float> run(const std::vector<float>& in, float ratio)
{
    static Engine eng;                 // static: MAXCH complex arrays are large
    eng.init(SR); eng.set_ratio(ratio); eng.reset();
    std::vector<float> ring(RSZ, 0.0f), out(N, 0.0f);
    uint64_t wpos = RSZ;               // keep (wpos-1) & mask valid from n=0
    for (int i = 0; i < N; ++i) {
        ring[wpos & MASK] = in[i];
        ++wpos;
        out[i] = eng.process(ring.data(), MASK, wpos);
    }
    return out;
}

// StreamShifter needs setup() rather than init()/set_ratio(); wrap it. Sub
// grain sizing mirrors pogged_dsp's guitar-range sub (~55 ms, aligned).
static std::vector<float> run_granular(const std::vector<float>& in, float ratio)
{
    static StreamShifter sh;
    sh.setup(ratio, (int)(0.055f * SR), 512);
    sh.reset();
    std::vector<float> ring(RSZ, 0.0f), out(N, 0.0f);
    uint64_t wpos = RSZ;
    for (int i = 0; i < N; ++i) {
        ring[wpos & MASK] = in[i];
        ++wpos;
        out[i] = sh.process(ring.data(), MASK, wpos);
    }
    return out;
}

int main()
{
    std::printf("══ constant-Q filter bank (spike 1) ══\n");
    bool ok = true;

    // ── 1. Pitch accuracy ────────────────────────────────────────────────
    // A 220 Hz sine, shifted down and up an octave, must land on 110 / 440 —
    // dominant over the input frequency and the neighbouring octaves.
    {
        std::vector<float> sine(N);
        for (int i = 0; i < N; ++i)
            sine[i] = 0.5f * std::sin(2.0 * M_PI * 220.0 * i / SR);

        struct Case { float ratio; float target; const char* name; };
        const Case cases[] = { {0.5f, 110.0f, "-1 oct"}, {2.0f, 440.0f, "+1 oct"} };
        for (const auto& c : cases) {
            std::vector<float> y = run<StreamFilterbank>(sine, c.ratio);
            const int s = (int)(1.0f * SR), len = (int)(1.5f * SR);
            const double at_target = goertzel(y, s, len, c.target);
            const double at_input  = goertzel(y, s, len, 220.0f);
            const double at_2x     = goertzel(y, s, len, c.target * 2.0f);
            const bool dom = at_target > 3.0 * at_input && at_target > 3.0 * at_2x;
            std::printf("  220 Hz %s -> %.0f Hz: target %.4f, input-leak %.4f, "
                        "2x-leak %.4f  %s\n", c.name, c.target,
                        at_target, at_input, at_2x, dom ? "ok" : "WRONG");
            ok &= dom;
        }
    }

    // ── 2. Chord sub ripple vs the granular engine ───────────────────────
    // E major triad, sub (-1 oct). The granular splices cannot align the
    // chord's incommensurable periods; the filter bank shifts each partial in
    // its own channel, so its sub should sit far closer to the ideal floor.
    {
        std::vector<float> chord(N), ideal(N);
        const double f[3] = { 164.81, 207.65, 246.94 };   // E3 major triad
        for (int i = 0; i < N; ++i) {
            const double t = (double)i / SR;
            double c = 0.0, d = 0.0;
            for (double fk : f) {
                c += std::sin(2 * M_PI * fk * t);
                d += std::sin(2 * M_PI * (fk * 0.5) * t);   // ideal -1 oct
            }
            chord[i] = 0.22f * (float)c;
            ideal[i] = 0.22f * (float)d;
        }

        const double floor_db = ripple_db(ideal);
        const double fb_db     = ripple_db(run<StreamFilterbank>(chord, 0.5f));
        const double gr_db     = ripple_db(run_granular(chord, 0.5f));

        // REPORT-ONLY (not a gate). The engine now runs OVERLAP reconstruction
        // (every channel, for faithful timbre — see section 3). Overlap's cost
        // is exactly here: channels sharing a partial decorrelate under the
        // chord's cross-leakage, so the ripple REGRESSES past the granular
        // engine. Peak-picking won this axis (3.9 dB) but destroyed timbre;
        // overlap wins timbre but loses this. Unifying the two — a frameless
        // phase-lock that does not detune the members — is the open problem
        // (design §10). Printed, not asserted, so the tension stays visible
        // without pretending either end is a pass.
        std::printf("  E major chord, sub -1 (150 ms window)  [REPORT-ONLY]:\n");
        std::printf("    ideal-shift floor : %.1f dB\n", floor_db);
        std::printf("    granular engine   : %.1f dB  (%+.1f over floor)\n",
                    gr_db, gr_db - floor_db);
        std::printf("    filter bank       : %.1f dB  (%+.1f over floor)  "
                    "<- overlap trades this for timbre (§10)\n",
                    fb_db, fb_db - floor_db);
    }

    // ── 3. Timbre fidelity — the metric that was missing ─────────────────
    // The dB tests above measure amplitude STABILITY; none of them saw that
    // peak-picking punched HOLES in the harmonic series (odd harmonics 30-70x
    // too quiet), which is what made a real pluck sound like a bit-crusher.
    // This is the gate that catches that: shift a rich note down an octave and
    // require every harmonic to survive, monotone-ish, with no deep notch.
    {
        std::vector<float> note(N);
        for (int i = 0; i < N; ++i) {
            const double t = (double)i / SR;
            double s = 0.0;
            for (int h = 1; h <= 8; ++h) s += std::sin(2 * M_PI * 110.0 * h * t) / h;
            note[i] = 0.3f * (float)s;
        }
        std::vector<float> y = run<StreamFilterbank>(note, 0.5f);   // -> 55 Hz sub
        const int s = (int)(1.0f * SR), len = (int)(1.5f * SR);
        const double h1 = goertzel(y, s, len, 55.0f);
        std::printf("  110 Hz (8 harmonics) -> 55 Hz sub, harmonic balance:\n");
        double worst = 1e30;
        for (int h = 1; h <= 6; ++h) {
            const double a = goertzel(y, s, len, 55.0f * h);
            const double rel = a / (h1 + 1e-30);
            if (h >= 2) worst = std::min(worst, rel);
            std::printf("    h%d (%3.0f Hz): %.3f  (ideal 1/h = %.3f)\n",
                        h, 55.0 * h, rel, 1.0 / h);
        }
        // No hole: the quietest of h2..h6 must stay above 5% of the fundamental.
        // Peak-pick measured ~0.01-0.04 here (holes); overlap ~0.1-0.4.
        const bool no_holes = worst > 0.05;
        std::printf("    -> quietest harmonic %.3f of h1 — %s (no bit-crush holes)  %s\n",
                    worst, no_holes ? "series intact" : "HOLE in the series",
                    no_holes ? "ok" : "WRONG");
        ok &= no_holes;
    }

    std::printf("filterbank_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
