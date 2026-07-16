#ifndef POGGED_DSP_H
#define POGGED_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {           /* required: the JUCE wrapper is C++ */
#endif

/* One value per control port, copied from LV2 ports / JUCE parameters.
   Raw values are accepted: the clamp happens inside pogged_dsp_process().
   Field names mirror the .ttl symbols; comments give the LV2 port index
   (control ports run idx 2..13). Field order IS the port order — new ports
   are appended, never inserted, because the index is a port's identity and
   renumbering would silently remap any state a host already saved. */
typedef struct {
    float dry_level;     /* idx 2  [0 – 2]      1 = neutral            */
    float sub1_level;    /* idx 3  [0 – 2]      -1 octave              */
    float sub2_level;    /* idx 4  [0 – 2]      -2 octaves             */
    float up1_level;     /* idx 5  [0 – 2]      +1 octave              */
    float up2_level;     /* idx 6  [0 – 2]      +2 octaves             */
    float detune_cents;  /* idx 7  [0 – 25]     on the +1/+2 voices    */
    float attack_ms;     /* idx 8  [0 – 2000]   0/≤1 = no swell        */
    float attack_sens;   /* idx 9  [0 – 1]      onset sensitivity      */
    float lp_cutoff;     /* idx 10 [20 – 20000] Hz, ≥19 kHz = bypass   */
    float lp_q;          /* idx 11 [0.5 – 8]                           */
    float out_level;     /* idx 12 [0 – 2]      1 = neutral            */
    float up5_level;     /* idx 13 [0 – 2]      +5th (POG3 voice)      */
    /* idx 14 is audio_out_r (an audio port, not a control). */
    /* Per-voice pan, POG3-style: -1 = hard left, 0 = centre, +1 = hard right */
    float pan_dry;       /* idx 15 [-1 – 1]                            */
    float pan_sub1;      /* idx 16 [-1 – 1]                            */
    float pan_sub2;      /* idx 17 [-1 – 1]                            */
    float pan_up5;       /* idx 18 [-1 – 1]                            */
    float pan_up1;       /* idx 19 [-1 – 1]                            */
    float pan_up2;       /* idx 20 [-1 – 1]                            */
    float spread;        /* idx 21 [0 – 1]   POG3 SPREAD: stereo delay on
                                             +5th/+1/+2 only. R is 3x L
                                             (L<=50ms, R<=150ms). 0 = off. */
    /* POG3 multimode filter + envelope sweep. lp_cutoff/lp_q above keep their
       "lp_" symbols: a symbol is the port's identity and renaming it would
       orphan saved state. They now mean the filter's frequency and Q in every
       mode. */
    float filter_mode;   /* idx 22 [0/1/2]   0 = LP (default), 1 = BP, 2 = HP */
    float filter_env;    /* idx 23 [-1 – 1]  sweep depth; 0 = envelope off,
                                             + sweeps up, - sweeps down       */
    float filter_env_a;  /* idx 24 [1 – 1000]   ms, sweep attack              */
    float filter_env_d;  /* idx 25 [1 – 2000]   ms, sweep decay               */
    float filter_sens;   /* idx 26 [0 – 1]   sweep trigger sensitivity        */
    float range_mode;    /* idx 27 [0/1/2]   instrument range: 0 = guitar (E2),
                                             1 = baritone (B1), 2 = bass (B0).
                                             Sizes the sub voices' grains and
                                             correlation scan. Trades sub
                                             latency for low-end stability.   */
    float focus;         /* idx 28 [0/1]  engine: 0 = granular (POG-style, low
                                            latency), 1 = phase vocoder (clean
                                            on chords, ~85 ms). POG3's FOCUS.  */
    float input_gain;    /* idx 29 [0.5 – 3]  POG3 INPUT GAIN: the level seen
                                            at the input, so it feeds the
                                            voices, the dry AND the onset
                                            detectors — as on the pedal.      */
    /* POG3 DRY buttons: route the dry through each effect. Off = the POG's
       defining undelayed, unprocessed dry. */
    float dry_attack;    /* idx 30 [0/1]  dry through the attack swell        */
    float dry_filter;    /* idx 31 [0/1]  dry through the filter              */
    float dry_detune;    /* idx 32 [0/1]  dry through the detune chorus — and
                                          through SPREAD, which the manual
                                          gates on this same button. Costs the
                                          dry its zero latency.               */
} PoggedParams;

typedef struct PoggedDsp PoggedDsp;          /* opaque state */

PoggedDsp* pogged_dsp_new(double sample_rate);
void       pogged_dsp_free(PoggedDsp*);
void       pogged_dsp_reset(PoggedDsp*);     /* = activate() */

/* Process n samples: mono in -> stereo out (POG3-style per-voice panning).
   out_l/out_r must be distinct buffers; either may alias in. */
void       pogged_dsp_process(PoggedDsp*, const PoggedParams*,
                              const float* in, float* out_l, float* out_r,
                              uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* POGGED_DSP_H */
