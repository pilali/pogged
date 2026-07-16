// stagger_test — the vocoder's hop phase must NOT move a voice's latency.
//
// Each vocoder voice does its 2 FFTs in one burst every HOP samples. With every
// voice starting its hop counter at 0 they fire in lockstep, so all of them pile
// into the SAME audio block: measured on a Pi 5 with 8 voices and 128-sample
// blocks, the worst-case block hit 128% of its deadline (xruns) while the median
// sat at 2%. Average load was never the problem; its shape was. Voices are
// therefore given staggered hop phases so at most one fires per block (peak 43%).
//
// That is only legitimate because the phase does not move WHEN a voice's output
// comes out — otherwise the voices would drift apart from each other by up to a
// hop (21 ms at N=4096) and the "free" saving would be a smeared octave. This
// pins that invariant down, since nothing else would catch it: the sound of any
// ONE voice is unchanged, so every other test in the suite stays green while the
// voices slide apart.
//
// Measured at ratio 1.0 and against the INPUT: at unity the vocoder reproduces
// its input, so the correlation peak IS the latency, in absolute samples. (An
// earlier version of this test correlated two SHIFTED outputs against each other
// and reported bogus lags — with different frame times they pick different peaks
// and genuinely differ, so there is no peak to find. The printed r is the guard:
// a meaningless correlation cannot pass unnoticed.)
#include "../src/stream_vocoder.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static constexpr float    SR   = 48000.0f;
static constexpr int      N    = (int)SR * 6;
static constexpr uint32_t RSZ  = 32768, MASK = RSZ - 1;

int main()
{
    std::printf("══ vocoder hop stagger ══\n");

    // Deterministic noise: a fixed LCG, so the correlation peak is unambiguous
    // (a sine would correlate at every period) and the test is reproducible.
    std::vector<float> ring(RSZ, 0.0f), in(N);
    uint32_t seed = 999u;
    for (int i = 0; i < N; ++i) {
        seed = seed * 1664525u + 1013904223u;
        in[i] = 0.3f * ((float)(seed >> 8) / 8388608.0f - 1.0f);
    }

    const int PH[4] = { 0, StreamVocoder::HOP / 4, StreamVocoder::HOP / 2,
                        3 * StreamVocoder::HOP / 4 };
    std::vector<std::vector<float>> out(4, std::vector<float>(N, 0.0f));
    static StreamVocoder pv[4];
    for (int k = 0; k < 4; ++k) { pv[k].init(SR, PH[k]); pv[k].set_ratio(1.0f); }

    uint64_t wpos = 0;
    for (int i = 0; i < N; ++i) {
        ring[wpos & MASK] = in[i];
        ++wpos;
        for (int k = 0; k < 4; ++k)
            out[k][i] = pv[k].process(ring.data(), MASK, wpos);
    }

    bool ok = true;
    int  lat[4] = {};
    for (int k = 0; k < 4; ++k) {
        int    best_d = 0;
        double best_c = -1e30, e_in = 0.0, e_out = 0.0;
        for (int d = 0; d <= 2 * StreamVocoder::N; ++d) {
            double c = 0.0;
            for (int i = (int)SR * 3; i < (int)SR * 5; i += 3)
                c += (double)in[i] * out[k][i + d];
            if (c > best_c) { best_c = c; best_d = d; }
        }
        for (int i = (int)SR * 3; i < (int)SR * 5; i += 3) {
            e_in  += (double)in[i] * in[i];
            e_out += (double)out[k][i + best_d] * out[k][i + best_d];
        }
        const double r = best_c / std::sqrt(e_in * e_out + 1e-30);
        lat[k] = best_d;
        // r must be near 1: at unity ratio the vocoder is a pure delay. A low r
        // would mean the latency below was read off noise.
        const bool r_ok = r > 0.9;
        std::printf("  hop phase %4d -> latency %5d samples (%5.1f ms)  r=%.2f%s\n",
                    PH[k], best_d, 1000.0 * best_d / SR, r,
                    r_ok ? "" : "  <-- correlation too weak to trust");
        ok &= r_ok;
    }

    int spread = 0;
    for (int k = 1; k < 4; ++k) spread = std::max(spread, std::abs(lat[k] - lat[0]));
    const bool aligned = spread == 0;
    std::printf("  inter-voice latency spread: %d samples (%.1f ms) — must be 0 "
                "or the voices drift apart  %s\n",
                spread, 1000.0 * spread / SR, aligned ? "ok" : "WRONG");
    ok &= aligned;

    std::printf("stagger_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
