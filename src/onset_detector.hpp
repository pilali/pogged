#pragma once
#include <cmath>

// Pick-attack detector for the POG2-style swell — the fast/slow RMS-ratio
// machinery extracted from Megalo's FreezeEngine (freeze_engine.hpp), minus
// the capture state machine. Fires once per attack, with hysteresis re-arm
// and a refractory hold so one pick cannot double-trigger.
class OnsetDetector {
public:
    // Absolute gate: the RMS ratio is level-independent, so in near-silence
    // the tiniest click can trip it. Require fast RMS (power) > ~-55 dBFS.
    static constexpr float ABS_GATE = 3.2e-6f;

    void init(float sr) noexcept {
        _fast_c     = 1.0f - std::exp(-1.0f / (0.002f * sr));
        _slow_c     = 1.0f - std::exp(-1.0f / (0.200f * sr));
        _refractory = (int)(0.080f * sr);
        reset();
    }

    void reset() noexcept {
        _rms_fast = 0.0f;
        _rms_slow = 1e-10f;
        _armed    = true;
        _hold     = 0;
    }

    // sens [0..1]: higher = more sensitive (lower threshold). Returns true
    // on the sample where an onset fires.
    bool process(float x, float sens) noexcept {
        const float x2 = x * x;
        _rms_fast += _fast_c * (x2 - _rms_fast);
        _rms_slow += _slow_c * (x2 - _rms_slow);

        if (_hold > 0) { --_hold; return false; }

        const float ratio      = _rms_fast / (_rms_slow + 1e-10f);
        const float thresh_low = 1.5f + (1.0f - sens) * 13.5f;
        const float thresh_hi  = thresh_low * 1.3f;

        bool fired = false;
        if (_armed && ratio > thresh_hi && _rms_fast > ABS_GATE) {
            _armed = false;
            _hold  = _refractory;
            fired  = true;
        }
        if (!_armed && ratio < thresh_low)
            _armed = true;
        return fired;
    }

    // Smoothed input power (fast window) — used for the silence release.
    float fast_power() const noexcept { return _rms_fast; }

private:
    float _fast_c = 0.0f, _slow_c = 0.0f;
    float _rms_fast = 0.0f, _rms_slow = 1e-10f;
    int   _refractory = 0, _hold = 0;
    bool  _armed = true;
};
