# Pogged

Pogged is a **polyphonic octave generator** for guitar (and other mono
sources), inspired by the Electro-Harmonix POG2: sub octave (−1), sub −2
octaves, octave up (+1), two octaves up (+2), a chorus detune on the up
voices, a pick-triggered attack/swell, and a resonant low-pass filter. It also
takes from the newer POG3 the **fifth-up voice**, **per-voice panning**,
**Spread**, and a **multimode filter with an envelope sweep**.

**Mono in → stereo out.** Each of the six voices has its own pan.

No pitch tracking — the whole polyphonic signal is transposed, so chords work
and the dry path stays at zero latency. Two engines, switchable (Focus):
a streaming **granular** shifter (correlation-aligned grain splicing, 3 ms) and
a streaming **phase vocoder** (per-peak spectral translation, clean on chords,
~85 ms).

- **LV2** — Linux desktop, MOD Audio (Dwarf / Duo X), Raspberry Pi...
- **VST3 / AU / Standalone** — macOS (universal) and Windows, via JUCE

Shares its engineering lineage with [Megalo](https://github.com/pilali/megalo)
(same shared-DSP-core / thin-wrapper architecture, biquad and envelope
components).

## Controls

| Control | Range | What it does |
|---|---|---|
| Input Gain | 0.5–3× | Level seen at the input — it feeds the voices, the dry and the onset detectors, as on the pedal. |
| Dry | 0–200 % | Level of the untouched input (zero latency). |
| Dry: Attack / Filter / Detune | on/off | Route the dry through each effect (POG3). All off = the untouched, undelayed dry that defines a POG. **Dry: Detune** also gates Spread onto the dry, and costs the dry its zero latency — a detuned dry is a shifted dry. |
| Sub Octave | 0–200 % | −1 octave voice. |
| Sub −2 Oct | 0–200 % | −2 octaves voice. |
| 5th Up | 0–200 % | Fifth above (equal-tempered). A POG3 voice. |
| Octave Up | 0–200 % | +1 octave voice. |
| 2 Octaves Up | 0–200 % | +2 octaves voice. |
| Detune | 0–25 cents | Chorus on the +1/+2 voices. Raises the LFO's depth *and* rate together, as on the POG2. |
| Attack | 0–2000 ms | Per-pick volume swell on the wet signal (0 = off). |
| Attack Sens | 0–100 % | Onset-detector sensitivity for the swell trigger. |
| Filter Mode | LP / BP / HP | Multimode resonant filter on the wet mix (POG3). |
| LP Filter | 20 Hz–20 kHz | Filter frequency, in every mode. Bypasses at each mode's transparent end (LP ≥19 kHz, HP ≤21 Hz); band-pass has no off position. Dry is unfiltered. |
| Resonance | Q 0.5–8 | Filter resonance. |
| Filter Env | −100…+100 % | Sweeps the filter frequency on each pick — up (+) or down (−), ±4 octaves at full. Centre = off (POG3). |
| Filter Env Attack | 1–1000 ms | Sweep rise time. |
| Filter Env Decay | 1–2000 ms | Sweep fall time. |
| Filter Env Sens | 0–100 % | Sweep trigger sensitivity — separate from Attack Sens, as on the POG3. |
| Focus | Granular / Vocoder *(/ Hybrid)* | Which transposition engine (POG3's FOCUS). **Granular** is the POG sound and answers in 3 ms, but a chord makes its grain splices cancel unevenly (+4.8 dB of ripple on the sub). **Vocoder** translates each spectral peak independently and is measurably perfect on chords (+0.0 dB over an ideal shift) at ~85 ms of latency. In a `HYBRID=1` build (see Build options) Focus is a three-way selector with **Hybrid** added: granular for the attack, vocoder for the sustained body, so a chord is clean without the vocoder's latency on the pluck. The dry path stays at zero in every case. |
| Range | Guitar / Baritone / Bass | Lowest note the instrument plays. Sizes the sub voices' grains, since a sub emits an octave *below* what you play (a baritone's low B lands the sub at 31 Hz). Longer grains stabilise single low notes but delay the sub and make chords ripple more — hence a switch, not an assumption. |
| Output | 0–200 % | Master output gain into a soft clipper. |
| Pan (×6) | L–C–R | Per-voice placement in the stereo field (dry, −1, −2, +5th, +1, +2). Centre is full level on **both** outputs, so a single output still carries everything. |
| Spread | 0–100 % | POG3 stereo delay on the +5th/+1/+2 voices — right channel 3× longer than left (≤150 ms / ≤50 ms). The sub octaves are excluded, as on the hardware. 0 = off (bit-transparent). |
| Sustain | 0 / 200 ms–5 s | Hands-free infinite sustain (distinct from Freeze). One fader carries both on/off **and** the hold time: **0 = off**; above 0 auto-holds each note (once the vocoder body settles) until the next attack, fading over that release time; the **maximum (5 s) = infinite** (holds until you play again). Works in **Vocoder or Hybrid** Focus (a vocoder must be sounding to freeze). Requires a `HYBRID=1` build. |

## Presets

Nine factory presets, generated from a single source (`tools/gen_presets.py`)
into both the LV2 preset TTLs and the JUCE header, so a program sounds
identical in MOD, a DAW, and standalone.

| Preset | Character |
|---|---|
| Classic POG | Dry + one octave down + one up, the canonical setting. |
| Fat Organ | All voices, detuned, filtered — drawbar organ. |
| 12-String | Dry + a detuned octave up, shimmer only. |
| Sub Bass | Both sub octaves under the dry, filtered low. |
| Slow Cathedral | Up voices with a long attack swell — bowed pad. |
| Resonant Synth | No dry, resonant filter — synth lead. |
| String Machine | Wide detune + medium swell, no dry — ensemble strings. |
| Bass Synth | Subs forward with a resonant filter — synth bass. |
| Quint Organ | Fifth + octaves, the classic drawbar quint. |

## How it works

No pitch tracking. The live input is written to a ring buffer, and each voice
is a 2-tap granular reader running at a fixed ratio (0.5 / 0.25 / 2^(7/12) / 2
/ 4) behind the write head, with correlation-aligned (SOLA-style) grain splicing so
the transposed grains stay phase-coherent on any polyphonic material. Each
voice then passes a fixed voicing filter — subs are gently low-passed to round
them, ups gently high-passed to strip the splice-rate modulation — which is
what pulls the octaves toward the POG2's character instead of sounding like raw
transposed grains. The detuned voices are a second reader per up-voice whose
ratio is swept by an LFO, so the detune is a moving chorus rather than a fixed
interval. The dry path is never delayed. See `docs/lv2-to-multiplatform.md` for
the shared-core architecture.

## Development

```sh
make audit                       # offline DSP regression suite
npm install && npm run screenshot   # regenerate the modgui store images
python3 tools/gen_presets.py     # regenerate LV2 preset TTLs + JUCE header
```

---

## Build LV2 (Linux desktop)

Requires `pkg-config` and the LV2 headers (`lv2-dev`).

```sh
make                       # produces pogged.lv2/pogged.so
sudo make install          # installs to /usr/lib/lv2/pogged.lv2
make audit                 # offline DSP regression suite
```

---

## Build for MOD with mod-plugin-builder

Copy `plugins/package/pogged/` into `mod-plugin-builder/plugins/package/`,
then from the mod-plugin-builder root:

```sh
./build moddwarf-new pogged        # MOD Dwarf (Cortex-A35)
./build modduox-new  pogged        # MOD Duo X / Cortex-A53
```

---

## Build VST3 / AU / Standalone (macOS and Windows)

JUCE project in `juce/`. JUCE is downloaded automatically (FetchContent).

```sh
cmake -B juce/build -S juce -DCMAKE_BUILD_TYPE=Release
cmake --build juce/build --config Release --parallel
```

macOS builds are universal (arm64 + x86_64) by default.

---

## Build options

The `make` build (LV2 desktop and the MOD / Raspberry Pi cross-builds) takes a
`TARGET` and a set of optional tuning flags. The shipped plugin uses the
defaults; the flags let you rebuild the DSP with different latency / quality /
feel trade-offs. They do **not** apply to the JUCE (VST3 / AU) build.

Make tracks the flag signature, so changing a flag forces a rebuild even when no
source file changed.

### Targets

| `TARGET=` | Hardware | Notes |
|---|---|---|
| `native` *(default)* | This machine | Desktop LV2, full engine choice. |
| `rpi5` | Raspberry Pi 5 / pistomp | Cross-compiles `aarch64` (Cortex-A76), static libstdc++/libgcc. |
| `moddwarf-new` | MOD Dwarf (Cortex-A35) | Vocoder compiled out (`POGGED_NO_VOCODER`) — the A35 can't carry it; granular only. |
| `modduox-new` | MOD Duo X (quad Cortex-A53) | Vocoder pinned to a single 2048 window (`POGGED_PV_N=2048`) for CPU. |

### Hybrid Focus — `HYBRID=1`

Enables the POG-class **dynamic Focus**: the granular engine renders the tight
attack (~12–20 ms), the phase vocoder the clean sustained body, crossfaded per
note by an onset detector — the low-latency answer to the vocoder's ~85 ms
"doublon". Off by default. These sub-flags apply only together with `HYBRID=1`:

| Flag | Default | Effect |
|---|---|---|
| `GRAIN_UP=<ms>` | `12` | Up-voice grain length. Shorter = tighter attack, more warble. |
| `GRAIN_PER=<periods>` | `1.5` | Sub-voice grain length, in periods of the sub's *output*. **2.0–3.0** stabilises the low octave; below its ~2-period floor the splice warbles into mush. |
| `PV_OS=<4\|8>` | `4` | Vocoder overlap factor on the up voices. 8 = denser frames, 2× the FFT cost. |
| `HYB_HOLD=<ms>` | `100` | Granular hold after each onset before handing to the vocoder body (covers the vocoder's latency). |
| `HYB_FLOOR=<0..1>` | `0.0` | Granular kept *under* the vocoder body during sustain (0 = pure vocoder body; higher = more granular immediacy, a little more warble). |

### Standalone tuning flags

Work with any target, and — where noted — alongside `HYBRID=1`:

| Flag | Default | Effect |
|---|---|---|
| `GRAIN_UP_PER=<periods>` | `0` *(off)* | Size each **up** voice's grain to span this many periods of its lowest output at the current range, floored at `GRAIN_UP`. Fixes the +1 / fifth warble ("bouillie") on low and baritone notes; `0` keeps the old fixed grain. |
| `HYB_SWELL=1` | off | Makes the **ATTACK** swell operative in hybrid: keys the swell to the granular engine's own onset, so it swells on every pluck like the vocoder's per-bin swell. Reads the hybrid detector only — the hybrid switch itself is untouched. |
| `ANCHOR=<gain>` | `0` *(off)* | Mixes a resynthesised octave-lock anchor under the sub voices to steady the low octave when the input fundamental is weak. Of little benefit for attack-style playing. |
| `XBAND=<1\|hz>` | off | Raises the vocoder crossover so the long 4096 window carries more low-mid and resolves the "dissonant wanderer" harmonic-pair midpoints. `XBAND=1` = 600 Hz; a higher value (e.g. `XBAND=900`) is cleaner but pays the 85 ms window over more of the band. |
| `SUSTAIN_SETTLE=<ms>` | `250` | Delay from the attack to the freeze for the **Sustain** control (runtime), so the capture lands on the note's *body*, past the ~85 ms vocoder latency — not the attack transient. Larger = later, more settled; too large captures a note already decaying. Sustain on/off and its release are the `sustain` / `sustain_ms` ports, not build flags. |

**A full tuned hybrid build** for the Pi 5 (current reference settings):

```sh
make TARGET=rpi5 HYBRID=1 GRAIN_UP=10 GRAIN_PER=2.5 GRAIN_UP_PER=2.2 HYB_SWELL=1
```

## License

GPL-3.0-or-later.
