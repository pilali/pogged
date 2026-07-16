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
} PoggedParams;

typedef struct PoggedDsp PoggedDsp;          /* opaque state */

PoggedDsp* pogged_dsp_new(double sample_rate);
void       pogged_dsp_free(PoggedDsp*);
void       pogged_dsp_reset(PoggedDsp*);     /* = activate() */

/* Process n mono samples. in may equal out (in-place is fine). */
void       pogged_dsp_process(PoggedDsp*, const PoggedParams*,
                              const float* in, float* out, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* POGGED_DSP_H */
