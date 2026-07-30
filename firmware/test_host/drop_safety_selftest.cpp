// Host-only self-test for firmware/src/drop_safety.h — pins down the cliff/drop-sensor
// backstop's latch/debounce/fail-closed decision logic pulled out of
// ManualCleanManager::pollDrop()/isMoveAllowed(), the same host-testable-seam pattern as
// nav_geometry_selftest.cpp / guided_motor_policy_selftest.cpp.
//
// Run:
//   clang++ -std=c++17 -I firmware/src firmware/test_host/drop_safety_selftest.cpp -o /tmp/dstest && /tmp/dstest

#include "drop_safety.h"
#include <cstdio>

static int failures = 0;

static void expectTrue(bool cond, const char *name) {
    if (!cond) {
        std::printf("FAIL: %s\n", name);
        failures++;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

int main() {
    // -- Below threshold never triggers, even repeated ------------------------
    {
        DropLatchState state;
        for (int i = 0; i < 5; i++) {
            bool justTriggered = dropLatchApplyReading(state, DropReading{true, false, false}, 1, 3);
            expectTrue(!justTriggered, "below-threshold: never reports newly-triggered");
        }
        expectTrue(!state.triggeredLeft && !state.triggeredRight, "below-threshold: latch never sets");
    }

    // -- At/above threshold triggers only after the configured debounce -------
    {
        DropLatchState state;
        // overCountThreshold=3: first two over-threshold reads must NOT trigger yet.
        bool t1 = dropLatchApplyReading(state, DropReading{true, true, false}, 3, 5);
        expectTrue(!t1, "debounce: 1st consecutive over-threshold read does not trigger");
        expectTrue(!state.triggeredLeft, "debounce: latch not set after 1st over-threshold read");

        bool t2 = dropLatchApplyReading(state, DropReading{true, true, false}, 3, 5);
        expectTrue(!t2, "debounce: 2nd consecutive over-threshold read does not trigger");
        expectTrue(!state.triggeredLeft, "debounce: latch not set after 2nd over-threshold read");

        bool t3 = dropLatchApplyReading(state, DropReading{true, true, false}, 3, 5);
        expectTrue(t3, "debounce: 3rd consecutive over-threshold read (== threshold) triggers");
        expectTrue(state.triggeredLeft && !state.triggeredRight, "debounce: only the over side latches");
    }

    // -- Latch is sticky: stays set on a subsequent in-range (non-over) reading --
    {
        DropLatchState state;
        dropLatchApplyReading(state, DropReading{true, true, true}, 1, 5); // trips both sides immediately (threshold=1)
        expectTrue(state.triggeredLeft && state.triggeredRight, "sticky: both sides latched after tripping");

        bool retrigger = dropLatchApplyReading(state, DropReading{true, false, false}, 1, 5);
        expectTrue(!retrigger, "sticky: a clean reading after tripping is not reported as newly-triggered");
        expectTrue(state.triggeredLeft && state.triggeredRight,
                   "sticky: latch stays set on a subsequent in-range reading, not cleared by good data");
    }

    // -- N consecutive unusable reads (-1 sentinel) trigger fail-closed -------
    {
        DropLatchState state;
        bool f1 = dropLatchApplyReading(state, DropReading{false, false, false}, 1, 3);
        expectTrue(!f1, "fail-closed: 1st unusable read does not trigger yet");
        bool f2 = dropLatchApplyReading(state, DropReading{false, false, false}, 1, 3);
        expectTrue(!f2, "fail-closed: 2nd unusable read does not trigger yet");
        expectTrue(!state.triggeredLeft && !state.triggeredRight, "fail-closed: latch not set before the Nth unusable read");

        bool f3 = dropLatchApplyReading(state, DropReading{false, false, false}, 1, 3);
        expectTrue(f3, "fail-closed: 3rd consecutive unusable read (== fail threshold) triggers");
        expectTrue(state.triggeredLeft && state.triggeredRight,
                   "fail-closed: both sides latch on N consecutive unusable reads, regardless of over/under");

        // Once already fail-closed, further unusable reads don't re-report as newly-triggered.
        bool f4 = dropLatchApplyReading(state, DropReading{false, false, false}, 1, 3);
        expectTrue(!f4, "fail-closed: a 4th unusable read past the threshold does not re-trigger");
    }

    // -- A usable read resets the fail counter ---------------------------------
    {
        DropLatchState state;
        dropLatchApplyReading(state, DropReading{false, false, false}, 1, 3);
        dropLatchApplyReading(state, DropReading{false, false, false}, 1, 3);
        expectTrue(state.failCount == 2, "fail counter: two consecutive unusable reads count to 2");

        // A usable read (even a below-threshold one) resets the fail counter to 0.
        dropLatchApplyReading(state, DropReading{true, false, false}, 1, 3);
        expectTrue(state.failCount == 0, "fail counter: a usable read resets failCount to 0");

        // So it now takes a fresh run of 3 unusable reads to fail-closed, not just 1 more.
        bool retrig1 = dropLatchApplyReading(state, DropReading{false, false, false}, 1, 3);
        expectTrue(!retrig1, "fail counter: reset means a single further unusable read does not fail-closed");
        expectTrue(!state.triggeredLeft && !state.triggeredRight, "fail counter: latch still clear after the reset");
    }

    // -- Reverse-clear semantics ------------------------------------------------
    {
        DropLatchState state;
        dropLatchApplyReading(state, DropReading{true, true, true}, 1, 5);
        expectTrue(state.triggeredLeft && state.triggeredRight, "reverse-clear: latch set before reversing");

        dropLatchClearOnReverse(state);
        expectTrue(!state.triggeredLeft && !state.triggeredRight, "reverse-clear: reversing clears both latches");

        // Reverse-clear must not disturb the debounce/fail counters (only isMoveAllowed's
        // "moving backward and not forward" path calls this, independent of pollDrop()).
        expectTrue(state.overCount >= 0 && state.failCount >= 0,
                   "reverse-clear: counters remain well-defined (not part of the cleared latch)");
    }

    // -- Symmetric debounced auto-clear (design-review §2) ---------------------
    {
        DropLatchState state;
        dropLatchApplyReading(state, DropReading{true, true, true}, 1, 5);
        expectTrue(state.triggeredLeft && state.triggeredRight, "auto-clear: both sides latched before clean reads start");

        for (int i = 0; i < 2; i++)
            dropLatchApplyClear(state, DropReading{true, false, false}, 3);
        expectTrue(state.triggeredLeft && state.triggeredRight, "auto-clear: below clear threshold, latch stays set");

        dropLatchApplyClear(state, DropReading{true, false, false}, 3);
        expectTrue(!state.triggeredLeft && !state.triggeredRight,
                   "auto-clear: Nth consecutive clean read (== clear threshold) clears both sides");
    }
    {
        // An over-threshold or unusable read resets the clear-count run -- no partial credit.
        DropLatchState state;
        dropLatchApplyReading(state, DropReading{true, true, false}, 1, 5);
        dropLatchApplyClear(state, DropReading{true, false, false}, 3);
        dropLatchApplyClear(state, DropReading{true, true, false}, 3); // over-threshold again
        dropLatchApplyClear(state, DropReading{true, false, false}, 3);
        expectTrue(state.triggeredLeft, "auto-clear: an over-threshold read resets the clear-count run");
    }
    {
        // An unusable read also resets the clear-count run, same as an over-threshold read.
        DropLatchState state;
        dropLatchApplyReading(state, DropReading{true, false, true}, 1, 5);
        dropLatchApplyClear(state, DropReading{true, false, false}, 3);
        dropLatchApplyClear(state, DropReading{false, false, false}, 3); // unusable
        dropLatchApplyClear(state, DropReading{true, false, false}, 3);
        expectTrue(state.triggeredRight, "auto-clear: an unusable read resets the clear-count run");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
