// Pogged — POG2-style polyphonic octave generator, host-agnostic DSP core.
//
// Live mono input is written to a ring buffer; up to seven StreamShifters read
// behind the write head (sub -1/-2 oct, +5th, +1/+2 oct, plus an LFO-detuned
// pair on the +1/+2 voices). The wet mix goes through the attack/swell
// envelope and the resonant low-pass, then joins the *undelayed* dry path:
// the POG's defining trait is a zero-latency dry signal, and since every wet
// voice sits at a different frequency there is no unison to comb-filter
// against.
#include "pogged_dsp.h"
#include "stream_shifter.hpp"
#ifndef POGGED_NO_VOCODER
#include "stream_vocoder.hpp"
#include "stream_multivocoder.hpp"
// FOCUS's vocoder path. Multi-resolution (§13), sized for STABILITY first
// (§16): two notes' partials collide (get closer than a window can resolve)
// at every register, and an unresolved pair makes the per-peak translation
// warble at the pair's beat rate — the "shimmer" heard on real chords. The
// 8192 window resolves everything a chord throws below the crossover (mean
// excess AM +1.4 dB vs the ideal shift on a realistic major third, against
// +14.7 dB for 4096+2048); per-sample FFT cost only grows as log N, so this
// costs ~9% over 4096+2048. The price is latency: ~171 ms below the
// crossover, ~85 ms above, so the attack is softer than the 42 ms it briefly
// had — the §12 transient-reinjection path is the planned reconciliation.
// A target that pins POGGED_PV_N (the Duo X pins 2048 for CPU) keeps the
// historic single window at that size instead.
#ifdef POGGED_PV_N
using PoggedVocoder = StreamVocoder;
#else
using PoggedVocoder = MultiVocoder<8192, 4096>;
// Input-side crossover constant (§15/§16): the short window only ever carries
// output made from input partials above this, where it resolves the
// collisions that remain. NOT a 3-band ladder (§17, measured): every added
// window costs a full engine per sample (~+50% CPU, past the Pi's budget)
// and cannot lower the tonal core's latency anyway — collisions needing the
// 8192 live everywhere below ~1200 Hz input, adjacent-scale-note
// fundamentals included. The latency answer is the TIME split (§12/§16
// transient reinjection), not more frequency bands.
static constexpr float VOC_XOVER_IN = 1200.0f;
#endif
#endif
#include "delay_line.hpp"
#include "freeze_loop.hpp"
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

// Per-voice pan gains. Pedal semantics, not constant-power: centre is unity on
// BOTH channels, so a voice panned centre comes out of a single jack at full
// level (the POG3 manual has you "connect either the LEFT or RIGHT" output).
// A constant-power law would drop that to -3 dB and quietly halve the power of
// every existing mono patch. Hard over mutes the opposite channel.
static inline void pan_gains(float pan, float& gl, float& gr) noexcept {
    gl = (pan <= 0.0f) ? 1.0f : 1.0f - pan;
    gr = (pan >= 0.0f) ? 1.0f : 1.0f + pan;
}

// filter_mode port -> Biquad::Type. The port follows the POG3's own menu order
// ("Low-Pass (default), Band-Pass, High-Pass"); Biquad happens to order its
// enum LP/HP/BP. The two orders are unrelated, so map explicitly: casting one
// onto the other silently swaps BP and HP.
static inline Biquad::Type filter_mode_of(float v) noexcept {
    switch ((int)std::clamp(v, 0.0f, 2.0f)) {
    case 1:  return Biquad::BP;
    case 2:  return Biquad::HP;
    default: return Biquad::LP;
    }
}

// ── Voice bank layout ────────────────────────────────────────────────────────
// Internal ordering only — nothing persists these, so they are free to be
// grouped logically (the LV2 port order is fixed separately in pogged_dsp.h).
enum Voice { V_SUB1 = 0, V_SUB2, V_UP5, V_UP1, V_UP2, V_UP1D, V_UP2D,
             V_DRYD,          // detuned copy of the dry (POG3 DRY DETUNE)
             N_VOICES };

// Perfect fifth, equal-tempered (2^(7/12)) rather than the just 3:2 = 1.5.
// The voice transposes the whole polyphonic signal, so an ET ratio keeps the
// shifted chord in tune with the (ET) instrument playing it; the just ratio
// would sit ~2 cents sharp against it.
static constexpr float FIFTH_RATIO = 1.4983070768766815f;

// Nominal pitch ratio of each voice, indexed by Voice. Both engines and the
// Warp bend read it, so it lives here once rather than as a literal at each
// site. V_UP1D/V_UP2D carry their octave's ratio; the detune LFO and Warp
// multiply it. V_DRYD is the dry, hence unity — and Warp leaves it alone.
static constexpr float VOICE_RATIO[N_VOICES] = {
    0.5f,        // V_SUB1
    0.25f,       // V_SUB2
    FIFTH_RATIO, // V_UP5
    2.0f,        // V_UP1
    4.0f,        // V_UP2
    2.0f,        // V_UP1D
    4.0f,        // V_UP2D
    1.0f,        // V_DRYD
};

// POG2 detune is a chorus, not a fixed offset: the manual says pushing the
// slider up increases "both the depth and rate of detune". So the slider maps
// to an LFO's depth (cents) AND its rate (Hz), and the detuned voice's ratio
// is modulated around its octave. Rate range is not published — these are
// tuned by ear for a POG2-ish shimmer and are the obvious knobs to revisit.
static constexpr float DET_RATE_MIN = 0.25f;   // Hz, at the smallest detune
static constexpr float DET_RATE_MAX = 3.0f;    // Hz, at detune = 25 cents

// POG3 SPREAD: a short delay per channel on the +5th/+1/+2 voices, right 3x
// longer than left, which throws them wide. Per the manual the two suboctave
// voices are deliberately excluded — a delayed sub just smears the low end.
static constexpr float SPREAD_L_MAX_MS = 50.0f;
static constexpr float SPREAD_R_MAX_MS = 150.0f;   // = 3x left

// POG3 filter envelope: the ENV knob sweeps the filter frequency up (CW) or
// down (CCW) from the slider's setting; centre disables it. The manual gives
// no depth in octaves — 4 is a musical full-scale sweep and is the obvious
// knob to revisit by ear.
static constexpr float ENV_SWEEP_OCTAVES = 4.0f;

// ── Instrument range ─────────────────────────────────────────────────────────
// The sub voices' grain has to span at least two periods of the note they
// EMIT, not of the note played: the sub of a low E sings at 41 Hz, so it needs
// ~49 ms of grain. Below that the correlation aligner has less than a full
// cycle to lock onto and the splice warbles. The lowest note therefore has to
// be a setting, not an assumption — a baritone's low B puts the sub at 31 Hz
// (32 ms period), where a guitar-sized 55 ms grain covers only 1.7 periods.
//
// It costs latency: the aligner may rewind the read by up to its scan range,
// so a longer scan delays the sub voice (guitar ~41 ms worst case, bass
// ~105 ms). Hence a switch rather than simply sizing for the worst case.
static inline float range_f_low(float v) noexcept {
    switch ((int)std::clamp(v, 0.0f, 2.0f)) {
    case 1:  return 61.74f;    // baritone, low B1
    case 2:  return 30.87f;    // bass, low B0 (5-string)
    default: return 82.41f;    // guitar, low E2
    }
}
// Grain spans this many periods of the voice's own output; the scan covers
// half a period, which is all the aligner needs to find the in-phase point.
static constexpr float GRAIN_PERIODS = 2.2f;
static constexpr float ALIGN_PERIODS = 0.5f;
// Capped so the read stays well inside the ring and smearing stays bounded:
// a bass's -2 voice emits 7.7 Hz, which is a rumble, not a pitch — sizing
// grains for it would smear everything else for nothing.
static constexpr float GRAIN_MAX_MS  = 160.0f;

// ── FOCUS: which pitch engine ────────────────────────────────────────────────
// The POG3 has a FOCUS button that swaps the transposition algorithm, and its
// manual notes the POG algorithm "features lower latency" than the alternative.
// Ours is the same trade, measured:
//
//              artifact over an ideal shift (sub, chord)   latency
//   granular             +4.8 dB                            3 ms
//   vocoder              +0.0 dB                           85 ms treble/attacks,
//                                                          171 ms low-mids
//                                                          (multi-res §13/§16;
//                                                          single-window 42 ms
//                                                          when POGGED_PV_N
//                                                          pins 2048)
//
// The granular engine's aligner can only lock onto one periodicity, so a chord
// — whose partials have incommensurable periods — makes its splices cancel
// unevenly. The vocoder translates each spectral peak on its own and lands
// exactly on the ideal-shift floor. It cannot do better; nothing can.
//
// Deviation from the POG3: its FOCUS acts on the +1/+2 voices only. Ours acts
// on every voice, because the granular engine's weakness is on the SUB — a
// faithful +1/+2-only FOCUS would never reach the voice that needs it.
//
// Switching engines crossfades over ~250 ms: they have different latencies (3
// vs up to 171 ms), so a hard switch would jump the signal, and the fade must
// outlast the long window's OLA fill (~171 ms) so the vocoder ramps in from
// real content rather than from its zero-padded start. Both engines run only
// during the fade; at rest exactly one does.
static constexpr float FOCUS_XFADE_MS = 250.0f;

// ── Freeze + Gliss ───────────────────────────────────────────────────────────
// The pedal's position sets the glide rate, "the closer the pedal is to the toe
// position the slower the glissando rate". The two ends are not published, so
// these are ears-first choices and the obvious knobs to revisit: at the heel end
// of the sweep the next note arrives almost at once, at the toe it takes two
// seconds to arrive.
static constexpr float FRZ_GLIDE_MIN_MS = 20.0f;
static constexpr float FRZ_GLIDE_MAX_MS = 2000.0f;
// Swapping the ring between live and loop is a hard cut; every hard switch in
// this engine has clicked, so it is crossfaded like all the others.
static constexpr float FRZ_XFADE_MS = 25.0f;
// Filter coefficients are refreshed on this stride while the sweep moves.
// Per-sample would mean transcendentals per sample per channel; 16 samples is
// a 3 kHz control rate at 48k, far above anything a filter sweep resolves.
static constexpr int   FILT_UPDATE_STRIDE = 16;

struct PoggedDsp {
    double sample_rate = 48000.0;

    std::vector<float> ring;      // power-of-2, allocated once in _new
    uint32_t mask = 0;
    uint64_t wpos = 0;            // absolute write count (starts at ring size)

    StreamShifter sh[N_VOICES];
    bool          sh_live[N_VOICES] = {};   // false ⇒ needs reset before reuse
#ifndef POGGED_NO_VOCODER
    PoggedVocoder pv[N_VOICES];
    bool          pv_live[N_VOICES] = {};
#endif
    float         g_focus = 0.0f;           // smoothed engine crossfade 0..1

    // Voices are panned before the mix, so the wet bus is stereo by the time
    // it reaches the filter — hence one filter per channel, same coefficients.
    Biquad        filter_l, filter_r;
    Biquad        vfilt[N_VOICES];   // fixed per-voice tone shaping (voicing)
    Envelope      env;
    OnsetDetector det;
    // Filter sweep: its own envelope and its own onset detector, because the
    // POG3 gives the sweep a Trigger Sensitivity separate from the ATTACK
    // slider's — the two effects can key off different playing dynamics.
    Envelope      filt_env;
    OnsetDetector filt_det;
    float         filt_env_level = 0.0f;
    int           filt_ctr       = 0;    // coefficient-refresh stride counter
    int           cached_mode    = -1;
    float         cached_range   = -1.0f;   // applied range_mode

    // Smoothed gains (per-sample one-pole, ~30 ms)
    float g_dry = 1.0f, g_sub1 = 0.0f, g_sub2 = 0.0f;
    float g_up1 = 0.0f, g_up2 = 0.0f, g_up5 = 0.0f, g_out = 1.0f;
    // Detune LFO phase, in turns [0,1). Advanced once per block: the LFO tops
    // out at 3 Hz, so even a 4096-sample block still samples it ~6x per cycle.
    float det_phase = 0.0f;
    float g_warp_st = 0.0f;   // smoothed Warp bend, in semitones
    FreezeLoop    frz;
    bool          frz_held = false;   // pedal off the heel on the last block
    float         g_frz    = 0.0f;    // live -> loop crossfade, 0..1
    // Detune-mix coefficient, ramped 0 → 0.5 so enabling/disabling detune
    // crossfades the (phase-independent) detuned voice in instead of hard-
    // switching to the 50/50 average, which was an audible click.
    float g_detmix = 0.0f;
    // SPREAD delay lines, one per spread-eligible voice (fed post-detune-mix,
    // pre-pan). Indexed by Voice for clarity; only V_UP5/V_UP1/V_UP2 are used.
    DelayLine     sdl[N_VOICES];
    float         g_spread = 0.0f;    // smoothed 0..1
    DelayLine     sdl_dry;            // SPREAD on the dry (gated by DRY DETUNE)
    // POG3 DRY routing, all crossfaded: these are switches, and every hard
    // switch in this file has turned out to click.
    float g_in      = 1.0f;   // smoothed input gain
    float g_dryatk  = 0.0f;   // dry through attack   0..1
    float g_dryfilt = 0.0f;   // dry through filter   0..1
    float g_drydet  = 0.0f;   // dry through detune   0..1 (ramps to 0.5 mix)

    // Smoothed pan positions (-1..+1), one per voice; gains are derived per
    // sample by pan_gains(). Smoothing the position rather than the two gains
    // keeps the pair consistent all the way through a sweep.
    float p_dry = 0.0f, p_sub1 = 0.0f, p_sub2 = 0.0f;
    float p_up5 = 0.0f, p_up1 = 0.0f, p_up2 = 0.0f;
    // LP-engage crossfade, ramped 0 (bypassed) → 1 (fully filtered) so the
    // filter fades in/out instead of engaging from zero state on a live
    // signal, which was an audible transient when sweeping cutoff off 20 kHz.
    float g_filtmix = 0.0f;

    // Filter smoothing/caching (Megalo pattern)
    float smooth_cutoff = -1.0f, smooth_q = 0.707f;
    float cached_cutoff = -1.0f, cached_q = -1.0f;

    // Attack/swell state
    float env_level   = 0.0f;
    int   pending_trig = 0;       // duck-then-swell: samples until trigger()
    int   sil_count    = 0;       // samples of near-silence (release gate)
};

// Size each sub voice's grain and correlation scan from the note IT emits.
// Shared by _new and process(): the two must not drift apart.
static void setup_subs(PoggedDsp* p, float f_low, float sr) noexcept;

static void setup_subs(PoggedDsp* p, float f_low, float sr) noexcept
{
    // Each sub emits f_low * its ratio; size from that, not from f_low.
    const float f_out[2] = { f_low * 0.5f, f_low * 0.25f };
    const Voice v[2]     = { V_SUB1, V_SUB2 };
    const float ratio[2] = { 0.5f, 0.25f };
    for (int i = 0; i < 2; ++i) {
        const float period_ms = 1000.0f / f_out[i];
        const int grain = (int)(0.001f * std::min(GRAIN_PERIODS * period_ms,
                                                  GRAIN_MAX_MS) * sr);
        const int align = (int)(0.001f * std::min(ALIGN_PERIODS * period_ms,
                                                  GRAIN_MAX_MS * 0.5f) * sr);
        p->sh[v[i]].setup(ratio[i], grain, align);
    }
}

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
    const int align     = (int)(0.010f * sr);   // 10 ms correlation scan

    // Aligned respawn everywhere: without it the source-position jump at
    // respawn ((ratio-1)·grain) lands at arbitrary phase — for a 25 ms grain
    // at ratio 2 that is exactly half a period of the doubled fundamental,
    // so alternate grains cancel the target pitch outright. Correlation
    // alignment (SOLA-style) keeps grains phase-coherent for any input.
    setup_subs(p, range_f_low(0.0f), sr);      // guitar until told otherwise
    p->sh[V_UP5 ].setup(FIFTH_RATIO, grain_up, align);
    p->sh[V_UP1 ].setup(2.0f,  grain_up,  align);
    p->sh[V_UP2 ].setup(4.0f,  grain_up,  align);
    p->sh[V_UP1D].setup(2.0f,  grain_up,  align);
    p->sh[V_UP2D].setup(4.0f,  grain_up,  align);
    // Detuned dry sits at unison; the LFO moves it around 1.0.
    p->sh[V_DRYD].setup(1.0f,  grain_up,  align);

    // Fixed per-voice tone shaping — voices the octaves toward the POG2's
    // character rather than passing raw transposed grains:
    //   subs → gentle LP: rounds the low octaves and tames the granular buzz
    //          (a real POG sub is smooth, not gritty);
    //   ups  → gentle HP: removes the low-frequency amplitude modulation that
    //          up-transposition leaves at the grain-splice rate, which a real
    //          POG octave-up does not have.
    // Q = 0.707 (Butterworth, no resonant colouration). Not user-exposed.
    // The +5th only shifts 7 semitones, so its splice-rate modulation is
    // milder than the octaves' — it gets a correspondingly lower HP.
    const float ny = std::min(sr * 0.499f, 20000.0f);
    p->vfilt[V_SUB1].setup(Biquad::LP, std::min(3500.0f, ny), 0.707f, sr);
    p->vfilt[V_SUB2].setup(Biquad::LP, std::min(2000.0f, ny), 0.707f, sr);
    p->vfilt[V_UP5 ].setup(Biquad::HP, 100.0f, 0.707f, sr);
    p->vfilt[V_UP1 ].setup(Biquad::HP, 140.0f, 0.707f, sr);
    p->vfilt[V_UP2 ].setup(Biquad::HP, 220.0f, 0.707f, sr);
    p->vfilt[V_UP1D].setup(Biquad::HP, 140.0f, 0.707f, sr);
    p->vfilt[V_UP2D].setup(Biquad::HP, 220.0f, 0.707f, sr);
    // The detuned dry doubles the DRY, so it must not be voiced at all — any
    // colour here would show up as a comb against the untouched dry beside it.
    p->vfilt[V_DRYD].setup(Biquad::HP, 5.0f, 0.707f, sr);

    // Sized for the longest delay SPREAD can ask for.
    const int spread_max = (int)(SPREAD_R_MAX_MS * 0.001f * sr) + 2;
    p->sdl_dry.init(spread_max);
    p->sdl[V_UP5].init(spread_max);
    p->sdl[V_UP1].init(spread_max);
    p->sdl[V_UP2].init(spread_max);

#ifndef POGGED_NO_VOCODER
    // Same ratios as the shifters: FOCUS swaps the engine, not the tuning.
    // Spread the voices' FFT bursts evenly across the hop so at most one lands
    // in any given audio block, instead of all N_VOICES colliding every HOP
    // samples. Same work, same sound — it is only *when* each voice computes.
    for (int v = 0; v < N_VOICES; ++v) {
        p->pv[v].init(sample_rate, v * (PoggedVocoder::HOP / N_VOICES));
        p->pv[v].set_ratio(VOICE_RATIO[v]);
#ifndef POGGED_PV_N
        // Input-referred crossovers (§15): a voice at `ratio` puts an input
        // partial at f on the output at ratio·f, so the output-side splits
        // sit at VOC_XOVER_IN*×ratio. Nominal ratio on purpose: Warp/detune
        // bend the pitch, not the crossover.
        p->pv[v].set_xover(VOC_XOVER_IN * VOICE_RATIO[v]);
#endif
    }
#endif

    p->frz.init(sample_rate);
    p->det.init(sr);
    p->filt_det.init(sr);
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
        p->vfilt[v].reset();
        p->sh_live[v] = false;
#ifndef POGGED_NO_VOCODER
        p->pv[v].reset();
        p->pv_live[v] = false;
#endif
    }
    p->g_focus = 0.0f;
    p->filter_l.reset();
    p->filter_r.reset();
    p->env.reset();
    p->det.reset();
    p->filt_env.reset();
    p->filt_det.reset();
    p->filt_env_level = 0.0f;
    p->filt_ctr       = 0;
    p->cached_mode    = -1;
    p->smooth_cutoff = -1.0f;
    p->smooth_q      = 0.707f;
    p->cached_cutoff = p->cached_q = -1.0f;
    p->env_level     = 0.0f;
    p->pending_trig  = 0;
    p->sil_count     = 0;
    p->g_detmix      = 0.0f;
    p->g_filtmix     = 0.0f;
    p->det_phase     = 0.0f;
    p->g_warp_st     = 0.0f;
    p->frz.reset();
    p->frz_held      = false;
    p->g_frz         = 0.0f;
    p->p_dry = p->p_sub1 = p->p_sub2 = 0.0f;
    p->p_up5 = p->p_up1 = p->p_up2 = 0.0f;
    p->g_spread = 0.0f;
    p->g_in = 1.0f;
    p->g_dryatk = p->g_dryfilt = p->g_drydet = 0.0f;
    p->sdl_dry.reset();
    p->sdl[V_UP5].reset();
    p->sdl[V_UP1].reset();
    p->sdl[V_UP2].reset();
}

// ── Processing ───────────────────────────────────────────────────────────────
void pogged_dsp_process(PoggedDsp* p, const PoggedParams* p_,
                        const float* in, float* out_l, float* out_r,
                        uint32_t n_samples)
{
    ScopedFlushToZero ftz;
    const float sr = (float)p->sample_rate;

    // ── Snapshot controls (block boundary) ────────────────────────────────
    const float dry_t   = std::clamp(p_->dry_level,    0.0f, 2.0f);
    const float sub1_t  = std::clamp(p_->sub1_level,   0.0f, 2.0f);
    const float sub2_t  = std::clamp(p_->sub2_level,   0.0f, 2.0f);
    const float up1_t   = std::clamp(p_->up1_level,    0.0f, 2.0f);
    const float up2_t   = std::clamp(p_->up2_level,    0.0f, 2.0f);
    const float up5_t   = std::clamp(p_->up5_level,    0.0f, 2.0f);
    const float det_ct  = std::clamp(p_->detune_cents, 0.0f, 25.0f);
    const float atk_ms  = std::clamp(p_->attack_ms,    0.0f, 2000.0f);
    const float sens    = std::clamp(p_->attack_sens,  0.0f, 1.0f);
    const float cutoff  = std::clamp(p_->lp_cutoff,   20.0f, std::min(20000.0f, sr * 0.499f));
    const float q       = std::clamp(p_->lp_q,         0.5f, 8.0f);
    const float out_t   = std::clamp(p_->out_level,    0.0f, 2.0f);
    const float pdry_t  = std::clamp(p_->pan_dry,     -1.0f, 1.0f);
    const float psub1_t = std::clamp(p_->pan_sub1,    -1.0f, 1.0f);
    const float psub2_t = std::clamp(p_->pan_sub2,    -1.0f, 1.0f);
    const float pup5_t  = std::clamp(p_->pan_up5,     -1.0f, 1.0f);
    const float pup1_t  = std::clamp(p_->pan_up1,     -1.0f, 1.0f);
    const float pup2_t  = std::clamp(p_->pan_up2,     -1.0f, 1.0f);
    const float spread_t = std::clamp(p_->spread,      0.0f, 1.0f);
    const Biquad::Type mode = filter_mode_of(p_->filter_mode);
    const float fenv_d  = std::clamp(p_->filter_env,   -1.0f, 1.0f);
    const float fenv_a  = std::clamp(p_->filter_env_a,  1.0f, 1000.0f);
    const float fenv_dc = std::clamp(p_->filter_env_d,  1.0f, 2000.0f);
    const float fsens   = std::clamp(p_->filter_sens,   0.0f, 1.0f);
    const bool  fenv_on = std::abs(fenv_d) > 1e-3f;   // ENV centred = off

    // Re-cut the sub grains only when the range actually changes: setup()
    // re-inits the taps when the grain length moves, which restarts the grains.
    // That is fine for a setup switch but must not happen every block.
    const float in_t     = std::clamp(p_->input_gain, 0.5f, 3.0f);
    const float dryatk_t  = (p_->dry_attack > 0.5f) ? 1.0f : 0.0f;
    const float dryfilt_t = (p_->dry_filter > 0.5f) ? 1.0f : 0.0f;
    const float drydet_t  = (p_->dry_detune > 0.5f) ? 1.0f : 0.0f;

    const float focus_t = (std::clamp(p_->focus, 0.0f, 1.0f) > 0.5f) ? 1.0f : 0.0f;
    const float focus_c = 1.0f - std::exp(-1.0f / (FOCUS_XFADE_MS * 0.001f * sr));

    const float range_t = std::clamp(p_->range_mode, 0.0f, 2.0f);
    if (range_t != p->cached_range) {
        setup_subs(p, range_f_low(range_t), sr);
        p->cached_range = range_t;
    }

    const bool detune_on = det_ct > 0.5f;
    const bool env_on    = atk_ms > 1.0f;

    // ── Warp: whammy-style bend on every voice except the dry ─────────────
    // `warp` is literally where the expression pedal is; heel and toe carry an
    // interval each, so the sweep runs heel → toe. Smoothed per block in
    // SEMITONES (not in ratio): a listener hears pitch logarithmically, so a
    // linear ramp in semitones is the one that sweeps evenly.
    const float warp_p    = std::clamp(p_->warp,       0.0f, 1.0f);
    const float warp_heel = std::clamp(p_->warp_heel, -12.0f, 12.0f);
    const float warp_toe  = std::clamp(p_->warp_toe,  -12.0f, 12.0f);
    const float warp_st_t = warp_heel + warp_p * (warp_toe - warp_heel);
    const float warp_c    = 1.0f - std::exp(-(float)n_samples / (0.030f * sr));
    p->g_warp_st += warp_c * (warp_st_t - p->g_warp_st);
    // At rest this is exactly 1.0, so every set_*() below restores precisely
    // what setup() had installed: warp off costs nothing and changes nothing.
    const float warp_mult = std::pow(2.0f, p->g_warp_st * (1.0f / 12.0f));

    for (int v = 0; v < N_VOICES; ++v) {
        if (v == V_DRYD) continue;          // "all voices except DRY"
        const float r = VOICE_RATIO[v] * warp_mult;
        // Budget first: it must cover the ratio actually about to be read.
        p->sh[v].set_lag_ratio(r);
        p->sh[v].set_ratio(r);
#ifndef POGGED_NO_VOCODER
        p->pv[v].set_ratio(r);              // no lag budget: it translates peaks
#endif
    }

    // Detune: modulate the detuned voices' ratios with an LFO whose depth AND
    // rate both rise with the slider (POG2 behaviour — a static offset gives a
    // fixed, lifeless beating instead of a chorus). Opposite LFO signs on +1 /
    // +2 so the two voices drift apart rather than in parallel. Advanced once
    // per block; the pow/sin cost is per-block, not per-sample.
    if (detune_on) {
        const float rate = DET_RATE_MIN + (det_ct / 25.0f) * (DET_RATE_MAX - DET_RATE_MIN);
        p->det_phase += rate * (float)n_samples / sr;
        p->det_phase -= std::floor(p->det_phase);
        const float lfo = std::sin(6.28318531f * p->det_phase);
        const float c   = (det_ct * lfo) / 1200.0f;      // instantaneous cents
        // Override the ratio the Warp loop just set, on top of the same bend
        // (the lag budget it set from base×warp stands: RATIO_HEADROOM covers
        // the LFO, and the anchor must not follow it — see set_lag_ratio).
        const float r1 = VOICE_RATIO[V_UP1D] * warp_mult * std::pow(2.0f,  c);
        const float r2 = VOICE_RATIO[V_UP2D] * warp_mult * std::pow(2.0f, -c);
        const float rd = std::pow(2.0f, c);   // dry copy: unison, and unwarped
        p->sh[V_UP1D].set_ratio(r1);
        p->sh[V_UP2D].set_ratio(r2);
        p->sh[V_DRYD].set_ratio(rd);
#ifndef POGGED_NO_VOCODER
        p->pv[V_UP1D].set_ratio(r1);
        p->pv[V_UP2D].set_ratio(r2);
        p->pv[V_DRYD].set_ratio(rd);
#endif
    }

    // ── Freeze + Gliss ────────────────────────────────────────────────────
    // Capturing on the pedal LEAVING the heel is what makes this work at all:
    // at that instant the ring still holds the live input, because the ring is
    // only fed the loop while frozen. Coming back to the heel refills it with
    // whatever is being played, so the next rise captures the next note — which
    // is exactly the pedal move the manual describes.
    const float frz_t  = std::clamp(p_->freeze, 0.0f, 1.0f);
    const bool  frz_on = frz_t > 1e-3f;
    if (frz_on && !p->frz_held) {
        const float glide_ms = FRZ_GLIDE_MIN_MS
                             + frz_t * (FRZ_GLIDE_MAX_MS - FRZ_GLIDE_MIN_MS);
        p->frz.capture(p->ring.data(), p->mask, p->wpos,
                       (int)(glide_ms * 0.001f * sr));
    }
    p->frz_held = frz_on;
    const float frz_target = (frz_on && p->frz.armed()) ? 1.0f : 0.0f;
    const float frz_c = 1.0f - std::exp(-1.0f / (FRZ_XFADE_MS * 0.001f * sr));

    // Which shifters run this block. A voice is live while its target OR its
    // smoothed gain is audible, so it fades out before being skipped.
    // Assigned by index, not as a positional list: the enum is free to be
    // reordered and this cannot silently fall out of step with it.
    bool act[N_VOICES] = {};
    act[V_SUB1] = sub1_t > 1e-4f || p->g_sub1 > 1e-4f;
    act[V_SUB2] = sub2_t > 1e-4f || p->g_sub2 > 1e-4f;
    act[V_UP5 ] = up5_t  > 1e-4f || p->g_up5  > 1e-4f;
    act[V_UP1 ] = up1_t  > 1e-4f || p->g_up1  > 1e-4f;
    act[V_UP2 ] = up2_t  > 1e-4f || p->g_up2  > 1e-4f;
    // Detuned voices stay alive while the detune-mix is still ramping down, so
    // their contribution can fade out continuously.
    act[V_UP1D] = (detune_on || p->g_detmix > 1e-4f) && act[V_UP1];
    act[V_UP2D] = (detune_on || p->g_detmix > 1e-4f) && act[V_UP2];
    // The dry copy only exists when DRY DETUNE asks for it.
    act[V_DRYD] = (detune_on || p->g_detmix > 1e-4f) &&
                  (drydet_t > 0.5f || p->g_drydet > 1e-4f) &&
                  (dry_t > 1e-4f || p->g_dry > 1e-4f);
    for (int v = 0; v < N_VOICES; ++v) {
        if (act[v] && !p->sh_live[v]) {                    // fresh grains
            p->sh[v].reset();
            p->vfilt[v].reset();
            p->sdl[v].reset();   // else SPREAD replays audio from before the
                                 // voice was silenced (no-op if uninitialised)
#ifndef POGGED_NO_VOCODER
            p->pv[v].reset();    // same: stale OLA tail from the last note
#endif
        }
        p->sh_live[v] = act[v];
    }

    // ── Attack envelope (A-only ADSR: decay 0, sustain 1) ─────────────────
    p->env.set(atk_ms, 0.0f, 1.0f, 60.0f, sr);
#ifndef POGGED_NO_VOCODER
    // The vocoder swells PER BIN instead of taking the global envelope: each
    // attack fades in on its own bins while the notes already ringing keep
    // their sustain (the POG3 ATTACK behaviour — see stream_vocoder.hpp).
    // V_DRYD is excluded: it feeds the DRY path, whose swell is the DRY
    // ATTACK button (the global envelope lerp below), not the wet envelope.
    for (int v = 0; v < N_VOICES; ++v)
        p->pv[v].set_swell((env_on && v != V_DRYD) ? atk_ms : 0.0f);
#endif
    // Filter sweep envelope: AD (sustain 0) — it rises on the pick then falls
    // back to the slider's frequency, which is what a filter sweep is.
    p->filt_env.set(fenv_a, fenv_dc, 0.0f, 60.0f, sr);
    const float ny = std::min(sr * 0.499f, 20000.0f);
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
    // Bypass is mode-dependent. "Cutoff at max = filter off" is a low-pass
    // idiom: in high-pass, 19 kHz is a legitimate (near-silent) setting, not an
    // off switch, and a band-pass has no off position at all. So bypass only at
    // each mode's own transparent end, and never in BP. This keeps the existing
    // LP presets (lp_cutoff = 20000 means "off") behaving exactly as before.
    // The filter is NOT reset on entering bypass — the crossfade below fades
    // its contribution out, and resetting mid-fade would itself click.
    const bool bypass =
        (mode == Biquad::LP && p->smooth_cutoff >= 19000.0f) ||
        (mode == Biquad::HP && p->smooth_cutoff <= 21.0f);

    // Per-sample gain smoothing coefficient (~30 ms)
    const float gc = 1.0f - std::exp(-1.0f / (0.030f * sr));

    float*   ring = p->ring.data();
    const uint32_t mask = p->mask;

    for (uint32_t i = 0; i < n_samples; ++i) {
        // INPUT GAIN is "the level of the signal seen at the input", so it is
        // applied before everything — the ring the voices read, the dry, and
        // both onset detectors, which therefore trigger on the boosted level
        // exactly as they would on the pedal.
        p->g_in += gc * (in_t - p->g_in);
        const float x = in[i] * p->g_in;

        // Freeze feeds the ring the loop instead of the input. Every voice and
        // both engines read the ring exactly as before and never learn that
        // time stopped.
        p->g_frz += frz_c * (frz_target - p->g_frz);
        float src = x;
        if (p->g_frz > 1e-4f) {
            const float loop = p->frz.process();
            src = x + p->g_frz * (loop - x);
        }

        // Write first: voices read at least MARGIN samples behind wpos.
        ring[p->wpos & mask] = src;
        ++p->wpos;

        p->g_dry  += gc * (dry_t  - p->g_dry);
        p->g_sub1 += gc * (sub1_t - p->g_sub1);
        p->g_sub2 += gc * (sub2_t - p->g_sub2);
        p->g_up1  += gc * (up1_t  - p->g_up1);
        p->g_up2  += gc * (up2_t  - p->g_up2);
        p->g_up5  += gc * (up5_t  - p->g_up5);
        p->g_out  += gc * (out_t  - p->g_out);
        p->g_detmix += gc * ((detune_on ? 0.5f : 0.0f) - p->g_detmix);
        const float m = p->g_detmix;

        p->p_dry  += gc * (pdry_t  - p->p_dry);
        p->p_sub1 += gc * (psub1_t - p->p_sub1);
        p->p_sub2 += gc * (psub2_t - p->p_sub2);
        p->p_up5  += gc * (pup5_t  - p->p_up5);
        p->p_up1  += gc * (pup1_t  - p->p_up1);
        p->p_up2  += gc * (pup2_t  - p->p_up2);

        // Attack/swell. The detector always runs so its RMS state is warm
        // when the user raises attack_ms mid-note. Computed BEFORE the voices
        // because the granular engine takes the envelope at its input: the
        // vocoder swells per bin on its own (see set_swell above), so a
        // global multiply on the mixed wet bus would double-swell it.
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
        } else {
            p->env_level = 1.0f;
        }

        // FOCUS crossfade. Both engines run ONLY while the fade is in flight;
        // at rest (g_focus pinned at 0 or 1) exactly one does, so the idle cost
        // is one engine. The vocoder's OLA needs ~N samples to fill, which the
        // 150 ms fade covers — it ramps in from silence rather than clicking.
        p->g_focus += focus_c * (focus_t - p->g_focus);
        const float fx = p->g_focus;
        // The wet swell per engine: granular takes the global envelope (a
        // POG2-style duck-and-reswell on every onset), the vocoder swells per
        // bin inside _process_frame. V_DRYD belongs to the DRY path and takes
        // neither — its swell is the DRY ATTACK lerp further down.
        auto voice_raw = [&](Voice v) noexcept -> float {
            const float ge = (env_on && v != V_DRYD) ? p->env_level : 1.0f;
#ifdef POGGED_NO_VOCODER
            return ge * p->sh[v].process(ring, mask, p->wpos);
#else
            float s = 0.0f;
            if (fx < 0.9999f) s += (1.0f - fx) * ge * p->sh[v].process(ring, mask, p->wpos);
            if (fx > 1e-4f)   s += fx * p->pv[v].process(ring, mask, p->wpos);
            return s;
#endif
        };

        // Each voice is tone-shaped by its fixed voicing filter, then panned
        // into the stereo wet bus (subs → LP, ups → HP; see pogged_dsp_new).
        // Detuned voices are filtered independently, then averaged with their
        // main voice and panned with it — the detune is a chorus on one voice,
        // not a separate voice with its own place in the field.
        float wet_l = 0.0f, wet_r = 0.0f;
        float gl, gr;
        if (act[V_SUB1]) {
            const float v = p->g_sub1 * p->vfilt[V_SUB1].process(voice_raw(V_SUB1));
            pan_gains(p->p_sub1, gl, gr);
            wet_l += gl * v; wet_r += gr * v;
        }
        if (act[V_SUB2]) {
            const float v = p->g_sub2 * p->vfilt[V_SUB2].process(voice_raw(V_SUB2));
            pan_gains(p->p_sub2, gl, gr);
            wet_l += gl * v; wet_r += gr * v;
        }
        // SPREAD: right channel delayed 3x longer than left, throwing the
        // upper voices wide. Applied post-detune-mix, pre-pan, and only to
        // +5th/+1/+2 — the manual excludes the suboctaves. At spread 0 the
        // delay line returns the sample just written, so this is transparent.
        p->g_spread += gc * (spread_t - p->g_spread);
        const float dl_s = p->g_spread * SPREAD_L_MAX_MS * 0.001f * sr;
        const float dr_s = p->g_spread * SPREAD_R_MAX_MS * 0.001f * sr;
        auto spread_mix = [&](Voice v, float s, float pan) noexcept {
            p->sdl[v].write(s);
            float lg, rg;
            pan_gains(pan, lg, rg);
            wet_l += lg * p->sdl[v].read(dl_s);
            wet_r += rg * p->sdl[v].read(dr_s);
        };

        // +5th takes no detune: on both the POG2 and the POG3 the DETUNE
        // slider acts on the +1/+2 voices only.
        if (act[V_UP5]) {
            const float v = p->g_up5 * p->vfilt[V_UP5].process(voice_raw(V_UP5));
            spread_mix(V_UP5, v, p->p_up5);
        }
        if (act[V_UP1]) {
            const float v = p->vfilt[V_UP1].process(voice_raw(V_UP1));
            float o = v;
            if (act[V_UP1D]) {
                const float vd = p->vfilt[V_UP1D].process(voice_raw(V_UP1D));
                o = (1.0f - m) * v + m * vd;   // m ramps 0..0.5 (0.5 = 50/50)
            }
            o *= p->g_up1;
            spread_mix(V_UP1, o, p->p_up1);
        }
        if (act[V_UP2]) {
            const float v = p->vfilt[V_UP2].process(voice_raw(V_UP2));
            float o = v;
            if (act[V_UP2D]) {
                const float vd = p->vfilt[V_UP2D].process(voice_raw(V_UP2D));
                o = (1.0f - m) * v + m * vd;
            }
            o *= p->g_up2;
            spread_mix(V_UP2, o, p->p_up2);
        }

        // ── Dry path (POG3 DRY buttons) ──────────────────────────────────
        // Chain order follows the POG2's own cycle (Attack, then LP Filter,
        // then Detune are added in turn), so: detune -> attack -> filter.
        // Every route is crossfaded rather than switched: these are buttons,
        // and every hard switch in this file has clicked.
        p->g_dryatk  += gc * (dryatk_t  - p->g_dryatk);
        p->g_dryfilt += gc * (dryfilt_t - p->g_dryfilt);
        p->g_drydet  += gc * (drydet_t  - p->g_drydet);

        float d = p->g_dry * x;
        // DRY DETUNE: mix in a detuned copy of the dry — a chorus, and the
        // one thing that costs the dry its zero latency (the copy is a shifted
        // voice, ~3 ms behind). Faithful: the hardware cannot detune the dry
        // without processing it either.
        if (act[V_DRYD]) {
            const float dd = p->vfilt[V_DRYD].process(voice_raw(V_DRYD));
            d = (1.0f - 0.5f * p->g_drydet) * d + (0.5f * p->g_drydet) * (p->g_dry * dd);
        }
        // SPREAD reaches the dry only through DRY DETUNE — the manual gates it
        // on that same button.
        p->sdl_dry.write(d);
        const float d_spread = p->g_drydet;
        float dl_dry = d, dr_dry = d;
        if (d_spread > 1e-4f) {
            dl_dry = d + d_spread * (p->sdl_dry.read(dl_s) - d);
            dr_dry = d + d_spread * (p->sdl_dry.read(dr_s) - d);
        }

        // DRY ATTACK: lerp between untouched and swelled, so the button fades.
        // (The wet swell happened per voice in voice_raw; the envelope itself
        // was advanced before the voices ran.)
        {
            const float e = 1.0f + p->g_dryatk * (p->env_level - 1.0f);
            dl_dry *= e; dr_dry *= e;
        }
        // DRY FILTER: split the dry between the pre-filter and post-filter
        // buses. Crossfading the ROUTE, not muting a branch — at rest it is
        // wholly on one side, mid-flip it is smoothly shared.
        pan_gains(p->p_dry, gl, gr);
        const float pre_l = p->g_dryfilt * gl * dl_dry;
        const float pre_r = p->g_dryfilt * gr * dr_dry;
        const float post_l = (1.0f - p->g_dryfilt) * gl * dl_dry;
        const float post_r = (1.0f - p->g_dryfilt) * gr * dr_dry;
        wet_l += pre_l;
        wet_r += pre_r;

        // Filter sweep (POG3 ENV). The detector always runs so its RMS state
        // is warm if the sweep is enabled mid-note; it is separate from the
        // ATTACK detector because the POG3 gives the sweep its own Trigger
        // Sensitivity.
        const bool fonset = p->filt_det.process(x, fsens);
        if (fenv_on) {
            if (fonset) p->filt_env.trigger();
            p->filt_env_level = p->filt_env.process();
        } else {
            p->filt_env_level = 0.0f;
        }

        // Refresh coefficients on a stride rather than per sample (see
        // FILT_UPDATE_STRIDE). Frequency, Q and mode are all cached together —
        // the mode has to be in the key or switching LP→BP would keep the old
        // coefficients until the frequency happened to move.
        if (--p->filt_ctr <= 0) {
            p->filt_ctr = FILT_UPDATE_STRIDE;
            const float f = std::clamp(
                p->smooth_cutoff *
                    std::exp2(fenv_d * p->filt_env_level * ENV_SWEEP_OCTAVES),
                20.0f, ny);
            if (f != p->cached_cutoff || p->smooth_q != p->cached_q ||
                (int)mode != p->cached_mode) {
                p->filter_l.setup(mode, f, p->smooth_q, sr);
                p->filter_r.setup(mode, f, p->smooth_q, sr);
                p->cached_cutoff = f;
                p->cached_q      = p->smooth_q;
                p->cached_mode   = (int)mode;
            }
        }

        // Crossfade the filter in/out (g_filtmix ramps 0..1). Feed the filters
        // whenever the mix is non-trivial so their state stays coherent
        // through the fade; at steady bypass (mix 0) they are skipped.
        p->g_filtmix += gc * ((bypass ? 0.0f : 1.0f) - p->g_filtmix);
        if (p->g_filtmix > 1e-4f) {
            const float fl = p->filter_l.process(wet_l);
            const float fr = p->filter_r.process(wet_r);
            wet_l += p->g_filtmix * (fl - wet_l);
            wet_r += p->g_filtmix * (fr - wet_r);
        }

        out_l[i] = soft_clip((wet_l + post_l) * p->g_out);
        out_r[i] = soft_clip((wet_r + post_r) * p->g_out);
    }
}
