// bench_vocoder — worst-BLOCK cost of the FOCUS vocoder path, single-window
// vs multi-resolution (§13). Not part of `make audit` (timing is machine-
// dependent); run it by hand:
//
//   g++ -O3 -ffast-math -std=c++17 -Isrc tools/bench_vocoder.cpp -o build/bench_vocoder
//   build/bench_vocoder
//
// What matters is the PEAK, not the average: a frame is 2 FFTs landing inside
// one audio block, and the deadline is per block. The harness mirrors the
// plugin: 8 instances (N_VOICES) staggered v*(HOP/8), 128-sample blocks at
// 48 kHz (2.667 ms deadline — the Pi/MOD configuration that motivated the
// stagger). Numbers on x86 are indicative; the Pi 5 must be measured on the
// device before shipping the multi-res there.
#include "../src/stream_vocoder.hpp"
#include "../src/stream_multivocoder.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <vector>

static constexpr float    SR     = 48000.0f;
static constexpr int      BLOCK  = 128;
static constexpr int      VOICES = 8;
static constexpr uint32_t RSZ    = 65536, MASK = RSZ - 1;
static constexpr double   DEADLINE_US = 1e6 * BLOCK / SR;   // 2666.7 us
static constexpr float    RATIOS[VOICES] = { 0.5f, 0.25f, 1.4983f, 2.0f, 4.0f,
                                             2.0f, 4.0f, 1.0f };

template <class Engine>
static void bench(const char* name, const std::vector<float>& in)
{
    static Engine eng[VOICES];
    for (int v = 0; v < VOICES; ++v) {
        eng[v].init(SR, v * (Engine::HOP / VOICES));
        eng[v].set_ratio(RATIOS[v]);
        eng[v].reset();
    }
    std::vector<float> ring(RSZ, 0.0f);
    uint64_t wpos = RSZ;
    std::vector<double> us_per_block;
    double total_us = 0.0;
    float sink = 0.0f;
    for (size_t i = 0; i + BLOCK <= in.size(); i += BLOCK) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int j = 0; j < BLOCK; ++j) {
            ring[wpos & MASK] = in[i + j];
            ++wpos;
            for (int v = 0; v < VOICES; ++v)
                sink += eng[v].process(ring.data(), MASK, wpos);
        }
        const double us = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - t0).count();
        us_per_block.push_back(us);
        total_us += us;
    }
    // The FFT bursts repeat every HOP samples (a handful of blocks), so the
    // p99 block IS a burst block; unlike the raw max it ignores the rare
    // scheduler spike that would otherwise dominate the readout.
    std::sort(us_per_block.begin(), us_per_block.end());
    const double p99 = us_per_block[(size_t)(0.99 * (us_per_block.size() - 1))];
    const double avg = total_us / us_per_block.size();
    std::printf("  %-22s avg %6.0f us/block (%4.1f%% of deadline)   "
                "p99 %6.0f us (%5.1f%%)   [sink %.3f]\n",
                name, avg, 100.0 * avg / DEADLINE_US,
                p99, 100.0 * p99 / DEADLINE_US, (double)sink);
}

int main()
{
    // 10 s of chord-ish material — the load is input-independent (the FFTs
    // run either way), the signal just keeps the numbers honest.
    const int n = (int)(10.0f * SR);
    std::vector<float> in(n);
    for (int i = 0; i < n; ++i) {
        const double t = i / (double)SR;
        in[i] = 0.2f * (std::sin(2 * M_PI * 82.41 * t) +
                        std::sin(2 * M_PI * 123.47 * t) +
                        std::sin(2 * M_PI * 164.81 * t));
    }

    std::printf("8 voices, %d-sample blocks at %g kHz (deadline %.0f us):\n",
                BLOCK, SR / 1000.0f, DEADLINE_US);
    bench<StreamVocoderT<4096>>("single window 4096", in);
    bench<StreamVocoderT<2048>>("single window 2048", in);
    bench<MultiVocoder<4096, 2048>>("multi-res 4096+2048", in);   // shipped (§20)
    bench<MultiVocoder<8192, 4096>>("multi-res 8192+4096", in);
    bench<MultiVocoder3<8192, 4096, 1024>>("3 bandes 8192/4096/1024", in);
    return 0;
}
