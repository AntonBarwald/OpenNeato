import { useCallback, useState } from "preact/hooks";
import { api } from "../../api";
import { ConfirmDialog } from "../../components/confirm-dialog";
import type { ErrorStackHandle } from "../../components/error-banner";
import { usePolling } from "../../hooks/use-polling";
import { T, useI18n } from "../../i18n";
import type { MaintenanceData } from "../../types";
import { normalizeError } from "../../utils";

interface MaintenanceCardProps {
    firmwareSupported: boolean;
    errorStack: ErrorStackHandle;
}

type MaintenanceItemKey = "brush" | "filter" | "sideBrush" | "sensors";

interface MaintenanceRow {
    key: MaintenanceItemKey;
    label: string;
    hours: number;
    interval: number;
}

function rowsFromData(data: MaintenanceData): MaintenanceRow[] {
    return [
        { key: "brush", label: "Main brush", hours: data.brushHours, interval: data.brushIntervalHours },
        { key: "filter", label: "Filter", hours: data.filterHours, interval: data.filterIntervalHours },
        { key: "sideBrush", label: "Side brush", hours: data.sideBrushHours, interval: data.sideBrushIntervalHours },
        { key: "sensors", label: "Sensors", hours: data.sensorHours, interval: data.sensorIntervalHours },
    ];
}

// Reuses the amber/red warning-color convention already established by
// battColor() in dashboard.tsx, rather than inventing a new color scale.
function maintColor(hours: number, interval: number): string {
    if (interval <= 0) return "green";
    const ratio = hours / interval;
    if (ratio >= 1) return "red";
    if (ratio >= 0.8) return "amber";
    return "green";
}

export function MaintenanceCard({ firmwareSupported, errorStack }: MaintenanceCardProps) {
    const { t, formatNumber } = useI18n();
    const maintPoll = usePolling<MaintenanceData>(api.getMaintenance, 30000);

    const [confirmItem, setConfirmItem] = useState<MaintenanceItemKey | null>(null);
    const [resettingItem, setResettingItem] = useState<MaintenanceItemKey | null>(null);

    const handleReset = useCallback(
        (item: MaintenanceItemKey) => {
            setConfirmItem(null);
            setResettingItem(item);
            api.resetMaintenanceItem(item)
                .catch((e: unknown) => {
                    errorStack.push(normalizeError(e, "Failed to reset maintenance item"));
                })
                .finally(() => setResettingItem(null));
        },
        [errorStack],
    );

    const data = maintPoll.data;
    const rows = data ? rowsFromData(data) : [];

    return (
        <>
            <div class="settings-section">
                <div class="settings-battery-card">
                    <div class="settings-battery-header">
                        <div>
                            <div class="settings-battery-title">
                                <T>Consumables</T>
                            </div>
                            <div class="settings-battery-desc">
                                <T>Estimated wear based on accumulated cleaning runtime, not a certified gauge</T>
                            </div>
                        </div>
                    </div>

                    {data ? (
                        <div class="settings-maint-rows">
                            {rows.map((row) => {
                                const color = maintColor(row.hours, row.interval);
                                const pct = row.interval > 0 ? Math.min(100, (row.hours / row.interval) * 100) : 0;
                                const isResetting = resettingItem === row.key;
                                return (
                                    <div class="settings-maint-row" key={row.key}>
                                        <div class="settings-maint-row-info">
                                            <div class="settings-maint-row-label">{t(row.label)}</div>
                                            <div class={`settings-maint-row-value ${color}`}>
                                                {formatNumber(row.hours, { maximumFractionDigits: 0 })} /{" "}
                                                {formatNumber(row.interval, { maximumFractionDigits: 0 })} {t("hrs")}
                                            </div>
                                        </div>
                                        <div class="settings-maint-progress">
                                            <div
                                                class={`settings-maint-progress-bar ${color}`}
                                                style={{ width: `${pct}%` }}
                                            />
                                        </div>
                                        <button
                                            type="button"
                                            class={`settings-maint-reset-btn${isResetting ? " pending" : ""}`}
                                            onClick={() => setConfirmItem(row.key)}
                                            disabled={isResetting || !firmwareSupported}
                                        >
                                            {t(isResetting ? "Resetting..." : "Reset")}
                                        </button>
                                    </div>
                                );
                            })}
                        </div>
                    ) : (
                        <div class="settings-battery-empty">
                            {maintPoll.error ?? <T>Loading maintenance data...</T>}
                        </div>
                    )}
                </div>
            </div>

            {confirmItem && (
                <ConfirmDialog
                    message={t("Mark the {item} as replaced? This resets its usage counter.", {
                        item: t(rows.find((r) => r.key === confirmItem)?.label ?? ""),
                    })}
                    confirmLabel={t("Reset")}
                    destructive={false}
                    onConfirm={() => handleReset(confirmItem)}
                    onCancel={() => setConfirmItem(null)}
                />
            )}
        </>
    );
}
