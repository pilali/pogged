# Pogged

Pogged is a **polyphonic octave generator** for guitar (and other mono
sources), inspired by the Electro-Harmonix POG2: sub octave (−1), sub −2
octaves, octave up (+1), two octaves up (+2), a chorus detune on the up
voices, a pick-triggered attack/swell, and a resonant low-pass filter. It also
takes from the newer POG3 the **fifth-up voice**, **per-voice panning**,
**Spread**, and a **multimode filter with an envelope sweep**.

**Mono in → stereo out.** Each of the six voices has its own pan.

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
| Range | Guitar / Baritone / Bass | Lowest note the instrument plays. Sizes the sub voices' grains, since a sub emits an octave *below* what you play (a baritone's low B lands the sub at 31 Hz). Longer grains stabilise single low notes but delay the sub and make chords ripple more — hence a switch, not an assumption. |
| Output | 0–200 % | Master output gain into a soft clipper. |
| Pan (×6) | L–C–R | Per-voice placement in the stereo field (dry, −1, −2, +5th, +1, +2). Centre is full level on **both** outputs, so a single output still carries everything. |
| Spread | 0–100 % | POG3 stereo delay on the +5th/+1/+2 voices — right channel 3× longer than left (≤150 ms / ≤50 ms). The sub octaves are excluded, as on the hardware. 0 = off (bit-transparent). |

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

## License

GPL-3.0-or-later.
