// CPU + memory profile of the DSP core — the measurement half of `make eval`.
//
// Unlike tools/bench_vocoder.cpp (which times the vocoder classes in isolation
// to choose a window shape), this drives the WHOLE pogged_dsp_process() the way
// a host does: fixed-size blocks, all voices up, a polyphonic input. What it
// reports is the number that actually has to fit — the per-block cost against
// the block deadline, worst case included, because a real-time audio thread is
// judged on its worst block, not its mean.
//
// It also prints the static footprint of the engine's members. The vocoder
// classes are hundreds of kilobytes each (fixed-size member arrays, no heap),
// so an instance that is allocated but never processed is invisible to a
// profiler and very visible in RSS.
//
// Machine-dependent by nature: run it by hand, on the target, and compare runs
// on the same machine. It asserts nothing and always exits 0.
//
//   make eval          # builds and runs this
//   build/eval/cpu_profile [blocksize] [seconds]

#include "pogged_dsp.h"
#include "stream_shifter.hpp"
#ifndef POGGED_NO_VOCODER
#include "stream_vocoder.hpp"
#include "stream_multivocoder.hpp"
#include "octave_anchor.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

namespace {

// Mirrors the core's default (see the PoggedVocoder alias in pogged_dsp.cpp),
// so the footprint report describes the vocoder this build actually contains.
#ifdef POGGED_PV_OS
constexpr int PV_OS = POGGED_PV_OS;
#else
constexpr int PV_OS = 8;
#endif

using Clock = std::chrono::steady_clock;

double now_us()
{
    return std::chrono::duration<double, std::micro>(
               Clock::now().time_since_epoch()).count();
}

// A chord, not a sine: the whole point of the engine is polyphonic material,
// and the vocoder's cost depends on how many spectral peaks it finds. E major
// on the low strings, eight harmonics each, re-plucked once a second.
std::vector<float> chord_input(double sr, size_t n)
{
    static const float kNotes[4] = { 82.41f, 110.0f, 138.59f, 164.81f };
    std::vector<float> in(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = (float)i / (float)sr;
        const float pluck = std::exp(-3.0f * std::fmod(t, 1.0f));
        float s = 0.0f;
        for (float f : kNotes)
            for (int h = 1; h <= 8; ++h)
                s += (0.25f / h) * std::sin(6.2831853f * f * h * t);
        in[i] = 0.2f * pluck * s;
    }
    return in;
}

PoggedParams all_voices_up()
{
    PoggedParams p {};
    p.dry_level = p.sub1_level = p.sub2_level = 1.0f;
    p.up1_level = p.up2_level = p.up5_level = 1.0f;
    p.detune_cents = 12.0f;          // the detuned voices are extra readers
    p.attack_sens  = 0.35f;
    p.lp_cutoff    = 20000.0f;
    p.lp_q         = 0.707f;
    p.out_level    = 1.0f;
    p.input_gain   = 1.0f;
    p.filter_env_a = 50.0f;
    p.filter_env_d = 200.0f;
    p.filter_sens  = 0.5f;
    p.spread       = 0.5f;
    p.warp_toe     = 12.0f;
    return p;
}

void report_footprint()
{
    std::printf("== static footprint (bytes per instance) ==\n");
    std::printf("  StreamShifter                %10zu\n", sizeof(StreamShifter));
#ifndef POGGED_NO_VOCODER
    std::printf("  StreamVocoderT<4096,4>       %10zu\n", sizeof(StreamVocoderT<4096, 4>));
    std::printf("  StreamVocoderT<2048,4>       %10zu\n", sizeof(StreamVocoderT<2048, 4>));
    std::printf("  MultiVocoder<4096,2048,4>    %10zu\n", sizeof(MultiVocoder<4096, 2048, 4>));
    std::printf("  MultiVocoder<4096,2048,8>    %10zu\n", sizeof(MultiVocoder<4096, 2048, 8>));
#ifndef POGGED_PV_N
    std::printf("  OctaveAnchor<>               %10zu\n", sizeof(OctaveAnchor<>));
    // What the engine actually holds. pv[] is sized N_VOICES-2, not N_VOICES:
    // the subs run on pv_sub[], and a Voice-sized array used to carry two
    // MultiVocoder instances that were never initialised and never processed.
    // The anchors are absent altogether unless the build asked for them.
    const size_t live = 6 * sizeof(MultiVocoder<4096, 2048, PV_OS>)   // pv[N_PV]
                      + 2 * sizeof(MultiVocoder<4096, 2048, 4>);      // pv_sub[2]
    std::printf("  -> vocoder members live in the engine: %zu KiB "
                "(6 x pv + 2 x pv_sub)\n", live / 1024);
#ifdef POGGED_ANCHOR_MIX
    std::printf("  -> plus %zu KiB of octave anchors (this build set ANCHOR)\n",
                (2 * sizeof(OctaveAnchor<>)) / 1024);
#else
    std::printf("  -> the 2 x %zu KiB of octave anchors are NOT built "
                "(no ANCHOR= in this build)\n", sizeof(OctaveAnchor<>) / 1024);
#endif
#endif
#endif
    std::printf("\n");
}

struct Stats { double mean, p50, p99, worst; };

Stats summarise(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    Stats s {};
    s.mean  = std::accumulate(v.begin(), v.end(), 0.0) / (double)v.size();
    s.p50   = v[v.size() / 2];
    s.p99   = v[(size_t)(0.99 * (double)v.size())];
    s.worst = v.back();
    return s;
}

void run_mode(const char* label, float focus, const std::vector<float>& in,
              double sr, int blk)
{
    PoggedParams p = all_voices_up();
    p.focus = focus;

    PoggedDsp* dsp = pogged_dsp_new(sr);
    if (!dsp) { std::printf("  %-22s allocation failed\n", label); return; }

    const int blocks = (int)(in.size() / (size_t)blk);
    std::vector<float> l(blk), r(blk);
    std::vector<double> t;
    t.reserve((size_t)blocks);
    for (int b = 0; b < blocks; ++b) {
        const double a = now_us();
        pogged_dsp_process(dsp, &p, in.data() + (size_t)b * blk,
                           l.data(), r.data(), (uint32_t)blk);
        t.push_back(now_us() - a);
    }
    pogged_dsp_free(dsp);

    // Drop the first 200 blocks: the vocoder's OLA has to fill and the gain
    // smoothers have to settle before the steady-state cost means anything.
    const size_t skip = std::min<size_t>(200, t.size() / 4);
    const Stats s = summarise({ t.begin() + (long)skip, t.end() });
    const double deadline = 1e6 * (double)blk / sr;

    std::printf("  %-22s mean %7.1f us (%5.1f%%)  p99 %7.1f us (%5.1f%%)  "
                "worst %7.1f us (%5.1f%%)\n",
                label, s.mean, 100.0 * s.mean / deadline,
                s.p99, 100.0 * s.p99 / deadline,
                s.worst, 100.0 * s.worst / deadline);
}

// The granular engine's grain envelope used to be two std::cos per sample per
// voice, measured at ~32 % of StreamShifter::process. It is now a rotating
// phasor plus an exact complement (see stream_shifter.hpp). This keeps timing
// the transcendental pair beside the real thing, as a standing figure for what
// putting a std::cos back on the per-sample path would cost.
void granular_hotspot(double sr)
{
    const int n = (int)(sr * 20.0);
    const int grain = (int)(0.053 * sr);

    std::vector<float> ring(16384);
    for (size_t i = 0; i < ring.size(); ++i) ring[i] = std::sin(0.01f * (float)i);

    StreamShifter sh;
    sh.setup(0.5f, grain, (int)(0.012 * sr));
    volatile float sink = 0.0f;
    double a = now_us();
    for (int i = 0; i < n; ++i)
        sink = sink + sh.process(ring.data(), 16383, 20000u + (uint64_t)i);
    const double t_total = now_us() - a;

    a = now_us();
    float acc = 0.0f;
    int cursor = 0;
    for (int i = 0; i < n; ++i) {
        acc += 0.5f * (1.0f - std::cos(6.28318531f * (float)cursor / (float)grain));
        acc += 0.5f * (1.0f - std::cos(6.28318531f
                                       * (float)((cursor + grain / 2) % grain)
                                       / (float)grain));
        if (++cursor >= grain) cursor = 0;
    }
    const double t_cos = now_us() - a;
    sink = sink + acc;

    std::printf("== granular envelope (one voice, %d samples) ==\n", n);
    std::printf("  StreamShifter::process       %8.0f us\n", t_total);
    std::printf("  a std::cos pair, for scale   %8.0f us  (%.0f%% of the above "
                "— what the phasor replaced)\n",
                t_cos, 100.0 * t_cos / t_total);
    std::printf("\n");
}

#ifndef POGGED_NO_VOCODER
// The vocoder's analysis walks every bin computing a magnitude (sqrt) and a
// phase (atan2). Measure that pair against a whole frame's work.
void vocoder_hotspot(double sr)
{
    using V = StreamVocoderT<4096, 4>;
    static V voc;                       // ~0.5 MB: not on the stack
    voc.init(sr, 0);
    voc.set_ratio(0.5f);

    const int n = V::HOP * 200;         // 200 frames
    std::vector<float> ring(16384);
    for (size_t i = 0; i < ring.size(); ++i)
        ring[i] = 0.3f * std::sin(0.011f * (float)i) + 0.2f * std::sin(0.027f * (float)i);

    volatile float sink = 0.0f;
    double a = now_us();
    for (int i = 0; i < n; ++i)
        sink = sink + voc.process(ring.data(), 16383, 20000u + (uint64_t)i);
    const double t_total = now_us() - a;

    std::vector<std::complex<float>> spec((size_t)V::BINS);
    for (int k = 0; k < V::BINS; ++k)
        spec[(size_t)k] = { std::sin(0.3f * (float)k), std::cos(0.7f * (float)k) };
    a = now_us();
    float acc = 0.0f;
    for (int f = 0; f < 200; ++f) {
        // The nudge is what stops the optimiser hoisting the whole inner loop
        // out of the frame loop: without it the spectrum is loop-invariant and
        // 200 frames' worth of transcendentals collapse into one.
        const std::complex<float> nudge((float)f * 1e-3f, 0.0f);
        for (int k = 0; k < V::BINS; ++k) {
            const std::complex<float> z = spec[(size_t)k] + nudge;
            acc += std::abs(z) + std::arg(z);
        }
    }
    const double t_polar = now_us() - a;
    sink = sink + acc;

    std::printf("== vocoder hot spot (StreamVocoderT<4096,4>, 200 frames) ==\n");
    std::printf("  ::process (2 FFTs/frame)     %8.0f us\n", t_total);
    std::printf("  per-bin std::abs + std::arg  %8.0f us  (%.0f%% of it)\n",
                t_polar, 100.0 * t_polar / t_total);
    std::printf("\n");
}
#endif

}  // namespace

int main(int argc, char** argv)
{
    const int    blk = (argc > 1) ? std::atoi(argv[1]) : 64;
    const double sec = (argc > 2) ? std::atof(argv[2]) : 6.0;
    const double sr  = 48000.0;

    std::printf("Pogged CPU profile — %d-sample blocks at %.0f Hz "
                "(deadline %.0f us/block)\n",
                blk, sr, 1e6 * blk / sr);
#ifdef POGGED_DYN_FOCUS
    std::printf("build: POGGED_DYN_FOCUS (hybrid Focus available)\n\n");
#else
    std::printf("build: static Focus (no POGGED_DYN_FOCUS — Focus 2 reads as "
                "Vocoder)\n\n");
#endif

    report_footprint();

    const std::vector<float> in = chord_input(sr, (size_t)(sr * sec));
    std::printf("== pogged_dsp_process, all six voices + detune + spread ==\n");
    run_mode("Focus 0 granular", 0.0f, in, sr, blk);
    run_mode("Focus 1 vocoder",  1.0f, in, sr, blk);
    run_mode("Focus 2 hybrid",   2.0f, in, sr, blk);
    std::printf("\n");

    granular_hotspot(sr);
#ifndef POGGED_NO_VOCODER
    vocoder_hotspot(sr);
#endif
    return 0;
}
