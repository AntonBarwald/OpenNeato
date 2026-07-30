#ifndef CONFIG_H
#define CONFIG_H

// Firmware version — passed via build flag (-DFIRMWARE_VERSION=...), fallback for local builds
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0"
#endif

// Chip model — passed via build flag (-DCHIP_MODEL=...) from board section in platformio.ini
#ifndef CHIP_MODEL
#error "CHIP_MODEL must be defined (e.g. -DCHIP_MODEL=\\\"ESP32-C3\\\")"
#endif

// WiFi Configuration
#define DEFAULT_HOSTNAME "neato"
#define WIFI_DEFAULT_TX_POWER                                                                                          \
    60 // WiFi TX power in 0.25 dBm units (60 = 15 dBm, ~32 mW)
       // Lower values caused boot connection failures (4-way handshake
       // timeouts) at marginal signal levels (-70 dBm range).
       // Range: 8 (2 dBm) to 84 (21 dBm). Common values:
       //   34 = 8.5 dBm,  52 = 13 dBm,  60 = 15 dBm (recommended)
       //   68 = 17 dBm,  78 = 19.5 dBm
#define WIFI_MAX_RECONNECT_BACKOFF 30000 // Max backoff between reconnect attempts (ms)

// Pin Configuration — boot/reset button and default UART pins vary by chip.
// Original ESP32: BOOT is GPIO0, GPIO1/3 are the USB-UART bridge (U0TXD/U0RXD).
// ESP32-C3: BOOT is GPIO9, GPIO1/3 are free GPIOs.
// Seeed Studio XIAO ESP32C3: D6/TX is GPIO21, D7/RX is GPIO20.
// ESP32-S3: BOOT is GPIO0, GPIO19/20 are native USB — use free GPIOs for UART.
#if CONFIG_IDF_TARGET_ESP32
#define RESET_BUTTON_PIN 0
#define NEATO_DEFAULT_TX_PIN 17
#define NEATO_DEFAULT_RX_PIN 16
#define MAX_GPIO_PIN 39
#elif CONFIG_IDF_TARGET_ESP32C3
#define RESET_BUTTON_PIN 9
#if defined(OPENNEATO_BOARD_XIAO_ESP32C3)
#define NEATO_DEFAULT_TX_PIN 21
#define NEATO_DEFAULT_RX_PIN 20
#else
#define NEATO_DEFAULT_TX_PIN 3
#define NEATO_DEFAULT_RX_PIN 4
#endif
#define MAX_GPIO_PIN 21
#elif CONFIG_IDF_TARGET_ESP32S3
#define RESET_BUTTON_PIN 0
#define NEATO_DEFAULT_TX_PIN 17
#define NEATO_DEFAULT_RX_PIN 18
#define MAX_GPIO_PIN 48
#else
#error "Unsupported chip — add pin definitions for this target"
#endif

// Actual UART pins are stored in NVS and configurable via settings API.
#define NEATO_BAUD_RATE 115200
#define NEATO_UART_RX_BUFFER 8192 // GetLDSScan response is 5451 bytes, exceeds the old 4096 with no margin

// Neato command queue timing (milliseconds)
#define NEATO_CMD_TIMEOUT_MS 3000
#define NEATO_INTER_CMD_DELAY_MS 50
#define NEATO_QUEUE_MAX_SIZE 16
#define NEATO_RESPONSE_TERMINATOR 0x1A // Ctrl-Z

// initSKey retry — GetVersion can fail at boot if the robot is still powering up.
// Retry with exponential backoff until the robot responds.
#define SKEY_RETRY_INITIAL_MS 2000 // First retry after 2s
#define SKEY_RETRY_MAX_MS 30000 // Cap backoff at 30s

// AsyncCache TTL values (milliseconds) — how long each response is considered fresh.
// Callers within the TTL window get the cached value instantly; concurrent requests
// during an in-flight fetch are coalesced (only one serial command dispatched).
#define CACHE_TTL_STATE 2000 // GetState + GetErr — polled every 2s
#define CACHE_TTL_CHARGER 30000 // GetCharger — battery data (30s)
#define CACHE_TTL_SENSORS 1000 // Analog/digital sensors, motors (1s — fast for manual clean safety)
#define CACHE_TTL_VERSION 300000 // GetVersion — rarely changes (5 min)
#define CACHE_TTL_LDS 1500 // LIDAR scan — 1.5s (scan takes ~800ms on serial)

// Sized-spot-clean safety net: how long after a spot start to check GetState for the
// house-clean-instead mismatch. Hardware capture showed the mismatch visible on the very
// first post-start poll (~1s); this comfortably clears that plus one CACHE_TTL_STATE cycle.
#define SPOT_VERIFY_DELAY_MS 4000
// If that GetState poll itself fails (serial timeout/desync), retry rather than silently
// losing the only safety net for this clean start.
#define SPOT_VERIFY_MAX_RETRIES 3
#define SPOT_VERIFY_RETRY_DELAY_MS 1000

// Whole-house-clean early-return timer: upper bound on requested minutes. 24h covers any
// plausible clean-plus-buffer request; well clear of the ~71583-minute point where
// durationMinutes * 60000UL overflows the 32-bit unsigned long deadline math (see
// clean_timer_policy.h) and wraps into a deadline seconds away instead of days away.
#define CLEAN_TIMER_MAX_MINUTES 1440

// Manual clean safety
#define MANUAL_SAFETY_POLL_MS 500 // Poll bumpers every 500ms during manual clean
#define MANUAL_STALL_POLL_MS 500 // Poll wheel load every 500ms while wheels are moving
// Fires on RightWheel_Load% reading 60 then 61 during normal undock breakaway effort (measured
// distribution: 50,51,57,57,60,61 across one undock) — not a genuine obstruction. Left at 60
// rather than raised: the false trip is specific to undock's breakaway-friction regime, not
// free-driving, so NavigationManager::beginUndock() uses moveRelaxedStall() (skips wheel-load
// stall detection for that one bounded move; bumper/drop safety stay fully active) instead of
// weakening this threshold for every other manual/guided move.
#define MANUAL_STALL_LOAD_PCT 60 // Wheel load % threshold — above this is considered stalled
#define MANUAL_STALL_COUNT 2 // Consecutive overloaded polls before stopping (2 × 500ms = 1s grace)
#define MANUAL_STALL_CLEAR_COUNT 6 // Consecutive clean polls before auto-clearing a stall latch (3s)
#define MANUAL_CLIENT_TIMEOUT_MS 5000 // Stop wheels if no API activity (any request) within this window
#define MANUAL_BRUSH_RPM 1200 // Default brush RPM in manual mode
#define MANUAL_VACUUM_SPEED_PCT 80 // Default vacuum speed (%) in manual mode
#define MANUAL_SIDE_BRUSH_POWER_MW 1500 // Default side brush power (mW) — universal Neato Botvac default

// Manual clean cliff/drop-sensor backstop
// UNCALIBRATED — bench-verify direction (assumed higher mm = farther from floor = drop) before unattended use.
// Real capture peaked at 39mm, 1mm below this threshold — marginal, but left as-is pending a
// bench procedure rather than re-guessed (see MANUAL_STALL_LOAD_PCT above for the same principle
// applied to the constant that DID have enough data to justify a change).
#define MANUAL_DROP_THRESHOLD_MM 40 // Floor-absent threshold for DropSensorLeft/Right (mm)
#define MANUAL_DROP_COUNT 1 // Consecutive over-threshold polls before latching
#define MANUAL_DROP_FAIL_COUNT 3 // Consecutive failed/absent drop readings before treating as triggered (fail-closed)
#define MANUAL_DROP_CLEAR_COUNT                                                                                        \
    10 // Consecutive clean polls before auto-clearing a drop latch (5s) — conservative given the cost of a miss

// Command completion status (for enhanced logging)
enum CommandStatus {
    CMD_SUCCESS, // Command succeeded, response received OK
    CMD_TIMEOUT, // No complete response within timeout (may have partial data)
    CMD_PARSE_FAILED, // Got response but parse failed (not used yet)
    CMD_SERIAL_ERROR, // UART error or other serial issue (e.g. response desync)
    CMD_UNSUPPORTED, // Robot responded with "Unknown Cmd" — command not available
    CMD_QUEUE_FULL // Command rejected because the serial queue was full
};

// Timing intervals (milliseconds)
#define WIFI_RECONNECT_INTERVAL 5000
#define RESET_BUTTON_HOLD_TIME 5000 // Hold for 5 seconds to reset

// Data logger
#define LOG_MAX_FILE_SIZE 32768 // 32 KB per file before rotation
#define LOG_MAX_FS_PERCENT 60 // Delete oldest logs when log dir exceeds this share of filesystem
#define LOG_MIN_FS_PERCENT 10 // Logs always get at least this % of filesystem, even if other data fills the rest
#define LOG_MAX_FILES 50 // Maximum number of archived log files to keep
#define LOG_DIR "/log"
#define LOG_CURRENT_FILE "/log/current.jsonl"
#define LOG_FLUSH_INTERVAL_MS 30000 // Flush write buffer to filesystem every 30 seconds (reduces flash wear)
#define LOG_FLUSH_MAX_LINES 128 // Also flush when buffer reaches this many lines
#define LOG_ENFORCE_LIMITS_MS 30000 // Check log dir size/count limits every 30s (not every 50ms tick)

// NVS (Non-Volatile Storage) — single shared namespace for all settings
#define NVS_NAMESPACE "neato"

// NVS keys — WiFi
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_AP_FALLBACK "ap_fallback"

// Fallback Access Point (provisioning AP)
// SSID is derived from hostname: "<hostname>-ap". Network is open (no password).
// AP is automatic: always on when no STA credentials saved; on/off based on
// apFallbackOnDisconnect setting when STA connection drops.
#define AP_SSID_SUFFIX "-ap"
#define AP_DEFAULT_IP IPAddress(192, 168, 4, 1)
#define AP_GATEWAY IPAddress(192, 168, 4, 1)
#define AP_SUBNET IPAddress(255, 255, 255, 0)
#define AP_CHANNEL 1
#define AP_MAX_CONNECTIONS 4

// NVS keys — Time/NTP
#define NVS_KEY_TIMEZONE "tz"

// NVS keys — Settings
#define NVS_KEY_HOSTNAME "hostname"
#define NVS_KEY_LOG_LEVEL "log_level"
#define NVS_KEY_DEBUG "debug" // TODO: Remove after v0.5 — legacy key, migrated to log_level on first boot
#define LOG_LEVEL_AUTO_OFF_DEBUG_MS 600000 // Auto-revert debug -> off after 10 minutes
#define LOG_LEVEL_AUTO_OFF_INFO_MS 3600000 // Auto-revert info -> off after 1 hour

// Log levels - controls what gets written to SPIFFS. Default off to minimize
// flash wear.
#define LOG_LEVEL_OFF 0 // Nothing written to SPIFFS (default)
#define LOG_LEVEL_INFO 1 // Errors, state transitions, boot, wifi, ota, ntp, cleaning events, notifications
#define LOG_LEVEL_DEBUG 2 // Everything in Info + all serial commands + raw responses
#define NVS_KEY_WIFI_TX_POWER "wifi_tx_pwr"
#define NVS_KEY_UART_TX_PIN "uart_tx_pin"
#define NVS_KEY_UART_RX_PIN "uart_rx_pin"
// NVS keys — Cleaning
#define NVS_KEY_NAV_MODE "nav_mode" // Navigation mode: "Normal", "Gentle", "Deep", "Quick"
// NVS keys — Manual clean
#define NVS_KEY_MC_STALL_THR "mc_stall_thr"
#define NVS_KEY_MC_BRUSH_RPM "mc_brush_rpm"
#define NVS_KEY_MC_VACUUM_PCT "mc_vacuum_pct"
#define NVS_KEY_MC_SBRUSH_MW "mc_sbrush_mw"

// Maintenance tracking — accumulated cleaning-runtime seconds per consumable (see
// maintenance_tracker.h). Keys kept well under the Preferences library's 15-char limit.
#define MAINT_POLL_MS                                                                                                  \
    5000 // Poll GetState this often to accumulate cleaning runtime (piggybacks the existing state cache/poll)
#define MAINT_POLL_IDLE_MS                                                                                             \
    30000 // Relaxed cadence while motors are idle — nothing accumulates, so don't add serial traffic
#define NVS_KEY_MAINT_RUNTIME "maint_run_s" // accumulated motor-active seconds, ALL consumables share this
#define NVS_KEY_MAINT_BRUSH_RST "maint_brush_r" // runtime value at last brush reset
#define NVS_KEY_MAINT_FILTER_RST "maint_filt_r"
#define NVS_KEY_MAINT_SBRUSH_RST "maint_sbr_r"
#define NVS_KEY_MAINT_SENSOR_RST "maint_sens_r"

// Runtime-hours (not calendar days) — matches the robot's own FilterChange/BrushChange
// thresholds (docs/neato-serial-protocol.md). Not vendor-verified for the D7 specifically.
#define MAINT_BRUSH_INTERVAL_HOURS 200 // Main brush — typically 6-12 months of average use
#define MAINT_FILTER_INTERVAL_HOURS 100 // Filter clogs faster than the brush wears
#define MAINT_SIDE_BRUSH_INTERVAL_HOURS 150
#define MAINT_SENSOR_INTERVAL_HOURS 30 // Lighter-touch: "wipe cliff/LIDAR sensors" reminder, not a replacement

// NVS keys — Remote syslog
#define NVS_KEY_SYSLOG_ENABLED "syslog_on"
#define NVS_KEY_SYSLOG_IP "syslog_ip"
#define SYSLOG_DEFAULT_PORT 514
#define SYSLOG_PRI "<14>" // facility=user (1), severity=info (6) -> 1*8+6=14

// NVS keys — Notifications
#define NVS_KEY_NTFY_TOPIC "ntfy_topic"
#define NVS_KEY_NTFY_ENABLED "ntfy_enabled"
#define NVS_KEY_NTFY_ON_DONE "ntfy_on_done"
#define NVS_KEY_NTFY_ON_ERR "ntfy_on_err"
#define NVS_KEY_NTFY_ON_ALERT "ntfy_on_alrt"
#define NVS_KEY_NTFY_ON_DOCK "ntfy_on_dock"

// NVS keys — Schedule (ESP32-managed, not robot serial)
#define NVS_KEY_SCHED_ENABLED "sched_on"
#define NVS_KEY_AUTO_RESTART_ENABLED "auto_rst_on"
#define NVS_KEY_AUTO_RESTART_HOUR "auto_rst_h"
#define NVS_KEY_AUTO_RESTART_MIN "auto_rst_m"
#define NVS_KEY_RESTART_BEFORE_CLEAN "rst_b4_clean"
// Per-day keys use suffix: "s0h","s0m","s0on" .. "s6h","s6m","s6on" (Mon=0..Sun=6); slot
// mode/guided-session/guided-zones use "md"/"ssn"/"zn", slot 1 adds trailing "1" ("s0md1").
// Built programmatically in SettingsManager — no individual defines needed.
#define SCHEDULE_DAYS 7
#define SCHEDULE_SLOTS_PER_DAY 2 // Two time slots per day (e.g. morning + afternoon)
#define SCHEDULE_CHECK_INTERVAL_MS 30000 // Check schedule against NTP time every 30s
#define SCHEDULE_WINDOW_MINS 5 // Fire if current time is 0..N minutes after scheduled slot
#define RESTART_BOOT_TIMEOUT_MS 120000 // Max wait time for robot to boot after restart (2 min)

// Unattended-run gate for scheduled guided cleans; checked only in Scheduler::triggerGuidedClean(),
// not for interactive/attended starts.
#define NVS_KEY_SCHED_GUIDED_ARM "gsched_arm"
// Stricter than the mid-route NAV_MGR_LOW_BATTERY_PCT pause threshold — avoids starting an
// unattended route that immediately pauses and redocks.
#define NAV_MGR_SCHED_MIN_START_BATTERY_PCT 30

// Notification manager — adaptive polling intervals
#define NOTIF_INTERVAL_ACTIVE_MS 3000 // Check state every 3s when robot is active (cleaning/docking)
#define NOTIF_INTERVAL_IDLE_MS 30000 // Check state every 30s when robot is idle

// Cleaning history
#define HISTORY_INTERVAL_IDLE_MS 30000 // Poll state every 30s when idle (detect cleaning start)
#define HISTORY_INTERVAL_ACTIVE_MS 2000 // Poll state/pose every 2s during active cleaning (~0.6m resolution at 300mm/s)
#define HISTORY_FLUSH_INTERVAL_MS 30000 // Flush buffered pose snapshots to disk every 30 seconds
#define HISTORY_COMPRESS_INTERVAL_MS 50 // Fast tick during post-session compression (512B/tick)
#define HISTORY_DIR "/history" // SPIFFS directory for session files
#define HISTORY_MAX_FS_PERCENT 50 // Delete oldest sessions when history dir exceeds this share of filesystem
#define HISTORY_MIN_FS_PERCENT 10 // History always gets at least this % of filesystem
#define HISTORY_MAX_FILES 20 // Maximum number of archived session files to keep
#define HISTORY_AREA_CELL_M 0.5f // Coarse grid cell size in meters for visited-area estimation
#define HISTORY_MIN_SNAPSHOTS 3 // Discard sessions with fewer snapshots (too short to render a useful map)
#define HISTORY_IMPORT_MAX_BYTES 262144 // 256 KB max import file size (2h clean at 2s intervals ~ 180KB)

// Zones (no-go lines / cleaning zones) — per-session opaque geometry, stored verbatim (#72).
#define ZONES_DIR "/zones" // SPIFFS directory for per-session zone/no-go geometry blobs
#define ZONES_MAX_BYTES 16384 // 16 KB cap per session's zone geometry blob
#define WEB_BODY_MAX_BYTES                                                                                             \
    32768 // Max accumulated request body for a multi-segment loggedBodyRoute PUT/POST; per-route caps (e.g.
          // ZONES_MAX_BYTES) still apply once the body is assembled

// Navigation — shared "rotate then drive straight toward a waypoint" tunables, used by both
// NavigationPoc and NavigationManager. x/y are robot-world meters, theta is degrees.
// UNVALIDATED — retune all NAV_* below against a real D7.
#define NAV_POLL_MS                                                                                                    \
    2500 // Poll GetRobotPos Smooth this often while navigating (mirrors HISTORY_INTERVAL_ACTIVE_MS cadence)
#define NAV_ARRIVAL_THRESHOLD_M 0.15f // Waypoint considered reached within this radius
#define NAV_HEADING_TOLERANCE_DEG 10.0f // Skip the rotate step if heading error is already within this many degrees
#define NAV_DRIVE_SPEED_MMS                                                                                            \
    120 // Wheel speed for a full-length drive step. Capped by the 500ms safety poll: ~77mm of travel
        // after contact, vs ~128mm at 200.
#define NAV_ROTATE_SPEED_MMS 100 // Wheel speed for a full-length (NAV_MAX_ROTATE_STEP_DEG) rotate
// Careful-driving spec §4.3: scale speed down for a shortened/final step rather than always
// commanding full speed then hard-stopping on arrival. Floors, not absolutes -- PROVISIONAL.
#define NAV_DRIVE_MIN_SPEED_MMS 80 // Floor for a near-zero-length drive step (never crawl to 0)
#define NAV_ROTATE_MIN_SPEED_MMS 40 // Floor for a small heading-correction rotate
#define NAV_MAX_ROTATE_STEP_DEG 90.0f // Cap a single rotate-in-place command (avoids one big blind turn overshooting)
#define NAV_MAX_DRIVE_STEP_M 0.5f // Cap a single drive-straight command so position is re-polled and corrected often
#define NAV_TRACK_WIDTH_MM                                                                                             \
    248 // Wheel separation (from docs/neato-serial-protocol.md) — converts a turn angle to differential wheel arc
        // length
#define NAV_ROTATE_SIGN 1 // Flip to -1 if a positive heading error rotates the wrong way on real hardware
#define NAV_MAX_WAYPOINTS 500 // Reject a /api/navigate waypoint array larger than this (memory/sanity cap)
#define NAV_STALE_POSE_MS 8000 // Abort if no successful GetRobotPos fix arrives within this window (don't drive blind)

// NavigationManager — Guided Clean planning/battery tunables (session load, zone filter,
// mid-route recharge pause); NAV_POLL_MS etc. above are shared with NavigationPoc.
#define NAV_MGR_DOWNSAMPLE_MIN_DIST_M                                                                                  \
    0.3f // Keep a recorded pose as a waypoint only if it's at least this far from the last kept one
#define NAV_MGR_MAX_WAYPOINTS 300 // Cap the downsampled waypoint plan
#define NAV_MGR_UNDOCK_DIST_M                                                                                          \
    0.3f // Fixed forward move off the dock before waypoint following begins (direction UNVALIDATED — see
         // navigation_manager.cpp)
#define NAV_MGR_MAX_REROUTES                                                                                           \
    5 // Consecutive local reroutes (bumper/stall/no-go skip) allowed before giving up (GUIDED_ERROR)
// Escape maneuver (careful-driving spec §3) -- reverse + turn-away on a physical block, before
// geometric reroute. Separate budget from NAV_MGR_MAX_REROUTES: an escape buys physical
// clearance, a reroute picks where to go next -- a successful escape still consumes a reroute too.
#define NAV_MGR_MAX_ESCAPE_ATTEMPTS 1 // One try per block; doesn't clear it -> hand back to reroute-exhaustion path
#define NAV_MGR_ESCAPE_REVERSE_MM 150 // PROVISIONAL, needs supervised real-run tuning (spec §5/§7)
#define NAV_MGR_ESCAPE_ROTATE_DEG 30 // PROVISIONAL, needs supervised real-run tuning (spec §5/§7)
#define NAV_MGR_BATTERY_CHECK_MS 15000 // How often to poll GetCharger while a guided route is active
#define NAV_MGR_LOW_BATTERY_PCT 20 // End the guided run when battery drops to/below this percent
#define NAV_MGR_MAX_ODOM_PATH_M                                                                                        \
    50.0f // PROVISIONAL, supervised runs only. Gates plan-time truncation (not refusal) and the live abort.
          // No drift measurement backs this number yet — revisit once measured.
#define NAV_MGR_MAX_ODOM_DURATION_MS 600000 // Wall-clock backstop (10 min) for pathological reroute/correction cycling
#define NAV_MGR_ORIGIN_TOLERANCE_M                                                                                     \
    0.3f // Session's first waypoint must be within this of (0,0) — matches NAV_MGR_DOWNSAMPLE_MIN_DIST_M
#define NAV_MGR_WAYPOINT_SKIP_RADIUS_M                                                                                 \
    0.45f // Skip leading waypoints within this of the post-undock pose (NAV_MGR_UNDOCK_DIST_M plus a
          // NAV_ARRIVAL_THRESHOLD_M margin for pose-estimate slop) so the route doesn't rotate back onto the dock

// LIDAR proximity veto (careful-driving spec §2) — coarse "something is close ahead" check,
// scanned only in the already-stationary poll gap before a drive step, never mid-drive (a
// GetLDSScan blacks out the serial link for ~800ms-3000ms — see CACHE_TTL_LDS). ALL PROVISIONAL,
// pending the bench measurements in the spec's hardware-verification checklist.
#define NAV_LDS_FRONT_OFFSET_DEG 0 // UNVERIFIED: raw scan index -> heading-relative angle offset
#define NAV_LDS_DRIVE_SECTOR_HALF_DEG 20 // Half-width of the forward sector checked before a drive step
#define NAV_LDS_MIN_VALID_SAMPLES 5 // Below this many non-error/non-zero samples, treat the sector as unreliable
#define NAV_LDS_FOOTPRINT_RADIUS_MM 200 // UNVERIFIED: robot footprint radius, rounded up from a commonly-cited spec
#define NAV_LDS_STOP_MARGIN_MM 80 // Extra clearance margin beyond the footprint radius
#define NAV_LDS_MIN_USEFUL_STEP_MM 50 // A shortened step below this is treated as Stop instead of a near-zero move

// Task Watchdog Timer (TWDT) — hardware watchdog that resets the ESP32 if
// loop() stops running (deadlock, infinite loop, blocking I/O). The main task
// must call esp_task_wdt_reset() every iteration; if it misses the deadline,
// the TWDT triggers a system reset. This complements the heap watchdog below
// which only catches memory exhaustion (and requires loop() to keep running).
#define TASK_WDT_TIMEOUT_S                                                                                             \
    15 // Seconds before TWDT triggers reset (generous
       // to accommodate slow filesystem operations and
       // LIDAR scans that can take several seconds)

// Heap watchdog — restart if free heap stays below threshold for this duration.
// Prevents the device from becoming unresponsive when memory is exhausted
// (e.g. by runaway async connections after a UART desync cascade).
#define HEAP_WATCHDOG_THRESHOLD 16384 // 16 KB — below this is critical
#define HEAP_WATCHDOG_DURATION_MS 30000 // Must stay low for 30s to trigger

// NTP / time sync
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
#define NTP_DEFAULT_TZ "UTC0" // POSIX TZ string, stored in NVS

// Logging — enabled by default, disable with -DENABLE_LOGGING=0
#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING 1
#endif

#if ENABLE_LOGGING
#define LOG(tag, fmt, ...) Serial.printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
#define LOG(tag, fmt, ...)
#endif

#endif // CONFIG_H
