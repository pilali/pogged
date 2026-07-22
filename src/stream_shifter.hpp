#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

// Streaming granular pitch shifter — continuous transposition of a LIVE
// signal, reading behind the write head of a shared ring buffer.
//
// Adapted from Megalo's GrainPlayer (granular_looper.hpp): the Catmull-Rom
// fractional read and the cosine grain envelope are kept; the frozen-loop
// semantics (random respawn, loop wrapping) are replaced by:
//   - two taps in complementary 50%-overlap Hann crossfade (staggered by
//     grain/2, envelopes sum to exactly 1 → no wsum normalisation, no
//     pumping),
//   - absolute 64-bit read positions, so lag = wpos - rpos directly,
//   - respawn anchored at a fixed lag behind the write head, sized so the
//     read never overtakes the write during a grain (up-shift) and never
//     falls further behind than lag0 + (1-ratio)*grain (down-shift).
//
// Optional correlation-aligned respawn (align_range > 0, used on the sub
// voices): the incoming grain is phase-aligned with what the other tap is
// currently playing, which suppresses the splice warble on low notes.
class StreamShifter {
public:
    // §32 warble experiments (all default to the shipping behaviour):
    //  - GRAIN_TAPS  : overlapping read heads. More taps = smoother OLA (the
    //                  Hann window sum is N/2 for any N, so we scale by 2/N).
    //  - GRAIN_JITTER: samples of random extra lag at respawn — breaks the
    //                  PERIODICITY of the splice, spreading the warble into
    //                  broadband noise (less tonal, less audible) instead of a
    //                  steady AM tone.
    //  - GRAIN_PLOCK : stronger correlation phase-lock (finer search + a longer,
    //                  period-scaled window) at respawn.
#ifndef POGGED_GRAIN_TAPS
#define POGGED_GRAIN_TAPS 2
#endif
#ifndef POGGED_GRAIN_JITTER
#define POGGED_GRAIN_JITTER 0
#endif
    static constexpr int N_TAPS   = POGGED_GRAIN_TAPS;
    static constexpr int MAX_TAPS = 4;
    static constexpr int JITTER   = POGGED_GRAIN_JITTER;
    // Protective lag behind the write head: covers the Catmull-Rom lookahead
    // (i1+2) plus a safety margin against block-boundary effects.
    static constexpr int MARGIN = 160;

    // Ratio limits. The voices themselves only span [0.25, 4] (-2 to +2 oct),
    // but Warp bends them by up to an octave either way: the -2 voice reaches
    // 0.125 and the +2 voice reaches 8.
    static constexpr float RATIO_MIN = 0.125f;
    static constexpr float RATIO_MAX = 8.0f;

    // ratio        : pitch ratio, clamped [0.125, 8.0] (0.5 = -1 oct, 2 = +1 oct)
    // grain_samples: grain duration; the crossfade is grain/2 (Hann overlap)
    // align_range  : > 0 enables correlation-aligned respawn, scanning that
    //                many samples of extra lag (0 = free respawn)
    // Headroom on the lag budget, so a ratio later modulated by set_ratio()
    // still cannot overtake the write head. 2% covers the ±25 cents (1.45%)
    // the detune LFO can ask for.
    static constexpr float RATIO_HEADROOM = 1.02f;

    void setup(float ratio, int grain_samples, int align_range) noexcept {
        _ratio     = std::clamp(ratio, RATIO_MIN, RATIO_MAX);
        // Fix the lag budget here, from the *base* ratio: _lag0() must not
        // follow set_ratio(), or modulating the ratio would drag the respawn
        // anchor with it and inject an unintended pitch wobble opposing the
        // detune (measurably lopsided chorus). Warp changes it deliberately,
        // through set_lag_ratio().
        _lag_ratio = _ratio * RATIO_HEADROOM;
        if (grain_samples != _grain) {
            _grain = std::max(grain_samples, 64);
            _init  = false;                     // stagger depends on grain
        }
        _align = align_range;
    }

    // Retune without disturbing the taps — for vibrato-style modulation of the
    // ratio (the detuned voices). Safe mid-grain: rpos advances by _ratio each
    // sample, so changing it bends the read speed continuously; only the
    // derivative steps, never the position. Deliberately leaves the lag budget
    // alone (see setup): the respawn anchor must stay put under modulation.
    void set_ratio(float ratio) noexcept {
        _ratio = std::clamp(ratio, RATIO_MIN, RATIO_MAX);
    }

    // Re-size the lag budget for a SUSTAINED ratio change (Warp), which
    // set_ratio() deliberately will not do.
    //
    // _lag0() must cover the ratio actually being read: an up-shifted tap eats
    // (ratio-1)·grain of lag over a grain, so a +2 voice warped up an octave
    // (ratio 8) needs ~7 grains of budget where its nominal ratio 4 needed 3.
    // Read with the nominal budget it would overtake the write head and return
    // samples that do not exist yet.
    //
    // This necessarily MOVES the respawn anchor, and that is precisely why
    // set_ratio() must not do it: the detune LFO would drag the anchor in
    // sympathy (±35 samples, measured) and fight the chorus it is creating.
    // Warp is different in kind — the pitch is already sweeping, so a moving
    // anchor is masked by the sweep itself. Pass the base ratio times the warp
    // factor only; RATIO_HEADROOM still covers the LFO on top.
    //
    // Sizing the budget statically for the worst case instead would cost the
    // +2 voice ~178 ms of lag permanently, warp or no warp — against ~78 ms
    // now — which would throw away the granular engine's whole reason to exist
    // (3 ms against the vocoder's 85 ms).
    void set_lag_ratio(float base_ratio) noexcept {
        _lag_ratio = std::clamp(base_ratio, RATIO_MIN, RATIO_MAX) * RATIO_HEADROOM;
    }

    void reset() noexcept { _init = false; }

    // ring : power-of-2 ring buffer (index & mask), already containing the
    //        current input sample (write happens before the voices read)
    // wpos : absolute write count (number of samples written so far)
    float process(const float* ring, uint32_t mask, uint64_t wpos) noexcept {
        const int g = _grain;

        if (!_init) {
            // Stagger the taps by grain/N so the Hann envelopes sum to a
            // constant (N/2) from the very first sample.
            for (int k = 0; k < N_TAPS; ++k) {
                _t[k].cursor = k * (g / N_TAPS);
                _t[k].rpos   = (double)wpos - _lag0()
                             - (double)_t[k].cursor * (double)_ratio;
            }
            _init = true;
        }

        float out = 0.0f;
        for (int k = 0; k < N_TAPS; ++k) {
            Tap& t = _t[k];

            // Complementary Hann: 0.5*(1-cos(2π c/g)); N staggered taps sum to
            // N/2 (the cos terms are N equally-spaced phases → they cancel).
            const float amp =
                0.5f * (1.0f - std::cos(6.28318531f * (float)t.cursor / (float)g));
            out += amp * _read(ring, mask, t.rpos);

            t.rpos += (double)_ratio;
            if (++t.cursor >= g) {
                t.cursor = 0;
                double anchor = (double)wpos - _lag0();
                if (_align > 0) {
                    const Tap& ref = _t[(k + 1) % N_TAPS];
                    anchor = _aligned(ring, mask, anchor, ref.rpos);
                }
                // §32 jitter: a random extra lag at respawn breaks the
                // periodicity of the splice, so the residual warble is
                // broadband noise rather than a steady AM tone.
                if (JITTER > 0) anchor -= (double)(_rand() % (uint32_t)(JITTER + 1));
                t.rpos = anchor;
            }
        }
        // N staggered Hann taps sum to N/2; scale to unity gain.
        return out * (2.0f / (float)N_TAPS);
    }

private:
    struct Tap { double rpos = 0.0; int cursor = 0; };

    double _lag0() const noexcept {
        return (double)MARGIN + std::max(0.0f, _lag_ratio - 1.0f) * (double)_grain;
    }

    // 4-point Catmull-Rom read at fractional absolute position (Megalo's
    // GrainPlayer::_read, re-indexed for a masked ring).
    static float _read(const float* ring, uint32_t mask, double rpos) noexcept {
        const int64_t i1   = (int64_t)std::floor(rpos);
        const float   frac = (float)(rpos - (double)i1);
        const float y0 = ring[(uint64_t)(i1 - 1) & mask];
        const float y1 = ring[(uint64_t)(i1    ) & mask];
        const float y2 = ring[(uint64_t)(i1 + 1) & mask];
        const float y3 = ring[(uint64_t)(i1 + 2) & mask];
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * frac + c2) * frac + c1) * frac + y1;
    }

    // Correlation-aligned respawn (Megalo's GrainPlayer::_aligned_pos on a
    // ring): scan extra lag [0, _align) and keep the candidate whose signal
    // best correlates with what the reference tap is playing right now, so
    // the incoming grain enters in phase with the outgoing one during the
    // crossfade. Cost ≈ (_align/2)·K MACs per respawn — respawns happen a
    // few tens of times per second, noise next to the per-sample work.
    // xorshift32 — a couple of ops, no state beyond the seed. Only used at
    // respawn (a few tens of times a second) for the §32 jitter.
    uint32_t _rand() noexcept {
        _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5; return _rng;
    }

    double _aligned(const float* ring, uint32_t mask,
                    double anchor, double ref_pos) const noexcept {
#ifdef POGGED_GRAIN_PLOCK
        // §32 phase-lock: a longer, period-scaled correlation window and a
        // finer (step-1) search, to lock the incoming grain's phase harder.
        const int K = std::clamp(_grain / 3, 24, MAXK);
        const int STEP = 1;
#else
        constexpr int K = 24, STEP = 2;
#endif
        float ref[MAXK];
        {
            double p = ref_pos;
            for (int i = 0; i < K; ++i) {
                ref[i] = ring[(uint64_t)(int64_t)p & mask];
                p += (double)_ratio;
            }
        }
        int   best_d = 0;
        float best_r = -1e30f;
        for (int d = 0; d < _align; d += STEP) {
            double p = anchor - (double)d;
            float r = 0.0f, e = 1e-9f;
            for (int i = 0; i < K; ++i) {
                const float v = ring[(uint64_t)(int64_t)p & mask];
                r += ref[i] * v;
                e += v * v;
                p += (double)_ratio;
            }
            const float rn = r / std::sqrt(e);
            if (rn > best_r) { best_r = rn; best_d = d; }
        }
        return anchor - (double)best_d;
    }

    static constexpr int MAXK = 1024;   // correlation-window buffer cap (PLOCK)

    Tap   _t[N_TAPS];
    float _ratio     = 1.0f;   // read speed; may be modulated per block
    float _lag_ratio = 1.0f;   // base ratio + headroom; fixes the lag budget
    int   _grain = 1200;
    int   _align = 0;
    bool  _init  = false;
    uint32_t _rng = 0x9e3779b9u;   // §32 jitter PRNG seed
};
