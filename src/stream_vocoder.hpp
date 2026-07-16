#pragma once
#include <complex>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>

// Streaming phase vocoder pitch shifter — no external dependencies.
//
// Adapted from Megalo's PhaseVocoder (phase_vocoder.hpp). The algorithm is
// unchanged and is the whole reason this exists: per-peak spectral translation
// (Laroche & Dolson 1999) treats every spectral peak INDEPENDENTLY, so a chord
// reconstructs cleanly. The granular StreamShifter cannot do that — its
// correlation aligner can only lock onto one periodicity, and a chord's
// periods are incommensurable, which is exactly what makes the sub voice pulse
// ~15 dB at ~12 Hz on real polyphonic material.
//
// What changed from Megalo: it read a FROZEN loop at a wrapped _read_pos; this
// reads the live ring buffer, always taking the newest N samples behind the
// write head. Nothing else — the analysis, the per-peak translation and the
// overlap-add are Megalo's, comments included.
//
// Pitch is shifted by translating peaks, NOT by resampling, so the frame is
// always read at normal speed. That is what makes streaming natural here.
//
// FFT size is controlled at compile time. 4096 is the default because 2048 is
// NOT enough for a guitar chord in the low register: its bins are 23.4 Hz at
// 48 kHz, while an E major triad's partials sit 42.8 Hz apart — 1.8 bins, so
// the Hann mainlobes (4 bins wide) merge and the peak picker sees one partial
// where there are three. Measured artifact over an ideal shift, sub on a
// chord: N=2048 -> +2.3 dB, N=4096 -> +0.0 dB. It costs latency:
//   -DPOGGED_PV_N=4096   (default) 84.5 ms, resolves low chords
//   -DPOGGED_PV_N=2048             41.7 ms, blurs them
// -DPOGGED_NO_VOCODER compiles this engine out entirely (MOD Dwarf).
//
// All buffers are member variables — zero stack allocation in process().

// Templated on the FFT size so several window lengths can coexist in one
// binary (the multi-resolution vocoder runs a long window for the bass and a
// short one for the treble/attacks). The plain `StreamVocoder` alias at the end
// keeps the historic single-window type — same behaviour as before.
template <int N_>
class StreamVocoderT {
public:
    static constexpr int N = N_;
    static constexpr int HOP    = N / 4;        // 75 % overlap
    static constexpr int BINS   = N / 2 + 1;
    static constexpr int OUTBUF = N * 4;        // ring buffer ≥ 2 × max unread

    // hop_phase staggers WHEN this instance does its FFT burst, in samples
    // [0, HOP). It changes nothing about the sound: each instance analyses the
    // ring at its own frame times and its OLA stays self-consistent — only the
    // moment of the work moves. See the note on _hop_cnt for why it matters.
    void init(double sr, int hop_phase = 0) noexcept {
        _hop_phase = ((hop_phase % HOP) + HOP) % HOP;
        _sr        = static_cast<float>(sr);
        _freq_pbin = _sr / N;
        _osamp     = static_cast<float>(N) / HOP;   // = 4
        for (int i = 0; i < N; i++)
            _win[i] = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i / (N - 1)));
        reset();
    }

    void reset() noexcept {
        std::memset(_ana_phase, 0, sizeof _ana_phase);
        std::memset(_ana_mag,   0, sizeof _ana_mag);
        std::memset(_ana_freq,  0, sizeof _ana_freq);
        for (auto& c : _ana_cx) c = {};
        std::memset(_rot,       0, sizeof _rot);
        std::memset(_swl,       0, sizeof _swl);
        std::memset(_out_buf,   0, sizeof _out_buf);
        for (auto& c : _cx) c = {};
        _hop_cnt   = _hop_phase;   // NOT 0: a reset must not re-align the burst
        _out_write = 0;
        _out_read  = 0;
        _out_fill  = 0;
    }

    // Direct ratio (0.5 = -1 oct, 2 = +1 oct): the voices are defined by
    // ratios, so converting through semitones would only lose precision on the
    // equal-tempered fifth.
    void set_ratio(float ratio) noexcept { _ratio = ratio; }

    // Per-bin attack swell (POG3 ATTACK, polyphonic). atk_ms is the time each
    // bin takes to cover 90 % of a level INCREASE; <= 1 disables it. Expressed
    // per frame: the envelope advances once per hop, and the OLA smooths the
    // per-frame steps.
    void set_swell(float atk_ms) noexcept {
        if (atk_ms <= 1.0f) { _swell_c = -1.0f; return; }
        const float frames = atk_ms * 0.001f * _sr / HOP;
        _swell_c = (frames > 1.0f) ? std::exp(-std::log(9.0f) / frames) : 0.0f;
    }

    // Returns one pitch-shifted sample. Call once per output sample.
    // ring/mask/wpos: the shared live ring buffer and its absolute write count,
    // exactly as StreamShifter takes them, so the two engines are swappable.
    float process(const float* ring, uint32_t mask, uint64_t wpos) noexcept {
        if (++_hop_cnt >= HOP) {
            _hop_cnt = 0;
            _process_frame(ring, mask, wpos);
        }

        if (_out_fill <= 0) return 0.0f;
        float out = _out_buf[_out_read];
        _out_buf[_out_read] = 0.0f;
        _out_read = (_out_read + 1) % OUTBUF;
        --_out_fill;
        return out;
    }

private:
    // ── FFT frame ─────────────────────────────────────────────────────────
    void _process_frame(const float* ring, uint32_t mask, uint64_t wpos) noexcept {
        // The newest N samples behind the write head. Megalo interpolated a
        // fractional loop position here; streaming needs none — the frame is
        // read at normal speed and lands on integer samples.
        const uint64_t start = wpos - (uint64_t)N;
        for (int i = 0; i < N; i++)
            _cx[i] = { ring[(start + (uint64_t)i) & mask] * _win[i], 0.0f };

        _fft(false);

        // ── Analysis: true instantaneous frequency per bin ─────────────────
        const float expct = 2.0f * float(M_PI) * HOP / N;
        for (int k = 0; k < BINS; k++) {
            float mag   = std::abs(_cx[k]);
            float phase = std::arg(_cx[k]);

            float dp = phase - _ana_phase[k];
            _ana_phase[k] = phase;

            // Remove expected phase advance, wrap deviation to [-π, π]
            dp -= k * expct;
            dp -= 2.0f * float(M_PI) * std::round(dp * float(M_1_PI) * 0.5f);

            _ana_mag[k]  = mag;
            // dev(Hz) = dp/(2π) · sr/HOP = dp/(2π) · osamp · freq_pbin.
            // The original code dropped the 1/(2π): every instantaneous
            // frequency came out ~6.3× too far from its bin centre, which is
            // why the vocoder never reconstructed cleanly, shifts included.
            _ana_freq[k] = k * _freq_pbin
                         + dp * _osamp * _freq_pbin * (float)(0.5 / M_PI);
            // De-alternated complex spectrum for the fractional lobe
            // translation below: a Hann-windowed sinusoid carries a linear
            // phase of ~−π per bin (sign alternation); removing it makes the
            // lobe a SMOOTH complex curve that can be linearly interpolated.
            _ana_cx[k] = (k & 1) ? -_cx[k] : _cx[k];
        }

        // ── Per-peak pitch shift (Laroche & Dolson 1999) ────────────────────
        // Each spectral peak (= one partial) is translated to its target
        // location as a RIGID block together with its whole region of
        // influence, and the region carries a per-peak phasor accumulating
        // the frequency difference. The window mainlobe therefore arrives
        // INTACT, and the analysis-side phase relationships inside a partial
        // are reproduced exactly (identity locking is structural here).
        //
        // The previous per-bin remap (dst = round(k·ratio)) stretched and
        // punched holes in the lobe — adjacent sources landed ~ratio bins
        // apart — which collapsed low fundamentals whose lobes only span a
        // few bins (measured −30 dB on a 220 Hz partial shifted to 330 Hz).
        // 1. Peaks on the ANALYSIS spectrum (local max over ±2 bins).
        int n_peaks = 0;
        for (int j = 2; j < BINS - 2; ++j) {
            const float m = _ana_mag[j];
            if (m > 1e-9f &&
                m >= _ana_mag[j - 1] && m >= _ana_mag[j - 2] &&
                m >  _ana_mag[j + 1] && m >  _ana_mag[j + 2])
                _peaks[n_peaks++] = j;
        }

        if (n_peaks == 0) {
            for (int k = 0; k < BINS; k++) _cx[k] = {};   // silent frame
        } else {
            // 2. Region boundaries at the magnitude minimum between peaks.
            _bounds[0] = 0;
            for (int i = 1; i < n_peaks; ++i) {
                int   bmin = _peaks[i - 1];
                float mmin = 1e30f;
                for (int j = _peaks[i - 1]; j <= _peaks[i]; ++j)
                    if (_ana_mag[j] < mmin) { mmin = _ana_mag[j]; bmin = j; }
                _bounds[i] = bmin;
            }
            _bounds[n_peaks] = BINS;

            // 3. Translate each region by the FRACTIONAL shift of its peak's
            // instantaneous frequency — the de-alternated complex lobe is
            // linearly interpolated at the exact offset, so the lobe SHAPE
            // (which encodes the partial's sub-bin position) and the
            // frame-to-frame phasor advance agree on the same frequency.
            // (An integer translation left them disagreeing by up to half a
            // bin: the overlapped frames partially cancelled and the low
            // partials smeared — the very defect this rework fixes.)
            // _rot[] is kept per SOURCE bin but incremented region-wide with
            // the peak's frequency, so a peak drifting by a bin between
            // frames inherits a coherent phasor history.
            for (int k = 0; k < BINS; k++) _cx[k] = {};
            const float rot_c = 2.0f * float(M_PI) * HOP * (_ratio - 1.0f) / _sr;
            for (int i = 0; i < n_peaks; ++i) {
                const int   p      = _peaks[i];
                const float inc    = rot_c * _ana_freq[p];
                const int   lo     = _bounds[i], hi = _bounds[i + 1];
                for (int j = lo; j < hi; ++j) {
                    _rot[j] += inc;
                    _rot[j] -= 2.0f * float(M_PI) *
                               std::round(_rot[j] * float(M_1_PI) * 0.5f);
                }
                const std::complex<float> ph = std::polar(1.0f, _rot[p]);
                const float d_frac = (_ana_freq[p] / _freq_pbin) * (_ratio - 1.0f);
                const int dst_lo = std::max(0, (int)std::ceil((float)lo + d_frac));
                const int dst_hi = std::min(BINS - 1,
                                            (int)std::floor((float)(hi - 1) + d_frac));
                for (int dst = dst_lo; dst <= dst_hi; ++dst) {
                    const float sp = (float)dst - d_frac;
                    int j0 = (int)sp;
                    if (j0 >= hi - 1) j0 = hi - 2;
                    if (j0 < lo) j0 = lo;
                    const float t = std::min(1.0f, std::max(0.0f, sp - (float)j0));
                    std::complex<float> v =
                        _ana_cx[j0] + t * (_ana_cx[j0 + 1] - _ana_cx[j0]);
                    v *= ph;
                    if (dst & 1) v = -v;      // restore the Hann alternation
                    _cx[dst] += v;            // overlapping regions just add
                }
            }
        }
        // ── Per-bin attack swell ────────────────────────────────────────────
        // Each bin carries its own envelope: it follows the synthesis
        // magnitude freely DOWNWARD (decays and silence are untouched) and
        // with the ATTACK time constant UPWARD. A fresh note's bins start
        // near zero, so its energy fades in over attack_ms; the bins of a
        // note already ringing sit at gain 1 and never move. That is the
        // POG3's polyphonic ATTACK: every attack swells, the sustain of the
        // notes underneath stays intact — a single wet-bus envelope cannot
        // do both (it either ducks the held notes or misses the attack).
        // While disabled the state keeps tracking, so enabling ATTACK
        // mid-note swells only the NEXT attack, not the note already heard.
        for (int k = 0; k < BINS; k++) {
            const float m = std::abs(_cx[k]);
            if (m <= _swl[k] || _swell_c < 0.0f) {
                _swl[k] = m;
            } else {
                _swl[k] = m - _swell_c * (m - _swl[k]);
                _cx[k] *= _swl[k] / m;     // real gain < 1, phase untouched
            }
        }

        // Hermitian symmetry for real output
        for (int k = BINS; k < N; k++)
            _cx[k] = std::conj(_cx[N - k]);

        _fft(true);

        // ── Overlap-add ───────────────────────────────────────────────────
        // Hann analysis+synthesis normalization: the sum of w² over the
        // overlapping frames is 0.375 × osamp (= 1.5 at 4× / 75 % overlap),
        // so dividing by it yields unity passthrough. (The previous factor was
        // wrong by ~N/2, making the pitch voices ~1340× too quiet / inaudible.)
        const float scale = 1.0f / (0.375f * _osamp);
        for (int i = 0; i < N; i++) {
            int idx = (_out_write + i) % OUTBUF;
            _out_buf[idx] += _cx[i].real() * _win[i] * scale;
        }
        _out_write = (_out_write + HOP) % OUTBUF;
        _out_fill  = std::min(_out_fill + HOP, OUTBUF);
    }

    // ── Radix-2 DIT Cooley-Tukey FFT (in-place, operates on _cx) ─────────
    void _fft(bool inverse) noexcept {
        // Bit-reversal permutation
        for (int i = 1, j = 0; i < N; i++) {
            int bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(_cx[i], _cx[j]);
        }
        // Butterfly stages
        for (int len = 2; len <= N; len <<= 1) {
            float ang = float(M_PI) * (inverse ? 1.0f : -1.0f) * 2.0f / len;
            std::complex<float> wlen(std::cos(ang), std::sin(ang));
            for (int i = 0; i < N; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                for (int j = 0; j < len / 2; j++) {
                    auto u = _cx[i + j];
                    auto v = _cx[i + j + len / 2] * w;
                    _cx[i + j]           = u + v;
                    _cx[i + j + len / 2] = u - v;
                    w *= wlen;
                }
            }
        }
        if (inverse)
            for (int i = 0; i < N; i++) _cx[i] /= float(N);
    }

    // ── State ──────────────────────────────────────────────────────────────
    float _sr        = 48000.0f;
    float _ratio     = 1.0f;
    float _freq_pbin = 48000.0f / N;
    float _osamp     = 4.0f;

    float _win[N]               = {};
    float _ana_phase[BINS]      = {};
    float _ana_mag[BINS]        = {};
    float _ana_freq[BINS]       = {};
    std::complex<float> _ana_cx[BINS] = {};   // de-alternated analysis lobe
    float _rot[BINS]            = {};   // per-region rotation accumulators
    float _swl[BINS]            = {};   // per-bin swell envelope (see set_swell)
    float _swell_c              = -1.0f;   // < 0 = swell off
    int   _peaks[BINS]          = {};   // per-frame peak list
    int   _bounds[BINS + 1]     = {};   // per-frame region boundaries
    float _out_buf[OUTBUF]      = {};
    std::complex<float> _cx[N]  = {};

    // Counts samples to the next frame. Its START VALUE is the whole point:
    // a frame costs 2 FFTs of N, ~70x a plain sample, and it all lands in one
    // audio block. With every instance starting at 0 they fire in lockstep, so
    // N_VOICES frames pile into the SAME block while the next HOP-1 blocks do
    // nothing. The average load is unchanged either way — the peak is not, and
    // the peak is what has to fit in the block's deadline. Measured on a Pi 5,
    // 8 voices, 128-sample blocks: worst-case block 128% of deadline (xruns)
    // in lockstep, 25% staggered.
    int    _hop_phase = 0;
    int    _hop_cnt   = 0;
    int    _out_write = 0;
    int    _out_read  = 0;
    int    _out_fill  = 0;
};

// Historic single-window type, default 4096 (or -DPOGGED_PV_N). Everything that
// used StreamVocoder before the templating keeps working unchanged.
#ifdef POGGED_PV_N
using StreamVocoder = StreamVocoderT<POGGED_PV_N>;
#else
using StreamVocoder = StreamVocoderT<4096>;
#endif
