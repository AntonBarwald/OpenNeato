#!/usr/bin/env bash
# Compiles and runs every host-only unit test under firmware/test_host/ with plain clang++ —
# no PlatformIO, no ESP32 toolchain, no target board.
#
# Adding a new suite: append a call to `add_suite <name> <compile-command>` below;
# <compile-command> must produce "$BUILD_DIR/<name>" and exit 0 on success.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_HOST_DIR="$ROOT_DIR/firmware/test_host"
SRC_DIR="$ROOT_DIR/firmware/src"
SHIM_DIR="$TEST_HOST_DIR/arduino_shim"

CXX="${CXX:-clang++}"
CXXFLAGS="-std=c++17 -Wall -Wextra"

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/openneato_host_tests.XXXXXX")"
cleanup() { rm -rf "$BUILD_DIR"; }
trap cleanup EXIT

declare -a SUITE_NAMES=()
declare -a SUITE_CMDS=()
declare -a SUITE_SRCS=() # source file(s) each suite covers, for the "known suites" check below

add_suite() {
    SUITE_NAMES+=("$1")
    SUITE_CMDS+=("$2")
    SUITE_SRCS+=("$3")
}

# -- geometry: header-only, zero Arduino dependency --------------------------
add_suite "geometry" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/geometry_selftest.cpp\" -o \"$BUILD_DIR/geometry\"" \
    "geometry_selftest.cpp"

# -- point_array_json: needs the Arduino String shim + its own .cpp ---------
# -DNAV_MAX_WAYPOINTS=8 sidesteps config.h's -DCHIP_MODEL requirement and gives the cap
# test a small, fast bound.
add_suite "point_array_json" \
    "$CXX $CXXFLAGS -I \"$SHIM_DIR\" -I \"$SRC_DIR\" -DNAV_MAX_WAYPOINTS=8 \"$TEST_HOST_DIR/point_array_json_selftest.cpp\" \"$SRC_DIR/point_array_json.cpp\" -o \"$BUILD_DIR/point_array_json\"" \
    "point_array_json_selftest.cpp"

# -- neato_commands: needs the Arduino String shim + its own .cpp + json_fields --
# Unlike point_array_json.cpp, this #includes config.h directly, so -DCHIP_MODEL and
# -DCONFIG_IDF_TARGET_ESP32C3=1 satisfy its #error guards; -DENABLE_LOGGING=0 no-ops LOG()
# since Serial isn't in the shim. json_fields.cpp is linked for fieldsToJson/jsonEscape.
add_suite "neato_commands" \
    "$CXX $CXXFLAGS -I \"$SHIM_DIR\" -I \"$SRC_DIR\" -DCHIP_MODEL=\\\"ESP32-C3\\\" -DCONFIG_IDF_TARGET_ESP32C3=1 -DENABLE_LOGGING=0 \"$TEST_HOST_DIR/neato_commands_selftest.cpp\" \"$SRC_DIR/neato_commands.cpp\" \"$SRC_DIR/json_fields.cpp\" -o \"$BUILD_DIR/neato_commands\"" \
    "neato_commands_selftest.cpp"

# -- nav_geometry: header-only, zero Arduino dependency (composes geometry.h) --
# Covers the live no-go-line/zone-boundary clearance check, factored out of
# NavigationManager so it's host-testable independent of Arduino/NeatoSerial/callbacks.
add_suite "nav_geometry" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/nav_geometry_selftest.cpp\" -o \"$BUILD_DIR/nav_geometry\"" \
    "nav_geometry_selftest.cpp"

# -- zone_label_match: needs the Arduino String shim --------------------------
# Covers resolving schedule/guided-start zone LABELS (not positional index) against a
# session's saved zones, including "Zone N" fallback and the unresolved-label error path.
add_suite "zone_label_match" \
    "$CXX $CXXFLAGS -I \"$SHIM_DIR\" -I \"$SRC_DIR\" \"$TEST_HOST_DIR/zone_label_match_selftest.cpp\" -o \"$BUILD_DIR/zone_label_match\"" \
    "zone_label_match_selftest.cpp"

# -- guided_motor_policy: header-only, zero Arduino dependency ---------------
# Pins down guidedMotorsShouldRun() (Guided Clean never turned on the cleaning motors).
add_suite "guided_motor_policy" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/guided_motor_policy_selftest.cpp\" -o \"$BUILD_DIR/guided_motor_policy\"" \
    "guided_motor_policy_selftest.cpp"

# -- drop_safety: header-only, zero Arduino dependency -----------------------
# Pins down ManualCleanManager::pollDrop()'s debounce/fail-closed/sticky-latch decision
# logic and isMoveAllowed()'s reverse-clear rule, factored out so the cliff-safety backstop
# is host-testable independent of Arduino/NeatoSerial.
add_suite "drop_safety" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/drop_safety_selftest.cpp\" -o \"$BUILD_DIR/drop_safety\"" \
    "drop_safety_selftest.cpp"

# -- motion_safety: header-only, zero Arduino dependency --------------------
# Pins down classifyMotion()/motionSafetyCheck() — the MotionKind fix for isMoveAllowed()'s
# overlapping-boolean bug (rotate deadlock + dead-code side-bumper guard). Also carries a
# standalone port of the legacy buggy logic to demonstrate both defects empirically.
add_suite "motion_safety" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/motion_safety_selftest.cpp\" -o \"$BUILD_DIR/motion_safety\"" \
    "motion_safety_selftest.cpp"

# -- lds_sector: header-only, zero Arduino dependency ------------------------
# Pins down the careful-driving spec's §2.3 sector reduction (ldsSectorMinDistance) and
# §2.4 drive-decision (ldsDriveDecision) -- the pure logic behind the LIDAR proximity veto.
add_suite "lds_sector" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/lds_sector_selftest.cpp\" -o \"$BUILD_DIR/lds_sector\"" \
    "lds_sector_selftest.cpp"

# -- escape_policy: header-only, zero Arduino dependency ---------------------
# Pins down the careful-driving spec's §3.2 MoveBlockReason -> escape-motion table.
add_suite "escape_policy" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/escape_policy_selftest.cpp\" -o \"$BUILD_DIR/escape_policy\"" \
    "escape_policy_selftest.cpp"

# -- drive_speed_policy: header-only, zero Arduino dependency ---------------
# Pins down the careful-driving spec's §4.3 high-confidence smoothing item: scale wheel speed
# proportionally to step size instead of always commanding full speed then hard-stopping.
add_suite "drive_speed_policy" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/drive_speed_policy_selftest.cpp\" -o \"$BUILD_DIR/drive_speed_policy\"" \
    "drive_speed_policy_selftest.cpp"

# -- spot_size: header-only, zero Arduino dependency ------------------------
# Pins down clampSpotDimension() -- the Width/Height clamp for sized spot clean.
add_suite "spot_size" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/spot_size_selftest.cpp\" -o \"$BUILD_DIR/spot_size\"" \
    "spot_size_selftest.cpp"

# -- spot_verify: header-only, zero Arduino dependency -----------------------
# Pins down isSpotStartMismatch() -- the spot-clean-started-a-house-clean safety-net check.
add_suite "spot_verify" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/spot_verify_selftest.cpp\" -o \"$BUILD_DIR/spot_verify\"" \
    "spot_verify_selftest.cpp"

# -- clean_timer_policy: header-only, zero Arduino dependency ---------------
# Pins down the whole-house-clean early-return timer's deadline math (cleanTimerExpired,
# cleanTimerRemainingSec), including millis() wraparound.
add_suite "clean_timer_policy" \
    "$CXX $CXXFLAGS -I \"$SRC_DIR\" \"$TEST_HOST_DIR/clean_timer_policy_selftest.cpp\" -o \"$BUILD_DIR/clean_timer_policy\"" \
    "clean_timer_policy_selftest.cpp"

# -- Safety net: warn about *_selftest.cpp files not wired into a suite above --
for f in "$TEST_HOST_DIR"/*_selftest.cpp; do
    base="$(basename "$f")"
    known=0
    for s in "${SUITE_SRCS[@]}"; do
        [ "$s" = "$base" ] && known=1 && break
    done
    if [ "$known" -eq 0 ]; then
        echo "WARNING: $base is not wired into scripts/run_host_tests.sh — it will not run." >&2
    fi
done

total=0
passed=0
declare -a SUMMARY=()

for i in "${!SUITE_NAMES[@]}"; do
    name="${SUITE_NAMES[$i]}"
    cmd="${SUITE_CMDS[$i]}"
    total=$((total + 1))

    echo "=============================================="
    echo "Suite: $name"
    echo "=============================================="

    compile_log="$BUILD_DIR/$name.compile.log"
    if ! eval "$cmd" >"$compile_log" 2>&1; then
        echo "FAIL: $name -- compile error"
        sed 's/^/    /' "$compile_log"
        SUMMARY+=("FAIL  $name  (compile error)")
        echo
        continue
    fi

    binary="$BUILD_DIR/$name"
    run_log="$BUILD_DIR/$name.run.log"
    if "$binary" >"$run_log" 2>&1; then
        cat "$run_log"
        passed=$((passed + 1))
        SUMMARY+=("PASS  $name")
    else
        cat "$run_log"
        echo "FAIL: $name -- one or more checks failed (see PASS/FAIL lines above)"
        SUMMARY+=("FAIL  $name  (assertion failure)")
    fi
    echo
done

echo "=============================================="
echo "Host test summary"
echo "=============================================="
for line in "${SUMMARY[@]}"; do
    echo "$line"
done
echo "----------------------------------------------"
echo "$passed / $total suites passed"

if [ "$passed" -ne "$total" ]; then
    exit 1
fi
exit 0
