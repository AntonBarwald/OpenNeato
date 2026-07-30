import { useCallback, useEffect, useRef, useState } from "preact/hooks";
import { api } from "../api";
import alertSvg from "../assets/icons/alert.svg?raw";
import boltSvg from "../assets/icons/bolt.svg?raw";
import checkSvg from "../assets/icons/check.svg?raw";
import clockSvg from "../assets/icons/clock.svg?raw";
import databaseSvg from "../assets/icons/database.svg?raw";
import dockSvg from "../assets/icons/dock.svg?raw";
import gearSvg from "../assets/icons/gear.svg?raw";
import historySvg from "../assets/icons/history.svg?raw";
import houseSvg from "../assets/icons/house.svg?raw";
import idleSvg from "../assets/icons/idle.svg?raw";
import manualSvg from "../assets/icons/manual.svg?raw";
import pauseSvg from "../assets/icons/pause.svg?raw";
import playSvg from "../assets/icons/play.svg?raw";
import sparkleSvg from "../assets/icons/sparkle.svg?raw";
import spotSvg from "../assets/icons/spot.svg?raw";
import stopSvg from "../assets/icons/stop.svg?raw";
import tagSvg from "../assets/icons/tag.svg?raw";
import vacuumSvg from "../assets/icons/vacuum.svg?raw";
import wifiSvg from "../assets/icons/wifi.svg?raw";
import wifiOffSvg from "../assets/icons/wifi-off.svg?raw";
import robotSvg from "../assets/robot.svg?raw";
import { BatteryIcon } from "../components/battery-icon";
import { ErrorBanner, ErrorBannerStack, useErrorStack } from "../components/error-banner";
import { Icon } from "../components/icon";
import { useNavigate } from "../components/router";
import { SpotSizeSheet } from "../components/spot-size-sheet";
import { WholeHouseTimerSheet } from "../components/whole-house-timer-sheet";
import type { PollResult } from "../hooks/use-polling";
import { usePolling } from "../hooks/use-polling";
import { T, useI18n } from "../i18n";
import type {
    ChargerData,
    CleanTimerStatus,
    ErrorData,
    FirmwareVersion,
    SettingsData,
    StateData,
    SystemData,
} from "../types";
import type { UpdateInfo } from "../update";
import { deriveRobotError, findRecordingSession, normalizeError } from "../utils";

// -- Helpers --

interface StatusInfo {
    label: string;
    color: string;
    icon: string;
}

function statusInfo(s: string): StatusInfo {
    if (s.includes("CLEANINGRUNNING")) return { label: "Cleaning", color: "green", icon: "sparkle" };
    if (s.includes("CLEANINGPAUSED")) return { label: "Paused", color: "amber", icon: "alert" };
    if (s.includes("CLEANINGSUSPENDED")) return { label: "Recharging", color: "amber", icon: "bolt" };
    if (s.includes("MANUALCLEANING")) return { label: "Cleaning", color: "green", icon: "sparkle" };
    if (s.includes("DOCKING")) return { label: "Docking", color: "amber", icon: "bolt" };
    return { label: "Active", color: "green", icon: "check" };
}

const STATUS_ICONS: Record<string, string> = {
    check: checkSvg,
    sparkle: sparkleSvg,
    alert: alertSvg,
    bolt: boltSvg,
    manual: manualSvg,
};

const MODE_ICONS: Record<string, string> = {
    idle: idleSvg,
    house: houseSvg,
    spot: spotSvg,
    bolt: boltSvg,
    alert: alertSvg,
    manual: manualSvg,
};

function modeInfo(
    charging: boolean,
    docked: boolean,
    isSpot: boolean,
    isCleaning: boolean,
    isManual: boolean,
): StatusInfo {
    if (isManual) return { label: "Manual", color: "blue", icon: "manual" };
    if (charging) return { label: "Charging", color: "amber", icon: "bolt" };
    if (docked) return { label: "Docked", color: "amber", icon: "bolt" };
    if (isSpot) return { label: "Spot", color: "blue", icon: "spot" };
    if (isCleaning) return { label: "House", color: "blue", icon: "house" };
    return { label: "Idle", color: "green", icon: "idle" };
}

function battColor(pct: number): string {
    if (pct <= 25) return "red";
    if (pct <= 50) return "amber";
    return "green";
}

function wifiStrength(rssi: number): string {
    if (rssi >= -50) return "Excellent";
    if (rssi >= -60) return "Good";
    if (rssi >= -70) return "Fair";
    return "Weak";
}

const LOCAL_TIME_DAY_TO_SCHED_DAY: Record<string, number> = {
    Mon: 0,
    Tue: 1,
    Wed: 2,
    Thu: 3,
    Fri: 4,
    Sat: 5,
    Sun: 6,
};

function readSlot(settings: SettingsData, day: number, slot: number) {
    const prefix = slot === 0 ? `sched${day}` : `sched${day}Slot${slot}`;
    return {
        hour: (settings[`${prefix}Hour` as keyof SettingsData] as number) ?? 0,
        minute: (settings[`${prefix}Min` as keyof SettingsData] as number) ?? 0,
        on: (settings[`${prefix}On` as keyof SettingsData] as boolean) ?? false,
    };
}

function readDaySlots(settings: SettingsData, day: number) {
    const slot0 = readSlot(settings, day, 0);
    const slot1 = readSlot(settings, day, 1);
    if (!slot0.on) slot1.on = false;
    return [slot0, slot1];
}

function formatSchedTime(hour: number, minute: number): string {
    return `${String(hour).padStart(2, "0")}:${String(minute).padStart(2, "0")}`;
}

function nextScheduleLabel(settings: SettingsData, localTime: string, t: (text: string) => string): string | null {
    if (!settings.scheduleEnabled) return null;

    const match = localTime.match(/^(Sun|Mon|Tue|Wed|Thu|Fri|Sat) (\d{2}):(\d{2})(?::\d{2})?$/);
    if (!match) return null;

    const currentDay = LOCAL_TIME_DAY_TO_SCHED_DAY[match[1]];
    const currentMinutes = Number.parseInt(match[2], 10) * 60 + Number.parseInt(match[3], 10);

    for (let dayOffset = 0; dayOffset <= 7; dayOffset++) {
        const day = (currentDay + dayOffset) % 7;
        const slots = readDaySlots(settings, day)
            .filter((slot) => slot.on)
            .sort((a, b) => a.hour * 60 + a.minute - (b.hour * 60 + b.minute));

        for (const slot of slots) {
            const slotMinutes = slot.hour * 60 + slot.minute;
            if (dayOffset === 0 && slotMinutes < currentMinutes) continue;

            const when =
                dayOffset === 0
                    ? t("Today")
                    : dayOffset === 1
                      ? t("Tomorrow")
                      : t(["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"][day]);
            return `${when} ${formatSchedTime(slot.hour, slot.minute)}`;
        }
    }

    return null;
}

// -- Dashboard view --

interface DashboardViewProps {
    firmware: PollResult<FirmwareVersion>;
    state: PollResult<StateData>;
    error: PollResult<ErrorData>;
    isManual: boolean;
    updateInfo: UpdateInfo | null;
    robotReady: boolean;
    identifying: boolean;
}

export function DashboardView({
    firmware,
    state,
    error,
    isManual,
    updateInfo,
    robotReady,
    identifying,
}: DashboardViewProps) {
    const { t, formatSystemTime } = useI18n();
    const navigate = useNavigate();
    const charger = usePolling<ChargerData>(api.getCharger, 5000);
    const settings = usePolling<SettingsData>(api.getSettings, 30000);
    const system = usePolling<SystemData>(api.getSystem, 10000);

    const connErr = state.error && charger.error;
    const hasData = state.data || charger.data;
    const offline = connErr && !hasData;

    const si = state.data
        ? statusInfo(state.data.uiState)
        : { label: state.error ? "Error" : "...", color: state.error ? "red" : "amber", icon: "alert" };

    // Pending state — disabled until backend confirms state change or timeout
    const [pending, setPending] = useState(false);
    const [modeChooser, setModeChooser] = useState(false);
    const lastUiState = useRef<string | null>(null);
    const pendingTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
    const pendingManual = useRef(false);
    const [actionErrors, actionErrorStack] = useErrorStack();
    const [openingLiveMap, setOpeningLiveMap] = useState(false);
    const [spotSizeSheet, setSpotSizeSheet] = useState(false);
    const [wholeHouseTimerSheet, setWholeHouseTimerSheet] = useState(false);

    if (state.data && state.data.uiState !== lastUiState.current) {
        lastUiState.current = state.data.uiState;
        if (pending) {
            setPending(false);
            if (pendingTimer.current) {
                clearTimeout(pendingTimer.current);
                pendingTimer.current = null;
            }
        }
    }

    // Navigate to manual page only after polled state confirms MANUALCLEANING
    useEffect(() => {
        if (isManual && pendingManual.current) {
            pendingManual.current = false;
            navigate("/manual");
        }
    }, [isManual, navigate]);

    // One-shot fetch (not a recurring poll) to find the live recording
    // session's filename, only needed at the moment the user taps this.
    const handleViewLiveMap = useCallback(() => {
        setOpeningLiveMap(true);
        api.getHistoryList()
            .then((files) => {
                const recording = findRecordingSession(files);
                navigate(recording ? `/history/${recording.name}` : "/history");
            })
            .catch(() => navigate("/history"))
            .finally(() => setOpeningLiveMap(false));
    }, [navigate]);

    const handleAction = useCallback(
        (action: () => Promise<unknown>) => {
            setPending(true);
            if (pendingTimer.current) clearTimeout(pendingTimer.current);
            pendingTimer.current = setTimeout(() => {
                setPending(false);
                pendingManual.current = false;
                pendingTimer.current = null;
            }, 10000);
            action().catch((e: unknown) => {
                setPending(false);
                pendingManual.current = false;
                if (pendingTimer.current) {
                    clearTimeout(pendingTimer.current);
                    pendingTimer.current = null;
                }
                actionErrorStack.push(normalizeError(e, "Action failed"));
            });
        },
        [actionErrorStack],
    );

    const isRunning = state.data?.uiState?.includes("CLEANINGRUNNING") ?? false;
    const isPaused = state.data?.uiState?.includes("CLEANINGPAUSED") ?? false;
    const isDocking = state.data?.uiState?.includes("DOCKING") ?? false;
    const isSuspended = state.data?.uiState?.includes("CLEANINGSUSPENDED") ?? false;
    const isCleaning = isRunning || isPaused || isSuspended;
    const isSpot = state.data?.uiState?.includes("SPOT") ?? false;
    const robotError = deriveRobotError(error);
    const hasRobotError = robotError?.kind === "error";

    // Whole-house early-return timer status, for the "docks in N min" hint on the live-map banner.
    const cleanTimer = usePolling<CleanTimerStatus>(api.getCleanTimerStatus, isCleaning ? 5000 : 0);

    // Best-effort: a stale armed timer must not outlive the run it was set for
    // (see web_server.cpp's /api/clean-timer comment) — fire on any action that
    // starts a different run or ends the current one; failure isn't fatal.
    const cancelTimerBestEffort = useCallback(() => {
        api.cancelCleanTimer().catch(() => {});
    }, []);

    // Close the mode chooser if the robot's state no longer supports it
    // (e.g. it starts erroring or the connection drops) while it's open.
    useEffect(() => {
        if (modeChooser && (!robotReady || offline || isDocking || isManual || hasRobotError)) {
            setModeChooser(false);
        }
    }, [modeChooser, robotReady, offline, isDocking, isManual, hasRobotError]);

    const charging = charger.data?.chargingActive ?? false;
    const docked = charger.data?.extPwrPresent ?? false;
    const pct = charger.data?.fuelPercent ?? 0;
    const bc = charger.data ? battColor(pct) : charger.error ? "red" : "amber";
    const modeErr = (!state.data && state.error) || (!charger.data && charger.error);
    const mi = modeErr
        ? { label: "Error", color: "red", icon: "alert" }
        : modeInfo(charging, docked, isSpot, isCleaning, isManual);
    const nextSchedule =
        settings.data?.scheduleEnabled && system.data?.localTime
            ? nextScheduleLabel(settings.data, system.data.localTime, t)
            : null;

    return (
        <>
            {/* Header */}
            <div class="header">
                <h1>
                    OpenNeato
                    {firmware.data?.hostname && <span class="header-hostname"> ({firmware.data.hostname})</span>}
                </h1>
                <div class="header-btns">
                    <button
                        type="button"
                        class="header-right-btn header-history-btn"
                        onClick={() => navigate("/history")}
                        disabled={!robotReady}
                    >
                        <Icon svg={historySvg} />
                        <T>History</T>
                    </button>
                    <button
                        type="button"
                        class="header-right-btn"
                        aria-label={t("Settings")}
                        onClick={() => navigate("/settings")}
                    >
                        <Icon svg={gearSvg} />
                    </button>
                </div>
            </div>

            {/* Status bar */}
            {system.data && !offline && (
                <div class="status-bar">
                    <div class="status-bar-item">
                        <div class="status-bar-label">
                            <T>WiFi</T>
                        </div>
                        <div class="status-bar-value">
                            <Icon svg={wifiSvg} />
                            {wifiStrength(system.data.rssi)}
                        </div>
                    </div>
                    <div class="status-bar-item">
                        <div class="status-bar-label">
                            <T>Time</T>
                        </div>
                        <div class="status-bar-value">
                            <Icon svg={clockSvg} />
                            {formatSystemTime(system.data.localTime)}
                        </div>
                    </div>
                    <div class="status-bar-item">
                        <div class="status-bar-label">
                            <T>Storage</T>
                        </div>
                        <div class="status-bar-value">
                            <Icon svg={databaseSvg} />
                            {Math.round((system.data.fsUsed / system.data.fsTotal) * 100)}%
                        </div>
                    </div>
                    {firmware.data && (
                        <div class="status-bar-item">
                            <div class="status-bar-label">
                                <T>Firmware</T>
                            </div>
                            <div class="status-bar-value">
                                <Icon svg={tagSvg} />
                                {firmware.data.version}
                            </div>
                        </div>
                    )}
                </div>
            )}

            {/* Update notification */}
            {updateInfo && (
                <a class="update-banner" href={updateInfo.url} target="_blank" rel="noopener noreferrer">
                    <Icon svg={tagSvg} />
                    {t("Update available: v{version} - tap to view release", { version: updateInfo.version })}
                </a>
            )}

            {/* Robot error/warning — fixed, clears automatically when robot resolves it */}
            {robotError && (
                <ErrorBanner
                    title={t(robotError.title)}
                    message={robotError.message}
                    hint={robotError.hint}
                    variant={robotError.kind}
                />
            )}
            {!error.data && error.error && !connErr && <ErrorBanner title={t("Warning")} message={error.error} />}

            {/* Action errors — dismissible, stackable */}
            <ErrorBannerStack errors={actionErrors} />

            {isCleaning && (
                <button type="button" class="schedule-banner" onClick={handleViewLiveMap} disabled={openingLiveMap}>
                    <Icon svg={vacuumSvg} />
                    <span>
                        {t(openingLiveMap ? "Opening..." : "Cleaning in progress - view live map")}
                        {cleanTimer.data?.armed &&
                            ` · ${t("docks in {min} min", { min: Math.max(1, Math.ceil(cleanTimer.data.remainingSec / 60)) })}`}
                    </span>
                </button>
            )}

            {settings.data?.scheduleEnabled && (
                <button type="button" class="schedule-banner" onClick={() => navigate("/schedule")}>
                    <Icon svg={clockSvg} />
                    <span>
                        {nextSchedule
                            ? t("Next clean: {time}", { time: nextSchedule })
                            : t("Schedule enabled - tap to view")}
                    </span>
                </button>
            )}

            {/* Hero area — robot right, cards left */}
            {!robotReady ? (
                <div class="hero-area gate-hero">
                    <div class="robot-float gate-robot">
                        <Icon svg={robotSvg} />
                    </div>
                    {identifying ? (
                        <p class="gate-message">
                            <T>Connecting to robot...</T>
                        </p>
                    ) : (
                        <div class="gate-message">
                            <Icon svg={alertSvg} />
                            <h2>
                                <T>Unsupported Robot</T>
                            </h2>
                            <p>
                                <T>OpenNeato requires a Neato Botvac D3, D4, D5, D6, or D7.</T>
                                <br />
                                <T>The connected robot could not be identified.</T>
                            </p>
                        </div>
                    )}
                </div>
            ) : offline ? (
                <div class="conn-error">
                    <Icon svg={wifiOffSvg} />
                    <T>Unable to reach robot</T>
                </div>
            ) : (
                <div class="hero-area">
                    <div class="robot-float">
                        <Icon svg={robotSvg} />
                    </div>

                    <div class="info-cards">
                        <div class="info-card">
                            <div class="info-card-left">
                                <div class="info-card-label">
                                    <T>Status</T>
                                </div>
                                <div class={`info-card-value ${si.color}`}>{t(si.label)}</div>
                            </div>
                            <div class={`info-card-icon ${si.color}`}>
                                <Icon svg={STATUS_ICONS[si.icon]} />
                            </div>
                        </div>

                        <div class="info-card">
                            <div class="info-card-left">
                                <div class="info-card-label">
                                    <T>Battery</T>
                                </div>
                                <div class={`info-card-value ${bc}`}>
                                    {charger.data ? `${pct}%` : charger.error ? "Error" : "..."}
                                </div>
                            </div>
                            <div class={`info-card-icon ${charger.error && !charger.data ? "red" : ""}`}>
                                {charger.error && !charger.data ? <Icon svg={alertSvg} /> : <BatteryIcon pct={pct} />}
                            </div>
                        </div>

                        <div class="info-card">
                            <div class="info-card-left">
                                <div class="info-card-label">
                                    <T>Mode</T>
                                </div>
                                <div class={`info-card-value ${mi.color}`}>{t(mi.label)}</div>
                            </div>
                            <div class={`info-card-icon ${mi.color}`}>
                                <Icon svg={MODE_ICONS[mi.icon]} />
                            </div>
                        </div>
                    </div>
                </div>
            )}

            {/* Bottom action bar — 2 or 3 buttons depending on state */}
            <div class="action-bar">
                <div class="action-bar-row">
                    {isCleaning ? (
                        <>
                            {/* Cleaning: Pause/Resume, Dock, Stop */}
                            <button
                                type="button"
                                class={`action-btn primary${pending ? " pending" : ""}`}
                                onClick={() => handleAction(isPaused ? api.cleanHouse : api.cleanPause)}
                                disabled={!robotReady || offline || pending}
                            >
                                <Icon svg={isPaused ? playSvg : pauseSvg} />
                                {t(isPaused ? "Resume" : "Pause")}
                            </button>
                            <button
                                type="button"
                                class={`action-btn${pending ? " pending" : ""}`}
                                onClick={() =>
                                    handleAction(() => {
                                        cancelTimerBestEffort();
                                        return api.cleanDock();
                                    })
                                }
                                disabled={!robotReady || offline || pending}
                            >
                                <Icon svg={dockSvg} />
                                <T>Dock</T>
                            </button>
                            <button
                                type="button"
                                class={`action-btn${pending ? " pending" : ""}`}
                                onClick={() =>
                                    handleAction(() => {
                                        cancelTimerBestEffort();
                                        return api.cleanStop();
                                    })
                                }
                                disabled={!robotReady || offline || pending}
                            >
                                <Icon svg={stopSvg} />
                                <T>Stop</T>
                            </button>
                        </>
                    ) : isManual ? (
                        <>
                            {/* Manual session active: reopen the joystick view, or exit manual mode */}
                            <button
                                type="button"
                                class="action-btn primary"
                                onClick={() => navigate("/manual")}
                                disabled={!robotReady || !!offline}
                            >
                                <Icon svg={manualSvg} />
                                <T>Manual</T>
                            </button>
                            <button
                                type="button"
                                class={`action-btn${pending ? " pending" : ""}`}
                                onClick={() => handleAction(() => api.manual(false))}
                                disabled={!robotReady || offline || pending}
                            >
                                <Icon svg={stopSvg} />
                                <T>Stop</T>
                            </button>
                        </>
                    ) : (
                        <>
                            {/* Idle: Start (chooser), Home. While docking, Home flips to Stop. */}
                            <button
                                type="button"
                                class={`action-btn primary${pending ? " pending" : ""}`}
                                onClick={() => setModeChooser(true)}
                                disabled={!robotReady || offline || isDocking || pending || hasRobotError}
                            >
                                <Icon svg={playSvg} />
                                <T>Start</T>
                            </button>
                            <button
                                type="button"
                                class={`action-btn${pending ? " pending" : ""}`}
                                onClick={() =>
                                    handleAction(() => {
                                        cancelTimerBestEffort();
                                        return isDocking ? api.cleanStop() : api.cleanDock();
                                    })
                                }
                                disabled={
                                    !robotReady ||
                                    offline ||
                                    pending ||
                                    (!isDocking && (hasRobotError || docked || charging))
                                }
                            >
                                <Icon svg={isDocking ? stopSvg : houseSvg} />
                                {t(isDocking ? "Stop" : "Home")}
                            </button>
                        </>
                    )}
                </div>
            </div>

            {modeChooser && (
                <div class="confirm-overlay" role="dialog" aria-modal="true" onClick={() => setModeChooser(false)}>
                    <div class="confirm-dialog mode-chooser-dialog" onClick={(e) => e.stopPropagation()}>
                        <div class="confirm-message">
                            <T>Start cleaning</T>
                        </div>
                        <div class="mode-chooser-list">
                            <button
                                type="button"
                                class="mode-chooser-row primary"
                                onClick={() => {
                                    setModeChooser(false);
                                    setWholeHouseTimerSheet(true);
                                }}
                            >
                                <span class="mode-chooser-row-icon">
                                    <Icon svg={vacuumSvg} />
                                </span>
                                <span class="mode-chooser-row-text">
                                    <span class="mode-chooser-row-title">
                                        <T>Whole house</T>
                                    </span>
                                    <span class="mode-chooser-row-desc">
                                        <T>Clean every room, then return to dock</T>
                                    </span>
                                </span>
                            </button>
                            <button
                                type="button"
                                class="mode-chooser-row"
                                onClick={() => {
                                    setModeChooser(false);
                                    setSpotSizeSheet(true);
                                }}
                            >
                                <span class="mode-chooser-row-icon">
                                    <Icon svg={spotSvg} />
                                </span>
                                <span class="mode-chooser-row-text">
                                    <span class="mode-chooser-row-title">
                                        <T>Spot</T>
                                    </span>
                                    <span class="mode-chooser-row-desc">
                                        <T>Clean a small area around the robot</T>
                                    </span>
                                </span>
                            </button>
                            <button
                                type="button"
                                class="mode-chooser-row"
                                onClick={() => {
                                    setModeChooser(false);
                                    handleAction(() => {
                                        cancelTimerBestEffort();
                                        pendingManual.current = true;
                                        return api.manual(true);
                                    });
                                }}
                            >
                                <span class="mode-chooser-row-icon">
                                    <Icon svg={manualSvg} />
                                </span>
                                <span class="mode-chooser-row-text">
                                    <span class="mode-chooser-row-title">
                                        <T>Manual</T>
                                    </span>
                                    <span class="mode-chooser-row-desc">
                                        <T>Drive the robot yourself</T>
                                    </span>
                                </span>
                            </button>
                        </div>
                    </div>
                </div>
            )}

            {spotSizeSheet && (
                <SpotSizeSheet
                    onCancel={() => setSpotSizeSheet(false)}
                    onStart={(widthCm, heightCm) => {
                        setSpotSizeSheet(false);
                        handleAction(() => {
                            cancelTimerBestEffort();
                            return api.cleanSpot(widthCm, heightCm);
                        });
                    }}
                />
            )}

            {wholeHouseTimerSheet && (
                <WholeHouseTimerSheet
                    onCancel={() => setWholeHouseTimerSheet(false)}
                    onStart={(stopAfterMin) => {
                        setWholeHouseTimerSheet(false);
                        handleAction(() => {
                            if (stopAfterMin === null) {
                                cancelTimerBestEffort();
                                return api.cleanHouse();
                            }
                            return api.cleanHouse().then(() => api.armCleanTimer(stopAfterMin));
                        });
                    }}
                />
            )}
        </>
    );
}
