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
