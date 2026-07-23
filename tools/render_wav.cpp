// render_wav — audible A/B: run a realistic played passage through each engine
// and write one WAV per engine, so the spike can be judged by EAR, not only in
// dB. Not part of `make audit` (it writes files); run it by hand:
//
//   g++ -O2 -std=c++17 -Isrc tools/render_wav.cpp src/pogged_dsp.cpp -o build/render_wav
//   build/render_wav build/renders            # writes dry/granular/vocoder/filterbank
//                                             #        + swell_[mix_]global/polyphonic
//
// The passage is two things the engines find hard, back to back:
//   1. a fingerstyle ARPEGGIO — an E-major shape plucked note by note, each note
//      ringing into the next. This is the §1.1 test made audible: does a new
//      pluck disturb the notes already sounding?
//   2. a strummed CHORD held ~3 s — the polyphonic ripple test made audible.
//
// Every engine is rendered at the SUB (-1 oct) voice, the hardest one, off a
// private ring exactly as the audit tools drive them. Each output is peak-
// normalised so loudness does not confound the comparison — this deliberately
// hides the filter bank's static colouration (follow-up #1) to isolate the
// ARTEFACTS (ripple, pumping, disturbance) that are what we are listening for.
//
// The swell pair (§14) renders the WHOLE plugin (wet-only +1 oct, ATTACK
// 500 ms) on the same arpeggio: swell_global is the granular engine's single
// wet-bus envelope (every pluck ducks the notes still ringing), swell_
// polyphonic the vocoder's per-bin swell (each pluck fades in on its own,
// the ringing notes hold).
#include "../src/stream_filterbank.hpp"
#include "../src/stream_shifter.hpp"
#include "../src/stream_vocoder.hpp"
#include "../src/pogged_dsp.h"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static constexpr float    SR   = 48000.0f;
static constexpr float    RATIO = 0.5f;                 // sub, -1 octave
static constexpr uint32_t RSZ  = 65536, MASK = RSZ - 1;

// One plucked string: harmonic stack with a soft attack and exponential decay,
// added into buf starting at t0 (seconds).
static void pluck(std::vector<float>& buf, double f0, double t0, double dur,
                  double gain, double decay = 4.5)      // decay per second
{
    const int start = (int)(t0 * SR), n = (int)(dur * SR);
    for (int i = 0; i < n && start + i < (int)buf.size(); ++i) {
        const double t = i / (double)SR;
        const double env = (1.0 - std::exp(-t / 0.004)) * std::exp(-decay * t);
        double s = 0.0;
        for (int h = 1; h <= 8; ++h)
            s += (1.0 / h) * std::sin(2.0 * M_PI * f0 * h * t);
        buf[start + i] += (float)(gain * env * s);
    }
}

// Minimal 16-bit mono PCM WAV writer.
static bool write_wav(const std::string& path, const std::vector<float>& x)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t sr = (uint32_t)SR, n = (uint32_t)x.size();
    const uint32_t data = n * 2, riff = 36 + data;
    const uint16_t ch = 1, bits = 16, ba = 2;
    const uint32_t bps = sr * ba;
    auto w32 = [&](uint32_t v){ std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v){ std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(riff); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(ch);
    w32(sr); w32(bps); w16(ba); w16(bits);
    std::fwrite("data", 1, 4, f); w32(data);
    for (float v : x) {
        int s = (int)std::lround(v * 32767.0f);
        s = s > 32767 ? 32767 : (s < -32768 ? -32768 : s);
        w16((uint16_t)(int16_t)s);
    }
    std::fclose(f);
    return true;
}

static void normalize(std::vector<float>& x, float peak = 0.7f)
{
    float m = 1e-9f;
    for (float v : x) m = std::max(m, std::fabs(v));
    const float g = peak / m;
    for (float& v : x) v *= g;
}

template <class Engine>
static std::vector<float> run(const std::vector<float>& in)
{
    static Engine eng;
    eng.init(SR); eng.set_ratio(RATIO); eng.reset();
    std::vector<float> ring(RSZ, 0.0f), out(in.size(), 0.0f);
    uint64_t wpos = RSZ;
    for (size_t i = 0; i < in.size(); ++i) {
        ring[wpos & MASK] = in[i];
        ++wpos;
        out[i] = eng.process(ring.data(), MASK, wpos);
    }
    return out;
}

static std::vector<float> run_granular(const std::vector<float>& in)
{
    static StreamShifter sh;
    sh.setup(RATIO, (int)(0.055f * SR), 512);
    sh.reset();
    std::vector<float> ring(RSZ, 0.0f), out(in.size(), 0.0f);
    uint64_t wpos = RSZ;
    for (size_t i = 0; i < in.size(); ++i) {
        ring[wpos & MASK] = in[i];
        ++wpos;
        out[i] = sh.process(ring.data(), MASK, wpos);
    }
    return out;
}

// Whole-plugin render for the ATTACK swell A/B (§14): 500 ms swell, engine
// picked by `focus`, voice mix by dry/sub1/up1.
static std::vector<float> run_plugin_swell(const std::vector<float>& in, float focus,
                                           float dry, float sub1, float up1)
{
    PoggedDsp* dsp = pogged_dsp_new(SR);
    PoggedParams p = {};
    p.dry_level   = dry;
    p.sub1_level  = sub1;
    p.up1_level   = up1;
    // Below the output soft-clip's knee even on the full mix (the renders are
    // peak-normalised afterwards, so this only buys linearity, not loudness).
    p.out_level   = 0.4f;
    p.input_gain  = 1.0f;
    p.lp_cutoff   = 20000.0f;
    p.lp_q        = 0.707f;
    p.attack_ms   = 500.0f;
    p.attack_sens = 0.9f;   // fingerstyle over ringing notes needs a hot trigger
    p.focus       = focus;

    const int n = (int)in.size();
    std::vector<float> out(n), out_r(n);
    constexpr int BLOCK = 256;
    for (int i = 0; i < n; i += BLOCK)
        pogged_dsp_process(dsp, &p, in.data() + i, out.data() + i, out_r.data() + i,
                           std::min<int>(BLOCK, n - i));
    pogged_dsp_free(dsp);
    return out;
}

int main(int argc, char** argv)
{
    const std::string dir = (argc > 1) ? argv[1] : "build/renders";

    const int total = (int)(8.5 * SR);
    std::vector<float> in(total, 0.0f);

    // 1. Fingerstyle arpeggio, E major (E2 B2 E3 G#3), plucked 0.45 s apart,
    //    each ringing ~2.6 s so they overlap — the §1.1 material.
    const double arp[4] = { 82.41, 123.47, 164.81, 207.65 };
    for (int k = 0; k < 4; ++k)
        pluck(in, arp[k], 0.2 + 0.45 * k, 2.6, 0.8);

    // 2. Strummed chord at 4.6 s, notes staggered 12 ms, held ~3 s — the
    //    polyphonic ripple material.
    for (int k = 0; k < 4; ++k)
        pluck(in, arp[k], 4.6 + 0.012 * k, 3.2, 0.8);

    normalize(in);

    struct Out { const char* name; std::vector<float> y; };
    std::vector<Out> outs = {
        { "dry",        in },
        { "granular",   run_granular(in) },
        { "vocoder",    run<StreamVocoder>(in) },
        { "filterbank", run<StreamFilterbank>(in) },
    };

    // The swell A/B gets a more SUSTAINED arpeggio (decay 1.2/s instead of
    // 4.5): the criterion is what happens to notes still ringing when the
    // next one is attacked, so the notes have to still be ringing.
    std::vector<float> in_swell((int)(6.0 * SR), 0.0f);
    for (int k = 0; k < 4; ++k)
        pluck(in_swell, arp[k], 0.3 + 0.8 * k, 4.5, 0.8, 1.2);
    normalize(in_swell);
    // Wet-only +1 octave: the swell in isolation…
    outs.push_back({ "swell_global",     run_plugin_swell(in_swell, 0.0f, 0, 0, 1) });  // granular, POG2 env
    outs.push_back({ "swell_polyphonic", run_plugin_swell(in_swell, 1.0f, 0, 0, 1) });  // vocoder, per-bin
    // …and the Classic-POG mix (dry + sub1 + up1): the dry attacks land
    // immediately while both octaves swell around them — or, in the global
    // version, duck the octaves of every note still ringing.
    outs.push_back({ "swell_mix_global",     run_plugin_swell(in_swell, 0.0f, 1, 1, 1) });
    outs.push_back({ "swell_mix_polyphonic", run_plugin_swell(in_swell, 1.0f, 1, 1, 1) });

    bool ok = true;
    for (auto& o : outs) {
        normalize(o.y);
        const std::string path = dir + "/" + o.name + ".wav";
        if (write_wav(path, o.y)) std::printf("  wrote %s\n", path.c_str());
        else { std::printf("  FAILED %s\n", path.c_str()); ok = false; }
    }
    std::printf("render_wav: %s\n", ok ? "OK" : "FAILED (dir must exist)");
    return ok ? 0 : 1;
}
