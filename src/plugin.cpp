// Thin LV2 wrapper around the host-agnostic DSP core (pogged_dsp.{h,cpp}).
//
// All processing lives in pogged_dsp.cpp. This file only:
//   - declares the LV2 port indices,
//   - holds port pointers + a PoggedDsp* instance,
//   - maps the connected control ports into a PoggedParams on each run().
#include <lv2/core/lv2.h>
#include <array>
#include <cstdint>
#include <new>

#include "pogged_dsp.h"

static constexpr char POGGED_URI[] = "https://github.com/pilali/pogged";

// ── Port indices ───────────────────────────────────────────────────────────
enum Port : uint32_t {
    P_AUDIO_IN     =  0,
    P_AUDIO_OUT    =  1,
    P_DRY_LEVEL    =  2,   // dry gain            [0 – 2]
    P_SUB1_LEVEL   =  3,   // -1 octave           [0 – 2]
    P_SUB2_LEVEL   =  4,   // -2 octaves          [0 – 2]
    P_UP1_LEVEL    =  5,   // +1 octave           [0 – 2]
    P_UP2_LEVEL    =  6,   // +2 octaves          [0 – 2]
    P_DETUNE_CT    =  7,   // +1/+2 detune        [0 – 25] cents
    P_ATTACK_MS    =  8,   // swell attack        [0 – 2000] ms
    P_ATTACK_SENS  =  9,   // onset sensitivity   [0 – 1]
    P_LP_CUTOFF    = 10,   // LP filter cutoff    [20 – 20000] Hz
    P_LP_Q         = 11,   // LP resonance        [0.5 – 8]
    P_OUT_LEVEL    = 12,   // output gain         [0 – 2]
    // Appended, not inserted: a port's index is its identity, so renumbering
    // would silently remap state a host already saved.
    P_UP5_LEVEL    = 13,   // +5th (POG3 voice)   [0 – 2]
    // The right output lands here rather than next to audio_out for the same
    // reason: appending keeps every existing index stable. LV2 does not
    // require audio ports to be contiguous.
    P_AUDIO_OUT_R  = 14,
    P_PAN_DRY      = 15,   // per-voice pan       [-1 – 1]
    P_PAN_SUB1     = 16,
    P_PAN_SUB2     = 17,
    P_PAN_UP5      = 18,
    P_PAN_UP1      = 19,
    P_PAN_UP2      = 20,
    P_SPREAD       = 21,   // POG3 stereo delay  [0 – 1]
    P_FILTER_MODE  = 22,   // 0 = LP, 1 = BP, 2 = HP
    P_FILTER_ENV   = 23,   // sweep depth        [-1 – 1]
    P_FILTER_ENV_A = 24,   // sweep attack       [1 – 1000] ms
    P_FILTER_ENV_D = 25,   // sweep decay        [1 – 2000] ms
    P_FILTER_SENS  = 26,   // sweep trigger sens [0 – 1]
    P_RANGE_MODE   = 27,   // 0 = guitar, 1 = baritone, 2 = bass
    P_FOCUS        = 28,   // 0 = granular, 1 = phase vocoder
    P_COUNT        = 29
};

// Control ports are 2..13 and 15..20; index 14 is audio, so the ctl[] slot at
// 14-2 is simply never connected.
static constexpr uint32_t N_CTL = P_COUNT - 2;

// ── Plugin instance ────────────────────────────────────────────────────────
struct PoggedLV2 {
    PoggedDsp* dsp = nullptr;

    const float* audio_in    = nullptr;
    float*       audio_out_l = nullptr;
    float*       audio_out_r = nullptr;
    std::array<const float*, N_CTL> ctl = {};
};

static inline float ctl(const PoggedLV2* p, Port port) noexcept {
    const float* ptr = p->ctl[port - 2];
    return ptr ? *ptr : 0.0f;
}

// ── LV2 callbacks ──────────────────────────────────────────────────────────
static LV2_Handle instantiate(const LV2_Descriptor*,
                              double rate,
                              const char*,
                              const LV2_Feature* const*)
{
    PoggedLV2* p = new (std::nothrow) PoggedLV2();
    if (!p) return nullptr;
    p->dsp = pogged_dsp_new(rate);
    if (!p->dsp) { delete p; return nullptr; }
    return p;
}

static void connect_port(LV2_Handle handle, uint32_t port, void* data)
{
    PoggedLV2* p = static_cast<PoggedLV2*>(handle);
    if (port == P_AUDIO_IN)
        p->audio_in = static_cast<const float*>(data);
    else if (port == P_AUDIO_OUT)
        p->audio_out_l = static_cast<float*>(data);
    else if (port == P_AUDIO_OUT_R)
        p->audio_out_r = static_cast<float*>(data);
    else if (port >= 2 && port < P_COUNT)
        p->ctl[port - 2] = static_cast<const float*>(data);
}

static void activate(LV2_Handle handle)
{
    PoggedLV2* p = static_cast<PoggedLV2*>(handle);
    pogged_dsp_reset(p->dsp);
}

static void run(LV2_Handle handle, uint32_t n_samples)
{
    PoggedLV2* p = static_cast<PoggedLV2*>(handle);

    const PoggedParams params {
        ctl(p, P_DRY_LEVEL),
        ctl(p, P_SUB1_LEVEL),
        ctl(p, P_SUB2_LEVEL),
        ctl(p, P_UP1_LEVEL),
        ctl(p, P_UP2_LEVEL),
        ctl(p, P_DETUNE_CT),
        ctl(p, P_ATTACK_MS),
        ctl(p, P_ATTACK_SENS),
        ctl(p, P_LP_CUTOFF),
        ctl(p, P_LP_Q),
        ctl(p, P_OUT_LEVEL),
        ctl(p, P_UP5_LEVEL),
        ctl(p, P_PAN_DRY),
        ctl(p, P_PAN_SUB1),
        ctl(p, P_PAN_SUB2),
        ctl(p, P_PAN_UP5),
        ctl(p, P_PAN_UP1),
        ctl(p, P_PAN_UP2),
        ctl(p, P_SPREAD),
        ctl(p, P_FILTER_MODE),
        ctl(p, P_FILTER_ENV),
        ctl(p, P_FILTER_ENV_A),
        ctl(p, P_FILTER_ENV_D),
        ctl(p, P_FILTER_SENS),
        ctl(p, P_RANGE_MODE),
        ctl(p, P_FOCUS),
    };

    pogged_dsp_process(p->dsp, &params, p->audio_in,
                       p->audio_out_l, p->audio_out_r, n_samples);
}

static void cleanup(LV2_Handle handle)
{
    PoggedLV2* p = static_cast<PoggedLV2*>(handle);
    pogged_dsp_free(p->dsp);
    delete p;
}

static const LV2_Descriptor descriptor = {
    POGGED_URI, instantiate, connect_port, activate, run, nullptr, cleanup, nullptr
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return (index == 0) ? &descriptor : nullptr;
}
