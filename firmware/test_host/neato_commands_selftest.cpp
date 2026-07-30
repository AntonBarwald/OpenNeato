// Host-only self-test for firmware/src/neato_commands.cpp, focused on parseRobotPosData()
// and the zero-pose sentinel fix. Kept out of PlatformIO's src_dir; compiles against the
// Arduino String shim in arduino_shim/Arduino.h. Needs -DCHIP_MODEL, -DCONFIG_IDF_TARGET_ESP32C3=1
// (config.h's #error guards) and -DENABLE_LOGGING=0 (Serial isn't in the shim).
//
// Run (see scripts/run_host_tests.sh for the exact invocation used in CI):
//   clang++ -std=c++17 -I firmware/test_host/arduino_shim -I firmware/src \
//       -DCHIP_MODEL=\"ESP32-C3\" -DCONFIG_IDF_TARGET_ESP32C3=1 -DENABLE_LOGGING=0 \
//       firmware/test_host/neato_commands_selftest.cpp firmware/src/neato_commands.cpp \
//       firmware/src/json_fields.cpp -o /tmp/nctest && /tmp/nctest

#include "neato_commands.h"
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

static bool approxEq(float a, float b, float eps = 1e-3f) {
    float d = a - b;
    if (d < 0)
        d = -d;
    return d <= eps;
}

int main() {
    // -- 1. Valid pose — real capture sample --
    {
        RobotPosData pos;
        bool ok = parseRobotPosData("Robot Smooth pose: X=0.105, Y=-0.000, Theta=359.999, Time=145559.850836", pos,
                                     /*smooth=*/true);
        expectTrue(ok, "valid pose: parse succeeds");
        expectTrue(pos.hasPose, "valid pose: hasPose is true");
        expectTrue(approxEq(pos.x, 0.105f) && approxEq(pos.y, -0.000f) && approxEq(pos.theta, 359.999f),
                   "valid pose: x/y/theta parsed correctly");
        expectTrue(approxEq(pos.time, 145559.850836f), "valid pose: time parsed correctly");
    }

    // -- 2. The zero-pose sentinel — the exact bug. Smooth must still reject it. -
    {
        RobotPosData pos;
        bool ok = parseRobotPosData("Robot Smooth pose: X=0.000, Y=0.000, Theta=0.000, Time=145700.123456", pos,
                                     /*smooth=*/true);
        expectTrue(ok, "smooth zero pose: raw non-empty, parse 'succeeds'");
        expectTrue(!pos.hasPose, "smooth zero pose: still rejected as sentinel (regression guard)");
        // Values are still populated even though the pose is rejected.
        expectTrue(approxEq(pos.x, 0.0f) && approxEq(pos.y, 0.0f) && approxEq(pos.theta, 0.0f),
                   "smooth zero pose: x/y/theta are still populated with the parsed (zero) values");
    }

    // -- 2b. Raw zero pose is the LEGITIMATE post-TestMode-entry origin — must NOT be rejected.
    {
        RobotPosData pos;
        bool ok = parseRobotPosData("X=0.000 Y=0.000 Theta=0.000 Time=100.0", pos, /*smooth=*/false);
        expectTrue(ok && pos.hasPose, "raw zero pose: accepted (legitimate TestMode-zeroed origin)");
    }

    // -- 2c. Raw non-zero post-drive sample, sanity baseline. --------------------
    {
        RobotPosData pos;
        bool ok = parseRobotPosData("X=1.166 Y=-0.053 Theta=355.221 Time=200.0", pos, /*smooth=*/false);
        expectTrue(ok && pos.hasPose, "raw non-zero pose: parses and is trusted");
    }

    // -- 3. Near-zero but NOT the sentinel — must NOT false-positive. -----------
    //    Guards the "any single field is 0" mistake explicitly.
    {
        RobotPosData pos;
        bool ok = parseRobotPosData("X=0.000 Y=1.234 Theta=45.000 Time=100.0", pos, /*smooth=*/true);
        expectTrue(ok && pos.hasPose, "one-axis-zero: X=0 alone with nonzero Y/Theta is NOT rejected");
    }
    {
        RobotPosData pos;
        bool ok = parseRobotPosData("X=-0.500 Y=0.000 Theta=0.000 Time=100.0", pos, /*smooth=*/true);
        expectTrue(ok && pos.hasPose, "one-axis-zero: Y=0,Theta=0 with nonzero X is NOT rejected");
    }

    // -- 4. Malformed: missing a token entirely (existing behavior, no regress) --
    {
        RobotPosData pos;
        bool ok = parseRobotPosData("Robot Smooth pose: X=0.105, Y=-0.000, Time=145559.850836", pos,
                                     /*smooth=*/true); // no Theta=
        expectTrue(ok, "malformed: missing Theta= token, raw non-empty, parse 'succeeds'");
        expectTrue(!pos.hasPose, "malformed: missing Theta= token yields hasPose=false");
    }

    // -- 5. Malformed: empty response. -------------------------------------------
    {
        RobotPosData pos;
        bool ok = parseRobotPosData("", pos, /*smooth=*/true);
        expectTrue(!ok, "empty response: parse fails (raw.length() == 0)");
        expectTrue(!pos.hasPose, "empty response: hasPose is false");
    }

    // -- 6. Real non-zero mid-clean sample, for a second passing baseline -------
    {
        RobotPosData pos;
        bool ok = parseRobotPosData("X=-0.342 Y=-0.664 Theta=180.0 Time=145600.0", pos, /*smooth=*/true);
        expectTrue(ok && pos.hasPose, "mid-clean sample: parses and is trusted");
        expectTrue(approxEq(pos.x, -0.342f) && approxEq(pos.y, -0.664f) && approxEq(pos.theta, 180.0f),
                   "mid-clean sample: x/y/theta parsed correctly");
    }

    // -- 7. GetAnalogSensors with drop sensor fields present ---------------------
    {
        BatteryAnalogData data;
        bool ok = parseBatteryAnalogData(
                "BatteryVoltage,mV,14585,\r\nBatteryCurrent,mA,0,\r\nBatteryTemperature,mC,25000,\r\n"
                "ExternalVoltage,mV,0,\r\nDropSensorLeft,mm,19,\r\nDropSensorRight,mm,19,\r\n",
                data);
        expectTrue(ok, "analog: parse succeeds");
        expectTrue(data.dropSensorLeftMM == 19 && data.dropSensorRightMM == 19,
                   "analog: drop sensor fields parsed correctly");
    }

    // -- 8. GetAnalogSensors with drop sensor lines omitted (older firmware) -----
    {
        BatteryAnalogData data;
        bool ok = parseBatteryAnalogData(
                "BatteryVoltage,mV,14585,\r\nBatteryCurrent,mA,0,\r\nBatteryTemperature,mC,25000,\r\n"
                "ExternalVoltage,mV,0,\r\n",
                data);
        expectTrue(ok, "analog: parse succeeds without drop fields (other fields present)");
        expectTrue(data.dropSensorLeftMM == -1 && data.dropSensorRightMM == -1,
                   "analog: missing drop fields stay at -1 sentinel");
    }

    // -- 9. GetAnalogSensors with a malformed (non-numeric) drop value -----------
    {
        BatteryAnalogData data;
        bool ok = parseBatteryAnalogData("DropSensorLeft,mm,abc,\r\nDropSensorRight,mm,19,\r\n", data);
        expectTrue(ok, "analog: parse succeeds with malformed drop field");
        expectTrue(data.dropSensorLeftMM == 0, "analog: non-numeric drop field parses to 0 (String::toInt() behavior)");
        expectTrue(data.dropSensorRightMM == 19, "analog: sibling valid field unaffected by malformed one");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}
