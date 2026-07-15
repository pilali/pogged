// Pogged — POG2-style polyphonic octave generator, host-agnostic DSP core.
//
// Live mono input is written to a ring buffer; up to six StreamShifters read
// behind the write head (sub -1/-2 oct, +1/+2 oct, plus a detuned pair on the
// up voices). The wet mix goes through the attack/swell envelope and the
// resonant low-pass, then joins the *undelayed* dry path: the POG's defining
// trait is a zero-latency dry signal, and since every wet voice sits at a
// different frequency there is no unison to comb-filter against.
#include "pogged_dsp.h"
#include "stream_shifter.hpp"
#include "onset_detector.hpp"
#include "biquad.hpp"
#include "envelope.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <new>
#include <vector>

#if defined(__SSE2__)
#include <immintrin.h>
#endif

// ── Denormal guard ───────────────────────────────────────────────────────────
// The LV2 build gets -ffast-math, but hosts embedding the core (JUCE/MSVC)
// may not: force FTZ/DAZ for the duration of process() and restore on exit.
class ScopedFlushToZero {
public:
    ScopedFlushToZero() noexcept {
#if defined(__SSE2__)
        _saved = _mm_getcsr();
        _mm_setcsr(_saved | 0x8040);            // FTZ | DAZ
#elif defined(__aarch64__)
        asm volatile("mrs %0, fpcr" : "=r"(_saved));
        asm volatile("msr fpcr, %0" :: "r"(_saved | (1ull << 24)));  // FZ
#endif
    }
    ~ScopedFlushToZero() noexcept {
#if defined(__SSE2__)
        _mm_setcsr(_saved);
#elif defined(__aarch64__)
        asm volatile("msr fpcr, %0" :: "r"(_saved));
#endif
    }
private:
#if defined(__SSE2__)
    unsigned int _saved = 0;
#elif defined(__aarch64__)
    uint64_t _saved = 0;
#endif
};

// Knee clipper (Megalo's soft_clip): bit-transparent below ~-3 dBFS, smooth
// tanh saturation above, bounded at ±1.
static inline float soft_clip(float x) noexcept {
    constexpr float KNEE = 0.7f;
    const float a = std::abs(x);
    if (a <= KNEE) return x;
    const float y = KNEE + (1.0f - KNEE) * std::tanh((a - KNEE) / (1.0f - KNEE));
    return (x < 0.0f) ? -y : y;
}

static inline uint32_t next_pow2(uint32_t v) noexcept {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// ── Voice bank layout ────────────────────────────────────────────────────────
enum Voice { V_SUB1 = 0, V_SUB2, V_UP1, V_UP2, V_UP1D, V_UP2D, N_VOICES };

struct PoggedDsp {
    double sample_rate = 48000.0;

    std::vector<float> ring;      // power-of-2, allocated once in _new
    uint32_t mask = 0;
    uint64_t wpos = 0;            // absolute write count (starts at ring size)

    StreamShifter sh[N_VOICES];
    bool          sh_live[N_VOICES] = {};   // false ⇒ needs reset before reuse

    Biquad        filter;
    Envelope      env;
    OnsetDetector det;

    // Smoothed gains (per-sample one-pole, ~30 ms)
    float g_dry = 1.0f, g_sub1 = 0.0f, g_sub2 = 0.0f;
    float g_up1 = 0.0f, g_up2 = 0.0f, g_out = 1.0f;

    // Filter smoothing/caching (Megalo pattern)
    float smooth_cutoff = -1.0f, smooth_q = 0.707f;
    float cached_cutoff = -1.0f, cached_q = -1.0f;
    bool  lp_bypassed   = true;

    // Attack/swell state
    float env_level   = 0.0f;
    int   pending_trig = 0;       // duck-then-swell: samples until trigger()
    int   sil_count    = 0;       // samples of near-silence (release gate)
};

// ── Lifecycle ────────────────────────────────────────────────────────────────
PoggedDsp* pogged_dsp_new(double sample_rate)
{
    PoggedDsp* p = new (std::nothrow) PoggedDsp();
    if (!p) return nullptr;
    p->sample_rate = sample_rate;

    const uint32_t len = std::clamp(next_pow2((uint32_t)(0.25 * sample_rate)),
                                    16384u, 131072u);
    p->ring.assign(len, 0.0f);
    p->mask = len - 1;

    const float sr = (float)sample_rate;
    const int grain_up  = (int)(0.025f * sr);   // 25 ms — low lag, POG shimmer
    const int grain_sub = (int)(0.055f * sr);   // 55 ms — ≥2 periods of low E
    const int align     = (int)(0.010f * sr);   // 10 ms correlation scan

    // Aligned respawn everywhere: without it the source-position jump at
    // respawn ((ratio-1)·grain) lands at arbitrary phase — for a 25 ms grain
    // at ratio 2 that is exactly half a period of the doubled fundamental,
    // so alternate grains cancel the target pitch outright. Correlation
    // alignment (SOLA-style) keeps grains phase-coherent for any input.
    p->sh[V_SUB1].setup(0.5f,  grain_sub, align);
    p->sh[V_SUB2].setup(0.25f, grain_sub, align);
    p->sh[V_UP1 ].setup(2.0f,  grain_up,  align);
    p->sh[V_UP2 ].setup(4.0f,  grain_up,  align);
    p->sh[V_UP1D].setup(2.0f,  grain_up,  align);
    p->sh[V_UP2D].setup(4.0f,  grain_up,  align);

    p->det.init(sr);
    pogged_dsp_reset(p);
    return p;
}

void pogged_dsp_free(PoggedDsp* p)
{
    delete p;
}

void pogged_dsp_reset(PoggedDsp* p)
{
    std::fill(p->ring.begin(), p->ring.end(), 0.0f);
    p->wpos = p->ring.size();     // keep absolute read positions non-negative
    for (int v = 0; v < N_VOICES; ++v) {
        p->sh[v].reset();
        p->sh_live[v] = false;
    }
    p->filter.reset();
    p->env.reset();
    p->det.reset();
    p->smooth_cutoff = -1.0f;
    p->smooth_q      = 0.707f;
    p->cached_cutoff = p->cached_q = -1.0f;
    p->lp_bypassed   = true;
    p->env_level     = 0.0f;
    p->pending_trig  = 0;
    p->sil_count     = 0;
}

// ── Processing ───────────────────────────────────────────────────────────────
void pogged_dsp_process(PoggedDsp* p, const PoggedParams* p_,
                        const float* in, float* out, uint32_t n_samples)
{
    ScopedFlushToZero ftz;
    const float sr = (float)p->sample_rate;

    // ── Snapshot controls (block boundary) ────────────────────────────────
    const float dry_t   = std::clamp(p_->dry_level,    0.0f, 2.0f);
    const float sub1_t  = std::clamp(p_->sub1_level,   0.0f, 2.0f);
    const float sub2_t  = std::clamp(p_->sub2_level,   0.0f, 2.0f);
    const float up1_t   = std::clamp(p_->up1_level,    0.0f, 2.0f);
    const float up2_t   = std::clamp(p_->up2_level,    0.0f, 2.0f);
    const float det_ct  = std::clamp(p_->detune_cents, 0.0f, 25.0f);
    const float atk_ms  = std::clamp(p_->attack_ms,    0.0f, 2000.0f);
    const float sens    = std::clamp(p_->attack_sens,  0.0f, 1.0f);
    const float cutoff  = std::clamp(p_->lp_cutoff,   20.0f, std::min(20000.0f, sr * 0.499f));
    const float q       = std::clamp(p_->lp_q,         0.5f, 8.0f);
    const float out_t   = std::clamp(p_->out_level,    0.0f, 2.0f);

    const bool detune_on = det_ct > 0.5f;
    const bool env_on    = atk_ms > 1.0f;

    // Detuned up-voice ratios: static offset, opposite signs on +1 / +2 for
    // a wider chorus (per-block pow is fine).
    if (detune_on) {
        const float c = det_ct / 1200.0f;
        const int grain_up = (int)(0.025f * sr);
        const int align    = (int)(0.010f * sr);
        p->sh[V_UP1D].setup(2.0f * std::pow(2.0f,  c), grain_up, align);
        p->sh[V_UP2D].setup(4.0f * std::pow(2.0f, -c), grain_up, align);
    }

    // Which shifters run this block. A voice is live while its target OR its
    // smoothed gain is audible, so it fades out before being skipped.
    const bool act[N_VOICES] = {
        sub1_t > 1e-4f || p->g_sub1 > 1e-4f,
        sub2_t > 1e-4f || p->g_sub2 > 1e-4f,
        up1_t  > 1e-4f || p->g_up1  > 1e-4f,
        up2_t  > 1e-4f || p->g_up2  > 1e-4f,
        detune_on && (up1_t > 1e-4f || p->g_up1 > 1e-4f),
        detune_on && (up2_t > 1e-4f || p->g_up2 > 1e-4f),
    };
    for (int v = 0; v < N_VOICES; ++v) {
        if (act[v] && !p->sh_live[v]) p->sh[v].reset();   // fresh grains
        p->sh_live[v] = act[v];
    }

    // ── Attack envelope (A-only ADSR: decay 0, sustain 1) ─────────────────
    p->env.set(atk_ms, 0.0f, 1.0f, 60.0f, sr);
    const int sil_max = (int)(0.150f * sr);           // 150 ms under -60 dBFS
    constexpr float SIL_GATE = 1e-6f;                 // -60 dBFS (power)

    // ── Filter smoothing (~30 ms toward targets, Megalo pattern) ──────────
    if (p->smooth_cutoff <= 0.0f) {
        p->smooth_cutoff = cutoff;                    // first block: snap
        p->smooth_q      = q;
    } else {
        const float aa = 1.0f - std::exp(-(float)n_samples / (0.030f * sr));
        p->smooth_cutoff += aa * (cutoff - p->smooth_cutoff);
        p->smooth_q      += aa * (q      - p->smooth_q);
        if (std::abs(p->smooth_cutoff - cutoff) < 0.001f * cutoff)
            p->smooth_cutoff = cutoff;
        if (std::abs(p->smooth_q - q) < 0.001f * q)
            p->smooth_q = q;
    }
    const bool bypass = p->smooth_cutoff >= 19000.0f;
    if (!bypass &&
        (p->smooth_cutoff != p->cached_cutoff || p->smooth_q != p->cached_q)) {
        p->filter.setup(Biquad::LP, p->smooth_cutoff, p->smooth_q, sr);
        p->cached_cutoff = p->smooth_cutoff;
        p->cached_q      = p->smooth_q;
    }
    if (bypass && !p->lp_bypassed) p->filter.reset();
    p->lp_bypassed = bypass;

    // Per-sample gain smoothing coefficient (~30 ms)
    const float gc = 1.0f - std::exp(-1.0f / (0.030f * sr));

    float*   ring = p->ring.data();
    const uint32_t mask = p->mask;

    for (uint32_t i = 0; i < n_samples; ++i) {
        const float x = in[i];

        // Write first: voices read at least MARGIN samples behind wpos.
        ring[p->wpos & mask] = x;
        ++p->wpos;

        p->g_dry  += gc * (dry_t  - p->g_dry);
        p->g_sub1 += gc * (sub1_t - p->g_sub1);
        p->g_sub2 += gc * (sub2_t - p->g_sub2);
        p->g_up1  += gc * (up1_t  - p->g_up1);
        p->g_up2  += gc * (up2_t  - p->g_up2);
        p->g_out  += gc * (out_t  - p->g_out);

        float wet = 0.0f;
        if (act[V_SUB1])
            wet += p->g_sub1 * p->sh[V_SUB1].process(ring, mask, p->wpos);
        if (act[V_SUB2])
            wet += p->g_sub2 * p->sh[V_SUB2].process(ring, mask, p->wpos);
        if (act[V_UP1]) {
            float v = p->sh[V_UP1].process(ring, mask, p->wpos);
            if (act[V_UP1D])
                v = 0.5f * (v + p->sh[V_UP1D].process(ring, mask, p->wpos));
            wet += p->g_up1 * v;
        }
        if (act[V_UP2]) {
            float v = p->sh[V_UP2].process(ring, mask, p->wpos);
            if (act[V_UP2D])
                v = 0.5f * (v + p->sh[V_UP2D].process(ring, mask, p->wpos));
            wet += p->g_up2 * v;
        }

        // Attack/swell. The detector always runs so its RMS state is warm
        // when the user raises attack_ms mid-note.
        const bool onset = p->det.process(x, sens);
        if (env_on) {
            if (onset) {
                if (p->env.is_active() && p->env_level > 0.1f) {
                    // Re-pick during a swell: duck fast, then restart the
                    // attack from low — every pick re-swells without a click.
                    p->env.release_capped(5.0f, sr);
                    p->pending_trig = (int)(0.005f * sr) + 1;
                } else {
                    p->env.trigger();
                }
            }
            if (p->pending_trig > 0 && --p->pending_trig == 0)
                p->env.trigger();
            // Safety net: signal present but the envelope sits at Idle
            // (missed onset on a legato swell, or attack enabled mid-note)
            // — swell in rather than staying silent.
            if (!p->env.is_active() && p->det.fast_power() > 4.0f * SIL_GATE)
                p->env.trigger();
            if (p->det.fast_power() < SIL_GATE) {
                if (++p->sil_count == sil_max) p->env.release();
            } else {
                p->sil_count = 0;
            }
            p->env_level = p->env.process();
            wet *= p->env_level;
        } else {
            p->env_level = 1.0f;
        }

        if (!bypass) wet = p->filter.process(wet);

        out[i] = soft_clip((wet + p->g_dry * x) * p->g_out);
    }
}
