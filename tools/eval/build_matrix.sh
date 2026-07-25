#!/usr/bin/env bash
# Compile the DSP core under every documented build-flag combination.
#
# The flags in the Makefile and the README multiply out into a matrix the
# normal `make` never touches: the shipped LV2 binary is ONE point of it
# (native, no HYBRID), CI builds that same point, and the JUCE build hardcodes
# another in juce/CMakeLists.txt. Anything the preprocessor only reaches under
# a flag combination nobody compiles rots silently — which is exactly what this
# catches.
#
# Only pogged_dsp.cpp is compiled: it is the whole DSP core and, unlike
# plugin.cpp, needs no LV2 headers, so the matrix runs on any machine with a
# C++17 compiler.
#
# Combinations that are known to be broken today live in known_failures.txt,
# one config name per line. The script fails on a NEW break (a regression) and
# on a listed break that has started working (the baseline is stale), so the
# list can only shrink deliberately.
#
# Usage: tools/eval/build_matrix.sh [-v]

set -eu

CXX=${CXX:-g++}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build/eval/matrix}
BASELINE="$ROOT/tools/eval/known_failures.txt"
VERBOSE=${1:-}

mkdir -p "$OUT"
rm -f "$OUT/.pass" "$OUT/.fail"

# The HYBRID=1 defaults, exactly as the Makefile expands them.
HYB="-DPOGGED_DYN_FOCUS -DPOGGED_GRAIN_UP_MS=12.0f -DPOGGED_GRAIN_PERIODS=3.0f
     -DPOGGED_PV_OS=4 -DPOGGED_HYB_HOLD_MS=100.0f -DPOGGED_HYB_FLOOR=0.0f"
# Applied by the Makefile to every build, HYBRID or not.
ALWAYS="-DPOGGED_GRAIN_UP_PERIODS=3.0f -DPOGGED_HYB_SWELL -DPOGGED_FREEZE_SMOOTH"

# name<TAB>flags — `make TARGET=x FLAG=y` reduced to the defines it produces.
CONFIGS=$(cat <<'EOF'
native
moddwarf-new	-DPOGGED_NO_VOCODER
modduox-new	-DPOGGED_PV_N=2048
rpi5
hybrid	$HYB
hybrid+pv_os8	$HYB -UPOGGED_PV_OS -DPOGGED_PV_OS=8
hybrid+xband	$HYB -DPOGGED_XBAND
hybrid+xband900	$HYB -DPOGGED_XBAND -DPOGGED_XOVER=900
hybrid+anchor	$HYB -DPOGGED_ANCHOR_MIX=0.35f
hybrid+settle	$HYB -DPOGGED_SUSTAIN_SETTLE_MS=400.0f
hybrid+sub_split	$HYB -DPOGGED_SUB_SPLIT
hybrid+up_longonly_off	$HYB -DPOGGED_UP_LONGONLY_OFF
hybrid+prony_debug	$HYB -DPOGGED_PRONY_DEBUG
grain_up_per0	-UPOGGED_GRAIN_UP_PERIODS -DPOGGED_GRAIN_UP_PERIODS=0.0f
hyb_swell_off	$HYB -UPOGGED_HYB_SWELL
freeze_smooth_off	$HYB -UPOGGED_FREEZE_SMOOTH
xband	-DPOGGED_XBAND
anchor	-DPOGGED_ANCHOR_MIX=0.35f
juce	$HYB
moddwarf-new+hybrid	-DPOGGED_NO_VOCODER $HYB
modduox-new+hybrid	-DPOGGED_PV_N=2048 $HYB
EOF
)

pass_list=""
fail_list=""

printf '== build matrix (%s) ==\n' "$($CXX --version | head -1)"

# `read` splits on the tab; the flag column is eval'd so $HYB expands.
printf '%s\n' "$CONFIGS" | while IFS='	' read -r name flags; do
    [ -n "$name" ] || continue
    # shellcheck disable=SC2086
    expanded=$(eval printf '%s' "\"$flags\"")
    log="$OUT/$name.log"
    if $CXX -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter \
            -I"$ROOT/src" $ALWAYS $expanded \
            -c "$ROOT/src/pogged_dsp.cpp" -o "$OUT/$name.o" >"$log" 2>&1; then
        warns=$(grep -c 'warning:' "$log" || true)
        printf '  PASS  %-24s %s\n' "$name" \
               "$([ "$warns" -gt 0 ] && echo "($warns warnings)")"
        echo "$name" >> "$OUT/.pass"
    else
        printf '  FAIL  %-24s %s\n' "$name" "$(grep -m1 'error:' "$log" || true)"
        echo "$name" >> "$OUT/.fail"
        [ -z "$VERBOSE" ] || sed 's/^/        /' "$log"
    fi
done

pass_list=$(sort -u "$OUT/.pass" 2>/dev/null || true)
fail_list=$(sort -u "$OUT/.fail" 2>/dev/null || true)
rm -f "$OUT/.pass" "$OUT/.fail"

known=$(grep -vE '^\s*(#|$)' "$BASELINE" 2>/dev/null | sort -u || true)

new_fail=$(comm -23 <(printf '%s\n' "$fail_list") <(printf '%s\n' "$known") 2>/dev/null || true)
fixed=$(comm -12 <(printf '%s\n' "$pass_list") <(printf '%s\n' "$known") 2>/dev/null || true)

status=0
if [ -n "$(printf '%s' "$new_fail")" ]; then
    printf '\nNEW breakage (not in known_failures.txt):\n'
    printf '%s\n' "$new_fail" | sed 's/^/  /'
    status=1
fi
if [ -n "$(printf '%s' "$fixed")" ]; then
    printf '\nListed as broken but now builds — drop it from known_failures.txt:\n'
    printf '%s\n' "$fixed" | sed 's/^/  /'
    status=1
fi
[ "$status" -eq 0 ] && printf '\nMATRIX OK (%d known-broken configs)\n' \
    "$(printf '%s\n' "$known" | grep -c . || true)"
exit "$status"
