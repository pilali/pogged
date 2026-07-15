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
# mod-plugin-builder (TARGET_CXX / TARGET_CXXFLAGS); the Makefile uses ?= for
# the moddwarf-new target so these win.
define POGGED_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) pogged \
		TARGET=moddwarf-new \
		CXX="$(TARGET_CXX)" \
		STRIP="$(TARGET_STRIP)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -std=c++17 -O3 -ffast-math -fvisibility=hidden"
endef

define POGGED_INSTALL_TARGET_CMDS
	cp -r $(@D)/pogged.lv2 $(TARGET_DIR)/usr/lib/lv2/
endef

$(eval $(generic-package))
