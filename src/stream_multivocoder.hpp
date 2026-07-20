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
// Both read the shared ring; their outputs cross over (Linkwitz-Riley 8th order)
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

template <int N_LO = 4096, int N_HI = 2048, int OS = 4>
class MultiVocoder {
public:
    static constexpr int   N     = N_LO;      // reported window (the long one)
    // Stagger stride, for interface parity with StreamVocoder: the caller
    // spreads its instances' FFT bursts over [0, HOP). The long window's hop
    // is the coarser grid; the short window folds the same phase into its own.
    static constexpr int   HOP   = StreamVocoderT<N_LO, OS>::HOP;
    static constexpr float XOVER = 250.0f;    // crossover frequency, Hz

    void init(double sr, int hop_phase = 0) noexcept {
        _sr = (float)sr;
        _lo.init(sr, hop_phase);
        // Half a short hop apart: with the SAME phase, every long-window burst
        // lands in the same audio block as one of the short window's (HOP_LO
        // is a multiple of HOP_HI), and the per-block peak — the number that
        // has to fit the deadline — nearly doubles. The offset moves only WHEN
        // the short window works, nothing about its sound (same invariant the
        // stagger relies on, pinned by stagger_test). Measured worst block for
        // 8 voices at 128 samples: 31% of the deadline in phase, 22% offset.
        _hi.init(sr, hop_phase + StreamVocoderT<N_HI, OS>::HOP / 2);
        set_xover(_xover);
        reset();
    }

    // Crossover frequency, on the OUTPUT. What actually matters is the INPUT
    // side: the short window's bins can only resolve input partials above
    // ~XOVER Hz, and a voice at `ratio` puts an input partial at f on the
    // output at ratio·f. So the up voices must cross at XOVER×ratio — with
    // the default 250 an up-1 voice hands the short window output down to
    // 250 Hz, i.e. input partials down to 125 Hz, which its 23 Hz bins CANNOT
    // separate on a chord (measured: 12-25 dB of amplitude warble on two
    // partials 34 Hz apart; the long window holds 0.00 dB — design §15).
    // The down voices already satisfy the constraint at 250 (output 250 =
    // input 500), so the caller only ever RAISES this. Setup-time only: it
    // rebuilds the filters, so it is not for per-block modulation.
    void set_xover(float hz) noexcept {
        _xover = hz;
        // Linkwitz-Riley 8th order = a squared 4th-order Butterworth per side
        // (biquad Qs 0.5412 / 1.3066, twice); LP+HP still sum allpass-flat.
        // LR4 was not steep enough HERE: the short window's rendition of the
        // partials just below the crossover is the very thing the split
        // exists to discard (it can be 30 dB of warble), and at 24 dB/oct it
        // leaked back in at ~-22 dB — an audible ±0.7 dB of residual AM on
        // the up voices. 48 dB/oct buries it (measured in stability_test).
        static constexpr float BW4_Q[2] = { 0.5412f, 1.3066f };
        for (int i = 0; i < 4; ++i) {
            _lp[i].setup(Biquad::LP, _xover, BW4_Q[i & 1], _sr);
            _hp[i].setup(Biquad::HP, _xover, BW4_Q[i & 1], _sr);
        }
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

    // §20 stability tuning, per window: the two windows sit in different
    // resolution regimes (the long one partially resolves what the short one
    // merges), so their best smoothing steps differ — measured, not assumed.
    void tune(float lo_slow, float hi_slow) noexcept {
        _lo.SMOOTH_SLOW = lo_slow;
        _hi.SMOOTH_SLOW = hi_slow;
    }

    // §22 parametric resynthesis, per window. The fit history must be
    // quasi-stationary: at the shipped hops (85/43 ms of history) it is; a
    // long-window instance (8192/OS4: 341 ms) is NOT and measures worse —
    // callers with such shapes should switch it off.
    void prony(bool lo, bool hi) noexcept {
        _lo.PRONY_ON = lo;
        _hi.PRONY_ON = hi;
    }

    // §24: lower frequency bound of the long window's §22 engagement. Below
    // it the long window renders RIGID — its §22 rendition of a beating
    // fundamental pair wobbles in frequency, and through the crossover LP
    // that wobble is the loud midpoint parasite the ear caught. Above it §22
    // keeps stabilising the collision belt (whose LP leakage still shades
    // the short window's band).
    void prony_fmin(float lo_hz) noexcept { _lo.PRONY_FMIN = lo_hz; }

    // §26: cap the analysed input peaks of BOTH windows (the sub voice uses
    // this to stop halving the chord's upper partials into a dissonant tone).
    void peak_fmax(float hz) noexcept { _lo.PEAK_FMAX = hz; _hi.PEAK_FMAX = hz; }

    // §24 harness knob: _rot writeback half-width of the short window's
    // hinted renderer (see StreamVocoderT::HINT_ROTW).
    void hint_rotw(int w) noexcept { _hi.HINT_ROTW = w; }

    // §29: LONG-window-only mode for the UP voices. The output crossover uses
    // OUTPUT frequency as a proxy for which INPUT partial fed a given output
    // bin — but that proxy BREAKS for low input notes, whose harmonics reach
    // high output frequencies (>XOVER) and get routed to the SHORT window even
    // though they came from a dense low cluster the short window cannot resolve
    // (measured on the user's real take: bass harmonicity 0.77 split vs 0.86
    // long-only, treble unchanged at ~1.0). The short window only ever bought
    // ATTACK tightness, which the user ruled secondary in the bass — so the up
    // voices render entirely from the long window. This also SKIPS the short
    // FFT and the crossover biquads, so it is cheaper, not just cleaner. The
    // ~85 ms latency it costs is perceptually free down there (and authorised).
    void long_only(bool on) noexcept { _long_only = on; }

    // One shifted sample. Both sub-vocoders read the shared ring; the crossover
    // keeps the long path's lows and the short path's highs.
    float process(const float* ring, uint32_t mask, uint64_t wpos) noexcept {
        float l = _lo.process(ring, mask, wpos);
        // §29: long-only — return the long window's full-band output directly,
        // skipping the short window's FFT, the §24 hints (there is no short
        // window to hint) and the crossover biquads. Cheaper and cleaner.
        if (_long_only) return l;
        // §24: whenever the long window produced a new frame, hand its
        // resolved partial frequencies to the short window — the short
        // window renders low pairs as exact kernels instead of translating
        // their merged lobe to the pair's midpoint (the loud dissonant
        // wanderer of §24).
        if (_lo.frame_seq() != _hint_seq) {
            _hint_seq = _lo.frame_seq();
            _hi.set_hints(_lo.hint_freqs(), _lo.hint_count());
        }
        float h = _hi.process(ring, mask, wpos);
        for (auto& b : _lp) l = b.process(l);
        for (auto& b : _hp) h = b.process(h);
        return l + h;
    }

private:
    uint32_t _hint_seq = 0;
    StreamVocoderT<N_LO, OS> _lo;   // bass, resolved
    StreamVocoderT<N_HI, OS> _hi;   // treble + attacks, tight
    Biquad _lp[4], _hp[4];      // LR8 crossover on the outputs (see set_xover)
    bool   _long_only = false;  // §29: up voices render from the long window only
    float  _sr    = 48000.0f;
    float  _xover = XOVER;      // output-side; see set_xover
};

// Three-window ladder (§17): the collision belt keeps the LONG window it
// needs (Gabor), everything above it climbs down the ladder — mid partials
// on the MID window, sparkle and the attack's high-frequency snap on the
// SHORT one, at a quarter of the latency. The crossover tree is two LR8
// splits: out = LP1(lo) + HP1( LP2(mid) + HP2(hi) ). LP2+HP2 sums to an
// allpass, so the outer pair still sees a Linkwitz-Riley-flat inner sum and
// the whole tree stays allpass-flat. Same interface as StreamVocoder.
template <int N_LO = 8192, int N_MID = 4096, int N_HI = 2048>
class MultiVocoder3 {
public:
    static constexpr int N   = N_LO;
    static constexpr int HOP = StreamVocoderT<N_LO>::HOP;   // stagger stride

    void init(double sr, int hop_phase = 0) noexcept {
        _sr = (float)sr;
        _lo.init(sr, hop_phase);
        // Sub-hop offsets so the three windows' FFT bursts land in different
        // audio blocks (same invariant as MultiVocoder's half-hop, pinned by
        // stagger_test: WHEN a window works is not part of its sound).
        _mid.init(sr, hop_phase + StreamVocoderT<N_MID>::HOP / 2);
        _hi.init(sr, hop_phase + StreamVocoderT<N_HI>::HOP / 4);
        set_xover(_x1, _x2);
        reset();
    }

    void reset() noexcept {
        _lo.reset(); _mid.reset(); _hi.reset();
        for (auto& b : _lp1) b.reset();
        for (auto& b : _hp1) b.reset();
        for (auto& b : _lp2) b.reset();
        for (auto& b : _hp2) b.reset();
    }

    void set_ratio(float ratio) noexcept {
        _lo.set_ratio(ratio); _mid.set_ratio(ratio); _hi.set_ratio(ratio);
    }

    void set_swell(float atk_ms) noexcept {
        _lo.set_swell(atk_ms); _mid.set_swell(atk_ms); _hi.set_swell(atk_ms);
    }

    // Output-side crossovers, lo|mid at x1 and mid|hi at x2 — same
    // input-referred logic as MultiVocoder::set_xover: each window may only
    // carry output made from input partials it can resolve. Setup-time only.
    void set_xover(float x1, float x2) noexcept {
        _x1 = x1; _x2 = x2;
        static constexpr float BW4_Q[2] = { 0.5412f, 1.3066f };
        for (int i = 0; i < 4; ++i) {
            _lp1[i].setup(Biquad::LP, _x1, BW4_Q[i & 1], _sr);
            _hp1[i].setup(Biquad::HP, _x1, BW4_Q[i & 1], _sr);
            _lp2[i].setup(Biquad::LP, _x2, BW4_Q[i & 1], _sr);
            _hp2[i].setup(Biquad::HP, _x2, BW4_Q[i & 1], _sr);
        }
    }

    float process(const float* ring, uint32_t mask, uint64_t wpos) noexcept {
        float l = _lo.process(ring, mask, wpos);
        float m = _mid.process(ring, mask, wpos);
        float h = _hi.process(ring, mask, wpos);
        for (auto& b : _lp2) m = b.process(m);
        for (auto& b : _hp2) h = b.process(h);
        float mh = m + h;
        for (auto& b : _lp1) l  = b.process(l);
        for (auto& b : _hp1) mh = b.process(mh);
        return l + mh;
    }

private:
    StreamVocoderT<N_LO>  _lo;    // collision belt, resolved
    StreamVocoderT<N_MID> _mid;   // mid partials
    StreamVocoderT<N_HI>  _hi;    // sparkle + attack snap
    Biquad _lp1[4], _hp1[4];      // LR8 at x1 (lo | mid+hi)
    Biquad _lp2[4], _hp2[4];      // LR8 at x2 (mid | hi)
    float  _sr = 48000.0f;
    float  _x1 = 1200.0f, _x2 = 5000.0f;
};
