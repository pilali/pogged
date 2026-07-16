// arpeggio_test — THE decisive test for the experimental branch (design §1.1).
//
// The POG3's signature that the block-FFT vocoder cannot reproduce: attacking a
// new note in an arpeggio does NOT modify the resonance of the notes already
// ringing. The streaming phase vocoder re-analyses the whole spectrum every
// hop, re-picks its peaks and re-partitions its regions between them, so when a
// second note appears the FIRST note's partials change region and phasor — its
// shifted output moves. A per-channel continuous engine (the filter bank) has
// no global partition: note A lives in its channels, note B in others, and A is
// untouched.
//
// Method: hold note A alone, let it settle, measure the amplitude of its
// SHIFTED fundamental. Then attack note B (a fifth up, cleanly resolved from A)
// and measure A's shifted fundamental again. The disturbance is the dB change.
// A is at 55 Hz shifted (110 Hz played, -1 oct); B lands at 82.5 Hz, so a
// Goertzel at 55 Hz sees ONLY A — any change is the engine disturbing A.
//
// Both engines are driven off a private ring, as in stagger_test/filterbank_test.
#include "../src/stream_filterbank.hpp"
#ifndef POGGED_NO_VOCODER
#include "../src/stream_vocoder.hpp"
#endif
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float    SR   = 48000.0f;
static constexpr int      N    = (int)SR * 3;         // A alone 0-1.5s, A+B after
static constexpr int      T_B  = (int)(1.5f * SR);    // note B onset
static constexpr uint32_t RSZ  = 65536, MASK = RSZ - 1;

static constexpr float F_A = 110.0f;   // A2 played
static constexpr float F_B = 165.0f;   // E3 played, a fifth up
static constexpr float RATIO = 0.5f;   // sub -1 oct
static constexpr float F_A_SUB = F_A * RATIO;   // 55 Hz — measured, A only

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

// Disturbance of A's shifted fundamental, in dB, across B's onset. Windows are
// placed well clear of both settling and B's onset transient, and offset by
// each engine's latency so "after" truly reflects the A+B input, not leftover A.
template <class Engine>
static double disturbance(const std::vector<float>& in, int latency, const char* tag)
{
    static Engine eng;
    eng.init(SR); eng.set_ratio(RATIO); eng.reset();
    std::vector<float> ring(RSZ, 0.0f), out(N, 0.0f);
    uint64_t wpos = RSZ;
    for (int i = 0; i < N; ++i) {
        ring[wpos & MASK] = in[i];
        ++wpos;
        out[i] = eng.process(ring.data(), MASK, wpos);
    }

    const int wlen = (int)(0.30f * SR);
    // Before: A alone, ending 50 ms before B's onset (plus latency).
    const int b_start = T_B + latency - wlen - (int)(0.05f * SR);
    // After: A+B, starting 150 ms after B's onset (plus latency) so B's attack
    // transient has fully settled.
    const int a_start = T_B + latency + (int)(0.15f * SR);

    const double before = goertzel(out, b_start, wlen, F_A_SUB);
    const double after  = goertzel(out, a_start, wlen, F_A_SUB);
    const double dist   = 20.0 * std::log10((after + 1e-30) / (before + 1e-30));
    std::printf("  %-11s A-sub before B: %.4f, after B: %.4f  -> %+.2f dB\n",
                tag, before, after, dist);
    return std::fabs(dist);
}

int main()
{
    std::printf("══ arpeggio: a new attack must not move ringing notes (§1.1) ══\n");

    // A rings the whole time; B enters at T_B with an instantaneous onset.
    std::vector<float> in(N, 0.0f);
    for (int i = 0; i < N; ++i) {
        const double t = (double)i / SR;
        double s = 0.5 * std::sin(2 * M_PI * F_A * t);
        if (i >= T_B) s += 0.5 * std::sin(2 * M_PI * F_B * t);
        in[i] = (float)s;
    }

    bool ok = true;

    // Filter bank: latency is the bass channel group delay (~40 ms at 55 Hz).
    const double fb = disturbance<StreamFilterbank>(in, (int)(0.045f * SR),
                                                    "filterbank");
    // The branch's DECISIVE criterion, and Spike 2 essentially closes it:
    //   naive overlap-sum resynthesis  : A dropped 6.1 dB when B attacked
    //   + one oscillator per partial    : 1.2 dB   (Spike 1)
    //   + phase handoff on channel swap : 0.45 dB  (Spike 2) — vocoder-grade
    // Every overlapping channel carrying A used to drift apart under B's
    // leakage; collapsing A onto one dominant channel and handing its phase
    // across channel crossings keeps it rock-steady when B lands (design §1.1).
    const bool fb_ok = fb < 0.6;
    ok &= fb_ok;

#ifndef POGGED_NO_VOCODER
    const double pv = disturbance<StreamVocoder>(in, StreamVocoder::N, "vocoder");
    std::printf("  -> filter bank disturbs A by %.2f dB (< 0.6), vocoder by %.2f dB\n",
                fb, pv);
    // HONEST CAVEAT, kept visible: on this clean TWO-tone input the vocoder's
    // re-partition is stable, so it too scores well — both are now excellent
    // here, so this metric is a floor check, NOT yet the decisive discriminator.
    // Reproducing the vocoder's real "ringing note moves" defect needs denser,
    // closer material (many partials that re-partition when B lands): Spike 3.
    std::printf("  filter bank holds A within 0.6 dB  %s   "
                "(both engines clean on this easy input — see caveat in source)\n",
                fb_ok ? "ok" : "WRONG");
#else
    std::printf("  filter bank disturbs A by %.2f dB (< 0.6)  %s\n",
                fb, fb_ok ? "ok" : "WRONG");
#endif

    std::printf("arpeggio_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
