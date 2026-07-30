import type { JSX } from "preact";
import { useCallback, useEffect, useRef, useState } from "preact/hooks";
import { api } from "../api";
import backSvg from "../assets/icons/back.svg?raw";
import houseSvg from "../assets/icons/house.svg?raw";
import spotSvg from "../assets/icons/spot.svg?raw";
import { ConfirmDialog } from "../components/confirm-dialog";
import { ErrorBanner, ErrorBannerStack, useErrorStack } from "../components/error-banner";
import { Icon } from "../components/icon";
import { TimeInput } from "../components/time-input";
import { useDirtyGuard } from "../hooks/use-dirty-guard";
import { useFetch } from "../hooks/use-fetch";
import { T, useI18n } from "../i18n";
import type { SettingsData, SystemData } from "../types";
import { fmtTime, normalizeError, parseTime } from "../utils";
import { findCurrentTzAbbrev, findPresetLabel } from "./settings/helpers";

const DAY_NAMES = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
const SLOTS_PER_DAY = 2;

// Retired NVS mode value — Guided Clean is no longer schedulable, but firmware still
// accepts it, so existing installs may have a slot persisted with this value.
const SCHED_MODE_GUIDED_LEGACY = 2;

type SchedMode = "house" | "spot";

interface SlotState {
    hour: number;
    minute: number;
    on: boolean;
    mode: SchedMode;
}

interface DayState {
    slots: SlotState[];
}

type SchedDay = 0 | 1 | 2 | 3 | 4 | 5 | 6;

function slotPrefix(day: number, slotIndex: number): string {
    return slotIndex === 0 ? `sched${day}` : `sched${day}Slot${slotIndex}`;
}

function modeFromNum(n: number | undefined): SchedMode {
    if (n === 1) return "spot";
    return "house";
}

function modeToNum(mode: SchedMode): number {
    if (mode === "spot") return 1;
    return 0;
}

// Reads one slot's 3 flattened SettingsData fields given its key prefix ("sched0" for slot
// 0, "sched0Slot1" for slot 1) — mirrors the existing hour/min/on flattening convention.
function readSlot(s: SettingsData, prefix: string): SlotState {
    return {
        hour: (s[`${prefix}Hour` as keyof SettingsData] as number) ?? 0,
        minute: (s[`${prefix}Min` as keyof SettingsData] as number) ?? 0,
        on: (s[`${prefix}On` as keyof SettingsData] as boolean) ?? false,
        mode: modeFromNum(s[`${prefix}Mode` as keyof SettingsData] as number | undefined),
    };
}

function readDays(s: SettingsData): DayState[] {
    const days: DayState[] = [];
    for (let d = 0; d < 7; d++) {
        const day = d as SchedDay;
        const slot0 = readSlot(s, slotPrefix(day, 0));
        const slot1 = readSlot(s, slotPrefix(day, 1));
        if (!slot0.on) slot1.on = false;
        days.push({ slots: [slot0, slot1] });
    }
    return days;
}

interface LegacyGuidedSlot {
    day: number;
    slotIndex: number;
}

// Guided Clean is retired from the UI, but firmware still accepts and fires mode=2
// (settings_manager.cpp's validation wasn't tightened), so an install that armed a guided
// slot before this shipped would otherwise keep running it unattended with no UI left to
// see or disarm it. Reads the raw mode ints directly since SlotState/modeFromNum no longer
// have a "guided" case to read them into.
function findLegacyGuidedSlots(s: SettingsData): LegacyGuidedSlot[] {
    const found: LegacyGuidedSlot[] = [];
    for (let day = 0; day < 7; day++) {
        for (let slotIndex = 0; slotIndex < SLOTS_PER_DAY; slotIndex++) {
            const mode = s[`${slotPrefix(day, slotIndex)}Mode` as keyof SettingsData] as number | undefined;
            if (mode === SCHED_MODE_GUIDED_LEGACY) found.push({ day, slotIndex });
        }
    }
    return found;
}

function normalizeDays(days: DayState[]): DayState[] {
    return days.map((day) => {
        const [slot0, slot1] = day.slots;
        return {
            slots: [slot0, { ...slot1, on: slot0.on && slot1.on }],
        };
    });
}

function daysToDrafts(days: DayState[]): string[][] {
    return days.map((d) => d.slots.map((s) => fmtTime(s.hour, s.minute)));
}

function buildSchedulePatch(days: DayState[], server: DayState[]): Partial<SettingsData> {
    const patch: Record<string, number | boolean> = {};
    for (let d = 0; d < 7; d++) {
        for (let s = 0; s < SLOTS_PER_DAY; s++) {
            const cur = days[d].slots[s];
            const srv = server[d].slots[s];
            const prefix = slotPrefix(d, s);
            if (cur.hour !== srv.hour) patch[`${prefix}Hour`] = cur.hour;
            if (cur.minute !== srv.minute) patch[`${prefix}Min`] = cur.minute;
            if (cur.on !== srv.on) patch[`${prefix}On`] = cur.on;
            if (cur.mode !== srv.mode) patch[`${prefix}Mode`] = modeToNum(cur.mode);
        }
    }
    return patch as Partial<SettingsData>;
}

function tzLabel(tz: string, isDst?: boolean): string {
    const abbrev = isDst !== undefined ? findCurrentTzAbbrev(tz, isDst) : null;
    const preset = findPresetLabel(tz);
    if (abbrev && preset) {
        // Extract the active UTC offset from the preset label
        const dstMatch = preset.match(/\(UTC([^)]+)\/([^)]+)\)/);
        const offset = dstMatch ? `UTC${isDst ? dstMatch[2] : dstMatch[1]}` : null;
        if (!offset) {
            const stdMatch = preset.match(/\(UTC([^)]+)\)/);
            return stdMatch ? `UTC${stdMatch[1]}` : abbrev;
        }
        return `${abbrev}, ${offset}`;
    }
    if (preset) return preset;
    const match = tz.match(/^([A-Z]{2,5})/);
    return match ? match[1] : tz;
}

// Validate all enabled time drafts. Returns set of "day-slot" keys that are invalid.
function validateDrafts(days: DayState[], drafts: string[][]): Set<string> {
    const invalid = new Set<string>();
    for (let d = 0; d < 7; d++) {
        for (let s = 0; s < SLOTS_PER_DAY; s++) {
            if (days[d].slots[s].on && !parseTime(drafts[d][s])) {
                invalid.add(`${d}-${s}`);
            }
        }
    }
    return invalid;
}

// Apply parsed draft times back into day state (only for enabled, valid slots)
function applyDrafts(days: DayState[], drafts: string[][]): DayState[] {
    return normalizeDays(
        days.map((day, d) => ({
            slots: day.slots.map((slot, s) => {
                if (!slot.on) return slot;
                const parsed = parseTime(drafts[d][s]);
                if (!parsed) return slot;
                return { ...slot, hour: parsed.hour, minute: parsed.minute };
            }),
        })),
    );
}

const MODE_ICONS: Record<SchedMode, string> = { house: houseSvg, spot: spotSvg };
const MODE_LABELS: Record<SchedMode, string> = { house: "House", spot: "Spot" };

interface SlotModeToggleProps {
    mode: SchedMode;
    dayLabel: string;
    onChange: (mode: SchedMode) => void;
}

// Compact 2-way segmented control.
function SlotModeToggle({ mode, dayLabel, onChange }: SlotModeToggleProps) {
    const { t } = useI18n();
    return (
        <div class="sched-mode-toggle">
            {(Object.keys(MODE_ICONS) as SchedMode[]).map((m) => (
                <button
                    type="button"
                    key={m}
                    class={`sched-mode-btn${mode === m ? " active" : ""}`}
                    onClick={() => onChange(m)}
                    aria-label={t("{day}: {mode} clean", { day: dayLabel, mode: t(MODE_LABELS[m]) })}
                    title={t(MODE_LABELS[m])}
                >
                    <Icon svg={MODE_ICONS[m]} />
                </button>
            ))}
        </div>
    );
}

export function ScheduleView() {
    const { t, formatSystemTime } = useI18n();
    const [errors, errorStack] = useErrorStack();
    const [saving, setSaving] = useState(false);

    const { data: settings, loading, error: fetchError } = useFetch(api.getSettings);
    const { data: system } = useFetch<SystemData>(api.getSystem);

    const [enabled, setEnabled] = useState(false);
    const [tz, setTz] = useState("UTC0");
    const [days, setDays] = useState<DayState[]>(() =>
        Array.from({ length: 7 }, () => ({
            slots: Array.from({ length: SLOTS_PER_DAY }, () => ({
                hour: 0,
                minute: 0,
                on: false,
                mode: "house" as SchedMode,
            })),
        })),
    );

    // Draft strings for time inputs (free-form text, validated at save)
    const [drafts, setDrafts] = useState<string[][]>(() => Array.from({ length: 7 }, () => ["00:00", "00:00"]));

    // Set of "day-slot" keys with validation errors (populated at save time)
    const [invalidSlots, setInvalidSlots] = useState<Set<string>>(new Set());

    // Set once this load found and disabled a retired guided slot. Naturally one-time: the
    // migrating PATCH clears the persisted mode=2, so a later reload finds nothing left to
    // migrate and this never gets set again.
    const [legacyGuidedNotice, setLegacyGuidedNotice] = useState(false);

    // Server-confirmed state for dirty detection
    const serverDays = useRef<DayState[]>(days);
    const serverEnabled = useRef(false);

    const applySettings = useCallback((res: SettingsData) => {
        const d = normalizeDays(readDays(res));
        setEnabled(res.scheduleEnabled);
        serverEnabled.current = res.scheduleEnabled;
        setTz(res.tz);
        setDays(d);
        setDrafts(daysToDrafts(d));
        serverDays.current = d;
        setInvalidSlots(new Set());
    }, []);

    useEffect(() => {
        if (!settings) return;
        const legacySlots = findLegacyGuidedSlots(settings);
        if (legacySlots.length === 0) {
            applySettings(settings);
            return;
        }
        // Retired Guided Clean would otherwise keep firing unattended on schedule — force
        // the slot off and disarm the gate together, then apply whatever the server confirms.
        const patch: Record<string, number | boolean> = { guidedScheduleArmed: false };
        for (const { day, slotIndex } of legacySlots) {
            const prefix = slotPrefix(day, slotIndex);
            patch[`${prefix}Mode`] = 0;
            patch[`${prefix}On`] = false;
        }
        api.saveSchedule(patch as Partial<SettingsData>)
            .then((res) => {
                applySettings(res);
                setLegacyGuidedNotice(true);
            })
            .catch((e: unknown) => {
                errorStack.push(normalizeError(e, "Failed to disable a retired Guided Clean schedule slot"));
            });
    }, [settings, applySettings, errorStack]);

    useEffect(() => {
        if (fetchError) errorStack.push(fetchError);
    }, [fetchError]);

    // Dirty = toggle changed, or any draft text differs from server
    const isDirty = enabled !== serverEnabled.current || !draftsMatchDays(drafts, days, serverDays.current);

    const { guardedGoBack, showDiscardConfirm, setShowDiscardConfirm, handleDiscard } = useDirtyGuard(isDirty);

    // Local-only state changes (no API call)
    const updateSlot = useCallback((day: number, slot: number, patch: Partial<SlotState>) => {
        setDays((cur) =>
            normalizeDays(
                cur.map((d, i) =>
                    i === day ? { slots: d.slots.map((s, si) => (si === slot ? { ...s, ...patch } : s)) } : d,
                ),
            ),
        );
        // When adding slot 2, seed draft with default time
        if (patch.on === true && patch.hour !== undefined) {
            setDrafts((cur) =>
                cur.map((r, i) =>
                    i === day ? r.map((v, si) => (si === slot ? fmtTime(patch.hour ?? 0, patch.minute ?? 0) : v)) : r,
                ),
            );
        }
    }, []);

    const updateDraft = useCallback((day: number, slot: number, value: string) => {
        setDrafts((cur) => cur.map((r, i) => (i === day ? r.map((v, si) => (si === slot ? value : v)) : r)));
        // Clear validation error for this slot on edit
        setInvalidSlots((cur) => {
            const key = `${day}-${slot}`;
            if (!cur.has(key)) return cur;
            const next = new Set(cur);
            next.delete(key);
            return next;
        });
    }, []);

    const handleModeChange = useCallback(
        (day: number, slot: number, mode: SchedMode) => {
            updateSlot(day, slot, { mode });
        },
        [updateSlot],
    );

    // Single batched save with validation
    const handleSave = useCallback(() => {
        // Apply drafts to get final day state
        const finalDays = applyDrafts(days, drafts);

        // Validate all enabled slots
        const invalid = validateDrafts(finalDays, drafts);
        if (invalid.size > 0) {
            setInvalidSlots(invalid);
            errorStack.push(t("Fix invalid times before saving (use HH:MM format, 00:00-23:59)"));
            return;
        }

        const patch: Partial<SettingsData> = {
            ...buildSchedulePatch(finalDays, serverDays.current),
        };
        if (enabled !== serverEnabled.current) {
            patch.scheduleEnabled = enabled;
        }

        setSaving(true);
        setInvalidSlots(new Set());
        api.saveSchedule(patch)
            .then((res) => {
                const d = readDays(res);
                serverDays.current = d;
                serverEnabled.current = res.scheduleEnabled;
                setDays(d);
                setDrafts(daysToDrafts(d));
                setEnabled(res.scheduleEnabled);
            })
            .catch((e: unknown) => {
                errorStack.push(normalizeError(e, "Failed to save schedule"));
            })
            .finally(() => setSaving(false));
    }, [days, drafts, enabled, errorStack, t]);

    const onKeyDown = useCallback((e: JSX.TargetedKeyboardEvent<HTMLInputElement>) => {
        if (e.key === "Enter") {
            e.currentTarget.blur();
        }
    }, []);

    const renderSlot = (day: number, slotIndex: number, slot: SlotState) => (
        <div class="sched-slot-block">
            <div class="sched-slot-row">
                <TimeInput
                    class={`sched-time-input${invalidSlots.has(`${day}-${slotIndex}`) ? " invalid" : ""}`}
                    value={drafts[day][slotIndex]}
                    maxLength={5}
                    placeholder={t("HH:MM")}
                    onInput={(v) => updateDraft(day, slotIndex, v)}
                    onKeyDown={onKeyDown}
                />
                <SlotModeToggle
                    mode={slot.mode}
                    dayLabel={t(DAY_NAMES[day])}
                    onChange={(mode) => handleModeChange(day, slotIndex, mode)}
                />
                {slotIndex === 1 && (
                    <button
                        type="button"
                        class="sched-remove-btn"
                        onClick={() => updateSlot(day, 1, { on: false })}
                        aria-label={t("Remove {day} second slot", { day: t(DAY_NAMES[day]) })}
                    >
                        x
                    </button>
                )}
            </div>
        </div>
    );

    return (
        <>
            <div class="header">
                <button
                    type="button"
                    class="header-back-btn"
                    onClick={() => guardedGoBack("/settings")}
                    aria-label={t("Back")}
                >
                    <Icon svg={backSvg} />
                </button>
                <h1>
                    <T>Schedule</T>
                </h1>
                <div class="header-right-spacer" />
            </div>

            <ErrorBannerStack errors={errors} />

            <div class="schedule-page">
                {loading ? (
                    <div class="schedule-loading">
                        <T>Loading schedule...</T>
                    </div>
                ) : (
                    <>
                        {/* Master toggle */}
                        <div class="schedule-master">
                            <div class="settings-toggle-label">
                                <span class="settings-toggle-title">
                                    <T>Schedule enabled</T>
                                </span>
                                <span class="settings-toggle-desc">
                                    <T>Automatically clean on set days and times</T>
                                </span>
                            </div>
                            <button
                                type="button"
                                class={`settings-toggle${enabled ? " on" : ""}`}
                                onClick={() => setEnabled((v) => !v)}
                                aria-label={t("Toggle schedule")}
                            />
                        </div>

                        {legacyGuidedNotice && (
                            <ErrorBanner
                                variant="warning"
                                title={t("Guided Clean retired")}
                                message={t(
                                    "A scheduled Guided clean was disabled — Guided Clean has been retired and is no longer available.",
                                )}
                                onDismiss={() => setLegacyGuidedNotice(false)}
                            />
                        )}

                        <div class="schedule-tz-hint">
                            {system?.localTime
                                ? `${formatSystemTime(system.localTime)} - ${tzLabel(tz, system.isDst)}`
                                : t("Times are in {timezone}", { timezone: tzLabel(tz) })}
                        </div>

                        {/* Day rows */}
                        <div class="schedule-days">
                            {days.map((day, i) => {
                                const s0 = day.slots[0];
                                const s1 = day.slots[1];

                                return (
                                    <div key={i} class="sched-row">
                                        <button
                                            type="button"
                                            class={`schedule-day-toggle${s0.on ? " on" : ""}`}
                                            onClick={() => updateSlot(i, 0, s0.on ? { on: false } : { on: true })}
                                            aria-label={t("Toggle {day}", { day: t(DAY_NAMES[i]) })}
                                        />
                                        <span class={`sched-day-label${s0.on ? "" : " off"}`}>{t(DAY_NAMES[i])}</span>

                                        <div class="sched-slots">
                                            {s0.on && renderSlot(i, 0, s0)}

                                            {s0.on && s1.on && renderSlot(i, 1, s1)}
                                            {s0.on && !s1.on && (
                                                <button
                                                    type="button"
                                                    class="sched-add-btn"
                                                    onClick={() => updateSlot(i, 1, { on: true, hour: 15, minute: 0 })}
                                                    aria-label={t("Add {day} second slot", { day: t(DAY_NAMES[i]) })}
                                                >
                                                    +
                                                </button>
                                            )}
                                        </div>
                                    </div>
                                );
                            })}
                        </div>

                        {/* Save button */}
                        <button
                            type="button"
                            class={`settings-save-btn${saving ? " pending" : ""}`}
                            onClick={handleSave}
                            disabled={saving || !isDirty}
                        >
                            {t(saving ? "Saving..." : "Save")}
                        </button>
                    </>
                )}
            </div>

            {showDiscardConfirm && (
                <ConfirmDialog
                    message={t("You have unsaved changes. Discard them?")}
                    confirmLabel={t("Discard")}
                    onConfirm={handleDiscard}
                    onCancel={() => setShowDiscardConfirm(false)}
                />
            )}
        </>
    );
}

// Check if drafts differ from server state (accounts for toggle changes + text edits + mode)
function draftsMatchDays(drafts: string[][], days: DayState[], server: DayState[]): boolean {
    for (let d = 0; d < 7; d++) {
        for (let s = 0; s < SLOTS_PER_DAY; s++) {
            const cur = days[d].slots[s];
            const srv = server[d].slots[s];
            // Toggle state changed
            if (cur.on !== srv.on) return false;
            if (cur.mode !== srv.mode) return false;
            // For enabled slots, check if draft text resolves to a different time
            if (cur.on) {
                const parsed = parseTime(drafts[d][s]);
                if (!parsed) return false; // unparseable = dirty (will fail validation)
                if (parsed.hour !== srv.hour || parsed.minute !== srv.minute) return false;
            }
        }
    }
    return true;
}
