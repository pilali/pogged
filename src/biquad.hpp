#pragma once
#include <cmath>
#include <algorithm>

// RBJ cookbook biquad — LP / HP / BP
class Biquad {
public:
    enum Type { LP = 0, HP = 1, BP = 2 };

    void setup(Type type, float freq, float q, float sr) {
        float w0    = 2.0f * float(M_PI) * freq / sr;
        float cw    = std::cos(w0);
        float sw    = std::sin(w0);
        float alpha = sw / (2.0f * q);
        float norm  = 1.0f + alpha;

        _a1 = -2.0f * cw / norm;
        _a2 = (1.0f - alpha) / norm;

        switch (type) {
        case LP:
            _b0 = (1.0f - cw) / (2.0f * norm);
            _b1 = (1.0f - cw) / norm;
            _b2 = _b0;
            break;
        case HP:
            _b0 = (1.0f + cw) / (2.0f * norm);
            _b1 = -(1.0f + cw) / norm;
            _b2 = _b0;
            break;
        case BP:
            // Constant 0 dB PEAK gain (RBJ "BPF, constant peak"): the Q knob
            // changes the bandwidth, not the level. The previous constant-
            // skirt form had a peak gain equal to Q — +20 dB at Q = 10,
            // driving the output clipper.
            _b0 =  alpha / norm;
            _b1 =  0.0f;
            _b2 = -_b0;
            break;
        }
    }

    float process(float x) noexcept {
        float y = _b0*x + _b1*_x1 + _b2*_x2 - _a1*_y1 - _a2*_y2;
        _x2 = _x1;  _x1 = x;
        _y2 = _y1;  _y1 = y;
        return y;
    }

    void reset() noexcept { _x1 = _x2 = _y1 = _y2 = 0.0f; }

private:
    float _b0 = 1.0f, _b1 = 0.0f, _b2 = 0.0f;
    float _a1 = 0.0f, _a2 = 0.0f;
    float _x1 = 0.0f, _x2 = 0.0f;
    float _y1 = 0.0f, _y2 = 0.0f;
};
