#!/usr/bin/env bash
# Compile the DSP core under every documented build-flag combination.
#
# The Makefile's flags multiply out into a matrix that `make` never touches: the
# shipped LV2 binary is ONE point of it and CI builds that same point, so
# anything the preprocessor only reaches under another combination rots
# silently. That is not hypothetical — it is how HYBRID=1 came to be broken on
# both MOD targets while the README advertised it as supported.
#
# The flags are ASKED OF THE MAKEFILE (`make print-flags TARGET=... HYBRID=...`)
# rather than restated here. A matrix carrying its own copy of the expansion
# tests the copy, not the build.
#
# Only the -D/-U defines are kept: the machine flags of a cross target
# (-mcpu=cortex-a76) mean nothing to the host compiler, and what this checks is
# which CODE compiles, which is a preprocessor question. Only pogged_dsp.cpp is
# compiled — it is the whole DSP core and, unlike plugin.cpp, needs no LV2
# headers, so the matrix runs anywhere a C++17 compiler does.
#
# Two kinds of entry:
#   BUILD     — must compile. A failure not listed in known_failures.txt is a
#               regression; a listed one that has started building means the
#               baseline is stale. So the list can only shrink, deliberately.
#   MUSTFAIL  — must be REFUSED by the Makefile before a compiler ever runs.
#               HYBRID=1 on a MOD board is a supported thing to be told off for,
#               not a supported build.
#
# Usage: tools/eval/build_matrix.sh [-v]

set -eu

CXX=${CXX:-g++}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build/eval/matrix}
BASELINE="$ROOT/tools/eval/known_failures.txt"
VERBOSE=${1:-}

mkdir -p "$OUT"
: > "$OUT/.pass"
: > "$OUT/.fail"

# name<TAB>kind<TAB>make arguments
CONFIGS=$(cat <<'EOF'
native	BUILD
native-nohybrid	BUILD	HYBRID=0
rpi5	BUILD	TARGET=rpi5
moddwarf-new	BUILD	TARGET=moddwarf-new
modduox-new	BUILD	TARGET=modduox-new
pv_os8	BUILD	PV_OS=8
xband	BUILD	XBAND=1
xband900	BUILD	XBAND=900
anchor	BUILD	ANCHOR=0.35
grain_up_per0	BUILD	GRAIN_UP_PER=0
grains	BUILD	GRAIN_UP=10 GRAIN_PER=2.0
hyb_swell_off	BUILD	HYB_SWELL=0
freeze_smooth_off	BUILD	FREEZE_SMOOTH=0
settle	BUILD	SUSTAIN_SETTLE=400
rpi5-full	BUILD	TARGET=rpi5 XBAND=1 ANCHOR=0.35 PV_OS=8
moddwarf-anchor	BUILD	TARGET=moddwarf-new ANCHOR=0.35
modduox-xband	BUILD	TARGET=modduox-new XBAND=1
sub_split	BUILD	EXTRA=-DPOGGED_SUB_SPLIT
up_longonly_off	BUILD	EXTRA=-DPOGGED_UP_LONGONLY_OFF
prony_debug	BUILD	EXTRA=-DPOGGED_PRONY_DEBUG
moddwarf-new+hybrid	MUSTFAIL	TARGET=moddwarf-new HYBRID=1
modduox-new+hybrid	MUSTFAIL	TARGET=modduox-new HYBRID=1
EOF
)

printf '== build matrix (%s) ==\n' "$($CXX --version | head -1)"

printf '%s\n' "$CONFIGS" | while IFS='	' read -r name kind args; do
    [ -n "$name" ] || continue
    log="$OUT/$name.log"

    # EXTRA= carries a raw define for a knob the Makefile has no variable for.
    extra=""
    case "$args" in
        EXTRA=*) extra="${args#EXTRA=}"; args="" ;;
    esac

    # shellcheck disable=SC2086
    if ! flags=$(make -s -C "$ROOT" print-flags $args 2>"$log"); then
        if [ "$kind" = MUSTFAIL ]; then
            printf '  OK    %-24s refused by the Makefile, as intended\n' "$name"
        else
            printf '  FAIL  %-24s make print-flags failed\n' "$name"
            echo "$name" >> "$OUT/.fail"
            [ -z "$VERBOSE" ] || sed 's/^/        /' "$log"
        fi
        continue
    fi
    if [ "$kind" = MUSTFAIL ]; then
        printf '  FAIL  %-24s the Makefile ACCEPTED a combination it must refuse\n' "$name"
        echo "$name" >> "$OUT/.fail"
        continue
    fi

    # Preprocessor defines only — see the header comment.
    defines=$(printf '%s\n' "$flags" | tr ' ' '\n' | grep -E '^-[DU]' | tr '\n' ' ' || true)

    # shellcheck disable=SC2086
    if $CXX -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter \
            -I"$ROOT/src" $defines $extra \
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

pass_list=$(sort -u "$OUT/.pass")
fail_list=$(sort -u "$OUT/.fail")
rm -f "$OUT/.pass" "$OUT/.fail"

known=$(grep -vE '^\s*(#|$)' "$BASELINE" 2>/dev/null | sort -u || true)

new_fail=$(comm -23 <(printf '%s\n' "$fail_list" | grep -v '^$' || true) \
                    <(printf '%s\n' "$known"     | grep -v '^$' || true))
fixed=$(comm -12 <(printf '%s\n' "$pass_list" | grep -v '^$' || true) \
                 <(printf '%s\n' "$known"     | grep -v '^$' || true))

status=0
if [ -n "$new_fail" ]; then
    printf '\nNEW breakage (not in known_failures.txt):\n'
    printf '%s\n' "$new_fail" | sed 's/^/  /'
    status=1
fi
if [ -n "$fixed" ]; then
    printf '\nListed as broken but now builds — drop it from known_failures.txt:\n'
    printf '%s\n' "$fixed" | sed 's/^/  /'
    status=1
fi
if [ "$status" -eq 0 ]; then
    n=$(printf '%s\n' "$known" | grep -c . || true)
    printf '\nMATRIX OK (%s known-broken config%s)\n' "$n" \
           "$([ "$n" = 1 ] || echo s)"
fi
exit "$status"
