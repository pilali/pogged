#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// POG3 FREEZE+GLISS — holds the sound under the octaves indefinitely.
//
// The trick is that this touches NEITHER the shifters NOR the vocoder: it
// replaces what gets WRITTEN INTO THE RING, so every voice goes on reading a
// live signal — the live signal is now a loop. Nothing else in the engine has
// to know that time has stopped. The dry is tapped from the input directly and
// never from the ring, so it stays live for free, which is exactly what the
// manual asks for: "the frozen sound sustains indefinitely, and you can play
// the dry voice over it".
//
// Capture is a window of the ring's own history, so the freeze is of "the
// sound you hear at the moment you move the pedal" — no forward recording, no
// gap. It is looped with a crossfaded seam (the tail is blended back into the
// head) so a held chord sustains without a tick at the join.
//
// GLISSANDO: on the pedal, going back to the heel and up again captures the
// next note and "slides from one frozen note to the next", the pedal's
// position setting the rate. Here that transition is a CROSSFADE between the
// old loop and the new one, whose length the position sets — NOT a pitch
// portamento. Sliding the pitch would mean knowing the interval between the
// two frozen chords, i.e. tracking their pitch, which this engine is built on
// not doing (and a chord has no single pitch to slide anyway). So this is a
// morph where the pedal glides, and it is the one place where the result is
// an interpretation rather than a reproduction.
class FreezeLoop {
public:
    static constexpr float LOOP_MS = 320.0f;   // captured window
    static constexpr float SEAM_MS = 64.0f;    // blended back into the head

    // Allocates: call from the constructor path, never from process().
    void init(double sr) noexcept {
        _len    = (int)(LOOP_MS * 0.001 * sr);
        _seam   = (int)(SEAM_MS * 0.001 * sr);
        _period = _len - _seam;
        for (auto& b : _buf) b.assign((size_t)_len, 0.0f);
        reset();
    }

    void reset() noexcept {
        _a = 0; _b = -1; _mix = 0.0f; _step = 0.0f;
        _pos[0] = _pos[1] = 0;
        _have = false;
    }

    // True once something has been captured; before that there is nothing to
    // hold and process() would only emit silence.
    bool armed() const noexcept { return _have; }

    // Capture the newest LOOP_MS behind wpos and start gliding to it over
    // `glide` samples. Copies out of the ring — after this the ring may be fed
    // the loop without eating its own tail.
    void capture(const float* ring, uint32_t mask, uint64_t wpos,
                 int glide) noexcept
    {
        // A capture landing mid-glide: settle on the note being glided to
        // rather than dropping it, or a quick heel-toe-heel-toe would leave a
        // half-faded chord behind.
        if (_b >= 0) { _a = _b; _b = -1; _mix = 0.0f; }

        const int slot = _have ? (1 - _a) : 0;
        const uint64_t start = wpos - (uint64_t)_len;
        for (int i = 0; i < _len; ++i)
            _buf[slot][(size_t)i] = ring[(start + (uint64_t)i) & mask];
        _pos[slot] = 0;

        if (!_have) {                 // first freeze: nothing to glide FROM
            _a = 0; _have = true; return;
        }
        _b    = slot;
        _mix  = 0.0f;
        _step = 1.0f / (float)std::max(1, glide);
    }

    // One sample of the frozen audio. Call once per output sample.
    float process() noexcept {
        if (!_have) return 0.0f;
        float out = _read(_a);
        if (_b >= 0) {
            out = out * (1.0f - _mix) + _read(_b) * _mix;
            _mix += _step;
            if (_mix >= 1.0f) { _a = _b; _b = -1; _mix = 0.0f; }
        }
        return out;
    }

private:
    // Crossfaded loop read: the first _seam samples blend the buffer's tail
    // (which sits _period later) into its head, so the seam is inaudible. The
    // loop's true period is therefore _period, not _len.
    float _read(int s) noexcept {
        const int p = _pos[s];
        float v;
        if (p < _seam) {
            const float t = (float)p / (float)_seam;
            v = _buf[s][(size_t)p] * t
              + _buf[s][(size_t)(p + _period)] * (1.0f - t);
        } else {
            v = _buf[s][(size_t)p];
        }
        if (++_pos[s] >= _period) _pos[s] = 0;
        return v;
    }

    std::vector<float> _buf[2];
    int   _len = 0, _seam = 0, _period = 0;
    int   _pos[2] = { 0, 0 };
    int   _a = 0;        // slot currently sounding
    int   _b = -1;       // slot being glided to, -1 = none
    float _mix = 0.0f, _step = 0.0f;
    bool  _have = false;
};
