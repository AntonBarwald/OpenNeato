// Guided Clean map editor: a view|draw pointer state machine for drawing and
// selecting no-go lines and zones on the history map canvas.
// Listens on `window` in the CAPTURE phase (fires before useMapGestures's own
// bubble-phase canvas listeners) so draw mode can stop pan/zoom from running;
// view mode leaves events alone except to inspect them for click-selection.

import { useCallback, useEffect, useMemo, useRef, useState } from "preact/hooks";
import type { MapBounds, MapTransform } from "../types";
import { canvasToWorld, distPointToSegment, pointInPolygon, type Point } from "../views/history/geometry";
import { computeMapProjection } from "../views/history/helpers";
import { useKeyShortcut } from "./use-key-shortcut";

// CLICK_SLOP_PX distinguishes a click (vertex add/selection) from a drag; PICK_RADIUS_PX
// is the view-mode line/zone hit-test tolerance.
const CLICK_SLOP_PX = 5;
const PICK_RADIUS_PX = 10;
const MIN_RECT_DRAG_PX = 6;
const HANDLE_RADIUS_PX = 12; // Vertex/midpoint handle hit-test tolerance; bigger than PICK_RADIUS_PX
const MIN_NOGO_POINTS = 2; // A line needs >=2 points, a zone polygon >=3
const MIN_ZONE_POINTS = 3;

export type EditorMode = "view" | "draw";
export type DrawTool = "nogo" | "zone-rect" | "zone-polygon";
export type SelectionKind = "nogo" | "zone";

export interface EditorSelection {
    kind: SelectionKind;
    index: number;
}

export interface ZonePolygon {
    points: Point[];
}

// For "midpoint", index is the edge's first endpoint (new vertex lands at index+1).
export interface HandleHit {
    field: "vertex" | "midpoint";
    index: number;
}

// Exposed so item.tsx's overlay can derive dragNoGoPoints/dragZonePoints from it.
export interface VertexDragPreview {
    kind: SelectionKind;
    index: number;
    points: Point[];
}

export interface NoGoEditorOptions {
    canvasRef: { current: HTMLCanvasElement | null };
    // Null while no map/bounds are loaded yet — the editor stays inert.
    bounds: MapBounds | null;
    transform: MapTransform;
    rotation: number;
    nogoLines: Point[][];
    zones: ZonePolygon[];
    onAddNoGoLine: (points: Point[]) => void;
    onDeleteNoGoLine: (index: number) => void;
    onAddZone: (points: Point[]) => void;
    onDeleteZone: (index: number) => void;
    // Replaces a shape's whole point array — vertex move, midpoint-insert, and
    // vertex-delete all reduce to this.
    onUpdateNoGoLine: (index: number, points: Point[]) => void;
    onUpdateZone: (index: number, points: Point[]) => void;
    enabled: boolean;
    resetKey: string; // Changing this resets mode/draft/selection (pass the session filename)
}

export interface NoGoEditorResult {
    mode: EditorMode;
    setMode: (mode: EditorMode) => void;
    tool: DrawTool;
    setTool: (tool: DrawTool) => void;
    draftLine: Point[] | null; // In-progress no-go polyline (tool === "nogo" only)
    draftZone: Point[] | null; // In-progress zone: polygon clicks, or rect corners while dragging
    selection: EditorSelection | null;
    clearSelection: () => void;
    vertexDragPreview: VertexDragPreview | null; // Non-null only during an active vertex/midpoint drag
}

function rectCorners(a: Point, b: Point): Point[] {
    const minX = Math.min(a.x, b.x);
    const maxX = Math.max(a.x, b.x);
    const minY = Math.min(a.y, b.y);
    const maxY = Math.max(a.y, b.y);
    return [
        { x: minX, y: minY },
        { x: maxX, y: minY },
        { x: maxX, y: maxY },
        { x: minX, y: maxY },
    ];
}

export function useNoGoEditor({
    canvasRef,
    bounds,
    transform,
    rotation,
    nogoLines,
    zones,
    onAddNoGoLine,
    onDeleteNoGoLine,
    onAddZone,
    onDeleteZone,
    onUpdateNoGoLine,
    onUpdateZone,
    enabled,
    resetKey,
}: NoGoEditorOptions): NoGoEditorResult {
    const [mode, setModeState] = useState<EditorMode>("view");
    const [tool, setTool] = useState<DrawTool>("nogo");
    const [clickDraft, setClickDraft] = useState<Point[] | null>(null);
    const [rectDrag, setRectDrag] = useState<{ anchor: Point; current: Point } | null>(null);
    const [selection, setSelection] = useState<EditorSelection | null>(null);
    const [vertexDragPreview, setVertexDragPreview] = useState<VertexDragPreview | null>(null);

    // Mirrors latest props/state so the window-level listener effect (registered once)
    // always reads fresh values without re-binding.
    const boundsRef = useRef(bounds);
    boundsRef.current = bounds;
    const transformRef = useRef(transform);
    transformRef.current = transform;
    const rotationRef = useRef(rotation);
    rotationRef.current = rotation;
    const nogoLinesRef = useRef(nogoLines);
    nogoLinesRef.current = nogoLines;
    const zonesRef = useRef(zones);
    zonesRef.current = zones;
    const modeRef = useRef(mode);
    modeRef.current = mode;
    const toolRef = useRef(tool);
    toolRef.current = tool;
    const clickDraftRef = useRef(clickDraft);
    clickDraftRef.current = clickDraft;
    const selectionRef = useRef(selection);
    selectionRef.current = selection;
    const downRef = useRef<{ x: number; y: number } | null>(null);
    const rectAnchorRef = useRef<Point | null>(null);
    // Authoritative working point array for an in-progress vertex/midpoint drag; kept in
    // lockstep with vertexDragPreview state for consumers that need to re-render.
    const vertexDragRef = useRef<{
        kind: SelectionKind;
        shapeIndex: number;
        vertexIndex: number;
        points: Point[];
    } | null>(null);

    const clearSelection = useCallback(() => setSelection(null), []);

    const setMode = useCallback((next: EditorMode) => {
        setModeState(next);
        setClickDraft(null);
        setRectDrag(null);
        rectAnchorRef.current = null;
        setSelection(null);
        vertexDragRef.current = null;
        setVertexDragPreview(null);
    }, []);

    // Reset on disable or session change so draw state never leaks between sessions.
    useEffect(() => {
        setModeState("view");
        setClickDraft(null);
        setRectDrag(null);
        rectAnchorRef.current = null;
        setSelection(null);
        vertexDragRef.current = null;
        setVertexDragPreview(null);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [resetKey, enabled]);

    const toWorld = useCallback(
        (clientX: number, clientY: number): Point | null => {
            const canvas = canvasRef.current;
            const b = boundsRef.current;
            if (!canvas || !b) return null;
            const rect = canvas.getBoundingClientRect();
            const sx = clientX - rect.left;
            const sy = clientY - rect.top;
            const displayW = canvas.clientWidth;
            const displayH = canvas.clientHeight;
            const proj = computeMapProjection(displayW, displayH, b);
            return canvasToWorld(sx, sy, proj, transformRef.current, rotationRef.current, displayW, displayH);
        },
        [canvasRef],
    );

    // Hit-test radius in world meters for `px` screen pixels at the current zoom level.
    const pickRadiusWorld = useCallback(
        (px: number = PICK_RADIUS_PX): number => {
            const canvas = canvasRef.current;
            const b = boundsRef.current;
            if (!canvas || !b) return 0;
            const proj = computeMapProjection(canvas.clientWidth, canvas.clientHeight, b);
            const denom = proj.scale * transformRef.current.zoom;
            return denom > 0 ? px / denom : 0;
        },
        [canvasRef],
    );

    const finishClickDraft = useCallback(() => {
        let pts = clickDraftRef.current;
        setClickDraft(null);
        if (!pts) return;
        // dblclick's second click places a coincident vertex first — drop the trailing
        // near-duplicate so we don't persist a zero-length segment.
        if (pts.length >= 2) {
            const a = pts[pts.length - 1];
            const b = pts[pts.length - 2];
            if (Math.hypot(a.x - b.x, a.y - b.y) <= pickRadiusWorld()) pts = pts.slice(0, -1);
        }
        if (toolRef.current === "nogo" && pts.length >= 2) onAddNoGoLine(pts);
        else if (toolRef.current === "zone-polygon" && pts.length >= 3) onAddZone(pts);
    }, [onAddNoGoLine, onAddZone, pickRadiusWorld]);

    const pickAt = useCallback(
        (world: Point): EditorSelection | null => {
            const radius = pickRadiusWorld();
            let bestKind: SelectionKind | null = null;
            let bestIndex = -1;
            let bestDist = Number.POSITIVE_INFINITY;

            const lines = nogoLinesRef.current;
            for (let idx = 0; idx < lines.length; idx++) {
                const line = lines[idx];
                for (let i = 0; i + 1 < line.length; i++) {
                    const d = distPointToSegment(world, line[i], line[i + 1]);
                    if (d <= radius && d < bestDist) {
                        bestKind = "nogo";
                        bestIndex = idx;
                        bestDist = d;
                    }
                }
            }

            const zoneList = zonesRef.current;
            for (let idx = 0; idx < zoneList.length; idx++) {
                const pts = zoneList[idx].points;
                if (pts.length < 2) continue;
                let edgeDist = Number.POSITIVE_INFINITY;
                for (let i = 0; i < pts.length; i++) {
                    const a = pts[i];
                    const b = pts[(i + 1) % pts.length];
                    edgeDist = Math.min(edgeDist, distPointToSegment(world, a, b));
                }
                const inside = pointInPolygon(world, pts);
                if (!inside && edgeDist > radius) continue;
                const d = inside ? Math.min(edgeDist, radius) : edgeDist;
                if (d < bestDist) {
                    bestKind = "zone";
                    bestIndex = idx;
                    bestDist = d;
                }
            }

            return bestKind ? { kind: bestKind, index: bestIndex } : null;
        },
        [pickRadiusWorld],
    );

    // Unlike pickAt, only scans the currently *selected* shape; a tie favors a vertex
    // over a midpoint (vertices scanned first, strict `<`).
    const pickHandleAt = useCallback(
        (world: Point): HandleHit | null => {
            const sel = selectionRef.current;
            if (!sel) return null;
            const points = sel.kind === "nogo" ? nogoLinesRef.current[sel.index] : zonesRef.current[sel.index]?.points;
            if (!points || points.length === 0) return null;
            const radius = pickRadiusWorld(HANDLE_RADIUS_PX);

            let bestField: "vertex" | "midpoint" | null = null;
            let bestIndex = -1;
            let bestDist = Number.POSITIVE_INFINITY;

            for (let i = 0; i < points.length; i++) {
                const d = Math.hypot(world.x - points[i].x, world.y - points[i].y);
                if (d <= radius && d < bestDist) {
                    bestField = "vertex";
                    bestIndex = i;
                    bestDist = d;
                }
            }

            // Zones close the polygon (last -> first edge included), no-go lines don't.
            const segCount = sel.kind === "zone" ? points.length : points.length - 1;
            for (let i = 0; i < segCount; i++) {
                const a = points[i];
                const b = points[(i + 1) % points.length];
                const mx = (a.x + b.x) / 2;
                const my = (a.y + b.y) / 2;
                const d = Math.hypot(world.x - mx, world.y - my);
                if (d <= radius && d < bestDist) {
                    bestField = "midpoint";
                    bestIndex = i;
                    bestDist = d;
                }
            }

            return bestField ? { field: bestField, index: bestIndex } : null;
        },
        [pickRadiusWorld],
    );

    // For a midpoint hit, splices in a new vertex first, then drags that (reuses the
    // vertex-move path).
    const startVertexDrag = useCallback((hit: HandleHit) => {
        const sel = selectionRef.current;
        if (!sel) return;
        const source = sel.kind === "nogo" ? nogoLinesRef.current[sel.index] : zonesRef.current[sel.index]?.points;
        if (!source) return;

        let points: Point[];
        let vertexIndex: number;
        if (hit.field === "midpoint") {
            const a = source[hit.index];
            const b = source[(hit.index + 1) % source.length];
            const mid: Point = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
            points = [...source.slice(0, hit.index + 1), mid, ...source.slice(hit.index + 1)];
            vertexIndex = hit.index + 1;
        } else {
            points = [...source];
            vertexIndex = hit.index;
        }

        vertexDragRef.current = { kind: sel.kind, shapeIndex: sel.index, vertexIndex, points };
        setVertexDragPreview({ kind: sel.kind, index: sel.index, points });
    }, []);

    // Double-click vertex delete — the only gesture that reduces point count, so the
    // MIN_NOGO_POINTS/MIN_ZONE_POINTS floor is checked only here.
    const deleteVertex = useCallback(
        (vertexIndex: number) => {
            const sel = selectionRef.current;
            if (!sel) return;
            if (sel.kind === "nogo") {
                const line = nogoLinesRef.current[sel.index];
                if (!line || line.length <= MIN_NOGO_POINTS) return;
                onUpdateNoGoLine(
                    sel.index,
                    line.filter((_, i) => i !== vertexIndex),
                );
            } else {
                const zone = zonesRef.current[sel.index];
                if (!zone || zone.points.length <= MIN_ZONE_POINTS) return;
                onUpdateZone(
                    sel.index,
                    zone.points.filter((_, i) => i !== vertexIndex),
                );
            }
        },
        [onUpdateNoGoLine, onUpdateZone],
    );

    useEffect(() => {
        if (!enabled) return;
        const canvas = canvasRef.current;
        if (!canvas) return;

        const isOurTarget = (e: Event) => e.target === canvas;

        const onPointerDown = (e: PointerEvent) => {
            if (!isOurTarget(e)) return;
            if (e.pointerType !== "touch" && e.button !== 0) return;
            downRef.current = { x: e.clientX, y: e.clientY };

            if (modeRef.current === "draw") {
                canvas.setPointerCapture(e.pointerId);
                if (toolRef.current === "zone-rect") {
                    const world = toWorld(e.clientX, e.clientY);
                    if (world) {
                        rectAnchorRef.current = world;
                        setRectDrag({ anchor: world, current: world });
                    }
                }
                e.stopPropagation();
            } else if (modeRef.current === "view" && e.pointerType !== "touch" && selectionRef.current) {
                // Touch excluded (handled separately below); a handle grab must stop
                // bubbling to useMapGestures's pan listener, same as draw mode's drags.
                const world = toWorld(e.clientX, e.clientY);
                const hit = world && pickHandleAt(world);
                if (hit) {
                    e.stopPropagation();
                    canvas.setPointerCapture(e.pointerId);
                    startVertexDrag(hit);
                }
            }
        };

        const onPointerMove = (e: PointerEvent) => {
            if (!isOurTarget(e)) return;
            if (vertexDragRef.current) {
                e.stopPropagation();
                const world = toWorld(e.clientX, e.clientY);
                if (world) {
                    const drag = vertexDragRef.current;
                    const points = drag.points.slice();
                    points[drag.vertexIndex] = world;
                    vertexDragRef.current = { ...drag, points };
                    setVertexDragPreview({ kind: drag.kind, index: drag.shapeIndex, points });
                }
                return;
            }
            if (modeRef.current !== "draw") return;
            e.stopPropagation();
            if (toolRef.current === "zone-rect" && rectAnchorRef.current) {
                const world = toWorld(e.clientX, e.clientY);
                if (world) setRectDrag({ anchor: rectAnchorRef.current, current: world });
            }
        };

        const onPointerUp = (e: PointerEvent) => {
            if (!isOurTarget(e)) return;
            const down = downRef.current;
            downRef.current = null;

            if (vertexDragRef.current) {
                // Commit once and return — must not fall through to view-mode click-select,
                // which would re-run selection on top of the just-finished drag.
                e.stopPropagation();
                const drag = vertexDragRef.current;
                vertexDragRef.current = null;
                setVertexDragPreview(null);
                if (drag.kind === "nogo") onUpdateNoGoLine(drag.shapeIndex, drag.points);
                else onUpdateZone(drag.shapeIndex, drag.points);
                return;
            }

            if (modeRef.current === "draw") {
                e.stopPropagation();
                const anchor = rectAnchorRef.current;
                rectAnchorRef.current = null;
                if (toolRef.current === "zone-rect") {
                    setRectDrag(null);
                    if (!anchor || !down) return;
                    if (Math.hypot(e.clientX - down.x, e.clientY - down.y) < MIN_RECT_DRAG_PX) return;
                    const world = toWorld(e.clientX, e.clientY);
                    if (!world) return;
                    onAddZone(rectCorners(anchor, world));
                    return;
                }

                // Only add a vertex if pointerdown->up stayed within the click slop.
                if (!down || Math.hypot(e.clientX - down.x, e.clientY - down.y) >= CLICK_SLOP_PX) return;
                const world = toWorld(e.clientX, e.clientY);
                if (!world) return;
                setClickDraft((cur) => (cur ? [...cur, world] : [world]));
                return;
            }

            // View mode: click (not drag) hit-tests existing lines/zones.
            if (!down) return;
            if (Math.hypot(e.clientX - down.x, e.clientY - down.y) >= CLICK_SLOP_PX) return;
            const world = toWorld(e.clientX, e.clientY);
            if (!world) return;
            setSelection(pickAt(world));
        };

        const onPointerCancel = (e: PointerEvent) => {
            if (!isOurTarget(e)) return;
            downRef.current = null;
            rectAnchorRef.current = null;
            setRectDrag(null);
            vertexDragRef.current = null;
            setVertexDragPreview(null);
        };

        const onDblClick = (e: MouseEvent) => {
            if (!isOurTarget(e)) return;
            if (modeRef.current === "draw") {
                if (toolRef.current === "zone-rect") return;
                e.stopPropagation();
                e.preventDefault();
                finishClickDraft();
                return;
            }
            // View mode: double-click on a vertex handle deletes it; a miss falls through
            // so useMapGestures's dblclick-to-zoom keeps working elsewhere.
            if (modeRef.current === "view" && selectionRef.current) {
                const world = toWorld(e.clientX, e.clientY);
                const hit = world && pickHandleAt(world);
                if (hit && hit.field === "vertex") {
                    e.stopPropagation();
                    e.preventDefault();
                    deleteVertex(hit.index);
                }
            }
        };

        // Touch dispatches both PointerEvents (handled above) and a separate legacy
        // TouchEvent stream useMapGestures listens for directly — stopping the PointerEvent
        // doesn't stop that stream, so draw mode must also swallow raw touch events here.
        const onTouchCapture = (e: TouchEvent) => {
            if (!isOurTarget(e)) return;
            if (modeRef.current !== "draw") return;
            e.stopPropagation();
            e.preventDefault();
        };

        window.addEventListener("pointerdown", onPointerDown, { capture: true });
        window.addEventListener("pointermove", onPointerMove, { capture: true });
        window.addEventListener("pointerup", onPointerUp, { capture: true });
        window.addEventListener("pointercancel", onPointerCancel, { capture: true });
        window.addEventListener("dblclick", onDblClick, { capture: true });
        window.addEventListener("touchstart", onTouchCapture, { capture: true, passive: false });
        window.addEventListener("touchmove", onTouchCapture, { capture: true, passive: false });
        window.addEventListener("touchend", onTouchCapture, { capture: true, passive: false });
        window.addEventListener("touchcancel", onTouchCapture, { capture: true, passive: false });
        return () => {
            window.removeEventListener("pointerdown", onPointerDown, { capture: true });
            window.removeEventListener("pointermove", onPointerMove, { capture: true });
            window.removeEventListener("pointerup", onPointerUp, { capture: true });
            window.removeEventListener("pointercancel", onPointerCancel, { capture: true });
            window.removeEventListener("dblclick", onDblClick, { capture: true });
            window.removeEventListener("touchstart", onTouchCapture, { capture: true });
            window.removeEventListener("touchmove", onTouchCapture, { capture: true });
            window.removeEventListener("touchend", onTouchCapture, { capture: true });
            window.removeEventListener("touchcancel", onTouchCapture, { capture: true });
        };
    }, [
        canvasRef,
        enabled,
        toWorld,
        pickAt,
        pickHandleAt,
        startVertexDrag,
        deleteVertex,
        finishClickDraft,
        onAddZone,
        onUpdateNoGoLine,
        onUpdateZone,
    ]);

    // Escape: cancel vertex drag, then draft/rect-drag, then leave draw mode, then clear selection.
    useKeyShortcut(
        () => {
            if (vertexDragRef.current) {
                vertexDragRef.current = null;
                setVertexDragPreview(null);
            } else if (clickDraftRef.current || rectAnchorRef.current) {
                setClickDraft(null);
                setRectDrag(null);
                rectAnchorRef.current = null;
            } else if (modeRef.current === "draw") {
                setMode("view");
            } else {
                setSelection(null);
            }
        },
        { key: "Escape", enabled },
    );

    // Enter finishes a click-based draft once it has enough points.
    useKeyShortcut(() => finishClickDraft(), {
        key: "Enter",
        enabled: enabled && mode === "draw" && tool !== "zone-rect" && (clickDraft?.length ?? 0) >= 2,
    });

    // Delete/Backspace remove the selected line or zone in view mode.
    const deleteSelected = useCallback(() => {
        setSelection((cur) => {
            if (!cur) return cur;
            if (cur.kind === "nogo") onDeleteNoGoLine(cur.index);
            else onDeleteZone(cur.index);
            return null;
        });
    }, [onDeleteNoGoLine, onDeleteZone]);
    useKeyShortcut(deleteSelected, { key: "Delete", enabled: enabled && mode === "view" && selection !== null });
    useKeyShortcut(deleteSelected, { key: "Backspace", enabled: enabled && mode === "view" && selection !== null });

    const draftLine = tool === "nogo" ? clickDraft : null;
    const draftZone = useMemo(() => {
        if (tool === "zone-polygon") return clickDraft;
        if (tool === "zone-rect" && rectDrag) return rectCorners(rectDrag.anchor, rectDrag.current);
        return null;
    }, [tool, clickDraft, rectDrag]);

    return { mode, setMode, tool, setTool, draftLine, draftZone, selection, clearSelection, vertexDragPreview };
}
