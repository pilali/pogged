// filter_test — verifies the POG3 multimode filter and its envelope sweep.
//
// One probe tone throughout: a 220 Hz input makes the +1 voice sing at 440 Hz
// (dry muted, so only the filtered wet path reaches the output). Moving the
// filter's frequency around that fixed 440 Hz probe exercises every mode with
// a single measurement, and the LP-at-20kHz bypass gives the unfiltered
// reference to compare against.
//
//   LP  low  cutoff -> 440 blocked      LP  high cutoff -> 440 passes
//   HP  high cutoff -> 440 blocked      HP  low  cutoff -> 440 passes
//   BP  far  centre -> 440 blocked      BP  on   centre -> 440 passes
//
// The sweep is checked the same way: with the LP parked at 200 Hz the probe is
// blocked, and turning ENV up must open the filter over it on the pick.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float SR    = 48000.0f;
static constexpr float FIN   = 220.0f;
static constexpr float PROBE = 440.0f;          // the +1 voice
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
    const double p = s1 * s1 + s2 * s2 - cw * s1 * s2;
    return 10.0 * std::log10(p + 1e-30);
}

// Renders and returns the 440 Hz level over [from_s, to_s). The window is an
// explicit argument: the sweep has to be read while it is open, and comparing
// it against a control read over a different window would compare nothing
// (Goertzel is not normalised by length).
static double level(int mode, float cutoff, float env_depth,
                    float from_s, float to_s)
{
    const int N = (int)(SR * 3);
    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.up1_level    = 1.0f;              // dry muted: only the wet path is heard
    p.out_level    = 1.0f;
    p.input_gain  = 1.0f;   // 0 would clamp to 0.5: a silent 6 dB cut
    p.lp_cutoff    = cutoff;
    p.lp_q         = 0.707f;
    p.attack_sens  = 0.35f;
    p.filter_mode  = (float)mode;
    p.filter_env   = env_depth;
    p.filter_env_a = 50.0f;
    p.filter_env_d = 2000.0f;
    p.filter_sens  = 0.5f;

    std::vector<float> in(N, 0.0f), l(N), r(N);
    // 0.2 s of silence first, so the onset detector sees a real attack.
    for (int i = (int)(0.2f * SR); i < N; ++i)
        in[i] = 0.5f * std::sin(2.0 * M_PI * FIN * i / SR);
    for (int i = 0; i < N; i += BLOCK)
        pogged_dsp_process(dsp, &p, in.data() + i, l.data() + i, r.data() + i,
                           std::min(BLOCK, N - i));
    pogged_dsp_free(dsp);

    return goertzel_db(l, (int)(SR * from_s), (int)(SR * to_s), PROBE);
}

int main()
{
    std::printf("══ multimode filter ══\n");
    bool ok = true;

    // Reference: LP at 20 kHz is the bypass, i.e. the unfiltered voice.
    const double ref = level(0, 20000.0f, 0.0f, 1.5f, 3.0f);
    std::printf("  reference (LP 20k = bypass): %.1f dB\n", ref);

    struct Case { const char* name; int mode; float cutoff; bool should_pass; };
    const Case cases[] = {
        { "LP 5 kHz  (probe below)", 0, 5000.0f, true  },
        { "LP 100 Hz (probe above)", 0,  100.0f, false },
        { "HP 100 Hz (probe above)", 2,  100.0f, true  },
        { "HP 5 kHz  (probe below)", 2, 5000.0f, false },
        { "BP 440 Hz (on centre)",   1,  440.0f, true  },
        { "BP 10 kHz (far off)",     1, 10000.0f, false },
    };
    for (const Case& c : cases) {
        const double d = level(c.mode, c.cutoff, 0.0f, 1.5f, 3.0f) - ref;
        // Pass: within 3 dB of unfiltered. Stop: at least 20 dB down.
        const bool good = c.should_pass ? (d > -3.0) : (d < -20.0);
        std::printf("  %-24s %+6.1f dB vs ref -> %-6s %s\n", c.name, d,
                    c.should_pass ? "pass" : "stop", good ? "ok" : "WRONG");
        ok &= good;
    }

    // Envelope: LP parked at 200 Hz blocks the 440 probe; ENV up must sweep
    // the filter over it on the pick.
    const double shut  = level(0, 200.0f, 0.0f, 0.28f, 0.55f);
    const double swept = level(0, 200.0f, 1.0f, 0.28f, 0.55f);
    const bool env_ok = (swept - shut) > 12.0;
    std::printf("  ENV sweep: LP 200 Hz shut %.1f dB -> ENV +1 %.1f dB "
                "(lift %+.1f dB, want > +12)  %s\n",
                shut, swept, swept - shut, env_ok ? "ok" : "WRONG");
    ok &= env_ok;

    std::printf("filter_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
