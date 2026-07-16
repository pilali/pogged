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
// frame, no block boundary. RESYNTH is OVERLAP: every channel above an adaptive
// floor emits at ratio× its own instantaneous frequency, so the FULL harmonic
// series is reconstructed and the timbre stays faithful.
//
// This is the current choice after three spikes (design §8-§10), and it is a
// TRADE, not a win on every axis:
//   - Overlap → faithful timbre, but the ~CPO/2 channels carrying one partial
//     DECORRELATE under another note's leakage, so a chord/arpeggio wobbles
//     (a new attack can drop a ringing note ~6 dB).
//   - Emitting ONE oscillator per partial (Spike 2, peak-pick + phase handoff)
//     held a ringing note to ~0.4 dB — but a sparse log bank keeps a WRONG
//     subset of partials, punching holes in the harmonic series (bit-crush on
//     real material). The ear rejected it.
//   - The unifier would be identity phase-locking (Laroche-Dolson) applied
//     continuously — but per-sample locking DETUNES the slaved members (their
//     analysis-phase difference drifts), so they cancel. A frameless phase-lock
//     that does not detune is the open problem. See §10.
// The engine currently prioritises timbre (what the ear flagged). See process().
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
//   - Polyphonic coherence is the OPEN axis: under overlap a chord/arpeggio
//     wobbles (channels of one partial decorrelate). The engine holds timbre,
//     not coherence — see the trade above and design §10. Not a tuning knob.
//   - Mild treble tilt: overlap under-weights the high harmonics (~1.5-3× low).
//     A fixed voicing EQ corrects it — follow-up, once a direction is chosen.
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
    static constexpr float PEAK_FLOOR = 1e-4f;   // absolute floor backstop
    static constexpr float EMIT_REL   = 0.02f;   // emit only above 2% of the loudest channel
    static constexpr float AMP_MS     = 3.0f;    // emitter fade time (ms), anti-click

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
        _amp_c = 1.0f - std::exp(-1.0f / (AMP_MS * 0.001f * _sr));
        reset();
    }

    void reset() noexcept {
        for (int i = 0; i < _nch; ++i) {
            _car[i]   = std::complex<float>(1.0f, 0.0f);
            _z[i]     = {};
            _zprev[i] = {};
            _theta[i] = 0.0f;
            _amp[i]   = 0.0f;
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
        const float two_pi = 2.0f * static_cast<float>(M_PI);
        // Pass 1 — analysis + per-channel resynth phase. Each channel runs its
        // OWN oscillator at ratio× its own instantaneous frequency. This is
        // OVERLAP reconstruction: every channel contributes, so the full harmonic
        // series is rebuilt (peak-picking punched holes in it → bit-crush timbre,
        // §10). The known cost is that channels sharing a partial can decorrelate
        // under another note's leakage (arpeggio/chord wobble); a frameless
        // phase-lock that would fix it without detuning the members is unsolved
        // here and left to future work (§10 lucidity note).
        for (int k = 0; k < _nch; ++k) {
            _car[k] *= _rot[k];
            if (renorm) _car[k] /= std::abs(_car[k]);
            const std::complex<float> demod = x * _car[k];
            _z[k] += _g[k] * (demod - _z[k]);

            const float dphi = std::arg(_z[k] * std::conj(_zprev[k]));
            _zprev[k] = _z[k];
            _theta[k] += _ratio * (_omega[k] + dphi);
            _theta[k] -= two_pi * std::round(_theta[k] * static_cast<float>(0.5 * M_1_PI));
            _a[k] = std::abs(_z[k]);
        }
        if (renorm) _renorm = 0;

        // Pass 2 — emit every channel above an ADAPTIVE FLOOR, amplitude-smoothed.
        // The floor (a fraction of the loudest channel this sample) gates the
        // noise-floor channels BETWEEN partials, whose random baseband phase made
        // them spit broadband grit; it tracks the decay down, and the skirt
        // channels of real partials sit above it so the harmonic series is kept.
        // Amplitude smoothing fades a channel in/out instead of clicking.
        float amax = 0.0f;
        for (int k = 0; k < _nch; ++k) amax = std::max(amax, _a[k]);
        const float floor = std::max(PEAK_FLOOR, EMIT_REL * amax);

        float out = 0.0f;
        for (int k = 0; k < _nch; ++k) {
            const float target = (_a[k] > floor) ? 2.0f * _a[k] : 0.0f;
            _amp[k] += _amp_c * (target - _amp[k]);
            out += _amp[k] * std::cos(_theta[k]);
        }
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
    float _theta[MAXCH] = {};                 // per-channel resynth phase accumulator
    float _a[MAXCH]     = {};                 // per-sample channel magnitude |z|
    float _amp[MAXCH]   = {};                 // smoothed emitted amplitude
    float _amp_c        = 0.0f;               // amplitude ramp coefficient
};
