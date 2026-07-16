# Pogged

Pogged is a **polyphonic octave generator** for guitar (and other mono
sources), inspired by the Electro-Harmonix POG2: sub octave (−1), sub −2
octaves, octave up (+1), two octaves up (+2), detune on the up voices, a
pick-triggered attack/swell, and a resonant low-pass filter.

No pitch tracking — the whole polyphonic signal is transposed by a streaming
granular engine (correlation-aligned grain splicing), so chords work and the
dry path stays at zero latency.

- **LV2** — Linux desktop, MOD Audio (Dwarf / Duo X), Raspberry Pi...
- **VST3 / AU / Standalone** — macOS (universal) and Windows, via JUCE

Shares its engineering lineage with [Megalo](https://github.com/pilali/megalo)
(same shared-DSP-core / thin-wrapper architecture, biquad and envelope
components).

## Controls

| Control | Range | What it does |
|---|---|---|
| Dry | 0–200 % | Level of the untouched input (zero latency). |
| Sub Octave | 0–200 % | −1 octave voice. |
| Sub −2 Oct | 0–200 % | −2 octaves voice. |
| Octave Up | 0–200 % | +1 octave voice. |
| 2 Octaves Up | 0–200 % | +2 octaves voice. |
| Detune | 0–25 cents | Static chorus detune on the two up voices. |
| Attack | 0–2000 ms | Per-pick volume swell on the wet signal (0 = off). |
| Attack Sens | 0–100 % | Onset-detector sensitivity for the swell trigger. |
| LP Filter | 20 Hz–20 kHz | Low-pass on the wet mix (≥19 kHz = bypass). Dry is unfiltered. |
| Resonance | Q 0.5–8 | Filter resonance. |
| Output | 0–200 % | Master output gain into a soft clipper. |

## Presets

Eight factory presets, mirroring the POG2's eight slots. They are generated
from a single source (`tools/gen_presets.py`) into both the LV2 preset TTLs and
the JUCE header, so a program sounds identical in MOD, a DAW, and standalone.

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

## How it works

No pitch tracking. The live input is written to a ring buffer, and each octave
voice is a 2-tap granular reader running at a fixed ratio (0.5 / 0.25 / 2 / 4)
behind the write head, with correlation-aligned (SOLA-style) grain splicing so
the transposed grains stay phase-coherent on any polyphonic material. Each
voice then passes a fixed voicing filter — subs are gently low-passed to round
them, ups gently high-passed to strip the splice-rate modulation — which is
what pulls the octaves toward the POG2's character instead of sounding like raw
transposed grains. The dry path is never delayed. See
`docs/lv2-to-multiplatform.md` for the shared-core architecture.

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

## License

GPL-3.0-or-later.
