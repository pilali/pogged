# TARGET: native (default) | moddwarf-new | modduox-new | rpi5
TARGET ?= native

# ── Pitch engine per target ────────────────────────────────────────────────
# FOCUS picks between the granular engine (3 ms, +4.8 dB artifact on a chord)
# and the streaming phase vocoder (85 ms, +0.0 dB — the ideal-shift floor).
# The vocoder costs ~54x real time per voice on x86; the Dwarf's Cortex-A35
# cannot carry that, so it is compiled out there rather than shipped as a
# setting that xruns. Powerful targets keep the full choice.
# NOT yet measured on real MOD hardware — the Duo X setting is a reasoned
# guess (quad A53) and should be confirmed on the device.

# ── Per-target defaults ────────────────────────────────────────────────────
# override is needed so cross-compilation targets win over the environment CXX.
ifeq ($(TARGET),rpi5)
    override CXX      := aarch64-linux-gnu-g++
    # -fno-tree-vectorize avoids emitting calls to libmvec (vectorized math),
    # which isn't packaged on the RPi5 mod/pistomp Buildroot system.
    override CXXFLAGS := -std=c++17 -O3 -ffast-math -funroll-loops \
                         -fno-tree-vectorize \
                         -mcpu=cortex-a76 -march=armv8.2-a \
                         -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
    # Static libstdc++/libgcc: embed the C++ runtime so the .so doesn't depend
    # on the target system's libstdc++ version.
    override LDFLAGS  := -static-libstdc++ -static-libgcc

else ifeq ($(TARGET),moddwarf-new)
    # MOD Dwarf — Cortex-A35, the most constrained target.
    # mod-plugin-builder injects CXX and CXXFLAGS via command-line args.
    # Use ?= so those take precedence; fallbacks serve only for manual builds.
    CXX      ?= aarch64-modaudio-linux-gnu-g++
    CXXFLAGS ?= -std=c++17 -O3 -ffast-math \
                -mcpu=cortex-a35 -DPOGGED_NO_VOCODER \
                -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter

else ifeq ($(TARGET),modduox-new)
    # MOD Duo X — quad Cortex-A53 (ARMv8-A).
    CXX      ?= aarch64-modaudio-linux-gnu-g++
    CXXFLAGS ?= -std=c++17 -O3 -ffast-math \
                -mcpu=cortex-a53 -DPOGGED_PV_N=2048 \
                -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter

else  # native
    CXX      ?= g++
    CXXFLAGS ?= -std=c++17 -O3 -ffast-math \
                -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
endif

# In cross-compilation CXXFLAGS already contains -I$(STAGING_DIR)/usr/include
LV2FLAGS ?= $(shell pkg-config --cflags lv2 2>/dev/null)

BUNDLE  = pogged.lv2
BINARY  = $(BUNDLE)/pogged.so

SOURCES = src/plugin.cpp src/glibc_compat.cpp src/pogged_dsp.cpp
HEADERS = src/pogged_dsp.h src/stream_shifter.hpp src/onset_detector.hpp \
          src/biquad.hpp src/envelope.hpp

all: $(BINARY)

pogged: $(BINARY)

$(BINARY): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(LV2FLAGS) -fPIC -shared -o $@ $(SOURCES) $(LDFLAGS)

clean:
	rm -f $(BINARY)
	rm -rf build/audit

# ── Audit: objective sound-quality regression suite ─────────────────────────
# Offline harnesses against the real DSP core; each prints its measurements
# and exits non-zero on regression:
#   shift_test — octave voices produce the target pitch (Goertzel), sub
#                splice tremolo bounded
#   swell_test  — attack envelope timing + re-pick re-swell
#   click_test  — level-step clicks bounded
#   detune_test — detune sweeps (chorus) rather than sitting at a fixed offset
#   pan_test    — mono->stereo path + per-voice pan law
#   spread_test — POG3 SPREAD: R delayed 3x L, suboctaves excluded
#   filter_test — LP/BP/HP modes + envelope sweep
#   range_test  — guitar/baritone/bass sub sizing + the known chord ripple
#   focus_test  — FOCUS engine switch: vocoder hits the ideal floor, no click
#   dry_test    — input gain + the three DRY routing buttons
#   stagger_test— the vocoder's staggered hop phase does not move a voice's
#                 latency (the invariant that lets the peak load be spread)
#   warp_test   — POG3 WARP: the bend hits its pitch on every voice, spares the
#                 dry, keeps warp=0 bit-identical, and grows the lag budget
AUDIT_DIR   = build/audit
AUDIT_FLAGS = -O2 -std=c++17 -Isrc

audit: $(HEADERS)
	@mkdir -p $(AUDIT_DIR)
	$(CXX) $(AUDIT_FLAGS) tools/shift_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/shift_test
	$(CXX) $(AUDIT_FLAGS) tools/swell_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/swell_test
	$(CXX) $(AUDIT_FLAGS) tools/click_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/click_test
	$(CXX) $(AUDIT_FLAGS) tools/detune_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/detune_test
	$(CXX) $(AUDIT_FLAGS) tools/pan_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/pan_test
	$(CXX) $(AUDIT_FLAGS) tools/spread_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/spread_test
	$(CXX) $(AUDIT_FLAGS) tools/filter_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/filter_test
	$(CXX) $(AUDIT_FLAGS) tools/range_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/range_test
	$(CXX) $(AUDIT_FLAGS) tools/focus_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/focus_test
	$(CXX) $(AUDIT_FLAGS) tools/dry_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/dry_test
	$(CXX) $(AUDIT_FLAGS) tools/stagger_test.cpp -o $(AUDIT_DIR)/stagger_test
	$(CXX) $(AUDIT_FLAGS) tools/warp_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/warp_test
	@echo "══ pitch content ══";  $(AUDIT_DIR)/shift_test
	@echo "══ attack swell ══";   $(AUDIT_DIR)/swell_test
	@echo "══ level clicks ══";   $(AUDIT_DIR)/click_test
	@$(AUDIT_DIR)/detune_test
	@$(AUDIT_DIR)/pan_test
	@$(AUDIT_DIR)/spread_test
	@$(AUDIT_DIR)/filter_test
	@$(AUDIT_DIR)/range_test
	@$(AUDIT_DIR)/focus_test
	@$(AUDIT_DIR)/dry_test
	@$(AUDIT_DIR)/stagger_test
	@$(AUDIT_DIR)/warp_test
	@echo "AUDIT OK"

.PHONY: audit

install: $(BINARY)
	install -d $(DESTDIR)/usr/lib/lv2
	cp -r $(BUNDLE) $(DESTDIR)/usr/lib/lv2/

.PHONY: all pogged clean install
