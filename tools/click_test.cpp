// click_test — steps every level control mid-signal and checks the output
// stays free of discontinuities: with ~30 ms gain smoothing in the core, the
// worst sample-to-sample delta must stay close to the input's own slope.
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

static constexpr float SR    = 48000.0f;
static constexpr int   BLOCK = 256;

int main()
{
    const int n = (int)(4.0f * SR);
    std::vector<float> in(n), out(n), out_r(n);   // pans centred: L == R
    for (int i = 0; i < n; ++i)
        in[i] = 0.5f * std::sin(2.0 * M_PI * 220.0 * i / SR);

    // The input's own max sample-to-sample delta (sine slope).
    const float in_delta = 0.5f * 2.0f * (float)M_PI * 220.0f / SR;

    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.dry_level   = 1.0f;
    p.out_level   = 1.0f;
    p.lp_cutoff   = 20000.0f;
    p.lp_q        = 0.707f;
    p.attack_sens = 0.35f;

    // Step schedule: every 0.5 s flip a control hard.
    struct Step { float t; float* field; float value; };
    Step steps[] = {
        { 0.5f, &p.sub1_level,   1.5f },
        { 1.0f, &p.up1_level,    1.5f },
        { 1.5f, &p.up2_level,    1.5f },
        { 2.0f, &p.sub2_level,   1.5f },
        { 2.5f, &p.detune_cents, 15.0f },
        { 3.0f, &p.sub1_level,   0.0f },
        { 3.2f, &p.up1_level,    0.0f },
        { 3.4f, &p.lp_cutoff,    800.0f },
    };
    size_t next = 0;

    for (int i = 0; i < n; i += BLOCK) {
        const float t = i / SR;
        while (next < sizeof(steps)/sizeof(steps[0]) && t >= steps[next].t) {
            *steps[next].field = steps[next].value;
            ++next;
        }
        pogged_dsp_process(dsp, &p, in.data() + i, out.data() + i, out_r.data() + i,
                           std::min(BLOCK, n - i));
    }
    pogged_dsp_free(dsp);

    float max_delta = 0.0f;
    for (int i = 1; i < n; ++i)
        max_delta = std::max(max_delta, std::abs(out[i] - out[i-1]));

    // Wet voices at higher pitch have steeper legitimate slopes (+2 oct on a
    // stack of gains): allow a generous headroom over the dry input slope,
    // clicks show up an order of magnitude above it.
    const float limit = 20.0f * in_delta;
    const bool ok = max_delta <= limit;
    std::printf("  max sample delta %.4f (limit %.4f, input slope %.4f)\n",
                max_delta, limit, in_delta);
    std::printf("click_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
