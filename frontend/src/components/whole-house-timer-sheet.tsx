import { useState } from "preact/hooks";
import { T, useI18n } from "../i18n";

// Meaningful presets only, same rationale as SpotSizeSheet's segmented sizes.
const TIMER_PRESETS_MIN = [15, 30, 45, 60, 90];
const DEFAULT_MINUTES = 30;

const MODE_KEY = "wholeHouseTimerMode";
const MINUTES_KEY = "wholeHouseTimerMinutes";

type TimerMode = "none" | "minutes";

function loadMode(): TimerMode {
    return localStorage.getItem(MODE_KEY) === "minutes" ? "minutes" : "none";
}

function loadMinutes(): number {
    const raw = Number(localStorage.getItem(MINUTES_KEY));
    return TIMER_PRESETS_MIN.includes(raw) ? raw : DEFAULT_MINUTES;
}

interface WholeHouseTimerSheetProps {
    onStart: (stopAfterMin: number | null) => void; // null = run until done
    onCancel: () => void;
}

export function WholeHouseTimerSheet({ onStart, onCancel }: WholeHouseTimerSheetProps) {
    const { t } = useI18n();
    const [mode, setMode] = useState<TimerMode>(() => loadMode());
    const [minutes, setMinutes] = useState(() => loadMinutes());

    const handleStart = () => {
        localStorage.setItem(MODE_KEY, mode);
        localStorage.setItem(MINUTES_KEY, String(minutes));
        onStart(mode === "minutes" ? minutes : null);
    };

    return (
        <div class="confirm-overlay" role="dialog" aria-modal="true" onClick={onCancel}>
            <div class="confirm-dialog whole-house-timer-dialog primary" onClick={(e) => e.stopPropagation()}>
                <div class="confirm-message">
                    <T>Whole house clean</T>
                </div>

                <div class="timer-mode-group">
                    <button
                        type="button"
                        class={`timer-mode-btn${mode === "none" ? " active" : ""}`}
                        onClick={() => setMode("none")}
                    >
                        <T>Run until done</T>
                    </button>
                    <button
                        type="button"
                        class={`timer-mode-btn${mode === "minutes" ? " active" : ""}`}
                        onClick={() => setMode("minutes")}
                    >
                        <T>Stop after</T>
                    </button>
                </div>

                {mode === "minutes" && (
                    <div class="timer-min-group">
                        {TIMER_PRESETS_MIN.map((preset) => (
                            <button
                                key={preset}
                                type="button"
                                class={`timer-min-btn${preset === minutes ? " active" : ""}`}
                                onClick={() => setMinutes(preset)}
                            >
                                {t("{count} min", { count: preset })}
                            </button>
                        ))}
                    </div>
                )}

                <div class="confirm-actions">
                    <button type="button" class="confirm-btn cancel" onClick={onCancel}>
                        {t("Cancel")}
                    </button>
                    <button type="button" class="confirm-btn primary" onClick={handleStart}>
                        {t("Start")}
                    </button>
                </div>
            </div>
        </div>
    );
}
