#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

// Small fractional delay line, for the POG3 SPREAD effect.
//
// Deliberately trivial: a power-of-2 ring, write-then-read, linear
// interpolation. Delay 0 returns the sample just written *exactly* (frac == 0
// selects the newest tap with no arithmetic), so SPREAD at zero is bit
// transparent rather than merely close — a delay line in the signal path must
// not colour the sound when the user has it switched off.
//
// The buffer is sized once by init() (off the audio thread); process() never
// allocates.
class DelayLine {
public:
    // max_samples: longest delay ever requested; rounded up to a power of 2.
    void init(int max_samples) {
        uint32_t n = 1;
        while (n < (uint32_t)(max_samples + 4)) n <<= 1;
        _buf.assign(n, 0.0f);
        _mask = n - 1;
        _w    = 0;
    }

    void reset() noexcept {
        std::fill(_buf.begin(), _buf.end(), 0.0f);
        _w = 0;
    }

    void write(float x) noexcept {
        _buf[_w & _mask] = x;
        ++_w;
    }

    // delay: in samples, 0 = the sample just written. Clamped to the buffer.
    float read(float delay) const noexcept {
        if (delay <= 0.0f) return _buf[(_w - 1) & _mask];
        const float d = std::min(delay, (float)(_mask - 2));
        const uint32_t i = (uint32_t)d;
        const float frac = d - (float)i;
        const float a = _buf[(_w - 1 - i) & _mask];
        const float b = _buf[(_w - 2 - i) & _mask];
        return a + frac * (b - a);
    }

private:
    std::vector<float> _buf;
    uint32_t _mask = 0;
    uint32_t _w    = 0;
};
