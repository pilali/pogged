#pragma once
#include <cmath>
#include <cstdint>
#include <complex>
#include <algorithm>

// §28 (Spike 12) — octave-anchor for the DOWN voices.
//
// A ÷2 / ÷4 pitch shift copies the input's harmonic BALANCE, so when a note's
// input fundamental is weak (frequent on guitar, where h2/h3 dominate) the
// shifted fundamental f·ratio is weak too and the ear locks back onto the
// original octave — the "octave wandering" the user heard (§27/§28). No fixed
// filter fixes it (the target moves 40-330 Hz per note; several coexist in a
// chord).
//
// Fix that measured 35/36 notes stable on the user's own recording: keep the
// raw shifter for the natural TIMBRE and ADD a resynthesised harmonic series on
// the shifted fundamental to lock the perceived octave. This is that anchor:
//   - a hop-rate SHS pitch detector with octave correction finds the salient
//     input f0's (polyphonic — several peaks coexist);
//   - a 1-Hz OSCILLATOR GRID (continuous phase per slot) sings a full harmonic
//     series on each f0·ratio, amplitude from the input's spectral envelope.
// Only the handful of slots that are actually sounding are summed each sample,
// so the grid is cheap. The caller mixes anchor() UNDER the raw shifted voice.
//
// Self-contained (own ring, FFT, grid). One N-point FFT per HOP.

template <int N_ = 4096, int HOP_ = 1024>
class OctaveAnchor {
public:
    static constexpr int N = N_, HOP = HOP_, NB = N/2 + 1;
    static constexpr int GMIN = 20, GMAX = 1400, NG = GMAX - GMIN + 1;
    static constexpr int NHARM = 12;   // harmonics synthesised per detected note
    static constexpr int MAXACT = 128; // max simultaneously sounding grid slots

    void init(double sr, float ratio) noexcept {
        _sr = (float)sr; _ratio = ratio; _df = _sr / N;
        for (int i = 0; i < N; ++i)
            _win[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (N - 1)));
        for (int k = 0; k < N; ++k) {
            int r = 0; for (int b = 0, x = k; b < LOGN; ++b, x >>= 1) r = (r << 1) | (x & 1);
            _br[k] = r;
        }
        for (int g = 0; g < NG; ++g) _dph[g] = 2.0f * (float)M_PI * (GMIN + g) / _sr;
        reset();
    }

    void reset() noexcept {
        for (auto& s : _ring) s = 0.0f;
        _wpos = 0; _cnt = 0;
        for (int g = 0; g < NG; ++g) { _ampc[g] = _ampt[g] = _ph[g] = 0.0f; }
        _nact = 0;
    }

    void set_ratio(float r) noexcept { _ratio = r; }

    // Per-sample: push the input, refresh the analysis every HOP, return the
    // anchor sample (grid amplitudes ramp between analyses; phase is continuous).
    float process(float x) noexcept {
        _ring[_wpos & (N - 1)] = x;
        ++_wpos;
        if (++_cnt >= HOP) { _cnt = 0; _analyse(); }
        const float t = (float)_cnt * (1.0f / HOP);
        float acc = 0.0f;
        for (int a = 0; a < _nact; ++a) {
            const int g = _act[a];
            const float amp = _ampc[g] + t * (_ampt[g] - _ampc[g]);
            acc += amp * std::sin(_ph[g]);
            _ph[g] += _dph[g];
            if (_ph[g] > 2.0f * (float)M_PI) _ph[g] -= 2.0f * (float)M_PI;
        }
        return acc;
    }

private:
    static constexpr int LOGN = []{ int l = 0, n = N_; while (n > 1) { n >>= 1; ++l; } return l; }();

    void _fft(std::complex<float>* a) noexcept {   // forward only
        for (int i = 0; i < N; ++i) if (i < _br[i]) std::swap(a[i], a[_br[i]]);
        for (int len = 2; len <= N; len <<= 1) {
            const float ang = -2.0f * (float)M_PI / len;
            const std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (int i = 0; i < N; i += len) {
                std::complex<float> w(1, 0);
                for (int k = 0; k < len/2; ++k) {
                    const std::complex<float> u = a[i+k], v = a[i+k+len/2] * w;
                    a[i+k] = u + v; a[i+k+len/2] = u - v; w *= wl;
                }
            }
        }
    }

    void _analyse() noexcept {
        for (int i = 0; i < N; ++i)
            _work[i] = std::complex<float>(_ring[(_wpos - N + i) & (N - 1)] * _win[i], 0.0f);
        _fft(_work);
        for (int k = 0; k < NB; ++k) _mag[k] = std::abs(_work[k]);

        const int BMIN = std::max(2, (int)(55.0f / _df)), BMAX = std::min(NB-1, (int)(700.0f / _df));
        float smax = 0;
        for (int b = BMIN; b <= BMAX; ++b) {
            float s = 0, wt = 1.0f;
            for (int h = 1; h <= 8; ++h) { const int kk = h*b; if (kk < NB) s += _mag[kk] * wt; wt *= 0.9f; }
            _sal[b] = s; smax = std::max(smax, s);
        }
        auto oddsup = [&](int b) -> float {
            if (b < 1 || b >= NB) return 0;
            float odd = _mag[b]; int no = 1;
            for (int h = 3; h <= 7; h += 2) { const int kk = h*b; if (kk < NB) { odd += _mag[kk]; ++no; } }
            float ev = 1e-9f; int ne = 0;
            for (int h = 2; h <= 8; h += 2) { const int kk = h*b; if (kk < NB) { ev += _mag[kk]; ++ne; } }
            return (odd / no) / (ev / std::max(1, ne) + 1e-9f);
        };
        auto env = [&](int fbin) -> float {
            const int w = std::max(2, fbin/12);
            float s = 0; int c = 0;
            for (int j = fbin-w; j <= fbin+w; ++j) if (j >= 0 && j < NB) { s += _mag[j]; ++c; }
            return c ? s / c : 0.0f;
        };

        // Latch: the target we ramped TO last block is what we ramp FROM now.
        for (int g = 0; g < NG; ++g) { _ampc[g] = _ampt[g]; _ampt[g] = 0.0f; }
        for (int b = BMIN; b <= BMAX; ++b) {
            if (_sal[b] < 0.25f * smax) continue;
            if (!(_sal[b] >= _sal[b-1] && _sal[b] > _sal[b+1])) continue;
            int f0 = b;
            while (f0/2 >= BMIN && _sal[f0/2] > 0.55f * _sal[b] && oddsup(f0/2) > 0.5f) f0 /= 2;
            if (oddsup(f0) < 0.35f) continue;
            const float sub = (f0 * _df) * _ratio;          // shifted fundamental, Hz
            for (int n = 1; n <= NHARM; ++n) {
                const float fn = n * sub;
                if (fn > (float)GMAX) break;
                const int g = (int)std::lround(fn) - GMIN;
                if (g < 0 || g >= NG) continue;
                // 4/N converts a Hann-windowed FFT-bin magnitude back to the
                // partial's time-domain amplitude, so the anchor comes out in
                // audio range (the caller then mixes it at a modest gain).
                _ampt[g] += (4.0f / N) * env(n * f0) / n;    // source-envelope, low-emphasis
            }
        }

        // Rebuild the active set: any slot ringing (cur) or targeted (tgt).
        // A slot born from silence gets phase 0 (it ramps up, so no click).
        _nact = 0;
        for (int g = 0; g < NG && _nact < MAXACT; ++g) {
            if (_ampt[g] > 1e-7f || _ampc[g] > 1e-7f) {
                if (_ampc[g] <= 1e-7f) _ph[g] = 0.0f;
                _act[_nact++] = g;
            }
        }
    }

    float _sr = 48000.0f, _ratio = 0.5f, _df = 11.7f;
    float _win[N] = {};
    float _mag[NB] = {};
    float _sal[NB] = {};
    std::complex<float> _work[N] = {};
    int   _br[N] = {};
    float _ring[N] = {};
    uint64_t _wpos = 0;
    int _cnt = 0;
    // 1-Hz oscillator grid
    float _dph[NG] = {};
    float _ph[NG]  = {};
    float _ampc[NG] = {};
    float _ampt[NG] = {};
    int   _act[MAXACT] = {};
    int   _nact = 0;
};
