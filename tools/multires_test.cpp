// multires_test — the multi-resolution vocoder (design §13) must keep the LONG
// window's bass resolution while running the short window for the treble.
//
// The single short window (N=2048) tightens attacks but blurs a low chord's sub;
// the long window (N=4096) resolves it but is late everywhere. MultiVocoder
// crosses the two over (long→bass, short→treble), so on a low chord it must
// resolve as well as the long window alone — measurably better than the short
// one. It must also still shift pitch correctly. (Attack tightness is the point
// too, but rise-time is a weak proxy for the felt dry↔voice offset — that is
// judged by ear, in the renders; here we pin the bass-resolution guarantee that
// makes the multi-res worth its extra cost.)
//
// Driven standalone off a shared ring, as the other engine tests are.
#include "../src/stream_multivocoder.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float    SR   = 48000.0f;
static constexpr int      N    = (int)SR * 3;
static constexpr uint32_t RSZ  = 65536, MASK = RSZ - 1;

static double goertzel(const std::vector<float>& x, int s, int len, float f)
{
    const double w = 2.0 * M_PI * f / SR, cw = std::cos(w), sw = std::sin(w), c = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (int n = 0; n < len; ++n) { const double s0 = x[s + n] + c * s1 - s2; s2 = s1; s1 = s0; }
    return 2.0 * std::sqrt((s1 - s2 * cw) * (s1 - s2 * cw) + (s2 * sw) * (s2 * sw)) / len;
}

static double ripple(const std::vector<float>& y)
{
    const int win = (int)(0.150f * SR), hop = (int)(0.010f * SR);
    double rmin = 1e30, rmax = 0.0;
    for (int i = (int)(0.4f * SR); i + win < N; i += hop) {
        double e = 0.0;
        for (int j = 0; j < win; ++j) e += (double)y[i + j] * y[i + j];
        const double r = std::sqrt(e / win);
        rmin = std::min(rmin, r); rmax = std::max(rmax, r);
    }
    return 20.0 * std::log10(rmax / (rmin + 1e-30));
}

template <class E>
static std::vector<float> run(const std::vector<float>& in, float ratio)
{
    static E e; e.init(SR); e.set_ratio(ratio); e.reset();
    std::vector<float> ring(RSZ, 0.0f), out(in.size(), 0.0f);
    uint64_t wpos = RSZ;
    for (size_t i = 0; i < in.size(); ++i) {
        ring[wpos & MASK] = in[i]; ++wpos;
        out[i] = e.process(ring.data(), MASK, wpos);
    }
    return out;
}

int main()
{
    std::printf("══ multi-resolution vocoder (§13) ══\n");
    bool ok = true;

    // 1. Pitch still correct through the crossover: 220 Hz -> 110 / 440.
    {
        std::vector<float> sine(N);
        for (int i = 0; i < N; ++i) sine[i] = 0.5f * std::sin(2 * M_PI * 220.0 * i / SR);
        const int s = (int)SR, len = (int)(1.5f * SR);
        for (auto c : { std::pair<float,float>{0.5f,110.0f}, {2.0f,440.0f} }) {
            auto y = run<MultiVocoder<4096,2048>>(sine, c.first);
            const double at = goertzel(y, s, len, c.second), in_ = goertzel(y, s, len, 220.0f);
            const bool dom = at > 3.0 * in_;
            std::printf("  220 Hz x%.2f -> %.0f Hz dominant (%.3f vs input %.3f)  %s\n",
                        c.first, c.second, at, in_, dom ? "ok" : "WRONG");
            ok &= dom;
        }
    }

    // 2. Low chord sub: the multi-res must resolve like the LONG window, not
    //    blur like the short one. Low E major triad, sub -1 oct.
    {
        std::vector<float> ch(N, 0.0f);
        const double f[3] = { 82.41, 103.83, 123.47 };
        for (int i = 0; i < N; ++i) {
            const double t = (double)i / SR; double c = 0.0;
            for (double fk : f) c += std::sin(2 * M_PI * fk * t);
            ch[i] = 0.22f * (float)c;
        }
        const double r4096 = ripple(run<StreamVocoderT<4096>>(ch, 0.5f));
        const double r2048 = ripple(run<StreamVocoderT<2048>>(ch, 0.5f));
        const double rmul  = ripple(run<MultiVocoder<4096,2048>>(ch, 0.5f));
        std::printf("  low E triad sub -1 ripple: 4096 %.1f, 2048 %.1f, multi %.1f dB\n",
                    r4096, r2048, rmul);
        // The multi-res keeps the long window's bass resolution: at least as
        // clean as the short window, and within 0.5 dB of the long one.
        const bool resolved = rmul <= r2048 + 0.05 && rmul <= r4096 + 0.5;
        std::printf("  -> multi resolves the bass like the long window (<= 2048, ~4096)  %s\n",
                    resolved ? "ok" : "WRONG");
        ok &= resolved;
    }

    std::printf("multires_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
