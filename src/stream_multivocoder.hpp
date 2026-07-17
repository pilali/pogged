#pragma once
#include "stream_vocoder.hpp"
#include "biquad.hpp"
#include <cstdint>

// Multi-resolution phase vocoder — design §13 (the "tight attack" fix).
//
// The single-window vocoder must choose ONE latency for every frequency: a long
// window (N=4096, ~85 ms) resolves low chords but arrives so late that the
// pitched voices feel soft under the zero-latency dry; a short window (N=2048,
// ~42 ms) tightens the attack but blurs the bass. The ear preferred the short
// one but heard the bass blur.
//
// So run BOTH and split by frequency:
//   - a LONG window carries the BASS  (resolved low chords / subs);
//   - a SHORT window carries the TREBLE and the ATTACK transients (tight).
// Both read the shared ring; their outputs cross over (Linkwitz-Riley 4th order)
// and sum — lows from the long path, highs from the short path. Frequency-
// dependent latency falls out for free: the bass pays the long window (~85 ms,
// forgiven down there) while attacks come through the short window (~42 ms), so
// the pitched voices sit tighter under the dry without losing bass resolution.
//
// The two paths run at different latencies, so near the crossover there is a
// small time offset; XOVER sits low (a spectral region with little transient
// energy) to keep that inaudible. Output crossover (not input) keeps the shared-
// ring interface intact: each sub-vocoder analyses the full band and we simply
// discard the half we do not use. Same interface as StreamVocoder.

template <int N_LO = 4096, int N_HI = 2048>
class MultiVocoder {
public:
    static constexpr int   N     = N_LO;      // reported window (the long one)
    // Stagger stride, for interface parity with StreamVocoder: the caller
    // spreads its instances' FFT bursts over [0, HOP). The long window's hop
    // is the coarser grid; the short window folds the same phase into its own.
    static constexpr int   HOP   = StreamVocoderT<N_LO>::HOP;
    static constexpr float XOVER = 250.0f;    // crossover frequency, Hz

    void init(double sr, int hop_phase = 0) noexcept {
        _lo.init(sr, hop_phase);
        // Half a short hop apart: with the SAME phase, every long-window burst
        // lands in the same audio block as one of the short window's (HOP_LO
        // is a multiple of HOP_HI), and the per-block peak — the number that
        // has to fit the deadline — nearly doubles. The offset moves only WHEN
        // the short window works, nothing about its sound (same invariant the
        // stagger relies on, pinned by stagger_test). Measured worst block for
        // 8 voices at 128 samples: 31% of the deadline in phase, 22% offset.
        _hi.init(sr, hop_phase + StreamVocoderT<N_HI>::HOP / 2);
        // Linkwitz-Riley 4th order = two cascaded Butterworth (Q=0.707) each
        // side; LP+HP then sum flat with no phase notch at XOVER.
        for (auto& b : _lp) b.setup(Biquad::LP, XOVER, 0.707f, (float)sr);
        for (auto& b : _hp) b.setup(Biquad::HP, XOVER, 0.707f, (float)sr);
        reset();
    }

    void reset() noexcept {
        _lo.reset();
        _hi.reset();
        for (auto& b : _lp) b.reset();
        for (auto& b : _hp) b.reset();
    }

    void set_ratio(float ratio) noexcept {
        _lo.set_ratio(ratio);
        _hi.set_ratio(ratio);
    }

    // Per-bin attack swell (§14), forwarded to both windows. The time constant
    // is in milliseconds, so the two hop rates advance their envelopes at the
    // same real-time speed and the crossover region swells coherently.
    void set_swell(float atk_ms) noexcept {
        _lo.set_swell(atk_ms);
        _hi.set_swell(atk_ms);
    }

    // One shifted sample. Both sub-vocoders read the shared ring; the crossover
    // keeps the long path's lows and the short path's highs.
    float process(const float* ring, uint32_t mask, uint64_t wpos) noexcept {
        float l = _lo.process(ring, mask, wpos);
        float h = _hi.process(ring, mask, wpos);
        for (auto& b : _lp) l = b.process(l);
        for (auto& b : _hp) h = b.process(h);
        return l + h;
    }

private:
    StreamVocoderT<N_LO> _lo;   // bass, resolved
    StreamVocoderT<N_HI> _hi;   // treble + attacks, tight
    Biquad _lp[2], _hp[2];      // LR4 crossover on the outputs
};
