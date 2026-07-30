import { useCallback, useEffect, useMemo, useRef, useState } from "preact/hooks";
import boltSvg from "../../assets/icons/bolt.svg?raw";
import rotateLeftSvg from "../../assets/icons/rotate-left.svg?raw";
import rotateRightSvg from "../../assets/icons/rotate-right.svg?raw";
import { Icon } from "../../components/icon";
import { useMapGestures } from "../../hooks/use-map-gestures";
import { type DrawTool, useNoGoEditor } from "../../hooks/use-nogo-editor";
import { T, useI18n } from "../../i18n";
import type { HistoryFileInfo, MapData } from "../../types";
import type { Point } from "./geometry";
import { renderMap, type ZoneShape } from "./helpers";
import { Wave } from "./loading-wave";
import { MotionPlayer } from "./motion-player";

interface HistoryItemViewProps {
    file: HistoryFileInfo;
    map: MapData | null;
    mapEmpty: boolean;
    recording: boolean;
    // Guided Clean no-go lines and zones saved against this session (issue
    // #73), lifted to the parent history view so unsaved-edit dirty
    // tracking and the discard-on-navigate guard live in one place.
    zonesReady: boolean;
    nogoLines: Point[][];
    zones: ZoneShape[];
    zonesDirty: boolean;
    savingZones: boolean;
    onAddNoGoLine: (points: Point[]) => void;
    onDeleteNoGoLine: (index: number) => void;
    onAddZone: (points: Point[]) => void;
    onDeleteZone: (index: number) => void;
    onUpdateNoGoLine: (index: number, points: Point[]) => void;
    onUpdateZone: (index: number, points: Point[]) => void;
    onRenameZone: (index: number, label: string) => void;
    onSaveZones: () => void;
    onDiscardZones: () => void;
}

// Persisted map rotation, in degrees. Always normalized to one of 0/90/180/270.
function loadRotation(): number {
    const raw = Number(localStorage.getItem("mapRotation"));
    if (!Number.isFinite(raw)) return 0;
    return (((Math.round(raw / 90) * 90) % 360) + 360) % 360;
}

export function HistoryItemView({
    file,
    map,
    mapEmpty,
    recording,
    zonesReady,
    nogoLines,
    zones,
    zonesDirty,
    savingZones,
    onAddNoGoLine,
    onDeleteNoGoLine,
    onAddZone,
    onDeleteZone,
    onUpdateNoGoLine,
    onUpdateZone,
    onRenameZone,
    onSaveZones,
    onDiscardZones,
}: HistoryItemViewProps) {
    const { t, formatDuration, formatNumber } = useI18n();
    const canvasRef = useRef<HTMLCanvasElement>(null);
    const [rotation, setRotation] = useState<number>(loadRotation);
    const transform = useMapGestures(canvasRef, rotation);

    // Enforcement preview toggle — greys out recorded path hops that cross
    // any no-go line, so drawing a line gives an immediate before/after.
    const [enforcePreview, setEnforcePreview] = useState(false);

    // Disabled while a save is in flight so a draw made during the round trip can't be
    // silently clobbered when the save response resets the draft.
    const editorEnabled = zonesReady && !recording && map !== null && !mapEmpty && !savingZones;
    const zonePolys = useMemo(() => zones.map((z) => ({ points: z.points })), [zones]);
    const editor = useNoGoEditor({
        canvasRef,
        bounds: map?.bounds ?? null,
        transform,
        rotation,
        nogoLines,
        zones: zonePolys,
        onAddNoGoLine,
        onDeleteNoGoLine,
        onAddZone,
        onDeleteZone,
        onUpdateNoGoLine,
        onUpdateZone,
        enabled: editorEnabled,
        resetKey: file.name,
    });

    useEffect(() => {
        localStorage.setItem("mapRotation", String(rotation));
    }, [rotation]);

    const rotateBy = useCallback((delta: number) => {
        setRotation((r) => (((r + delta) % 360) + 360) % 360);
    }, []);

    // Expose the live canvas element so MotionPlayer can drive the renderer
    // without duplicating gesture handling or the ref plumbing.
    const [canvasEl, setCanvasEl] = useState<HTMLCanvasElement | null>(null);
    useEffect(() => {
        setCanvasEl(canvasRef.current);
    }, [map]);

    // True while the wave is on screen — covers both the loading phase
    // and the brief carrier-driven reveal phase. Flips to false when the
    // carrier wave's trailing edge clears the canvas, at which point the
    // wave hands the canvas back to the motion player / static render.
    const [revealing, setRevealing] = useState<boolean>(true);

    // Single Wave instance lives across the loading -> revealing
    // transition so in-flight idle pulses carry over into the reveal
    // phase rather than being torn down and replaced. We create it once
    // on mount; a separate effect calls startReveal() when map arrives.
    const waveRef = useRef<Wave | null>(null);
    useEffect(() => {
        const canvas = canvasRef.current;
        if (!canvas) return;
        const wave = new Wave({ canvas });
        waveRef.current = wave;
        let canceled = false;
        wave.done.then(() => {
            if (!canceled) setRevealing(false);
        });
        return () => {
            canceled = true;
            wave.cancel();
            waveRef.current = null;
        };
    }, []);

    // Kick the reveal phase the first time map data arrives. The wave
    // keeps its existing in-flight idle pulses; the carrier joins them
    // as one extra pulse instead of replacing the rhythm. Subsequent
    // map updates (e.g. recording-session polling) are ignored — the
    // reveal animation runs once.
    const revealStartedRef = useRef(false);
    useEffect(() => {
        if (revealStartedRef.current) return;
        if (!map || map.path.length === 0) return;
        const wave = waveRef.current;
        if (!wave) return;
        wave.startReveal(map, transform, rotation);
        revealStartedRef.current = true;
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [map]);

    // Motion playback mounts as soon as a finished session's data is
    // present. While `revealing` is true its canvas effects are
    // suspended via the `canvasSuspended` prop — controls render and
    // are interactive, but it doesn't fight the wave for the canvas.
    const showPlayer = !recording && map !== null && map.path.length > 0;

    // Editor overlay state for renderMap, shared between the static-render fallback and MotionPlayer.
    const overlay = useMemo(
        () => ({
            zones,
            draftZone: editor.draftZone,
            selectedNoGoIndex: editor.selection?.kind === "nogo" ? editor.selection.index : null,
            selectedZoneIndex: editor.selection?.kind === "zone" ? editor.selection.index : null,
            enforcePreview,
            dragNoGoPoints: editor.vertexDragPreview?.kind === "nogo" ? editor.vertexDragPreview.points : null,
            dragZonePoints: editor.vertexDragPreview?.kind === "zone" ? editor.vertexDragPreview.points : null,
        }),
        [zones, editor.draftZone, editor.selection, enforcePreview, editor.vertexDragPreview],
    );

    // Static render fallback — only when the player is not present
    // (recording sessions). The player handles its own canvas draws when
    // it owns them. We also skip while `revealing` so the wave keeps the
    // canvas to itself.
    useEffect(() => {
        if (showPlayer) return;
        if (revealing) return;
        if (map && canvasRef.current) {
            renderMap(
                canvasRef.current,
                map,
                recording,
                transform,
                undefined,
                rotation,
                nogoLines,
                editor.draftLine,
                overlay,
            );
        }
    }, [map, recording, transform, showPlayer, revealing, rotation, nogoLines, editor.draftLine, overlay]);

    useEffect(() => {
        if (showPlayer) return;
        if (revealing) return;
        if (!map) return;
        const handleResize = () => {
            if (map && canvasRef.current)
                renderMap(
                    canvasRef.current,
                    map,
                    recording,
                    transform,
                    undefined,
                    rotation,
                    nogoLines,
                    editor.draftLine,
                    overlay,
                );
        };
        window.addEventListener("resize", handleResize);
        return () => window.removeEventListener("resize", handleResize);
    }, [map, recording, transform, showPlayer, revealing, rotation, nogoLines, editor.draftLine, overlay]);

    // Prefer list metadata summary (available immediately), fall back to
    // the summary parsed from the full JSONL data (available after fetch)
    const summary = file.summary ?? map?.summary ?? null;
    const session = file.session ?? map?.session ?? null;

    // The selected zone's index in view mode, so a rename field can be shown
    // for it — null whenever nothing is selected, a no-go line is selected
    // instead, or we're in draw mode (selection is cleared on mode switch).
    const selectedZoneIndex =
        editor.mode === "view" && editor.selection?.kind === "zone" ? editor.selection.index : null;

    const toolLabel = (tool: DrawTool): string => {
        if (tool === "nogo") return "No-go line";
        if (tool === "zone-rect") return "Zone (rectangle)";
        return "Zone (polygon)";
    };

    return (
        <>
            {/* Summary bar */}
            {summary && (
                <div class="history-detail-stats">
                    <div class="history-stat">
                        <span class="history-stat-label">
                            <T>Duration</T>
                        </span>
                        <span class="history-stat-value">{formatDuration(summary.duration)}</span>
                    </div>
                    <div class="history-stat">
                        <span class="history-stat-label">
                            <T>Distance</T>
                        </span>
                        <span class="history-stat-value">
                            {`${formatNumber(summary.distanceTraveled, {
                                minimumFractionDigits: 1,
                                maximumFractionDigits: 1,
                            })} m`}
                        </span>
                    </div>
                    <div class="history-stat">
                        <span class="history-stat-label">
                            <T>Area</T>
                        </span>
                        <span class="history-stat-value">
                            {`${formatNumber(summary.areaCovered, {
                                minimumFractionDigits: 1,
                                maximumFractionDigits: 1,
                            })} m²`}
                        </span>
                    </div>
                    <div class="history-stat">
                        <span class="history-stat-label">
                            <T>Battery</T>
                        </span>
                        <span class="history-stat-value">
                            {session?.battery ?? "?"}% &rarr; {summary.batteryEnd ?? "?"}%
                        </span>
                    </div>
                    {summary.recharges > 0 && (
                        <div class="history-stat">
                            <span class="history-stat-label">
                                <T>Recharges</T>
                            </span>
                            <span class="history-stat-value">{summary.recharges}</span>
                        </div>
                    )}
                </div>
            )}

            {/* Guided Clean editor toolbar — draw/select no-go lines and
                zones, preview enforcement, save or discard edits. Hidden
                until the session's map and saved zones have both loaded. */}
            {editorEnabled && (
                <div class="nogo-toolbar">
                    <div class="nogo-toolbar-row">
                        <button
                            type="button"
                            class={`nogo-mode-btn${editor.mode === "draw" ? " on" : ""}`}
                            onClick={() => editor.setMode(editor.mode === "draw" ? "view" : "draw")}
                        >
                            <T>Draw</T>
                        </button>
                        {editor.mode === "draw" && (
                            <select
                                class="nogo-tool-select"
                                value={editor.tool}
                                onChange={(e) => editor.setTool((e.target as HTMLSelectElement).value as DrawTool)}
                                aria-label={t("Drawing tool")}
                            >
                                <option value="nogo">{t(toolLabel("nogo"))}</option>
                                <option value="zone-rect">{t(toolLabel("zone-rect"))}</option>
                                <option value="zone-polygon">{t(toolLabel("zone-polygon"))}</option>
                            </select>
                        )}
                        <button
                            type="button"
                            class={`nogo-mode-btn${enforcePreview ? " on" : ""}`}
                            onClick={() => setEnforcePreview((v) => !v)}
                        >
                            <T>Preview enforcement</T>
                        </button>
                    </div>
                    {selectedZoneIndex !== null && (
                        <div class="nogo-toolbar-row">
                            <label class="nogo-zone-rename-label" htmlFor="nogo-zone-rename">
                                <T>Zone name</T>
                            </label>
                            <input
                                id="nogo-zone-rename"
                                type="text"
                                class="nogo-zone-rename-input"
                                value={zones[selectedZoneIndex]?.label ?? ""}
                                onInput={(e) => onRenameZone(selectedZoneIndex, (e.target as HTMLInputElement).value)}
                                placeholder={t("Zone name")}
                            />
                        </div>
                    )}
                    {(zonesDirty || savingZones) && (
                        <div class="nogo-toolbar-row">
                            <button
                                type="button"
                                class="nogo-discard-btn"
                                onClick={onDiscardZones}
                                disabled={savingZones}
                            >
                                <T>Discard</T>
                            </button>
                            <button
                                type="button"
                                class={`nogo-save-btn${savingZones ? " pending" : ""}`}
                                onClick={onSaveZones}
                                disabled={savingZones}
                            >
                                {t(savingZones ? "Saving..." : "Save")}
                            </button>
                        </div>
                    )}
                </div>
            )}

            {/* Map canvas. The wave owns this canvas while `revealing`;
                afterwards the motion player or the static-render effect
                takes over. The empty-data message replaces it only when
                we know the session has no usable map. */}
            <div class="history-canvas-wrap">
                {mapEmpty && (
                    <div class="history-empty">
                        <T>Not enough data to display map</T>
                    </div>
                )}
                <canvas
                    ref={canvasRef}
                    class={`history-canvas${editorEnabled && editor.mode === "view" && editor.selection ? " has-selection" : ""}`}
                    style={mapEmpty ? { display: "none" } : undefined}
                />
                {map && !mapEmpty && (
                    <>
                        <button
                            type="button"
                            class="history-rotate-btn left"
                            onClick={() => rotateBy(-90)}
                            aria-label={t("Rotate map counter-clockwise")}
                        >
                            <Icon svg={rotateLeftSvg} />
                        </button>
                        <button
                            type="button"
                            class="history-rotate-btn right"
                            onClick={() => rotateBy(90)}
                            aria-label={t("Rotate map clockwise")}
                        >
                            <Icon svg={rotateRightSvg} />
                        </button>
                    </>
                )}
            </div>

            {/* Motion player mounts immediately when data arrives so its
                controls render right away. Its canvas effects are
                suspended via `canvasSuspended` until the wave resolves. */}
            {showPlayer && map && (
                <MotionPlayer
                    canvas={canvasEl}
                    map={map}
                    transform={transform}
                    rotation={rotation}
                    nogoLines={nogoLines}
                    draftLine={editor.draftLine}
                    overlay={overlay}
                    canvasSuspended={revealing}
                />
            )}

            {/* Legend */}
            {map && (
                <div class="history-legend">
                    <span class="history-legend-item">
                        <span class="history-legend-dot start" /> <T>Start</T>
                    </span>
                    <span class="history-legend-item">
                        <span class={`history-legend-dot ${recording ? "current" : "end"}`} />{" "}
                        {t(recording ? "Current" : "End")}
                    </span>
                    <span class="history-legend-item">
                        <span class="history-legend-swatch coverage" /> <T>Coverage</T>
                    </span>
                    {map.recharges.length > 0 && (
                        <span class="history-legend-item">
                            <span class="history-legend-bolt">
                                <Icon svg={boltSvg} />
                            </span>{" "}
                            <T>Recharge</T>
                        </span>
                    )}
                </div>
            )}
            {map && !editorEnabled && (
                <div class="history-map-hint">
                    <T>Pinch or scroll to zoom, drag to pan, double-tap to zoom in or reset</T>
                </div>
            )}
            {editorEnabled && editor.mode === "view" && (
                <div class="history-map-hint">
                    {editor.selection ? (
                        <T>
                            Drag a handle to reshape, drag a midpoint to add a point, double-click a handle to remove
                            it, Delete to remove the selected line or zone
                        </T>
                    ) : (
                        <T>Pinch or scroll to zoom, drag to pan · click a line or zone to select it</T>
                    )}
                </div>
            )}
            {editorEnabled && editor.mode === "draw" && editor.tool !== "zone-rect" && (
                <div class="history-map-hint">
                    <T>Click to add a point, Enter or double-click to finish, Esc to cancel</T>
                </div>
            )}
            {editorEnabled && editor.mode === "draw" && editor.tool === "zone-rect" && (
                <div class="history-map-hint">
                    <T>Drag to draw a rectangle zone</T>
                </div>
            )}
            {showPlayer && (
                <div class="history-map-hint">
                    <T>Space to play or pause, arrow keys to seek, hold shift for a larger jump</T>
                </div>
            )}
        </>
    );
}
