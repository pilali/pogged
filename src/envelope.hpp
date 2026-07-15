#pragma once
#include <cmath>

// One-pole exponential ADSR applied to the freeze layer.
// Times are in milliseconds. Triggered by onset detection; release
// fires automatically when a new onset arrives (see plugin.cpp).
class Envelope {
public:
    enum class State { Idle, Attack, Decay, Sustain, Release };

    void set(float atk_ms, float dcy_ms, float sustain, float rel_ms, float sr) noexcept {
        _sustain = sustain;
        // coeff = exp(-1 / tau_samples) — reaches ~63% of target per tau
        // We use log(9) so the envelope covers 90% of its range in the specified time.
        auto coeff = [&](float ms) -> float {
            float samples = ms * 0.001f * sr;
            return (samples > 1.0f) ? std::exp(-std::log(9.0f) / samples) : 0.0f;
        };
        _atk_c = coeff(atk_ms);
        _dcy_c = coeff(dcy_ms);
        _rel_c = coeff(rel_ms);
    }

    void trigger() noexcept { _state = State::Attack; _rel_cap_c = 1.0f; }
    void release() noexcept { if (_state != State::Idle) _state = State::Release; }
    void reset()   noexcept { _state = State::Idle; _level = 0.0f; }

    // Release, but guarantee ~90% of the decay happens within cap_ms even if
    // the user's release time is longer. Used when the pad is known to be
    // REPLACED shortly (the freeze buffer swap at LoopReady is instantaneous,
    // so any level still present at that moment becomes an audible click).
    void release_capped(float cap_ms, float sr) noexcept {
        release();
        const float samples = cap_ms * 0.001f * sr;
        _rel_cap_c = (samples > 1.0f) ? std::exp(-std::log(9.0f) / samples) : 0.0f;
    }

    float process() noexcept {
        switch (_state) {
        case State::Attack:
            _level = 1.0f - _atk_c * (1.0f - _level);
            if (_level >= 0.999f) { _level = 1.0f; _state = State::Decay; }
            break;

        case State::Decay:
            _level = _sustain + _dcy_c * (_level - _sustain);
            if (_level <= _sustain + 0.001f) { _level = _sustain; _state = State::Sustain; }
            break;

        case State::Sustain:
            _level = _sustain;
            break;

        case State::Release:
            // The cap (see release_capped) can only make the decay faster.
            _level *= std::min(_rel_c, _rel_cap_c);
            if (_level < 0.0001f) { _level = 0.0f; _state = State::Idle; }
            break;

        case State::Idle:
            _level = 0.0f;
            break;
        }
        return _level;
    }

    bool is_active() const noexcept { return _state != State::Idle; }

private:
    State _state   = State::Idle;
    float _level   = 0.0f;
    float _sustain = 1.0f;
    float _atk_c   = 0.0f;
    float _dcy_c   = 0.0f;
    float _rel_c   = 0.0f;
    float _rel_cap_c = 1.0f;   // release_capped() override; 1 = no cap
};
