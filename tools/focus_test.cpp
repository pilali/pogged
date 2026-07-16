// focus_test — the FOCUS engine switch (granular <-> streaming vocoder).
//
// The measurement here is the one the earlier ripple tests got wrong, so it is
// spelled out: a chord of non-harmonic partials beats on its own (E major's
// partials sit 42.8 Hz apart -> a 23 ms beat), so ripple is measured through a
// 150 ms window — past every beat period — and compared against an IDEALLY
// shifted chord rather than against zero. Through a 20 ms window a *perfect*
// shift reads ~12 dB and every engine looks broken.
//
// What is pinned:
//   1. FOCUS on removes the granular engine's chord artifact — the whole point.
//   2. Switching engines does not click, despite the two having very different
//      latencies (3 ms vs 85 ms), which a hard switch would jump.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float SR = 48000.0f;
static constexpr int   N  = (int)SR * 4, BLOCK = 256;

static void chord_into(std::vector<float>& v, double f1, double f2, double f3)
{
    v.assign(N, 0.0f);
    for (int i = 0; i < N; ++i) {
        const double t = (double)i / SR;
        v[i] = 0.22 * (std::sin(2*M_PI*f1*t) + std::sin(2*M_PI*f2*t)
                     + std::sin(2*M_PI*f3*t));
    }
}

// 150 ms window: longer than any beat period in the chord.
static double ripple_db(const std::vector<float>& y, int from)
{
    const int win = (int)(0.150f*SR), hop = (int)(0.010f*SR);
    double rmin = 1e30, rmax = 0;
    for (int i = from; i + win < (int)y.size(); i += hop) {
        double e = 0; for (int j = 0; j < win; ++j) e += (double)y[i+j]*y[i+j];
        const double r = std::sqrt(e/win);
        rmin = std::min(rmin, r); rmax = std::max(rmax, r);
    }
    return 20.0 * std::log10(rmax / (rmin + 1e-30));
}

static void render(const std::vector<float>& in, float focus,
                   std::vector<float>& l, std::vector<float>& r)
{
    PoggedDsp* d = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.out_level  = 1.0f;
    p.input_gain = 1.0f;    // 0 would clamp to 0.5: a silent 6 dB cut
    p.lp_cutoff  = 20000.0f;
    p.lp_q       = 0.707f;
    p.attack_sens = 0.35f; p.sub1_level = 1.0f; p.focus = focus;
    l.assign(N, 0.0f); r.assign(N, 0.0f);
    for (int i = 0; i < N; i += BLOCK)
        pogged_dsp_process(d, &p, in.data()+i, l.data()+i, r.data()+i,
                           std::min(BLOCK, N-i));
    pogged_dsp_free(d);
}

int main()
{
    std::printf("══ FOCUS engine switch ══\n");
    bool ok = true;

    std::vector<float> chord, ideal, l, r;
    chord_into(chord, 164.81, 207.65, 246.94);          // E major
    chord_into(ideal,  82.41, 103.83, 123.47);          // the same, an octave down
    const double floor_db = ripple_db(ideal, (int)SR);

#ifdef POGGED_NO_VOCODER
    std::printf("  vocoder compiled out (POGGED_NO_VOCODER) — granular only\n");
    render(chord, 0.0f, l, r);
    std::printf("  granular: %.1f dB (ideal floor %.1f)\n",
                ripple_db(l, (int)SR), floor_db);
    std::printf("focus_test: PASS (nothing to switch)\n");
    return 0;
#else
    render(chord, 0.0f, l, r);
    const double gran = ripple_db(l, (int)SR);
    render(chord, 1.0f, l, r);
    const double voc  = ripple_db(l, (int)SR);

    // 1. The vocoder must land near the ideal floor, and clearly beat granular.
    const bool clean = (voc - floor_db) < 1.0 && voc < gran - 2.0;
    std::printf("  sub -1 on a chord: ideal floor %.1f | granular %.1f (+%.1f) "
                "| FOCUS %.1f (+%.1f)  %s\n",
                floor_db, gran, gran - floor_db, voc, voc - floor_db,
                clean ? "ok" : "WRONG");
    ok &= clean;

    // 2. Switching mid-signal must not click. The engines differ by ~82 ms of
    //    latency, so this is only smooth because of the crossfade.
    PoggedDsp* d = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.out_level  = 1.0f;
    p.input_gain = 1.0f;
    p.lp_cutoff  = 20000.0f;
    p.lp_q       = 0.707f;
    p.attack_sens = 0.35f; p.sub1_level = 1.0f; p.focus = 0.0f;
    std::vector<float> ol(N), orr(N);
    for (int i = 0; i < N; i += BLOCK) {
        if (i > N/2) p.focus = 1.0f;                    // flip mid-render
        pogged_dsp_process(d, &p, chord.data()+i, ol.data()+i, orr.data()+i,
                           std::min(BLOCK, N-i));
    }
    pogged_dsp_free(d);
    double mx = 0.0;
    for (int i = (int)SR; i < N; ++i)
        mx = std::max(mx, (double)std::abs(ol[i] - ol[i-1]));
    double in_mx = 0.0;
    for (int i = 1; i < N; ++i)
        in_mx = std::max(in_mx, (double)std::abs(chord[i] - chord[i-1]));
    const bool smooth = mx <= 20.0 * in_mx;
    std::printf("  engine flip mid-signal: max sample delta %.4f "
                "(limit %.4f = 20x input slope)  %s\n",
                mx, 20.0*in_mx, smooth ? "ok" : "WRONG");
    ok &= smooth;

    std::printf("focus_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
#endif
}
