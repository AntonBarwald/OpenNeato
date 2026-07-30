import { useState } from "preact/hooks";
import { T, useI18n } from "../i18n";

// Meaningful sizes only — the robot clamps Width/Height to 100-400cm anyway, so a
// segmented control (not a stepper) matches the actual range with no dead interaction.
const SPOT_SIZES_CM = [100, 200, 300, 400];
const DEFAULT_SIZE_CM = 200;

const WIDTH_KEY = "spotCleanWidthCm";
const HEIGHT_KEY = "spotCleanHeightCm";

function loadSize(key: string): number {
    const raw = Number(localStorage.getItem(key));
    return SPOT_SIZES_CM.includes(raw) ? raw : DEFAULT_SIZE_CM;
}

interface SpotSizeSheetProps {
    onStart: (widthCm: number, heightCm: number) => void;
    onCancel: () => void;
}

export function SpotSizeSheet({ onStart, onCancel }: SpotSizeSheetProps) {
    const { t } = useI18n();
    const [widthCm, setWidthCm] = useState(() => loadSize(WIDTH_KEY));
    const [heightCm, setHeightCm] = useState(() => loadSize(HEIGHT_KEY));

    const maxCm = SPOT_SIZES_CM[SPOT_SIZES_CM.length - 1];
    const previewW = (widthCm / maxCm) * 100;
    const previewH = (heightCm / maxCm) * 100;

    const handleStart = () => {
        localStorage.setItem(WIDTH_KEY, String(widthCm));
        localStorage.setItem(HEIGHT_KEY, String(heightCm));
        onStart(widthCm, heightCm);
    };

    return (
        <div class="confirm-overlay" role="dialog" aria-modal="true" onClick={onCancel}>
            <div class="confirm-dialog spot-size-dialog primary" onClick={(e) => e.stopPropagation()}>
                <div class="confirm-message">
                    <T>Spot clean size</T>
                </div>

                <div class="spot-size-picker">
                    <div class="spot-size-row">
                        <span class="spot-size-label">
                            <T>Width</T>
                        </span>
                        <div class="spot-size-group">
                            {SPOT_SIZES_CM.map((size) => (
                                <button
                                    key={size}
                                    type="button"
                                    class={`spot-size-btn${size === widthCm ? " active" : ""}`}
                                    onClick={() => setWidthCm(size)}
                                >
                                    {size / 100} m
                                </button>
                            ))}
                        </div>
                    </div>
                    <div class="spot-size-row">
                        <span class="spot-size-label">
                            <T>Height</T>
                        </span>
                        <div class="spot-size-group">
                            {SPOT_SIZES_CM.map((size) => (
                                <button
                                    key={size}
                                    type="button"
                                    class={`spot-size-btn${size === heightCm ? " active" : ""}`}
                                    onClick={() => setHeightCm(size)}
                                >
                                    {size / 100} m
                                </button>
                            ))}
                        </div>
                    </div>
                </div>

                <div class="spot-preview-wrap">
                    <div class="spot-preview-frame">
                        <div class="spot-preview-box" style={{ width: `${previewW}%`, height: `${previewH}%` }}>
                            <span class="spot-preview-dot" />
                        </div>
                    </div>
                </div>
                <div class="spot-preview-note">
                    <T>
                        This covers the area around and ahead of wherever the robot is standing — exact placement
                        varies. Carry it to the middle of the area you want cleaned, then start.
                    </T>
                </div>

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
