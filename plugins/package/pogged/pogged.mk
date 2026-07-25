################################################################################
# pogged — POG2-style polyphonic octave generator LV2 effect
#
# Real-time streaming granular octave shifter (sub -1/-2, +1/+2 oct, detune,
# attack swell, resonant LP). Builds the single Pogged bundle from the shared
# pogged_dsp core. Viable on MOD Dwarf (Cortex-A35).
#
# To update: set POGGED_VERSION to the desired commit hash, then rebuild.
################################################################################

POGGED_VERSION = master
POGGED_SITE    = $(call github,pilali,pogged,$(POGGED_VERSION))
POGGED_BUNDLES = pogged.lv2

# The `pogged` target builds the single LV2 bundle. Cross flags come from
# mod-plugin-builder (TARGET_CXX / TARGET_CXXFLAGS).
#
# Two things this file must get right, both of which it used to get wrong:
#
#  1. TARGET is what selects the ENGINE, and it was hardcoded to moddwarf-new.
#     Building the Duo X package (`./build modduox-new pogged`) therefore
#     compiled the Dwarf's engine — no vocoder at all — instead of the Duo X's
#     single pinned 2048 window. Derive it from the platform instead.
#  2. Passing CXXFLAGS= on the command line OVERRIDES the Makefile's per-target
#     `CXXFLAGS ?=`, which is where -DPOGGED_NO_VOCODER and -DPOGGED_PV_N=2048
#     used to live: they were silently dropped, and the Dwarf shipped with the
#     very vocoder its A35 cannot carry. Those defines are now `override +=` in
#     the Makefile, so they survive whatever is passed here. Nothing to add on
#     this side — but do not move them back.
#
# HYBRID is deliberately NOT passed: the Makefile defaults it OFF for both MOD
# boards (neither can run two engines through an onset) and errors out if asked
# for it, so the board's engine is chosen by TARGET alone.
#
# The two boards are told apart by Buildroot's own BR2_GCC_TARGET_CPU — Dwarf is
# a Cortex-A35, Duo X a Cortex-A53 — which is the same thing that already picks
# the -mcpu. Anything else falls back to the Dwarf's engine, which is the
# conservative direction (granular only). Override on the command line if a
# future platform needs a different mapping.
POGGED_MOD_TARGET ?= $(if $(filter cortex-a53,$(call qstrip,$(BR2_GCC_TARGET_CPU))),modduox-new,moddwarf-new)

define POGGED_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) pogged \
		TARGET=$(POGGED_MOD_TARGET) \
		CXX="$(TARGET_CXX)" \
		STRIP="$(TARGET_STRIP)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -std=c++17 -O3 -ffast-math -fvisibility=hidden"
endef

define POGGED_INSTALL_TARGET_CMDS
	cp -r $(@D)/pogged.lv2 $(TARGET_DIR)/usr/lib/lv2/
endef

$(eval $(generic-package))
