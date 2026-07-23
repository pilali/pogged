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
    # The bench is a standalone executable copied onto the device: fully
    # static, so it runs on the Buildroot system regardless of its glibc.
    BENCH_LDFLAGS     := -static

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

# §25 experiment: XBAND raises the vocoder crossover from the default 250, so
# the 4096 long window carries more of the low-mid and resolves the harmonic
# pairs whose midpoints are the "dissonant wanderer" parasites. It is a
# PURITY/LATENCY DIAL: XBAND=1 is the §25 default 600 (kills the h2 midpoint
# ~498 Hz); XBAND=<hz> sets any crossover (e.g. XBAND=900 also kills the h3
# midpoint ~745 Hz, cleaner but more of the band is at the 85 ms window).
# Off by default. Works with any TARGET, e.g. `make TARGET=rpi5 XBAND=900`.
ifdef XBAND
    override CXXFLAGS += -DPOGGED_XBAND
    ifneq ($(XBAND),1)
        override CXXFLAGS += -DPOGGED_XOVER=$(XBAND)
    endif
endif

# §30 experiment: HYBRID=1 enables the dynamic-Focus POG-class hybrid — the
# granular engine renders the tight ATTACK (low latency), the vocoder the clean
# sustained BODY, crossfaded per note by the onset detector. This is the fix for
# the ~81 ms "doublon" the vocoder alone leaves under the zero-latency dry.
# HYBRID=1 also shortens the grains (12 ms up / 1.5 periods down) since the
# granular only carries the brief attack, pulling the attack latency toward the
# POG's ~12-20 ms. Off by default. Works with any TARGET, e.g.
# `make TARGET=rpi5 HYBRID=1`. Override grains: `make HYBRID=1 GRAIN_UP=10 GRAIN_PER=1.4`.
ifdef HYBRID
    override CXXFLAGS += -DPOGGED_DYN_FOCUS
    GRAIN_UP  ?= 12
    # Sub-voice grain length in periods. 3.0 (validated by ear) holds the low
    # octave stably; drop it (e.g. GRAIN_PER=2.0) for a tighter, slightly less
    # stable sub. Below ~2 it warbles.
    GRAIN_PER ?= 3.0
    override CXXFLAGS += -DPOGGED_GRAIN_UP_MS=$(GRAIN_UP).0f -DPOGGED_GRAIN_PERIODS=$(GRAIN_PER)f
    # §30 CPU: the hybrid runs both engines, so trim the vocoder to fit tight
    # JACK buffers (64). OS=4 on the up voices (the user confirmed OS=8≈OS=4 by
    # ear) halves their FFT rate; the sub's short window is dropped in-code
    # under POGGED_DYN_FOCUS (its attack now comes from the granular). Measured
    # on the real take: mean 73→38 us/block, worst-block below the pre-hybrid
    # vocoder. Override with e.g. `HYBRID=1 PV_OS=8` to keep OS=8 on the ups.
    PV_OS ?= 4
    override CXXFLAGS += -DPOGGED_PV_OS=$(PV_OS)
    # §30 feel: HYB_HOLD = ms of granular after each onset before handing to the
    # vocoder body; HYB_FLOOR = granular kept UNDER the vocoder body during
    # sustain (0 = pure-vocoder body; higher = more of the granular's immediacy,
    # a little more warble). g_gran during sustain = sqrt(FLOOR). Tune by ear:
    # `make HYBRID=1 HYB_HOLD=150 HYB_FLOOR=0.15`.
    HYB_HOLD  ?= 100
    HYB_FLOOR ?= 0.0
    override CXXFLAGS += -DPOGGED_HYB_HOLD_MS=$(HYB_HOLD).0f -DPOGGED_HYB_FLOOR=$(HYB_FLOOR)f
endif

# §28 octave-lock anchor for the DOWN voices. A ÷2 shift copies the input's
# harmonic balance, so on a low note with a weak fundamental the shifted f0/2 is
# weak and the ear locks back onto the original octave — the "instability below
# G". The anchor (octave_anchor.hpp) resynthesises a harmonic series on the
# detected f0·ratio, following the input's spectral envelope, mixed UNDER the
# raw shifted sub which keeps the natural timbre. ANCHOR sets that mix gain;
# unset (default 0) leaves the anchor compiled out to a zero constant. Tune by
# ear on hardware, e.g. `make TARGET=rpi5 HYBRID=1 ANCHOR=0.35`.
ifdef ANCHOR
    override CXXFLAGS += -DPOGGED_ANCHOR_MIX=$(ANCHOR)f
endif

# §34 up-voice grain sizing. The UP voices use a FIXED grain (GRAIN_UP ms),
# which on a LOW note spans less than a period of the up-output (the +5th of a
# baritone low B sings at 92 Hz) so the granular splice warbles into "bouillie".
# GRAIN_UP_PER > 0 sizes each up voice's grain to span that many periods of its
# LOWEST output at the range, floored at GRAIN_UP so high notes stay tight.
# Defaults to 3.0 (validated by ear — stable +1/fifth on low notes); pass
# GRAIN_UP_PER=0 to restore the old fixed grain, or another value to trade
# stability against latency. `make ... GRAIN_UP_PER=2.2`.
GRAIN_UP_PER ?= 3.0
override CXXFLAGS += -DPOGGED_GRAIN_UP_PERIODS=$(GRAIN_UP_PER)f

# §35 ATTACK swell in HYBRID mode. In hybrid the granular renders the attack and
# swells only via the global env, which was keyed to `det` (attack_sens) — far
# less sensitive than the hybrid's own onset detector (hyb_det, 0.88). So the
# granular took over on every re-pluck but the swell did not re-trigger, and
# ATTACK felt inoperative in hybrid. HYB_SWELL=1 keys the swell to the same
# onset that drives the granular takeover, so it swells on every pluck like the
# vocoder's per-bin swell. Reads hyb_det only — the §30 hybrid fix is untouched.
# Off by default (hybrid unchanged from 1f1f52b). `make ... HYBRID=1 HYB_SWELL=1`.
ifdef HYB_SWELL
    override CXXFLAGS += -DPOGGED_HYB_SWELL
endif

# §37 infinite sustain is a RUNTIME feature now — the `sustain` (on/off) and
# `sustain_ms` (release; max = infinite) ports, compiled into every HYBRID build
# and toggled while playing. Nothing to enable at build time. The one build knob
# left is SUSTAIN_SETTLE: ms from the attack to the freeze, so the capture lands
# on the note's BODY (past the ~85 ms vocoder latency), not the attack transient.
# Larger = a later, more settled capture; too large captures a note already
# decaying. `make ... HYBRID=1 SUSTAIN_SETTLE=250`.
ifdef SUSTAIN_SETTLE
    override CXXFLAGS += -DPOGGED_SUSTAIN_SETTLE_MS=$(SUSTAIN_SETTLE).0f
endif

# §38 FREEZE_SMOOTH: route the manual FREEZE through the §36 spectral hold in
# Vocoder/Hybrid Focus, so the held octaves stop re-analysing the loop (which
# repeats at the loop rate — "sounds like a loop") and hold their spectrum
# smoothly instead. The gesture is unchanged: freeze on pedal-off-heel, glide on
# heel-tap (re-captures the new note), unfreeze on heel-hold; the dry stays live.
# Granular Focus keeps the loop. Off by default (freeze byte-identical). A/B knob.
ifdef FREEZE_SMOOTH
    override CXXFLAGS += -DPOGGED_FREEZE_SMOOTH
endif

# In cross-compilation CXXFLAGS already contains -I$(STAGING_DIR)/usr/include
LV2FLAGS ?= $(shell pkg-config --cflags lv2 2>/dev/null)

BUNDLE  = pogged.lv2
BINARY  = $(BUNDLE)/pogged.so

SOURCES = src/plugin.cpp src/glibc_compat.cpp src/pogged_dsp.cpp
HEADERS = src/pogged_dsp.h src/stream_shifter.hpp src/onset_detector.hpp \
          src/biquad.hpp src/envelope.hpp src/freeze_loop.hpp \
          src/stream_filterbank.hpp src/stream_vocoder.hpp \
          src/stream_multivocoder.hpp \
          src/octave_anchor.hpp

all: $(BINARY)

pogged: $(BINARY)

# Make tracks FILE dependencies, not compiler FLAGS — so a flags-only change
# (e.g. `make XBAND=350` after a plain `make`) would leave the old .so in place
# and print "nothing to do". Record the flag signature in a stamp file and
# depend on it, so any change to CXX/CXXFLAGS/XBAND forces a rebuild.
.PHONY: FORCE
build/.flagsig: FORCE
	@mkdir -p build
	@sig='$(CXX)|$(CXXFLAGS)|$(LV2FLAGS)|$(LDFLAGS)'; \
	 [ -f $@ ] && [ "$$(cat $@)" = "$$sig" ] || printf '%s' "$$sig" > $@

$(BINARY): $(SOURCES) $(HEADERS) build/.flagsig
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
#   freeze_test — POG3 FREEZE+GLISS: the octaves hold, the dry stays live over
#                 them, and the pedal's position sets the glide rate
#   filterbank_test — experimental spike: constant-Q filter bank shifts pitch
#                 and holds a chord's sub steadier than the granular splices
#   arpeggio_test   — experimental spike (§1.1): a new attack must not move the
#                 resonance of notes already ringing (filter bank vs vocoder)
#   multires_test   — multi-resolution vocoder: keeps the long window's bass
#                 resolution while the short window tightens attacks
#   polyswell_test  — POG3 polyphonic ATTACK (§14): a new attack swells in on
#                 its own bins while notes already ringing keep their sustain
#                 (vocoder asserted; granular reported, it stays POG2-global)
#   stability_test  — shift stability on close partials (§15/§22): merged
#                 pairs are resolved from the frame HISTORY (Prony) and each
#                 partial rendered on its own kernel — ideal-floor asserted
#   shimmer_test    — shimmer on a realistic chord (§16): colliding harmonics
#                 between two notes must not warble (excess AM vs the ideal
#                 shift bounded; the 8192 long window is what buys this)
#   reinject_test   — transient reinjection (§18): a wet-only preset attacks
#                 within ~20 ms (the reinjected pick snap) while the tonal
#                 body blooms at its own pace; gated off by DRY and ATTACK
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
	$(CXX) $(AUDIT_FLAGS) tools/freeze_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/freeze_test
	$(CXX) $(AUDIT_FLAGS) tools/filterbank_test.cpp -o $(AUDIT_DIR)/filterbank_test
	$(CXX) $(AUDIT_FLAGS) tools/arpeggio_test.cpp -o $(AUDIT_DIR)/arpeggio_test
	$(CXX) $(AUDIT_FLAGS) tools/multires_test.cpp -o $(AUDIT_DIR)/multires_test
	$(CXX) $(AUDIT_FLAGS) tools/polyswell_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/polyswell_test
	$(CXX) $(AUDIT_FLAGS) tools/stability_test.cpp -o $(AUDIT_DIR)/stability_test
	$(CXX) $(AUDIT_FLAGS) tools/shimmer_test.cpp -o $(AUDIT_DIR)/shimmer_test
	$(CXX) $(AUDIT_FLAGS) tools/reinject_test.cpp src/pogged_dsp.cpp -o $(AUDIT_DIR)/reinject_test
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
	@$(AUDIT_DIR)/freeze_test
	@$(AUDIT_DIR)/filterbank_test
	@$(AUDIT_DIR)/arpeggio_test
	@$(AUDIT_DIR)/multires_test
	@echo "══ polyphonic attack swell (§14) ══"; $(AUDIT_DIR)/polyswell_test
	@$(AUDIT_DIR)/stability_test
	@$(AUDIT_DIR)/shimmer_test
	@$(AUDIT_DIR)/reinject_test
	@echo "AUDIT OK"

.PHONY: audit

# ── Bench: worst-block cost of the vocoder path (§13) ───────────────────────
# Machine-dependent, so run BY HAND, never in audit. Native: `make bench &&
# build/bench_vocoder`. For the Pi 5: `make bench TARGET=rpi5`, copy
# build/bench_vocoder to the device (it is fully static) and run it there —
# ideally once with the audio host stopped and once while it plays, three
# passes each, and note the CPU governor (a Pi idling at `ondemand` reads
# slower than it plays).
bench: tools/bench_vocoder.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc tools/bench_vocoder.cpp -o build/bench_vocoder \
	       $(LDFLAGS) $(BENCH_LDFLAGS)
	@echo "-> build/bench_vocoder  (TARGET=$(TARGET))"

.PHONY: bench

install: $(BINARY)
	install -d $(DESTDIR)/usr/lib/lv2
	cp -r $(BUNDLE) $(DESTDIR)/usr/lib/lv2/

.PHONY: all pogged clean install
