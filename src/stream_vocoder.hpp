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
//
// OS_ is the oversampling (overlap) factor: 4 = 75 % overlap (hop N/4, the
// historic value), 8 = 87.5 % (hop N/8, §21) — denser frames at the SAME
// window and latency, so the OLA averages 8 renderings instead of 4 and
// frame-rate artifacts (topology flips, phase steps) are smoothed. Costs a
// proportional factor of CPU: frames come twice as often.
template <int N_, int OS_ = 4>
class StreamVocoderT {
public:
    static constexpr int N = N_;
    static constexpr int HOP    = N / OS_;      // hop; OS_=4 -> 75 % overlap
    static constexpr int BINS   = N / 2 + 1;
    static constexpr int M      = N / 2;        // real-FFT complex size (§19)
    // Estimator baseline, in hops (§21): the instantaneous-frequency estimate
    // is a phase difference over EB hops. Densifying the frames (OS_ > 4)
    // must NOT shorten that baseline — halving it doubles the estimate's
    // wobble on merged pairs, which is the §16 shimmer engine. EB keeps the
    // baseline at N/4 seconds' worth of hops for every overlap factor.
    static constexpr int EB     = (OS_ > 4) ? OS_ / 4 : 1;
    // §22 (Spike 7) — parametric resynthesis of merged regions. Any bin of a
    // region carries, across frames, the sum of the region's partials as
    // complex exponentials: an order-2 Prony fit over the last PK frames
    // recovers TWO frequencies beyond the window's resolution (estimation
    // history is not signal latency). A region diagnosed bi-tonal is then
    // resynthesised as two analytic Hann kernels, each at its own target
    // frequency with its own phasor — no wobble, no mistuning, and even the
    // pair's output spacing comes out ×ratio (rigid translation kept the
    // input spacing). Falls back to rigid translation whenever the fit is
    // not trustworthy.
    static constexpr int PK  = 8;    // Prony history, frames
    static constexpr int KW  = 4;    // synthesis kernel half-width, bins
    static constexpr int KOS = 32;   // kernel table oversampling
    // §24 — cross-window frequency import. A short companion window cannot
    // resolve low pairs; translating their merged lobe by the merged
    // estimate lands BOTH partials near the pair's midpoint (the loud
    // dissonant wanderer the user heard, measured -5 dB vs a real partial).
    // The RESOLVING window exports its partial frequencies (peaks + §22
    // pair tracks); the short window renders those regions as exact kernels
    // with amplitudes solved on its OWN current frame — the long window's
    // frequency precision at the short window's time resolution.
    static constexpr int MAXHINT = 48;
    static constexpr int OUTBUF = N * 4;        // ring buffer ≥ 2 × max unread

    // hop_phase staggers WHEN this instance does its FFT burst, in samples
    // [0, HOP). It changes nothing about the sound: each instance analyses the
    // ring at its own frame times and its OLA stays self-consistent — only the
    // moment of the work moves. See the note on _hop_cnt for why it matters.
    void init(double sr, int hop_phase = 0) noexcept {
        _hop_phase = ((hop_phase % HOP) + HOP) % HOP;
        _sr        = static_cast<float>(sr);
        _freq_pbin = _sr / N;
        _osamp     = static_cast<float>(N) / HOP;   // = OS_
        for (int i = 0; i < N; i++)
            _win[i] = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i / (N - 1)));
        // §22 kernel table: the de-alternated (centre-referenced) transform
        // of the Hann window, analytic — W(x) = D(x)/2 + [D(x-1)+D(x+1)]/4,
        // D(x) = sin(πx)/sin(πx/N) (Dirichlet). Same table serves the
        // amplitude solve AND the synthesis, so no normalization is needed.
        auto dirich = [&](double x) -> double {
            const double sx = std::sin(M_PI * x / N);
            if (std::abs(sx) < 1e-12) return (double)N * std::cos(M_PI * x);
            return std::sin(M_PI * x) / sx;
        };
        for (int t = 0; t <= 2 * KW * KOS; ++t) {
            const double x = (double)(t - KW * KOS) / KOS;
            _kern[t] = (float)(0.5 * dirich(x)
                               + 0.25 * (dirich(x - 1.0) + dirich(x + 1.0)));
        }
        // Twiddle tables (double-precision trig at init, no cost in process).
        for (int j = 0; j < M / 2; j++)
            _tw[j] = std::complex<float>((float)std::cos(2.0 * M_PI * j / M),
                                         (float)-std::sin(2.0 * M_PI * j / M));
        for (int k = 0; k <= M; k++)
            _tu[k] = std::complex<float>((float)std::cos(2.0 * M_PI * k / N),
                                         (float)-std::sin(2.0 * M_PI * k / N));
        reset();
    }

    void reset() noexcept {
        std::memset(_ana_phase, 0, sizeof _ana_phase);
        _ph_idx = 0;
        std::memset(_ana_mag,   0, sizeof _ana_mag);
        std::memset(_ana_freq,  0, sizeof _ana_freq);
        for (auto& c : _ana_cx) c = {};
        std::memset(_rot,       0, sizeof _rot);
        std::memset(_swl,       0, sizeof _swl);
        std::memset(_trk_f,     0, sizeof _trk_f);
        std::memset(_trk_on,    0, sizeof _trk_on);
        for (auto& h : _hist) for (auto& c : h) c = {};
        _hist_idx = 0;
        _warm     = 0;
        std::memset(_pr_on,     0, sizeof _pr_on);
        _n_hint_out = 0;
        _n_hints    = 0;
        std::memset(_ht_on,     0, sizeof _ht_on);
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

    // §20 stability tunables — build-time knobs, not host-exposed. Kept as
    // members (not constexpr) so the offline harnesses can sweep them; the
    // defaults are the values frozen by the §20 sweep.
    float PEAK_FLOOR  = 0.003f;   // drop peaks < this x frame max (~-50 dB)
    float SMOOTH_SLOW = 0.20f;    // per-frame step on beat-wobble-sized moves
    float SMOOTH_FAST = 0.75f;    // ...on real moves > SMOOTH_TH bins
    float SMOOTH_TH   = 0.80f;    // fast/slow boundary, in bins
    bool  PRONY_ON    = true;     // §22 parametric resynthesis of merged pairs
    float PRONY_FMIN  = 0.0f;     // §24: engage §22 only on peaks above this
                                  // input Hz — lets a resolving window keep
                                  // its own low fundamentals RIGID (its §22
                                  // wobble through the crossover LP is the
                                  // midpoint parasite) while §22 still covers
                                  // the collision belt above
    float HINT_FMAX   = 900.0f;   // §24: export/import partials below this, Hz
    int   HINT_ROTW   = 3;        // §24: half-width (bins) of the _rot
                                  // writeback around a hinted peak — region-
                                  // wide writes bleed into neighbour regions
                                  // when boundaries drift (measured flutter)
    float HINT_SEP_MAX = 1.6f;    // §24: hinted render only when the pair is
                                  // UNRESOLVABLE here (closer than this many
                                  // of the receiving window's bins) — where
                                  // it is partially resolvable, the window's
                                  // own rigid rendering measured better

    // §24 producer side: resolved partial frequencies of the LAST frame.
    int          hint_count() const noexcept { return _n_hint_out; }
    const float* hint_freqs() const noexcept { return _hint_out; }
    uint32_t     frame_seq()  const noexcept { return _frame_seq; }

    // §24 consumer side: the resolving companion's frequencies.
    void set_hints(const float* f, int n) noexcept {
        _n_hints = std::min(n, MAXHINT);
        for (int i = 0; i < _n_hints; ++i) _hints[i] = f[i];
    }
#ifdef POGGED_PRONY_DEBUG
    int dbg_engage = 0, dbg_birth = 0;   // frames rendered parametrically / new pairs
    int dbg_rej[10] = {};   // rejection counters, indexed by gate
#define PRONY_REJ(i) do { ++dbg_rej[i]; return false; } while (0)
#else
#define PRONY_REJ(i) return false
#endif

    void tune(float floor_, float slow, float fast, float th) noexcept {
        PEAK_FLOOR = floor_; SMOOTH_SLOW = slow; SMOOTH_FAST = fast; SMOOTH_TH = th;
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
        //
        // REAL-input FFT (§19): the frame is real, so the transform runs on
        // M = N/2 complex points (even samples in the real part, odd in the
        // imaginary) and the true half-spectrum X[0..M] is recovered by the
        // standard unpack below — half the butterfly work of the old
        // full-size complex FFT with a zeroed imaginary half.
        const uint64_t start = wpos - (uint64_t)N;
        for (int i = 0; i < M; i++)
            _work[i] = { ring[(start + (uint64_t)(2 * i))     & mask] * _win[2 * i],
                         ring[(start + (uint64_t)(2 * i + 1)) & mask] * _win[2 * i + 1] };
        _fft(_work, false);

        // Unpack Z = FFT_M(even + i·odd) into the real signal's half-spectrum
        // _cx[0..BINS): X[k] = (Z[k]+conj(Z[M-k]))/2 − i·tu[k]·(Z[k]−conj(Z[M-k]))/2
        // with tu[k] = e^{-i2πk/N} and Z[M] ≡ Z[0].
        for (int k = 0; k <= M; k++) {
            const std::complex<float> zk  = _work[k & (M - 1)];
            const std::complex<float> zmk = std::conj(_work[(M - k) & (M - 1)]);
            _cx[k] = 0.5f * (zk + zmk)
                   - 0.5f * (std::complex<float>(0.0f, 1.0f) * _tu[k]) * (zk - zmk);
        }

        // ── Analysis: true instantaneous frequency per bin ─────────────────
        // Phase difference over EB hops (the estimator baseline, see EB): the
        // per-bin phase history ring holds the phase EB frames back.
        const float expct = 2.0f * float(M_PI) * HOP / N;
        for (int k = 0; k < BINS; k++) {
            float mag   = std::abs(_cx[k]);
            float phase = std::arg(_cx[k]);

            float dp = phase - _ana_phase[_ph_idx * BINS + k];
            _ana_phase[_ph_idx * BINS + k] = phase;

            // Remove expected phase advance, wrap deviation to [-π, π]
            dp -= k * expct * (float)EB;
            dp -= 2.0f * float(M_PI) * std::round(dp * float(M_1_PI) * 0.5f);

            _ana_mag[k]  = mag;
            // dev(Hz) = dp/(2π) · sr/(EB·HOP) = dp/(2π) · (osamp/EB) · freq_pbin.
            // The original code dropped the 1/(2π): every instantaneous
            // frequency came out ~6.3× too far from its bin centre, which is
            // why the vocoder never reconstructed cleanly, shifts included.
            _ana_freq[k] = k * _freq_pbin
                         + dp * (_osamp / (float)EB) * _freq_pbin
                              * (float)(0.5 / M_PI);
            // De-alternated complex spectrum for the fractional lobe
            // translation below: a Hann-windowed sinusoid carries a linear
            // phase of ~−π per bin (sign alternation); removing it makes the
            // lobe a SMOOTH complex curve that can be linearly interpolated.
            _ana_cx[k] = (k & 1) ? -_cx[k] : _cx[k];
            _hist[_hist_idx][k] = _ana_cx[k];          // §22 Prony history
        }

        _ph_idx = (_ph_idx + 1) % EB;

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

        // ── §20 stability levers (latency-free) ───────────────────────────
        // An unresolved pair of partials (closer than the window's mainlobe)
        // is the shimmer engine: the picker sees one peak or two depending on
        // the beat phase, the merged lobe gets chopped into regions on the
        // two-peak frames, and the frequency estimate wobbles at the beat
        // rate, which the phasor integrates. Two counter-measures survived
        // measurement (a trough-gated merge of shallow-valley peak pairs was
        // tried and measured HARMFUL, like §16's blind absorption: the
        // surviving peak alternates with the beat and the weak partial is
        // mistuned while merged — see §20):

        // (1) Relative peak floor: peaks below PEAK_FLOOR of the frame's
        // strongest are noise-born phantoms; their regions would chop real
        // lobes' skirts. The ENERGY at those bins is untouched — they simply
        // join a real peak's region.
        _mmax = 0.0f;
        for (int i = 0; i < n_peaks; ++i)
            _mmax = std::max(_mmax, _ana_mag[_peaks[i]]);
        if (n_peaks > 0 && PEAK_FLOOR > 0.0f) {
            const float floor_m = PEAK_FLOOR * _mmax;
            int w = 0;
            for (int i = 0; i < n_peaks; ++i)
                if (_ana_mag[_peaks[i]] >= floor_m) _peaks[w++] = _peaks[i];
            n_peaks = w;
        }

        // (2) Two-speed per-track frequency smoothing: a merged pair's
        // estimate swings at the beat rate, and integrating that swing into
        // the phasor (and jittering d_frac) is audible FM. Match each peak
        // to the nearest track of the previous frame (±2 bins) and smooth
        // SLOWLY when the step is beat-wobble-sized, FAST when it is a real
        // move (a bent string sweeps ~a bin per frame; wobble stays well
        // under half a bin) — so bends keep tracking while wobble is tamed.
        std::memset(_non, 0, sizeof _non);
        for (int i = 0; i < n_peaks; ++i) {
            const int   p  = _peaks[i];
            const float fe = _ana_freq[p];
            int best = -1; float bd = 1e30f;
            for (int b = std::max(0, p - 2); b <= std::min(BINS - 1, p + 2); ++b)
                if (_trk_on[b]) {
                    const float d = std::abs(_trk_f[b] - fe);
                    if (d < bd) { bd = d; best = b; }
                }
            float fs = fe;
            if (best >= 0 && bd < 2.0f * _freq_pbin) {
                const float a = (bd > SMOOTH_TH * _freq_pbin) ? SMOOTH_FAST
                                                          : SMOOTH_SLOW;
                fs = _trk_f[best] + a * (fe - _trk_f[best]);
            }
            _nf[p]  = fs;
            _non[p] = true;
        }
        for (int b = 0; b < BINS; ++b) {
            _trk_on[b] = _non[b];
            if (_non[b]) _trk_f[b] = _nf[b];
        }

        std::memset(_npr_on, 0, sizeof _npr_on);   // §22: refilled per frame
        std::memset(_nht_on, 0, sizeof _nht_on);   // §24
        std::memset(_pk_prony, 0, sizeof _pk_prony);

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
            // §22: parametric rendering needs a full history and a real shift
            // (the unity-ratio dry copy must stay bit-faithful to the rigid
            // path, which is an identity there).
            const bool prony_ready = PRONY_ON && _warm >= PK &&
                                     std::abs(_ratio - 1.0f) > 0.01f;
            const bool hints_ready = _n_hints > 0 &&
                                     std::abs(_ratio - 1.0f) > 0.01f;
            for (int i = 0; i < n_peaks; ++i) {
                const int   p      = _peaks[i];
                const float inc    = rot_c * _nf[p];
                const int   lo     = _bounds[i], hi = _bounds[i + 1];
                for (int j = lo; j < hi; ++j) {
                    _rot[j] += inc;
                    _rot[j] -= 2.0f * float(M_PI) *
                               std::round(_rot[j] * float(M_1_PI) * 0.5f);
                }
                // §24 first: imported frequencies are ground truth. Take
                // the two hints nearest the peak inside the region's span;
                // if the resolver's peaks flickered away this frame, a held
                // pair track keeps the rendering engaged.
                if (hints_ready) {
                    float h1 = 0, h2 = 0; int nh = 0;
                    float d1 = 1e30f, d2m = 1e30f;
                    for (int t = 0; t < _n_hints; ++t) {
                        const float bh = _hints[t] / _freq_pbin;
                        if (bh < (float)lo - 0.5f || bh > (float)hi + 0.5f)
                            continue;
                        const float d = std::abs(bh - (float)p);
                        if (d < d1)       { d2m = d1; h2 = h1; d1 = d; h1 = _hints[t]; ++nh; }
                        else if (d < d2m) { d2m = d; h2 = _hints[t]; ++nh; }
                        else ++nh;
                    }
                    bool done = false;
                    if (nh >= 2) {
                        if (h2 < h1) std::swap(h1, h2);
                        done = _render_hinted(p, lo, hi, h1, h2);
                    } else {
                        // hold-over: continue a recently hinted pair
                        int bb = -1;
                        for (int b = std::max(0, p - 2);
                             b <= std::min(BINS - 1, p + 2); ++b)
                            if (_ht_on[b] && _ht_hold[b] > 0) { bb = b; break; }
                        if (bb >= 0) {
                            done = _render_hinted(p, lo, hi, _ht_f1[bb], _ht_f2[bb]);
                            if (done) _nht_hold[p] = _ht_hold[bb] - 1;
                        }
                    }
                    if (done) continue;
                }
                if (prony_ready && _nf[p] >= PRONY_FMIN &&
                    _try_parametric(p, lo, hi)) { _pk_prony[p] = true; continue; }
                const std::complex<float> ph = std::polar(1.0f, _rot[p]);
                const float d_frac = (_nf[p] / _freq_pbin) * (_ratio - 1.0f);
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
        // §22 frame bookkeeping: commit pair tracks, advance the history.
        for (int b = 0; b < BINS; ++b) {
            _pr_on[b] = _npr_on[b];
            if (_npr_on[b]) {
                _pr_f1[b] = _npr_f1[b]; _pr_f2[b] = _npr_f2[b];
                _pr_r1[b] = _npr_r1[b]; _pr_r2[b] = _npr_r2[b];
                _pr_cnt[b] = _npr_cnt[b];
            }
        }
        for (int b = 0; b < BINS; ++b) {
            _ht_on[b] = _nht_on[b];
            if (_nht_on[b]) {
                _ht_f1[b] = _nht_f1[b]; _ht_f2[b] = _nht_f2[b];
                _ht_r1[b] = _nht_r1[b]; _ht_r2[b] = _nht_r2[b];
                _ht_hold[b] = _nht_hold[b];
            }
        }
        // §24 hint export: RESOLVED peaks below HINT_FMAX only. Two exclusions,
        // both measured: a peak whose region the §22 fit took is a MERGED
        // pair — its own estimate sits near the pair's midpoint, and the §22
        // pair frequencies wobble on beating material (§23) — either export
        // poisons the receiver (parasite -4.4 dB vs -9.1 clean). Export
        // nothing for those; the receiver's hold-over bridges the gap.
        _n_hint_out = 0;
        for (int i = 0; i < n_peaks && _n_hint_out < MAXHINT; ++i) {
            const int p = _peaks[i];
            if (_nf[p] > HINT_FMAX) break;         // peaks sorted by bin
            if (_ana_mag[p] < 0.02f * _mmax) continue;
            if (_pk_prony[p]) continue;
            _hint_out[_n_hint_out++] = _nf[p];
        }
        ++_frame_seq;
        _hist_idx = (_hist_idx + 1) % PK;
        if (_warm < PK + 1) ++_warm;

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

        // Inverse real FFT: pack the Hermitian half-spectrum back into M
        // complex points (the exact inverse of the unpack above), IFFT_M,
        // and the time samples come out interleaved re/im = even/odd.
        for (int k = 0; k < M; k++) {
            const std::complex<float> a = _cx[k] + std::conj(_cx[M - k]);
            const std::complex<float> b = _cx[k] - std::conj(_cx[M - k]);
            _work[k] = 0.5f * (a + (std::complex<float>(0.0f, 1.0f)
                                    * std::conj(_tu[k])) * b);
        }
        _fft(_work, true);

        // ── Overlap-add ───────────────────────────────────────────────────
        // Hann analysis+synthesis normalization: the sum of w² over the
        // overlapping frames is 0.375 × osamp (= 1.5 at 4× / 75 % overlap),
        // so dividing by it yields unity passthrough. (The previous factor was
        // wrong by ~N/2, making the pitch voices ~1340× too quiet / inaudible.)
        const float scale = 1.0f / (0.375f * _osamp);
        for (int i = 0; i < M; i++) {
            const float e = _work[i].real() * _win[2 * i]     * scale;
            const float o = _work[i].imag() * _win[2 * i + 1] * scale;
            int idx = (_out_write + 2 * i) % OUTBUF;
            _out_buf[idx] += e;
            idx = (_out_write + 2 * i + 1) % OUTBUF;
            _out_buf[idx] += o;
        }
        _out_write = (_out_write + HOP) % OUTBUF;
        _out_fill  = std::min(_out_fill + HOP, OUTBUF);
    }

    // §22 — kernel lookup, linear interpolation, W(0) = N/2.
    float _kernel(float x) const noexcept {
        const float t = x * KOS + KW * KOS;
        if (t <= 0.0f || t >= 2 * KW * KOS) return 0.0f;
        const int   i = (int)t;
        const float f = t - i;
        return _kern[i] + f * (_kern[i + 1] - _kern[i]);
    }

    // §24 — render the region as the two IMPORTED partial frequencies, with
    // amplitudes solved on the current frame. No estimation gates needed:
    // the frequencies come from a window that resolves them. Returns false
    // only on numerical ill-conditioning (then rigid renders).
    bool _render_hinted(int p, int rlo, int rhi, float f1, float f2) noexcept {
        const float db1 = f1 / _freq_pbin - (float)p;
        const float db2 = f2 / _freq_pbin - (float)p;
        if (std::abs(db1) > 3.5f || std::abs(db2) > 3.5f ||
            std::abs(db1 - db2) < 0.05f ||
            std::abs(db1 - db2) > HINT_SEP_MAX) return false;
        const int q2 = (p + 1 <= BINS - 2 &&
                        (p == 0 || _ana_mag[p + 1] >= _ana_mag[p - 1]))
                       ? p + 1 : p - 1;
        if (q2 < 0) return false;
        const float k11 = _kernel(-db1),                  k12 = _kernel(-db2);
        const float k21 = _kernel((float)(q2 - p) - db1), k22 = _kernel((float)(q2 - p) - db2);
        const float d2  = k11 * k22 - k12 * k21;
        const float k0  = 0.5f * (float)N;
        if (std::abs(d2) < 0.02f * k0 * k0) return false;
        const std::complex<float> z1 = _ana_cx[p], z2 = _ana_cx[q2];
        const std::complex<float> A = ( z1 * k22 - z2 * k12) / d2;
        const std::complex<float> B = (-z1 * k21 + z2 * k11) / d2;

        // Phasor tracks keyed by bin; birth is synced to the rigid path's
        // rotation so the first hinted frame is phase-continuous.
        float r1 = _rot[p], r2 = _rot[p];
        int best = -1; float bd = 1e30f;
        for (int b = std::max(0, p - 2); b <= std::min(BINS - 1, p + 2); ++b)
            if (_ht_on[b]) {
                const float d = std::abs(_ht_f1[b] - f1) + std::abs(_ht_f2[b] - f2);
                if (d < bd) { bd = d; best = b; }
            }
        if (best >= 0 && bd < 4.0f * _freq_pbin) {
            r1 = (std::abs(_ht_f1[best] - f1) <= std::abs(_ht_f2[best] - f1))
                 ? _ht_r1[best] : _ht_r2[best];
            r2 = (std::abs(_ht_f2[best] - f2) <= std::abs(_ht_f1[best] - f2))
                 ? _ht_r2[best] : _ht_r1[best];
        }
        const float rc = 2.0f * float(M_PI) * HOP * (_ratio - 1.0f) / _sr;
        r1 += rc * f1;
        r1 -= 2.0f * float(M_PI) * std::round(r1 * float(M_1_PI) * 0.5f);
        r2 += rc * f2;
        r2 -= 2.0f * float(M_PI) * std::round(r2 * float(M_1_PI) * 0.5f);
        _nht_f1[p] = f1; _nht_f2[p] = f2;
        _nht_r1[p] = r1; _nht_r2[p] = r2;
        _nht_hold[p] = 12;            // survives the resolver's peak flicker
        _nht_on[p] = true;

        // Keep the rigid path's rotation riding the dominant partial — but
        // only within HINT_ROTW bins of the peak: region boundaries drift
        // frame to frame, and a wider write is inherited by the NEIGHBOUR
        // region's rigid phasors when the boundary shifts (measured as added
        // 5-80 Hz flutter on the neighbouring harmonics).
        const float rdom = (std::abs(A) >= std::abs(B)) ? r1 : r2;
        for (int j = std::max(rlo, p - HINT_ROTW);
             j < std::min(rhi, p + HINT_ROTW + 1); ++j)
            _rot[j] = rdom;

        const float fs[2] = { f1, f2 };
        const float rs[2] = { r1, r2 };
        const std::complex<float> as[2] = { A, B };
        for (int i = 0; i < 2; ++i) {
            const float tb = fs[i] * _ratio / _freq_pbin;
            const std::complex<float> ph = as[i] * std::polar(1.0f, rs[i]);
            const int d_lo = std::max(1, (int)std::ceil(tb - KW));
            const int d_hi = std::min(BINS - 2, (int)std::floor(tb + KW));
            for (int dst = d_lo; dst <= d_hi; ++dst) {
                std::complex<float> v = ph * _kernel((float)dst - tb);
                if (dst & 1) v = -v;
                _cx[dst] += v;
            }
        }
        return true;
    }

    // §22 — try the parametric (two-partial) rendering for the region whose
    // peak is at bin p. Returns true when it rendered the region (the caller
    // then skips the rigid translation); false = fall back.
    bool _try_parametric(int p, int rlo, int rhi) noexcept {
        // Significance: the parametric path exists for the LOUD colliding
        // partials of a chord; on noise-floor regions an order-2 fit happily
        // overfits and was measured to fire hundreds of phantom births.
        if (_ana_mag[p] < 0.03f * _mmax) PRONY_REJ(0);

        // The series of the de-alternated spectrum at bin p over the last PK
        // frames, oldest first — any bin of the region carries the same two
        // exponentials, so the series may stay at p even if the peak drifted.
        std::complex<float> sr_[PK];
        for (int m = 0; m < PK; ++m)
            sr_[m] = _hist[(_hist_idx + 1 + m) % PK][p];

        // Least-squares Prony, order 2: z[m] ≈ a1 z[m-1] + a2 z[m-2].
        std::complex<float> Suv = 0, Syu = 0, Syv = 0;
        float Suu = 0, Svv = 0, Eyy = 0;
        for (int m = 2; m < PK; ++m) {
            const std::complex<float> y = sr_[m], u = sr_[m - 1], v = sr_[m - 2];
            Suu += std::norm(u);
            Svv += std::norm(v);
            Suv += std::conj(u) * v;
            Syu += std::conj(u) * y;
            Syv += std::conj(v) * y;
            Eyy += std::norm(y);
        }
        const float det = Suu * Svv - std::norm(Suv);
        if (!(det > 1e-12f * Suu * Svv) || Eyy < 1e-12f) PRONY_REJ(1);
        const std::complex<float> a1 = (Svv * Syu - Suv * Syv) / det;
        const std::complex<float> a2 = (Suu * Syv - std::conj(Suv) * Syu) / det;

        // Gate hysteresis: as a beating pair's peak bin drifts onto a bin
        // one partial dominates, the LOCAL series under-represents the other
        // and the strict residual gates flicker — and every flicker is a
        // rendering transition. A pair already alive nearby may continue at
        // looser gates; only NEW pairs must pass the strict ones. Singles
        // never create a pair track, so the loose path never opens for them.
        bool near_pair = false;
        for (int b = std::max(0, p - 2); b <= std::min(BINS - 1, p + 2); ++b)
            near_pair |= _pr_on[b];

        // The fit must actually explain the series (decays, onsets and noise
        // do not look like two steady exponentials — those fall back).
        float E = 0;
        for (int m = 2; m < PK; ++m)
            E += std::norm(sr_[m] - a1 * sr_[m - 1] - a2 * sr_[m - 2]);
        if (E > (near_pair ? 0.15f : 0.05f) * Eyy) PRONY_REJ(2);

        // THE decisive question — is there really a second exponential? The
        // order-2 fit must beat the order-1 fit by an order of magnitude. A
        // single partial (+noise) is already explained by one exponential,
        // so its E1 is tiny and the ratio rejects; a genuine pair leaves
        // order-1 with the whole beat as residual.
        //
        std::complex<float> Sc = 0; float Su1 = 0;
        for (int m = 1; m < PK; ++m) {
            Sc  += std::conj(sr_[m - 1]) * sr_[m];
            Su1 += std::norm(sr_[m - 1]);
        }
        if (Su1 < 1e-20f) PRONY_REJ(3);
        const std::complex<float> c1 = Sc / Su1;
        float E1 = 0;
        for (int m = 1; m < PK; ++m)
            E1 += std::norm(sr_[m] - c1 * sr_[m - 1]);
        if (E > (near_pair ? 0.30f : 0.08f) * E1) PRONY_REJ(3);

        // Roots of r² − a1·r − a2: the two per-hop phase advances.
        const std::complex<float> sq = std::sqrt(a1 * a1 + 4.0f * a2);
        const std::complex<float> r1c = 0.5f * (a1 + sq), r2c = 0.5f * (a1 - sq);
        const float m1 = std::abs(r1c), m2 = std::abs(r2c);
        if (m1 < 0.6f || m1 > 1.5f || m2 < 0.6f || m2 > 1.5f) PRONY_REJ(4);

        // Angles -> frequency offsets from bin p (±OS_/2 bins unambiguous).
        const float tob = (float)OS_ * (float)(0.5 / M_PI);   // rad -> bins
        float db1 = std::remainder(std::arg(r1c) * tob - (float)p, (float)OS_);
        float db2 = std::remainder(std::arg(r2c) * tob - (float)p, (float)OS_);
        const float sep = std::abs(db1 - db2);
        // Too far apart = not one merged lobe. Too close = NOT a chord
        // collision but a single partial's own fine structure (a real
        // string's polarization doublet / AM sidebands sit within ~3 Hz):
        // engaging there fits a two-component model to a three-component
        // reality and mangles it — 0.3 bins keeps every measured inter-note
        // collision while staying above intra-note structure.
        if (sep < 0.30f || sep > 3.5f ||
            std::abs(db1) > 3.2f || std::abs(db2) > 3.2f) PRONY_REJ(5);

        // Complex amplitudes from the CURRENT frame: two bins, 2x2 solve
        // against the known kernel. kern(0) = N/2 keeps everything in the
        // analysis convention — the same table synthesises, so amplitudes
        // need no normalization.
        const int q2 = (p + 1 <= BINS - 2 &&
                        (p == 0 || _ana_mag[p + 1] >= _ana_mag[p - 1]))
                       ? p + 1 : p - 1;
        if (q2 < 0) return false;
        const float k11 = _kernel(-db1),            k12 = _kernel(-db2);
        const float k21 = _kernel(q2 - p - db1),    k22 = _kernel(q2 - p - db2);
        const float d2  = k11 * k22 - k12 * k21;
        const float k0  = 0.5f * (float)N;
        if (std::abs(d2) < 0.02f * k0 * k0) PRONY_REJ(6);
        const std::complex<float> z1 = _ana_cx[p], z2 = _ana_cx[q2];
        std::complex<float> A = ( z1 * k22 - z2 * k12) / d2;
        std::complex<float> B = (-z1 * k21 + z2 * k11) / d2;

        // Two alias/noise guards. At OS_=8 a root's frequency is only known
        // modulo OS_ bins, so a NEIGHBOURING harmonic's sidelobe leak can
        // masquerade as an in-range second partial (9.4 bins folds to 1.4).
        // (1) a genuine pair carries real level on both sides; (2) the pair
        // hypothesis must explain the lobe SHAPE at a third bin the 2x2
        // solve never saw — an aliased component cannot.
        const float aA = std::abs(A), aB = std::abs(B);
        if (aB < 0.06f * aA || aA < 0.06f * aB) PRONY_REJ(7);
        const int q3 = (q2 == p + 1) ? p - 1 : p + 1;
        // ...and at the second bin on the off-centre candidate's side: the
        // Hann kernel is EXACTLY zero at integer offsets >= 2, so a genuine
        // off-centre partial must put energy there, while an aliased phantom
        // (a distant harmonic folded mod OS_) predicts energy the real
        // spectrum does not have. This is the decisive alias discriminator.
        const float far_db = (std::abs(db1) > std::abs(db2)) ? db1 : db2;
        const int   q4 = p + ((far_db >= 0.0f) ? 2 : -2);
        for (int q : { q3, q4 }) {
            if (q < 0 || q > BINS - 1) continue;
            const std::complex<float> pred =
                A * _kernel((float)(q - p) - db1) +
                B * _kernel((float)(q - p) - db2);
            if (std::abs(pred - _ana_cx[q]) >
                0.15f * (std::abs(_ana_cx[p]) + 1e-20f)) PRONY_REJ(8);
        }

        // Pair track: inherit phasors and smoothed frequencies from the
        // nearest pair of the previous frame (±2 bins), assignment by
        // nearest frequency so the two partials cannot swap phasors.
        float f1 = ((float)p + db1) * _freq_pbin;
        float f2 = ((float)p + db2) * _freq_pbin;
        int best = -1; float bd = 1e30f;
        for (int b = std::max(0, p - 2); b <= std::min(BINS - 1, p + 2); ++b)
            if (_pr_on[b]) {
                const float d = std::abs(_pr_f1[b] - f1) + std::abs(_pr_f2[b] - f2);
                const float dx = std::abs(_pr_f1[b] - f2) + std::abs(_pr_f2[b] - f1);
                const float dm = std::min(d, dx);
                if (dm < bd) { bd = dm; best = b; }
            }
        // Engagement hysteresis: starting a NEW pair demands a strong,
        // unambiguous separation; an already-tracked pair may continue at
        // the lower threshold — the borderline cases cannot flap between
        // the parametric and rigid renderings frame to frame.
        const bool tracked = (best >= 0 && bd < 4.0f * _freq_pbin);
        if (!tracked && sep < 0.45f) PRONY_REJ(9);
        int cnt = 1;
        float r1 = _rot[p], r2 = _rot[p];
        if (tracked) {
            if (std::abs(_pr_f1[best] - f2) + std::abs(_pr_f2[best] - f1) <
                std::abs(_pr_f1[best] - f1) + std::abs(_pr_f2[best] - f2)) {
                std::swap(f1, f2);
                std::swap(A, B);
            }
            f1 = _pr_f1[best] + 0.3f * (f1 - _pr_f1[best]);
            f2 = _pr_f2[best] + 0.3f * (f2 - _pr_f2[best]);
            r1 = _pr_r1[best];
            r2 = _pr_r2[best];
            cnt = _pr_cnt[best] + 1;
        }
        // Probation: a pair renders only after 3 consecutive good fits —
        // one-frame flukes never reach the output. While on probation the
        // phasors stay synced to the rigid path's rotation, so the first
        // rendered frame is phase-CONTINUOUS with what was playing.
        if (cnt < 3) {
            _npr_f1[p] = f1; _npr_f2[p] = f2;
            _npr_r1[p] = _rot[p]; _npr_r2[p] = _rot[p];
            _npr_on[p] = true; _npr_cnt[p] = cnt;
            return false;
        }
        const float rc = 2.0f * float(M_PI) * HOP * (_ratio - 1.0f) / _sr;
        r1 += rc * f1;
        r1 -= 2.0f * float(M_PI) * std::round(r1 * float(M_1_PI) * 0.5f);
        r2 += rc * f2;
        r2 -= 2.0f * float(M_PI) * std::round(r2 * float(M_1_PI) * 0.5f);
        _npr_f1[p] = f1; _npr_f2[p] = f2;
        _npr_r1[p] = r1; _npr_r2[p] = r2;
        _npr_on[p] = true; _npr_cnt[p] = cnt;
        // Reverse handoff: keep the rigid path's rotation riding along the
        // DOMINANT partial's phasor, so a later fallback frame resumes in
        // phase instead of jumping.
        {
            const float rdom = (std::abs(A) >= std::abs(B)) ? r1 : r2;
            for (int j = rlo; j < rhi; ++j) _rot[j] = rdom;
        }
#ifdef POGGED_PRONY_DEBUG
        ++dbg_engage;
        if (!tracked) {
            ++dbg_birth;
            std::printf("[prony birth] p=%d db1=%+.2f db2=%+.2f sep=%.2f |A|=%.3g |B|=%.3g\n",
                        p, db1, db2, sep, std::abs(A), std::abs(B));
        }
#endif

        // Synthesis: one analytic kernel per partial at its own ×ratio
        // target, rotated by its own phasor, alternation restored like the
        // rigid path does.
        const struct { float f, r; std::complex<float> a; } part[2] = {
            { f1, r1, A }, { f2, r2, B }
        };
        for (const auto& q : part) {
            const float tb = q.f * _ratio / _freq_pbin;
            const std::complex<float> ph = q.a * std::polar(1.0f, q.r);
            const int d_lo = std::max(1, (int)std::ceil(tb - KW));
            const int d_hi = std::min(BINS - 2, (int)std::floor(tb + KW));
            for (int dst = d_lo; dst <= d_hi; ++dst) {
                std::complex<float> v = ph * _kernel((float)dst - tb);
                if (dst & 1) v = -v;
                _cx[dst] += v;
            }
        }
        return true;
    }

    // ── Radix-2 DIT Cooley-Tukey FFT, size M, table-driven ────────────────
    // Twiddles come from the precomputed _tw table instead of the running
    // product w *= wlen the old code used: the table kills both the serial
    // dependency in the inner loop (the vectorizer's enemy) and the rounding
    // drift the product accumulated over long stages. Inverse via conjugate
    // twiddles + 1/M.
    void _fft(std::complex<float>* a, bool inverse) noexcept {
        // Bit-reversal permutation
        for (int i = 1, j = 0; i < M; i++) {
            int bit = M >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        // Butterfly stages; _tw[j·step] = e^{-2πi·j/len} with step = M/len.
        for (int len = 2; len <= M; len <<= 1) {
            const int step = M / len;
            for (int i = 0; i < M; i += len) {
                for (int j = 0; j < len / 2; j++) {
                    std::complex<float> w = _tw[j * step];
                    if (inverse) w = std::conj(w);
                    const auto u = a[i + j];
                    const auto v = a[i + j + len / 2] * w;
                    a[i + j]           = u + v;
                    a[i + j + len / 2] = u - v;
                }
            }
        }
        if (inverse) {
            const float s = 1.0f / (float)M;
            for (int i = 0; i < M; i++) a[i] *= s;
        }
    }

    // ── State ──────────────────────────────────────────────────────────────
    float _sr        = 48000.0f;
    float _ratio     = 1.0f;
    float _freq_pbin = 48000.0f / N;
    float _osamp     = 4.0f;

    float _win[N]               = {};
    float _ana_phase[EB * BINS] = {};   // phase history ring (see EB)
    int   _ph_idx               = 0;
    float _ana_mag[BINS]        = {};
    float _ana_freq[BINS]       = {};
    std::complex<float> _ana_cx[BINS] = {};   // de-alternated analysis lobe
    float _rot[BINS]            = {};   // per-region rotation accumulators
    // §20 stability state: per-track smoothed frequencies + merge hysteresis
    float _trk_f[BINS]          = {};
    float _nf[BINS]             = {};
    bool  _trk_on[BINS]         = {};
    bool  _non[BINS]            = {};
    // §22 state: spectra history ring for the Prony fit, per-pair tracks
    // (frequencies + per-partial shift phasors, keyed by peak bin), kernel.
    std::complex<float> _hist[PK][BINS] = {};
    int   _hist_idx             = 0;
    int   _warm                 = 0;
    float _pr_f1[BINS]          = {};
    float _pr_f2[BINS]          = {};
    float _pr_r1[BINS]          = {};
    float _pr_r2[BINS]          = {};
    bool  _pr_on[BINS]          = {};
    int   _pr_cnt[BINS]         = {};
    int   _npr_cnt[BINS]        = {};
    float _mmax                 = 0.0f;
    float _npr_f1[BINS]         = {};
    float _npr_f2[BINS]         = {};
    float _npr_r1[BINS]         = {};
    float _npr_r2[BINS]         = {};
    bool  _npr_on[BINS]         = {};
    float _kern[2 * KW * KOS + 1] = {};
    // §24 state: hint export/import + hinted-pair tracks (phasors per pair,
    // keyed by peak bin, with a hold-over so the resolver's peak flicker
    // cannot flap the rendering).
    float    _hint_out[MAXHINT] = {};
    int      _n_hint_out        = 0;
    uint32_t _frame_seq         = 0;
    float    _hints[MAXHINT]    = {};
    int      _n_hints           = 0;
    bool     _pk_prony[BINS]    = {};   // peak's region rendered by §22 this frame
    float    _ht_f1[BINS]       = {};
    float    _ht_f2[BINS]       = {};
    float    _ht_r1[BINS]       = {};
    float    _ht_r2[BINS]       = {};
    int      _ht_hold[BINS]     = {};
    bool     _ht_on[BINS]       = {};
    float    _nht_f1[BINS]      = {};
    float    _nht_f2[BINS]      = {};
    float    _nht_r1[BINS]      = {};
    float    _nht_r2[BINS]      = {};
    int      _nht_hold[BINS]    = {};
    bool     _nht_on[BINS]      = {};

    float _swl[BINS]            = {};   // per-bin swell envelope (see set_swell)
    float _swell_c              = -1.0f;   // < 0 = swell off
    int   _peaks[BINS]          = {};   // per-frame peak list
    int   _bounds[BINS + 1]     = {};   // per-frame region boundaries
    float _out_buf[OUTBUF]      = {};
    // Real-FFT machinery (§19): half-size complex work buffer, spectrum
    // (BINS, not N — the Hermitian mirror never exists in memory any more),
    // and the two twiddle tables filled in init().
    std::complex<float> _cx[BINS]    = {};   // half-spectrum, analysis+synthesis
    std::complex<float> _work[N / 2] = {};   // M-point FFT buffer
    std::complex<float> _tw[N / 4]   = {};   // FFT twiddles e^{-2πi j/M}
    std::complex<float> _tu[N / 2 + 1] = {}; // (un)pack twiddles e^{-2πi k/N}

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
