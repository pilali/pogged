#pragma once
#include <complex>
#include <cstdint>
#include <cmath>
#include <cstring>

// Streaming constant-Q heterodyne filter bank pitch shifter — SPIKES 1–2 of the
// experimental POG3 branch. See docs/pog3-experimental-design.md §8.
//
// WHY THIS EXISTS. The two shipping engines both have a structural ceiling:
//   - StreamShifter (granular, 3 ms) locks its correlation aligner onto ONE
//     periodicity, so a chord's incommensurable periods make the sub pulse
//     ~+4.8 dB.
//   - StreamVocoder (block FFT, 84.5 ms) re-analyses the WHOLE spectrum every
//     hop, re-picks its peaks and re-partitions its regions. A new attack in an
//     arpeggio redraws that partition, so the partials already ringing shift
//     region/phasor and their resonance MOVES. Plus its latency is uniform:
//     4096 samples everywhere, though only the bass needs the resolution.
//
// This engine is the answer to both, and it is deliberately NOT a block FFT:
//
//   x(t) --[ demod by e^-jωk t ]--[ complex 1-pole LPF ]--> zk(t)  (baseband)
//                                                            |
//         ampl ak=|zk|,  inst freq = ωk + d/dt·arg(zk)  <----+
//                                                            |
//   out += ak·cos(θk),   θk += ratio·(ωk + dφk)  <-----------+   (resynth)
//
// Each channel is an INDEPENDENT, sample-by-sample oscillator. There is no
// frame, no block boundary, no global re-partition: a new note excites only the
// channels in its band and leaves every other channel's θk accumulating
// undisturbed. That is the arpeggio criterion (§1.1), structural here — but only
// if the RESYNTH does not undo it. Emitting every channel (Spike 1's first try)
// did: a partial is carried by ~CPO/2 overlapping channels, and another note's
// leakage pulls each channel's nonlinear frequency estimate differently, so they
// decorrelate and cancel — a new attack DROPPED a ringing note by 6 dB, §1.1
// inverted. The fix (Spike 2) is to emit ONE oscillator per partial — the local-
// maximum channel — with a PHASE HANDOFF when a drifting partial crosses to the
// next channel, so the crossover is seamless. That took the arpeggio disturbance
// to ~0.4 dB (vocoder-comparable) and the chord sub below the granular engine.
// See pass 2 in process().
//
// CONSTANT-Q. Channels are log-spaced (CPO per octave) with bandwidth ∝ centre
// frequency (Q = fk / BWk fixed). Two consequences, both wanted:
//   - resolution follows frequency — narrow in the bass (resolves close chord
//     partials), wide in the treble (cheap, and the ear needs no more there);
//   - latency follows frequency — a channel's group delay is ~1/(π·BWk), so the
//     treble answers in well under a millisecond and only the deep bass pays
//     ~40 ms. A block FFT cannot do this; it is one window for all frequencies.
//     This is the "few ms at the POG3" felt latency (§1.2): you hear the fast
//     bands.
//
// SCOPE / KNOWN LIMITS (spike, not a finished engine):
//   - The bank does not perfect-reconstruct: summed constant-Q one-poles are
//     not flat, so unity (ratio 1) is COLOURED. A pitch voice is never unity —
//     it is shifted, voiced and mixed against the dry — so this is tolerable
//     for the spike, and every measurement below is taken RELATIVE to this
//     engine's own unity pass to isolate the shift from the colouration.
//     Flattening the bank is follow-up #1.
//   - cos/atan2 per channel per sample: clarity over speed here. ~46 channels
//     is fine offline; a real-time pass wants a recurrence for the resynth
//     oscillator and a table/polynomial for arg. Not this commit.
//   - Chord sub still sits ~3 dB above the vocoder's ideal floor: a WEAK partial
//     next to a STRONG one in a sparse log bank is masked by the strong one's
//     skirt, so its channel is an intermittent local max and it emits unevenly.
//     Beating that needs real partial tracking (parabolic peak amplitude +
//     birth/death matching) or higher resolution without the latency — Spike 3.
//   - No transient split yet (design §7 keeps it as the next perceptual win).
//
// Same interface as StreamShifter / StreamVocoder (init / reset / set_ratio /
// process on the shared ring), so it can drop into FOCUS once it earns it.
// All buffers are members — zero allocation in process().

class StreamFilterbank {
public:
    static constexpr int   MAXCH = 64;
    // Analysis span: low enough for a bass low-E fundamental (~41 Hz), high
    // enough to carry a guitar's useful partials. The SHIFT happens at resynth
    // (θk ·= ratio); analysis only has to cover the INPUT's partials.
    static constexpr float F_LO  = 45.0f;
    static constexpr float F_HI  = 9000.0f;
    static constexpr float CPO   = 6.0f;    // channels per octave (log spacing)
    static constexpr float Q      = 6.0f;   // fk / bandwidth, fixed (constant-Q)
    static constexpr float PEAK_FLOOR = 1e-4f;   // below this a channel is silence

    // hop_phase is accepted for signature-compatibility with StreamVocoder's
    // init(); this engine has no hop, so it is ignored.
    void init(double sr, int /*hop_phase*/ = 0) noexcept {
        _sr = static_cast<float>(sr);
        _nch = 0;
        const float two_pi = 2.0f * static_cast<float>(M_PI);
        // Log-spaced centres F_LO·2^(i/CPO) up to F_HI.
        for (int i = 0; i < MAXCH; ++i) {
            const float fk = F_LO * std::pow(2.0f, static_cast<float>(i) / CPO);
            if (fk > F_HI) break;
            const float wk = two_pi * fk / _sr;          // rad/sample carrier
            _omega[i] = wk;
            _rot[i]   = std::polar(1.0f, -wk);            // demod rotation e^-jωk
            // Complex one-pole LPF on the baseband. Cutoff fc = fk/(2Q) gives a
            // two-sided passband of fk/Q around the carrier, i.e. Q = fk/BW.
            const float fc = fk / (2.0f * Q);
            _g[i] = 1.0f - std::exp(-two_pi * fc / _sr);
            ++_nch;
        }
        reset();
    }

    void reset() noexcept {
        for (int i = 0; i < _nch; ++i) {
            _car[i]   = std::complex<float>(1.0f, 0.0f);
            _z[i]     = {};
            _zprev[i] = {};
            _theta[i] = 0.0f;
            _emitting[i] = false;
            _emit[i]     = false;
        }
        _renorm = 0;
    }

    // Direct ratio (0.5 = -1 oct, 2 = +1 oct), like the other engines.
    void set_ratio(float ratio) noexcept { _ratio = ratio; }

    // One shifted sample per call. Reads the newest sample the ring holds — the
    // one just written at wpos-1 — so this engine is causal-streaming and its
    // only latency is the per-channel filter group delay (frequency-dependent).
    float process(const float* ring, uint32_t mask, uint64_t wpos) noexcept {
        const float x = ring[(uint32_t)(wpos - 1) & mask];

        const bool renorm = (++_renorm >= 256);
        // Pass 1 — analysis. EVERY channel keeps tracking every sample (carrier,
        // envelope, instantaneous frequency, resynth phase), so a channel that
        // later becomes a partial's home has a coherent phase history and never
        // starts cold. Only WHICH channels are emitted is decided in pass 2.
        for (int k = 0; k < _nch; ++k) {
            // Advance this channel's demodulation carrier e^-jωk n and shift the
            // input down to baseband. The rotating multiply drifts in magnitude
            // over thousands of samples; renormalise it a few hundred samples
            // apart (cheap, and imperceptible between renorms).
            _car[k] *= _rot[k];
            if (renorm) _car[k] /= std::abs(_car[k]);
            const std::complex<float> demod = x * _car[k];

            // Complex one-pole low-pass: zk is the channel's slowly-varying
            // complex envelope (amplitude + phase relative to the carrier).
            _z[k] += _g[k] * (demod - _z[k]);

            // Baseband phase advance since last sample = deviation of the
            // partial from this channel's centre. arg(zk·conj(zk_prev)) wraps to
            // (-π,π] on its own — no manual unwrap. Input inst freq = ωk + dφ.
            const std::complex<float> prod = _z[k] * std::conj(_zprev[k]);
            const float dphi = std::arg(prod);
            _zprev[k] = _z[k];

            // Resynthesise this channel at ratio× its instantaneous frequency.
            // θk carries the whole history, so the channel keeps ringing through
            // silences and is untouched by attacks in OTHER channels.
            _theta[k] += _ratio * (_omega[k] + dphi);
            _theta[k] -= 2.0f * static_cast<float>(M_PI) *
                         std::round(_theta[k] * static_cast<float>(0.5 * M_1_PI));

            _a[k] = std::abs(_z[k]);
        }
        if (renorm) _renorm = 0;

        // Pass 2 — emit ONE oscillator per detected partial. Emitting every
        // overlapping channel was Spike 1's first failure: a partial is carried
        // by ~CPO/2 channels at once, and leakage from OTHER notes pulls each
        // channel's (nonlinear) frequency estimate differently, so those
        // channels decorrelate and their sum cancels — a new attack then dropped
        // a ringing note by 6 dB, the exact opposite of §1.1. Keeping only
        // local-maximum channels collapses each partial onto its dominant
        // channel, whose frequency estimate is barely perturbed by distant
        // notes. That cut the damage ~5×, but left a residual FLICKER: a partial
        // sitting between two channels makes the local max jump between them as
        // |z| wobbles, and the two channels' θ free-ran from different histories,
        // so each jump was a phase step (arpeggio 1.2 dB).
        //
        // Spike 2 kills that flicker with a PHASE HANDOFF: when the emitting
        // channel for a partial changes, the new channel inherits the departing
        // one's θ. Both channels track the same partial frequency, so once
        // aligned they stay aligned and the crossover is seamless — arpeggio
        // 1.2 → 0.4 dB. (A hysteresis margin was tried on top and dropped: it
        // bought a little more but every safe formulation either deadlocked a
        // straddling tone into silence or latched whole clusters on. The handoff
        // alone is robust on every |z| pattern.)
        for (int k = 0; k < _nch; ++k) {
            const float lo = (k > 0)        ? _a[k - 1] : 0.0f;
            const float hi = (k < _nch - 1) ? _a[k + 1] : 0.0f;
            // Strict on the left, ≥ on the right: breaks ties so a partial
            // straddling two EQUAL channels yields exactly ONE winner, never a
            // deadlock (a two-sided margin could be met by neither and silence a
            // whole note; a retention BOOST vs raw neighbours let whole clusters
            // latch on — both tried, both rejected). Mutual exclusion holds on
            // every |z| pattern, and the phase handoff below does the smoothing
            // that a hysteresis margin was reaching for, without its failure
            // modes.
            const float m = _a[k];
            _emit[k] = (m > lo) && (m >= hi) && (m > PEAK_FLOOR);
        }
        for (int k = 0; k < _nch; ++k) {
            if (_emit[k] && !_emitting[k]) {              // rising edge: hand off
                int src = -1;
                if (k > 0        && _emitting[k - 1]) src = k - 1;
                if (k < _nch - 1 && _emitting[k + 1] &&
                    (src < 0 || _a[k + 1] > _a[src]))  src = k + 1;
                if (src >= 0) _theta[k] = _theta[src];   // inherit phase
            }
        }
        float out = 0.0f;
        for (int k = 0; k < _nch; ++k) {
            // ×2: demodulating a real cosine A·cos keeps the A/2 baseband term,
            // so the envelope is half the real amplitude — double it back.
            if (_emit[k]) out += 2.0f * _a[k] * std::cos(_theta[k]);
            _emitting[k] = _emit[k];
        }

        // One emitter per partial now, so no overlap sum to divide out. A
        // single channel captures only part of a between-centres partial's
        // energy, so the level runs a little low — fine for the spike's
        // relative measurements; a per-channel gain trim is follow-up #1.
        return out;
    }

    int channels() const noexcept { return _nch; }

private:
    float _sr    = 48000.0f;
    float _ratio = 1.0f;
    int   _nch   = 0;
    int   _renorm = 0;

    float _omega[MAXCH] = {};
    float _g[MAXCH]     = {};
    std::complex<float> _rot[MAXCH]   = {};   // per-channel demod rotation
    std::complex<float> _car[MAXCH]   = {};   // running demod carrier
    std::complex<float> _z[MAXCH]     = {};   // baseband envelope
    std::complex<float> _zprev[MAXCH] = {};   // previous envelope (for dφ)
    float _theta[MAXCH] = {};                 // resynth phase accumulators
    float _a[MAXCH]     = {};                 // per-sample channel magnitude
    bool  _emitting[MAXCH] = {};              // was channel k an emitter last sample
    bool  _emit[MAXCH]     = {};              // is channel k an emitter this sample
};
