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
#include "octave_anchor.hpp"
// FOCUS's vocoder path. Multi-resolution (§13): the long window resolves the
// bass, the short one carries the treble and the attacks. A target that pins
// POGGED_PV_N (the Duo X pins 2048 for CPU) keeps the historic single window
// at that size instead.
#ifdef POGGED_PV_N
using PoggedVocoder = StreamVocoder;
using SubVocoder    = StreamVocoder;   // §27: single 2048 window is already OS=4
#else
// OS=8 (§21): 87.5 % overlap — same windows, same latency, twice the frame
// density, so the OLA averages 8 renderings and frame-rate artifacts smooth
// out (chord worst case 74.5 -> 71.9 dB, no regression elsewhere once the
// estimator baseline was decoupled from the hop). Costs 2x the FFT work —
// affordable since the real FFT (§19).
#ifndef POGGED_PV_OS
#define POGGED_PV_OS 8   // up-voice overlap factor; -DPOGGED_PV_OS=4 for the A/B
#endif
using PoggedVocoder = MultiVocoder<4096, 2048, POGGED_PV_OS>;
// §27: the DOWN voices (÷2, ÷4) get their OWN engine at OS=4, bare. The whole
// §20-§26 arc (OS=8, §22 Prony, §24 hints, §26 input cap) was tuned for the UP
// voices' chord artefacts and applied globally; on the sub it SMEARED attacks
// (measured 73 ms rise at OS=4 -> 153 ms at OS=8) and added the "gurgle" the
// ear rejected — the user judged the sub "largely better" at commit 351591f,
// which was exactly this simple OS=4 shape. So the sub goes back to it while
// the up voices keep the OS=8 machinery that helps THEM.
using SubVocoder    = MultiVocoder<4096, 2048, 4>;
// §20 — the LATENCY BUDGET is the spec. The user A/B'd every build of the
// §13-§19 arc on the pedalboard and ruled: the 4096+2048 profile (85 ms
// bass, 42 ms above the crossover) is the usable quality/latency point;
// anything slower is out of spec. The 8192-based §16 shape measures far
// cleaner on collisions (excess AM +1.4 vs +14.7 dB) but its 171/85 ms feel
// failed the instrument test. So the windows and the FIXED output-side
// 250 Hz crossover below are the §13 shape — while every latency-NEUTRAL
// gain since is kept: LR8 crossover (§15), real FFT (§19), transient
// reinjection (§18), per-bin swell (§14). Timbre stability within this
// budget is the open §20 program; shimmer_test ratchets it.
//
// §25 EXPERIMENT (build with XBAND=1; default OFF). The dominant audible
// artifact the ear caught was NOT the §24 fundamental-pair midpoint (~249 Hz,
// fixed) but the SECOND-harmonic pair's midpoint (~498 Hz on a low third), at
// -8.5 dB — "almost the same level as the rest". It sits ABOVE the 250
// crossover, so the SHORT window rendered it, and 2048 cannot resolve that
// pair (2.4 bins) -> it smears the merged lobe to the pair midpoint. §25
// raises the crossover 250 -> 600, handing the whole 250-600 band to the
// 4096 LONG window, which DOES resolve the h2 pair (4.9 bins): 498 drops
// -8.5 -> -23 dB. With the pair no longer at the crossover edge, the long
// window's §22 also engages on the FUNDAMENTAL pair (fmin 0) and kills the
// 249 midpoint (-> -28). Chord excess-AM mean improves 17.4 -> 12.9 dB.
//   TWO measured costs, both for the ear to rule on:
//   1. LATENCY: the 250-600 Hz output band moves from the 42 ms short window
//      to the 85 ms long window — a real low-mid latency increase.
//   2. ROUGHNESS: §22 on the fundamental pair adds 5-80 Hz flutter (shimmer
//      roughness mean 1.87 -> 2.32 — but the WORST added flutter improves
//      13.5 -> 10.6; the mean rise is reduced over-smoothing, not audible
//      shimmer). Still a FLAG until it is the default.
//
// The crossover is a PURITY/LATENCY DIAL, not a single point: each step up
// hands one more harmonic pair to the 4096 window and removes its whole
// family of midpoint parasites (triad forest, lines > -25 dB: 250 -> 106,
// 600 -> 67, 900 -> 49; the h2 midpoint dies past ~560, the h3 past ~830).
// Higher = cleaner but the attack in that band comes through the 85 ms
// window. Build XBAND=<hz> to set it (XBAND=1 keeps the §25 default 600).
#ifdef POGGED_XBAND
  #ifdef POGGED_XOVER
static constexpr float VOC_XOVER_OUT  = (float)POGGED_XOVER;
  #else
static constexpr float VOC_XOVER_OUT  = 600.0f;
  #endif
static constexpr float VOC_PRONY_FMIN = 0.0f;
#else
static constexpr float VOC_XOVER_OUT  = 250.0f;
static constexpr float VOC_PRONY_FMIN = 160.0f;
#endif
// §28: how loud the octave-lock anchor sits under the raw sub. It is a
// work-in-progress (Spike 12): the streaming build currently only nudges the
// octave (33/36 vs the raw sub's 32) and beats faintly against the raw sub on
// sustained clean chords — so it ships OFF (0.0f), with zero CPU cost (the
// anchor's per-sample work is guarded by ANCHOR_MIX > 0 and folded away when
// it is a zero constant). Build -DPOGGED_ANCHOR_MIX=1.0f to develop/audition
// it. Open work: sub-Hz f0 (parabolic peak) to stop the beat, stronger
// detection for the hard notes (weak-fundamental / sub-audible).
#ifndef POGGED_ANCHOR_MIX
#define POGGED_ANCHOR_MIX 0.0f
#endif
static constexpr float ANCHOR_MIX = POGGED_ANCHOR_MIX;
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
#ifndef POGGED_GRAIN_PERIODS
#define POGGED_GRAIN_PERIODS 2.2f
#endif
static constexpr float GRAIN_PERIODS = POGGED_GRAIN_PERIODS;
static constexpr float ALIGN_PERIODS = 0.5f;
// Capped so the read stays well inside the ring and smearing stays bounded:
// a bass's -2 voice emits 7.7 Hz, which is a rumble, not a pitch — sizing
// grains for it would smear everything else for nothing.
static constexpr float GRAIN_MAX_MS  = 160.0f;

// The UP voices' fixed grain length (ms). Kept short for a tight, low-latency
// attack (the granular's whole reason to exist beside the vocoder).
#ifndef POGGED_GRAIN_UP_MS
#define POGGED_GRAIN_UP_MS 25.0f
#endif
static constexpr float GRAIN_UP_MS_C = POGGED_GRAIN_UP_MS;

// §34: the UP voices used a FIXED grain, sized once for every note. On a LOW
// note the up-output is still low enough that a fixed short grain spans less
// than a period — the +5th of a baritone low B sings at 92 Hz, and a 10 ms
// grain is 0.9 of its 10.8 ms period — so the aligner has no full cycle to
// lock onto and the splice warbles into "bouillie" (the user, on the +1 and
// the fifth from F# down). When > 0, size each up voice's grain to span at
// least this many periods of the LOWEST output it emits at the current range,
// floored at GRAIN_UP_MS so high notes keep their tight attack. 0 = the old
// fixed behaviour (1f1f52b).
#ifndef POGGED_GRAIN_UP_PERIODS
#define POGGED_GRAIN_UP_PERIODS 0.0f
#endif
static constexpr float GRAIN_UP_PERIODS = POGGED_GRAIN_UP_PERIODS;

// ── FOCUS: which pitch engine ────────────────────────────────────────────────
// The POG3 has a FOCUS button that swaps the transposition algorithm, and its
// manual notes the POG algorithm "features lower latency" than the alternative.
// Ours is the same trade, measured:
//
//              artifact over an ideal shift (sub, chord)   latency
//   granular             +4.8 dB                            3 ms
//   vocoder              +0.0 dB                           42 ms above 250 Hz,
//                                                          85 ms below
//                                                          (multi-res §13/§20)
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
// Switching engines crossfades over ~150 ms: they have different latencies (3
// vs up to 85 ms), so a hard switch would jump the signal, and the fade must
// outlast the long window's OLA fill (~85 ms) so the vocoder ramps in from
// real content rather than from its zero-padded start. Both engines run only
// during the fade; at rest exactly one does.
static constexpr float FOCUS_XFADE_MS = 150.0f;

// §30 — dynamic Focus (the POG-class hybrid). The vocoder renders a clean but
// ~85 ms-late octave body; the granular engine renders a tight but warbly one.
// The vocoder's latency only hurts at the ONSET (the "doublon" under the 0-
// latency dry); its warble-free body wins once the note is ringing. So on each
// onset we hold the GRANULAR engine (tight, fills the latency gap), then switch
// to the VOCODER for the sustained body:
//   - HOLD: granular-only after the onset, long enough to cover the vocoder's
//     latency so the switch lands where the vocoder is already present;
//   - RISE: a SHORT switch to the vocoder. The two engines are phase-incoherent,
//     so their overlap combs — a short crossover minimises that band (the user
//     A/B'd fast vs slow and slow-in-steady-state; the fast switch, "D1", won);
//   - FALL: a fast-but-smooth drop back to granular on the next onset (no hard
//     step, which would click the notes still ringing).
// Equal-power (sqrt) crossfade, so the decorrelated pair does not dip -3 dB
// mid-fade. Values chosen by ear on the user's own take (D1: 100/12/8 ms).
//
// HYB_FLOOR keeps a granular COMPONENT under the vocoder body during sustain
// (g_focus tops out at 1-FLOOR instead of 1, so g_gran = sqrt(FLOOR) stays):
// the body was pure vocoder and therefore FELT like the vocoder — the granular
// floor gives it back some of the granular's immediacy/presence at the cost of
// a little warble. 0 = the original pure-vocoder body. Tunable by ear.
#ifndef POGGED_HYB_HOLD_MS
#define POGGED_HYB_HOLD_MS 100.0f
#endif
#ifndef POGGED_HYB_FLOOR
#define POGGED_HYB_FLOOR 0.0f
#endif
static constexpr float HYB_HOLD_MS = POGGED_HYB_HOLD_MS;
static constexpr float HYB_FLOOR   = POGGED_HYB_FLOOR;
static constexpr float HYB_RISE_MS = 12.0f;
// Fixed sensitivity of the hybrid's own onset detector — high enough to catch
// a re-pluck over a ringing note (8/8 on eighth-note repeats) but not so high
// it fires on a held note (measured knee: 0.88).
static constexpr float HYB_ONSET_SENS = 0.88f;
static constexpr float HYB_FALL_MS = 8.0f;

// ── Transient reinjection (§18) ──────────────────────────────────────────────
// The wet's FELT latency is its attack's: the pitched body cannot arrive
// earlier (Gabor — §16/§17), but the pick's broadband snap can. On each onset
// a short enveloped burst of the high-passed INPUT is summed into the wet bus
// at (near) zero latency; the tonal body blooms behind it. The HP keeps the
// burst pitch-agnostic — a pick transient is percussive, its pitch does not
// matter (§12). Gated by the DRY level (a present dry already IS the
// zero-latency attack; this serves wet-only presets) and by ATTACK (a click
// would defeat a deliberate swell). Constants are ears-first starting points.
static constexpr float BURST_HP_HZ = 1800.0f;   // click passband
static constexpr float BURST_MS    = 12.0f;     // 90 % decay of the burst
static constexpr float BURST_GAIN  = 1.6f;      // level vs the wet it fronts

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
// §31 gesture: a heel touch SHORTER than this re-captures + glides (octaves stay
// frozen); a heel hold LONGER unfreezes (octaves go live). Lets you slide from
// one frozen note to the next without the return-to-heel resetting everything —
// the capture reads a DEDICATED always-live buffer, not the shared ring.
static constexpr float FRZ_UNFREEZE_MS = 150.0f;
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
    PoggedVocoder pv[N_VOICES];      // up voices + dry-detune use these
    SubVocoder    pv_sub[2];         // §27: V_SUB1, V_SUB2 — OS=4, bare
#ifndef POGGED_PV_N
    OctaveAnchor<> anc[2];           // §28: octave-lock anchor for V_SUB1/2
    float          anc_out[2] = {};  // this sample's anchor value, per sub voice
#endif
    bool          pv_live[N_VOICES] = {};
#endif
    float         g_focus = 0.0f;           // smoothed engine crossfade 0..1
    int           hyb_hold = 0;             // §30: samples left holding granular

    // Voices are panned before the mix, so the wet bus is stereo by the time
    // it reaches the filter — hence one filter per channel, same coefficients.
    Biquad        filter_l, filter_r;
    Biquad        vfilt[N_VOICES];   // fixed per-voice tone shaping (voicing)
    Envelope      env;
    OnsetDetector det;
#ifdef POGGED_DYN_FOCUS
    // §30: the hybrid's engine switch needs its OWN onset detector, at a fixed
    // high sensitivity — the swell's detector (keyed to attack_sens) has too
    // high a threshold to catch a RE-PLUCK over a still-ringing note (measured:
    // it fired once on 8 eighth-note re-plucks), so only the first attack got
    // the granular snap and every note after felt like the vocoder. This one
    // fires on each re-pluck (8/8) without spuriously triggering on a held note.
    OnsetDetector hyb_det;
#endif
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
    float         g_frz    = 0.0f;    // live -> loop crossfade, 0..1
    // §31: dedicated always-live history for freeze capture (never the loop),
    // so a new note can be captured WITHOUT unfreezing — the fix for "return to
    // heel resets everything". Plus the latched engage state and heel timer.
    std::vector<float> frz_live;
    uint32_t frz_live_mask = 0;
    uint64_t frz_live_wpos = 0;
    bool     frz_engaged   = false;   // octaves currently held (latched)
    bool     frz_at_heel   = true;    // pedal at heel on the last block
    int      frz_heel_smp  = 0;       // samples spent at heel since leaving off
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

    // Transient reinjection state (§18)
    Biquad burst_hp;              // input HP, always warm
    float  burst_env = 0.0f;      // per-onset decaying envelope
    float  g_burst   = 0.0f;      // smoothed enable (dry/ATTACK gates)
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

// §34: size the UP voices' grains from the range, mirroring setup_subs. With
// GRAIN_UP_PERIODS == 0 every up voice keeps the fixed GRAIN_UP grain (old
// behaviour); otherwise a voice whose lowest output at this range would underrun
// GRAIN_UP_PERIODS periods gets a proportionally longer grain, so the aligner
// always has a full cycle to lock onto. Floored at the fixed grain so the high
// notes (short output period) stay tight and low-latency. Called from _new and
// on every range change, like setup_subs.
static void setup_ups(PoggedDsp* p, float f_low, float sr) noexcept
{
    const int   grain_up = (int)(0.001f * GRAIN_UP_MS_C * sr);
    const int   align    = (int)(0.010f * sr);          // 10 ms correlation scan
    const Voice v[5]     = { V_UP5, V_UP1, V_UP2, V_UP1D, V_UP2D };
    const float rat[5]   = { FIFTH_RATIO, 2.0f, 4.0f, 2.0f, 4.0f };
    for (int i = 0; i < 5; ++i) {
        int grain = grain_up;
        if (GRAIN_UP_PERIODS > 0.0f) {
            const float period_ms = 1000.0f / (rat[i] * f_low);   // lowest output
            const int g_per = (int)(0.001f * std::min(GRAIN_UP_PERIODS * period_ms,
                                                      GRAIN_MAX_MS) * sr);
            grain = std::max(grain_up, g_per);
        }
        p->sh[v[i]].setup(rat[i], grain, align);
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
    const int grain_up  = (int)(0.001f * GRAIN_UP_MS_C * sr);  // POG shimmer/lag
    const int align     = (int)(0.010f * sr);   // 10 ms correlation scan

    // Aligned respawn everywhere: without it the source-position jump at
    // respawn ((ratio-1)·grain) lands at arbitrary phase — for a 25 ms grain
    // at ratio 2 that is exactly half a period of the doubled fundamental,
    // so alternate grains cancel the target pitch outright. Correlation
    // alignment (SOLA-style) keeps grains phase-coherent for any input.
    setup_subs(p, range_f_low(0.0f), sr);      // guitar until told otherwise
    setup_ups (p, range_f_low(0.0f), sr);      // §34: up grains sized from range
    // Detuned dry sits at unison (ratio 1 → no splice warble), so it keeps the
    // plain fixed grain regardless of GRAIN_UP_PERIODS.
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
        // §27: DOWN voices on the bare OS=4 SubVocoder (see the type alias).
        if (v == V_SUB1 || v == V_SUB2) {
            p->pv_sub[v].init(sample_rate, v * (SubVocoder::HOP / N_VOICES));
            p->pv_sub[v].set_ratio(VOICE_RATIO[v]);
#ifndef POGGED_PV_N
            p->pv_sub[v].prony(false, false);   // bare, like 351591f — no §22
#if defined(POGGED_DYN_FOCUS) && !defined(POGGED_SUB_SPLIT)
            // §30 CPU: under the hybrid the GRANULAR engine renders the sub's
            // attack, so its vocoder only carries the sustained body — it no
            // longer needs the short (2048) window for attack tightness. Drop
            // it to long-only (1 FFT instead of 2), same §29 logic as the up
            // voices, and cleaner in the bass too. -DPOGGED_SUB_SPLIT keeps the
            // two-window split (for A/B).
            p->pv_sub[v].long_only(true);
#endif
            // §28: octave-lock anchor (resynthesised harmonic series on the
            // shifted fundamental) sits UNDER the raw sub to stop the octave
            // wandering when the input fundamental is weak.
            p->anc[v].init(sample_rate, VOICE_RATIO[v]);
#endif
            continue;
        }
        p->pv[v].init(sample_rate, v * (PoggedVocoder::HOP / N_VOICES));
        p->pv[v].set_ratio(VOICE_RATIO[v]);
#ifndef POGGED_PV_N
        // §29: the UP-shift voices render from the LONG window only. The §20
        // trade (fixed low crossover, short window rendering input partials it
        // cannot resolve) was the source of the "bass confusion" the user heard
        // on the +1 voice — low notes/chords have densely-spaced input partials
        // whose harmonics reach >XOVER output and got routed to the 23 Hz-bin
        // short window. Measured on the user's own take: bass harmonicity 0.77
        // -> 0.86 long-only, treble unchanged (~1.0); confirmed by ear. The
        // short window only bought attack tightness, secondary in the bass per
        // the user. V_DRYD (unity, a unison detune) has no shift and keeps the
        // low-latency split. See MultiVocoder::long_only.
#ifndef POGGED_UP_LONGONLY_OFF
        if (VOICE_RATIO[v] > 1.05f) p->pv[v].long_only(true);
#endif
        // Output-side crossover, used only by V_DRYD now (the split path).
        p->pv[v].set_xover(VOC_XOVER_OUT);
        // §20 frequency smoothing: LONG window only. Measured on the full
        // engine: smoothing the short window is stable-but-MISTUNED on its
        // merged pairs, and that beats against the long window's correct
        // rendering through the crossover skirt — worse than the wobble it
        // removes. The long window's smoothing is free of that (its pairs
        // are at least partially resolved) and cleans the crossover band
        // (34 Hz pair: 37.7 -> 27.6 dB AM).
        p->pv[v].tune(0.20f, 1.0f);
        // §24/§25: the long window's §22 below 160 Hz is gated OFF at the
        // default 250 crossover — the fundamental pair is AT the crossover
        // edge there and §22's frequency wobble leaks through the LP as the
        // midpoint parasite. Under §25 (XBAND, crossover 600) the pair is deep
        // in the long window's passband, no longer at the edge, so §22 engages
        // and resolves the 249 midpoint (measured -8.7 -> -28 dB). See the
        // VOC_PRONY_FMIN definition for the two configs.
        p->pv[v].prony_fmin(VOC_PRONY_FMIN);
        // §26 (the sub input-peak cap) is RETIRED: measured on the user's own
        // signal it removed the F# fold but SMEARED the attack (73 -> 104-170
        // ms rise), because the attack transient is broadband and the cap ate
        // its high-frequency snap. The sub's clarity mattered more; the F# on
        // a sustained chord will be handled by tonal/noise separation (§27
        // piste 3) without touching attacks. The DOWN voices now use pv_sub.
#endif
    }
#endif

    p->frz.init(sample_rate);
    {
        const uint32_t fl = std::clamp(next_pow2((uint32_t)(0.5 * sample_rate)),
                                       16384u, 65536u);
        p->frz_live.assign(fl, 0.0f);
        p->frz_live_mask = fl - 1;
        p->frz_live_wpos = fl;
    }
    p->det.init(sr);
#ifdef POGGED_DYN_FOCUS
    p->hyb_det.init(sr);
#endif
    p->filt_det.init(sr);
    p->burst_hp.setup(Biquad::HP, std::min(BURST_HP_HZ, ny), 0.707f, sr);
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
        if (v == V_SUB1 || v == V_SUB2) {
            p->pv_sub[v].reset();
#ifndef POGGED_PV_N
            p->anc[v].reset();
#endif
        } else p->pv[v].reset();
        p->pv_live[v] = false;
#endif
    }
    p->g_focus = 0.0f;
    p->hyb_hold = 0;
    p->filter_l.reset();
    p->filter_r.reset();
    p->env.reset();
    p->det.reset();
#ifdef POGGED_DYN_FOCUS
    p->hyb_det.reset();
#endif
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
    p->burst_hp.reset();
    p->burst_env     = 0.0f;
    p->g_burst       = 0.0f;
    p->g_detmix      = 0.0f;
    p->g_filtmix     = 0.0f;
    p->det_phase     = 0.0f;
    p->g_warp_st     = 0.0f;
    p->frz.reset();
    p->g_frz         = 0.0f;
    std::fill(p->frz_live.begin(), p->frz_live.end(), 0.0f);
    p->frz_live_wpos = p->frz_live.size();
    p->frz_engaged   = false;
    p->frz_at_heel   = true;
    p->frz_heel_smp  = 0;
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

    // Engine crossfade time constant. Manual mode changes (turning Focus) use
    // this slow ramp; the hybrid's per-onset dynamics use the fast ones below.
    const float focus_c = 1.0f - std::exp(-1.0f / (FOCUS_XFADE_MS * 0.001f * sr));
#ifdef POGGED_DYN_FOCUS
    // §30: Focus becomes a 3-way engine SELECTOR (0 granular, 1 vocoder, 2
    // hybrid). The static crossfade still drives modes 0/1; mode 2 is the
    // onset-driven hold/rise/fall (granular attack, vocoder body).
    const int   hyb_hold_n = (int)(HYB_HOLD_MS * 0.001f * sr);
    const float hyb_rise_c = 1.0f - std::exp(-1.0f / (HYB_RISE_MS * 0.001f * sr));
    // §35: are we in the hybrid engine mode this block? Used to key the ATTACK
    // swell to the granular's own onset (see the swell block below).
    [[maybe_unused]] const bool hyb_mode = std::clamp(p_->focus, 0.0f, 2.0f) >= 1.5f;
#else
    const float focus_t = (std::clamp(p_->focus, 0.0f, 1.0f) > 0.5f) ? 1.0f : 0.0f;
#endif

    const float range_t = std::clamp(p_->range_mode, 0.0f, 2.0f);
    if (range_t != p->cached_range) {
        setup_subs(p, range_f_low(range_t), sr);
        setup_ups (p, range_f_low(range_t), sr);   // §34: up grains follow range
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
        if (v == V_SUB1 || v == V_SUB2) {
            p->pv_sub[v].set_ratio(r);
#ifndef POGGED_PV_N
            p->anc[v].set_ratio(r);
#endif
        } else p->pv[v].set_ratio(r);   // translates peaks
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

    // ── Freeze + Gliss (§31) ──────────────────────────────────────────────
    // Capture from a DEDICATED always-live buffer, so the octaves never have to
    // go live to grab a new note (the shared ring holds the LOOP while frozen,
    // which is why the old return-to-heel capture "reset everything"). A heel
    // touch shorter than FRZ_UNFREEZE_MS re-captures the current live note and
    // GLIDES to it while the octaves stay frozen; a longer heel hold unfreezes
    // (goes live). Every heel→off transition (and the first engage from live)
    // captures — the FreezeLoop glides on all but the first (nothing to glide
    // from). The pedal position at capture sets the glide rate (20 ms → 2 s).
    const float frz_t  = std::clamp(p_->freeze, 0.0f, 1.0f);
    const bool at_heel = frz_t <= 1e-3f;
    if (at_heel) {
        p->frz_heel_smp += (int)n_samples;
        if (p->frz_heel_smp >= (int)(FRZ_UNFREEZE_MS * 0.001f * sr))
            p->frz_engaged = false;
    } else {
        if (p->frz_at_heel || !p->frz_engaged) {
            const float glide_ms = FRZ_GLIDE_MIN_MS
                                 + frz_t * (FRZ_GLIDE_MAX_MS - FRZ_GLIDE_MIN_MS);
            p->frz.capture(p->frz_live.data(), p->frz_live_mask, p->frz_live_wpos,
                           (int)(glide_ms * 0.001f * sr));
            p->frz_engaged = true;
        }
        p->frz_heel_smp = 0;
    }
    p->frz_at_heel = at_heel;
    const float frz_target = (p->frz_engaged && p->frz.armed()) ? 1.0f : 0.0f;
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
            if (v == V_SUB1 || v == V_SUB2) {
                p->pv_sub[v].reset();
#ifndef POGGED_PV_N
                p->anc[v].reset();
#endif
            } else p->pv[v].reset();   // stale OLA tail
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
    for (int v = 0; v < N_VOICES; ++v) {
        const float sw = (env_on && v != V_DRYD) ? atk_ms : 0.0f;
        if (v == V_SUB1 || v == V_SUB2) p->pv_sub[v].set_swell(sw);
        else                            p->pv[v].set_swell(sw);
    }
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
    // Burst envelope decay: 90 % gone in BURST_MS (§18).
    const float burst_c = std::exp(-std::log(9.0f) / (BURST_MS * 0.001f * sr));

    float*   ring = p->ring.data();
    const uint32_t mask = p->mask;

    for (uint32_t i = 0; i < n_samples; ++i) {
        // INPUT GAIN is "the level of the signal seen at the input", so it is
        // applied before everything — the ring the voices read, the dry, and
        // both onset detectors, which therefore trigger on the boosted level
        // exactly as they would on the pedal.
        p->g_in += gc * (in_t - p->g_in);
        const float x = in[i] * p->g_in;

        // §31: the dedicated freeze buffer always records the LIVE input, even
        // while frozen — so capture() can grab a new note without unfreezing.
        p->frz_live[p->frz_live_wpos & p->frz_live_mask] = x;
        ++p->frz_live_wpos;

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

#ifndef POGGED_NO_VOCODER
#ifndef POGGED_PV_N
        // §28: advance the octave-lock anchors on every input sample (their
        // analysis must stay continuous even when a sub voice is momentarily
        // silent); voice_raw mixes the result under the raw sub. Guarded so the
        // whole anchor cost folds away when ANCHOR_MIX is the default 0.
        if (ANCHOR_MIX > 0.0f) {
            p->anc_out[0] = p->anc[0].process(src);
            p->anc_out[1] = p->anc[1].process(src);
        }
#endif
#endif

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
#ifdef POGGED_DYN_FOCUS
        // §30: the hybrid's dedicated onset (fires on every re-pluck).
        const bool hyb_onset = p->hyb_det.process(x, HYB_ONSET_SENS);
#endif
        // §35: what re-triggers the ATTACK swell. Normally the swell's own
        // detector (`det`, keyed to attack_sens). In hybrid the GRANULAR renders
        // the attack and swells only via the global env — but `det` is far less
        // sensitive than the hybrid's hyb_det (0.88), so the granular took over
        // on every re-pluck while the swell did NOT re-trigger, and ATTACK felt
        // dead in hybrid. Key the swell to the SAME onset that drives the
        // granular takeover, so the granular swells on every pluck like the
        // vocoder's per-bin swell. hyb_det is only READ — the §30 fix is intact.
        bool swell_trig = onset;
#if defined(POGGED_DYN_FOCUS) && defined(POGGED_HYB_SWELL)
        if (hyb_mode && hyb_onset) swell_trig = true;
#endif
        if (env_on) {
            if (swell_trig) {
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
#ifdef POGGED_DYN_FOCUS
        // §30: Focus is a 3-way selector — 0 granular, 1 vocoder, 2 hybrid.
        //   modes 0/1: fixed target, slow manual crossfade (focus_c);
        //   mode 2 (hybrid): hold granular (0) for HYB_HOLD after each onset
        //   (covering the vocoder's latency), then switch to vocoder (1) over
        //   the short HYB_RISE; fall back fast-but-smooth on the next onset.
        const float fsel = std::clamp(p_->focus, 0.0f, 2.0f);
        float hyb_target, hyb_coef;
        if (fsel < 0.5f) {                     // 0 = granular only
            hyb_target = 0.0f; hyb_coef = focus_c;
        } else if (fsel < 1.5f) {              // 1 = vocoder only
            hyb_target = 1.0f; hyb_coef = focus_c;
        } else {                               // 2 = hybrid (onset-driven)
            // SNAP to granular on the onset sample itself — a smoothed fall
            // would ramp the granular gain up over its first few ms and soften
            // the very pluck transient that makes the attack feel instant. The
            // onset's own broadband transient masks the step on notes still
            // ringing. Then hold granular through the vocoder's latency and
            // rise to the vocoder body over the short HYB_RISE.
            if (hyb_onset) { p->hyb_hold = hyb_hold_n; p->g_focus = 0.0f; }
            hyb_target = (p->hyb_hold > 0) ? 0.0f : (1.0f - HYB_FLOOR);
            if (p->hyb_hold > 0) --p->hyb_hold;
            hyb_coef = hyb_rise_c;
        }
        p->g_focus += hyb_coef * (hyb_target - p->g_focus);
#else
        p->g_focus += focus_c * (focus_t - p->g_focus);
#endif
        const float fx = p->g_focus;
#ifdef POGGED_DYN_FOCUS
        // §30: equal-power crossfade — the granular and vocoder renderings are
        // phase-incoherent, so a linear fade would dip up to -3 dB mid-cross.
        const float g_gran = std::sqrt(std::max(0.0f, 1.0f - fx));
        const float g_voc  = std::sqrt(std::max(0.0f, fx));
#else
        const float g_gran = 1.0f - fx;
        const float g_voc  = fx;
#endif
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
#ifdef POGGED_DYN_FOCUS
            // §30: g_focus alternates every onset, so BOTH engines must be
            // advanced every sample — skipping one freezes its streaming state
            // (OLA / grain taps) and clicks when it resumes. Gain may be 0; the
            // call must not be.
            constexpr bool always = true;
#else
            // Static Focus rests at 0 or 1, so exactly one engine runs at rest.
            constexpr bool always = false;
#endif
            if (always || g_gran > 1e-4f) s += g_gran * ge * p->sh[v].process(ring, mask, p->wpos);
            if (always || g_voc > 1e-4f) {
                float w;
                if (v == V_SUB1 || v == V_SUB2) {
                    w = p->pv_sub[v].process(ring, mask, p->wpos);
#ifndef POGGED_PV_N
                    // §28: octave-lock anchor under the raw sub (ge so it swells
                    // with the note like the wet, gated by the vocoder mix fx).
                    w += ge * ANCHOR_MIX * p->anc_out[v];
#endif
                } else {
                    w = p->pv[v].process(ring, mask, p->wpos);
                }
                s += g_voc * w;
            }
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

        // ── Transient reinjection (§18) ───────────────────────────────────
        // The HP always runs so it is warm when a burst fires. The gate
        // fades with the dry level (dry present = the attack already exists
        // at zero latency), with ATTACK (a click would defeat the swell),
        // and scales with the wet voices actually mixed in.
        {
            const float hpx = p->burst_hp.process(x);
            if (onset) p->burst_env = 1.0f;
            else       p->burst_env *= burst_c;
            // §18 burst is a 1800 Hz+ pick snap — it tightens the PITCHED (up)
            // voices' attack, but on the smooth low-passed subs it lands as an
            // out-of-place tick ("pic de saturation"), and the subs get their
            // attack from the granular engine anyway. So scale it by the UP
            // voices only, not the subs.
            const float wet_sum = std::min(1.0f, p->g_up5 + p->g_up1 + p->g_up2);
            const float tgt = (env_on ? 0.0f : 1.0f) *
                              std::max(0.0f, 1.0f - p->g_dry) * wet_sum;
            p->g_burst += gc * (tgt - p->g_burst);
            const float b = BURST_GAIN * p->g_burst * p->burst_env * hpx;
            wet_l += b;
            wet_r += b;
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
