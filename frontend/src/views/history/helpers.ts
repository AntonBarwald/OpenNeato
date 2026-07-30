// Shared helpers for history views

import clockSvg from "../../assets/icons/clock.svg?raw";
import houseSvg from "../../assets/icons/house.svg?raw";
import manualSvg from "../../assets/icons/manual.svg?raw";
import sparkleSvg from "../../assets/icons/sparkle.svg?raw";
import spotSvg from "../../assets/icons/spot.svg?raw";
import type { MapBounds, MapData, MapPathPoint, MapTransform } from "../../types";
import { polylineCrossesSegment, type Point } from "./geometry";

const DEFAULT_TRANSFORM: MapTransform = { panX: 0, panY: 0, zoom: 1 };
const MAP_PAD = 20;
const GRID_STEP = 0.5;
// Draggable handle radii, divided by zoom before drawing so they stay a constant on-screen size.
const HANDLE_DRAW_RADIUS_PX = 6;
const HANDLE_MIDPOINT_DRAW_RADIUS_PX = 4;

export interface MapProjection {
    minX: number;
    maxX: number;
    minY: number;
    maxY: number;
    scale: number;
    // Exposed so geometry.ts's inverse transform shares one source of truth with toX/toY.
    offX: number;
    offY: number;
    toX: (wx: number) => number;
    toY: (wy: number) => number;
}

// World-to-canvas projection used by the static renderer and the loading wave.
// `pad` is in display pixels; the world is centered within the available area.
export function computeMapProjection(displayW: number, displayH: number, bounds: MapBounds): MapProjection {
    const { minX, maxX, minY, maxY } = bounds;
    const worldW = maxX - minX;
    const worldH = maxY - minY;
    const availW = displayW - MAP_PAD * 2;
    const availH = displayH - MAP_PAD * 2;
    const scale = Math.min(availW / worldW, availH / worldH);
    const offX = MAP_PAD + (availW - worldW * scale) / 2;
    const offY = MAP_PAD + (availH - worldH * scale) / 2;
    return {
        minX,
        maxX,
        minY,
        maxY,
        scale,
        offX,
        offY,
        toX: (wx) => offX + (wx - minX) * scale,
        toY: (wy) => offY + (maxY - wy) * scale,
    };
}

// 0.5m background grid. Same look in both the static map and the wave reveal.
export function drawMapGrid(ctx: CanvasRenderingContext2D, proj: MapProjection, isDark: boolean): void {
    ctx.strokeStyle = isDark ? "rgba(255, 255, 255, 0.04)" : "rgba(0, 0, 0, 0.06)";
    ctx.lineWidth = 1;
    const { minX, maxX, minY, maxY, toX, toY } = proj;
    for (let gx = Math.floor(minX / GRID_STEP) * GRID_STEP; gx <= maxX; gx += GRID_STEP) {
        ctx.beginPath();
        ctx.moveTo(toX(gx), toY(minY));
        ctx.lineTo(toX(gx), toY(maxY));
        ctx.stroke();
    }
    for (let gy = Math.floor(minY / GRID_STEP) * GRID_STEP; gy <= maxY; gy += GRID_STEP) {
        ctx.beginPath();
        ctx.moveTo(toX(minX), toY(gy));
        ctx.lineTo(toX(maxX), toY(gy));
        ctx.stroke();
    }
}

export function isDarkSurface(canvas: HTMLCanvasElement): boolean {
    return getComputedStyle(canvas).getPropertyValue("--surface").trim().startsWith("#1");
}

// Mirrors zoneLabelFor() in firmware/src/zone_label_match.h; keep in lockstep.
export function zoneLabel(zone: { label?: string }, idx: number): string {
    return zone.label?.trim() || `Zone ${idx + 1}`;
}

export function modeInfo(mode: string): { label: string; icon: string } {
    if (mode === "house") return { label: "House Clean", icon: houseSvg };
    if (mode === "spot") return { label: "Spot Clean", icon: spotSvg };
    if (mode === "manual") return { label: "Manual Clean", icon: manualSvg };
    if (mode === "guided") return { label: "Guided Clean", icon: sparkleSvg };
    return { label: mode, icon: clockSvg };
}

// Total session duration derived from the map data. Prefers the summary
// value written by the firmware, falls back to the last pose timestamp.
export function sessionDuration(map: MapData): number {
    if (map.summary?.duration && map.summary.duration > 0) return map.summary.duration;
    if (map.path.length > 0) return map.path[map.path.length - 1].ts;
    return 0;
}

// Shortest-arc interpolation between two headings in degrees.
function lerpAngleDeg(a: number, b: number, f: number): number {
    const delta = ((b - a + 540) % 360) - 180;
    return a + delta * f;
}

// Interpolate robot pose at a given session-relative timestamp. Returns
// null when the path is empty. Uses linear interpolation for x/y and
// shortest-arc interpolation for the heading so the sprite never snaps.
export function interpolatePose(
    path: MapPathPoint[],
    ts: number,
): { x: number; y: number; t: number; ts: number } | null {
    if (path.length === 0) return null;
    if (ts <= path[0].ts) return { ...path[0] };
    const last = path[path.length - 1];
    if (ts >= last.ts) return { ...last };

    // Binary search for the segment containing ts
    let lo = 0;
    let hi = path.length - 1;
    while (hi - lo > 1) {
        const mid = (lo + hi) >> 1;
        if (path[mid].ts <= ts) lo = mid;
        else hi = mid;
    }

    const a = path[lo];
    const b = path[hi];
    const span = b.ts - a.ts;
    const f = span > 0 ? (ts - a.ts) / span : 0;
    return {
        x: a.x + (b.x - a.x) * f,
        y: a.y + (b.y - a.y) * f,
        t: lerpAngleDeg(a.t, b.t, f),
        ts,
    };
}

// A closed-polygon zone to render (translucent fill + labeled outline).
export interface ZoneShape {
    points: Point[];
    label?: string;
}

// Guided Clean editor overlay passed to renderMap; the motion player and static fallback
// leave this at its default and pass nogoLines/draftLine directly instead.
export interface MapEditorOverlay {
    zones?: ZoneShape[];
    draftZone?: Point[] | null; // In-progress zone: polygon clicks or live rectangle drag corners
    selectedNoGoIndex?: number | null;
    selectedZoneIndex?: number | null;
    enforcePreview?: boolean; // Recolors path hops crossing a no-go line, previewing what replay would skip
    // Only one of these is ever set at a time; renderMap substitutes it into the relevant
    // line/zone for that render instead of mutating state.
    dragNoGoPoints?: Point[] | null;
    dragZonePoints?: Point[] | null;
}

// Canvas renderer for map visualization. When `currentTime` is provided the
// renderer draws only the portion of the session up to that timestamp and
// shows the interpolated robot pose as a directional sprite — used by the
// motion player. Without `currentTime` the full static map is drawn.
// `rotation` rotates the map content around the canvas center (degrees, 0/90/180/270).
export function renderMap(
    canvas: HTMLCanvasElement,
    map: MapData,
    recording = false,
    tf?: MapTransform,
    currentTime?: number,
    rotation = 0,
    nogoLines: Point[][] = [],
    draftLine: Point[] | null = null,
    overlay: MapEditorOverlay = {},
) {
    const ctx = canvas.getContext("2d");
    if (!ctx || !map.bounds) return;

    const { panX, panY, zoom } = tf ?? DEFAULT_TRANSFORM;
    const playing = currentTime !== undefined;
    const tNow = currentTime ?? Number.POSITIVE_INFINITY;

    const dpr = window.devicePixelRatio || 1;
    const displayW = canvas.clientWidth;
    const displayH = canvas.clientHeight;
    canvas.width = displayW * dpr;
    canvas.height = displayH * dpr;
    ctx.scale(dpr, dpr);

    // Fill the unrotated background first so rotation doesn't expose the
    // underlying transparent canvas at the corners.
    ctx.fillStyle = getComputedStyle(canvas).getPropertyValue("--surface").trim() || "#1a1a1c";
    ctx.fillRect(0, 0, displayW, displayH);

    // Rotate the map content around the canvas center. Pan/zoom are applied
    // in the rotated coordinate space so gestures stay aligned with the
    // visible orientation (handled in useMapGestures).
    if (rotation) {
        ctx.translate(displayW / 2, displayH / 2);
        ctx.rotate((rotation * Math.PI) / 180);
        ctx.translate(-displayW / 2, -displayH / 2);
    }

    // Apply zoom + pan: zoom from top-left origin, panX/panY computed by
    // useMapGestures already account for the cursor-relative zoom anchor.
    ctx.translate(panX, panY);
    ctx.scale(zoom, zoom);

    const proj = computeMapProjection(displayW, displayH, map.bounds);
    const { scale, toX, toY } = proj;
    const isDark = isDarkSurface(canvas);

    // Substitute the live drag preview into the shape being dragged so the enforcement
    // preview, drawZones and drawNoGoLines all see the in-flight reshape, not stale points.
    const effectiveNoGoLines =
        overlay.dragNoGoPoints && overlay.selectedNoGoIndex != null
            ? nogoLines.map((l, i) => (i === overlay.selectedNoGoIndex ? overlay.dragNoGoPoints! : l))
            : nogoLines;
    const effectiveZones =
        overlay.dragZonePoints && overlay.selectedZoneIndex != null
            ? (overlay.zones ?? []).map((z, i) =>
                  i === overlay.selectedZoneIndex ? { ...z, points: overlay.dragZonePoints! } : z,
              )
            : (overlay.zones ?? []);

    // Grid lines - draw first so coverage/path render on top
    drawMapGrid(ctx, proj, isDark);

    // Coverage cells — filtered by timestamp during playback
    const cellPx = map.cellSize * scale;
    ctx.fillStyle = isDark ? "rgba(52, 199, 89, 0.15)" : "rgba(22, 130, 50, 0.22)";
    for (const cell of map.coverage) {
        const [cx, cy, ts] = cell;
        if (ts > tNow) continue;
        const wx = cx * map.cellSize;
        const wy = cy * map.cellSize;
        ctx.fillRect(toX(wx) - cellPx / 2, toY(wy) - cellPx / 2, cellPx, cellPx);
    }

    // Resolve the subset of path points to draw and the current robot pose
    // when playing. During playback we cut the path at `tNow` and append
    // an interpolated endpoint so the line keeps up with the robot sprite.
    let pathEnd = map.path.length;
    let liveHead: { x: number; y: number; t: number; ts: number } | null = null;
    if (playing) {
        pathEnd = 0;
        while (pathEnd < map.path.length && map.path[pathEnd].ts <= tNow) pathEnd++;
        liveHead = interpolatePose(map.path, tNow);
    }

    // Path line
    const drawnPath: { x: number; y: number; ts: number }[] = [];
    for (let i = 0; i < pathEnd; i++) drawnPath.push(map.path[i]);
    if (playing && liveHead && (drawnPath.length === 0 || drawnPath[drawnPath.length - 1].ts !== liveHead.ts)) {
        drawnPath.push(liveHead);
    }

    if (drawnPath.length > 1) {
        const pathColor = isDark ? "rgba(249, 235, 178, 0.6)" : "rgba(180, 140, 40, 0.5)";
        if (overlay.enforcePreview && effectiveNoGoLines.length > 0) {
            // Draw each hop individually so a hop crossing a no-go line can be greyed out.
            const blockedColor = isDark ? "rgba(142, 142, 147, 0.65)" : "rgba(120, 120, 120, 0.55)";
            ctx.lineJoin = "round";
            ctx.lineCap = "round";
            ctx.lineWidth = 2;
            for (let i = 1; i < drawnPath.length; i++) {
                const a = drawnPath[i - 1];
                const b = drawnPath[i];
                const blocked = effectiveNoGoLines.some((line) => polylineCrossesSegment(line, a, b));
                ctx.beginPath();
                ctx.moveTo(toX(a.x), toY(a.y));
                ctx.lineTo(toX(b.x), toY(b.y));
                ctx.strokeStyle = blocked ? blockedColor : pathColor;
                ctx.stroke();
            }
        } else {
            ctx.beginPath();
            ctx.moveTo(toX(drawnPath[0].x), toY(drawnPath[0].y));
            for (let i = 1; i < drawnPath.length; i++) {
                ctx.lineTo(toX(drawnPath[i].x), toY(drawnPath[i].y));
            }
            ctx.strokeStyle = pathColor;
            ctx.lineWidth = 2;
            ctx.lineJoin = "round";
            ctx.lineCap = "round";
            ctx.stroke();
        }
    }

    // Start point
    if (map.path.length > 0) {
        const start = map.path[0];
        ctx.beginPath();
        ctx.arc(toX(start.x), toY(start.y), 5, 0, Math.PI * 2);
        ctx.fillStyle = "rgba(52, 199, 89, 0.9)";
        ctx.fill();
    }

    // End point / animated robot sprite
    if (playing && liveHead) {
        drawRobotSprite(ctx, toX(liveHead.x), toY(liveHead.y), liveHead.t);
    } else if (map.path.length > 1) {
        const end = map.path[map.path.length - 1];
        const ex = toX(end.x);
        const ey = toY(end.y);
        if (recording) {
            ctx.save();
            ctx.shadowColor = "rgba(52, 199, 89, 0.6)";
            ctx.shadowBlur = 10;
            ctx.beginPath();
            ctx.arc(ex, ey, 6, 0, Math.PI * 2);
            ctx.strokeStyle = "rgba(52, 199, 89, 0.9)";
            ctx.lineWidth = 2.5;
            ctx.stroke();
            ctx.restore();
        } else {
            ctx.beginPath();
            ctx.arc(ex, ey, 5, 0, Math.PI * 2);
            ctx.fillStyle = "rgba(255, 69, 58, 0.9)";
            ctx.fill();
        }
    }

    // Recharge points (bolt icon with glow) — hidden until reached during playback
    for (const rp of map.recharges) {
        if (rp.ts > tNow) continue;
        const rx = toX(rp.x);
        const ry = toY(rp.y);
        const s = 10;
        const drawBolt = () => {
            ctx.beginPath();
            ctx.moveTo(rx + s * 0.15, ry - s);
            ctx.lineTo(rx - s * 0.55, ry + s * 0.05);
            ctx.lineTo(rx - s * 0.05, ry + s * 0.05);
            ctx.lineTo(rx - s * 0.15, ry + s);
            ctx.lineTo(rx + s * 0.55, ry - s * 0.05);
            ctx.lineTo(rx + s * 0.05, ry - s * 0.05);
            ctx.closePath();
        };
        ctx.save();
        ctx.shadowColor = "rgba(255, 204, 0, 0.7)";
        ctx.shadowBlur = 8;
        drawBolt();
        ctx.fillStyle = "rgba(255, 204, 0, 1)";
        ctx.fill();
        ctx.restore();
        drawBolt();
        ctx.strokeStyle = isDark ? "rgba(0, 0, 0, 0.5)" : "rgba(0, 0, 0, 0.3)";
        ctx.lineWidth = 1.5;
        ctx.stroke();
    }

    // Editor overlay drawn last so it sits above path/coverage/recharge layers.
    drawZones(ctx, proj, effectiveZones, overlay.draftZone ?? null, overlay.selectedZoneIndex ?? null, isDark);
    drawNoGoLines(ctx, proj, effectiveNoGoLines, draftLine, overlay.selectedNoGoIndex ?? null);
    drawVertexHandles(
        ctx,
        proj,
        zoom,
        overlay.selectedNoGoIndex ?? null,
        overlay.selectedZoneIndex ?? null,
        effectiveNoGoLines,
        effectiveZones,
    );
}

// Dashed red polylines with vertex dots, plus the in-progress draft in dashed amber; the
// selected line is drawn thicker in orange.
function drawNoGoLines(
    ctx: CanvasRenderingContext2D,
    proj: MapProjection,
    lines: Point[][],
    draft: Point[] | null,
    selectedIndex: number | null,
) {
    const { toX, toY } = proj;
    const strokePolyline = (pts: Point[], color: string, dash: number[], width: number) => {
        if (pts.length === 0) return;
        ctx.save();
        ctx.strokeStyle = color;
        ctx.fillStyle = color;
        ctx.lineJoin = "round";
        ctx.lineCap = "round";
        if (pts.length > 1) {
            ctx.setLineDash(dash);
            ctx.lineWidth = width;
            ctx.beginPath();
            ctx.moveTo(toX(pts[0].x), toY(pts[0].y));
            for (let i = 1; i < pts.length; i++) ctx.lineTo(toX(pts[i].x), toY(pts[i].y));
            ctx.stroke();
        }
        ctx.setLineDash([]);
        for (const p of pts) {
            ctx.beginPath();
            ctx.arc(toX(p.x), toY(p.y), 3, 0, Math.PI * 2);
            ctx.fill();
        }
        ctx.restore();
    };

    lines.forEach((line, idx) => {
        const selected = idx === selectedIndex;
        strokePolyline(
            line,
            selected ? "rgba(255, 159, 10, 0.95)" : "rgba(255, 69, 58, 0.9)",
            [6, 4],
            selected ? 4 : 3,
        );
    });
    if (draft) strokePolyline(draft, "rgba(255, 204, 0, 0.95)", [4, 4], 2);
}

// Translucent filled polygons with a labeled outline, plus the in-progress draft in dashed
// amber; the selected zone gets a brighter, thicker outline.
function drawZones(
    ctx: CanvasRenderingContext2D,
    proj: MapProjection,
    zones: ZoneShape[],
    draft: Point[] | null,
    selectedIndex: number | null,
    isDark: boolean,
) {
    const { toX, toY } = proj;
    const fillPolygon = (pts: Point[], fill: string, stroke: string, width: number, dash: number[] = []) => {
        if (pts.length < 2) return;
        ctx.save();
        ctx.beginPath();
        ctx.moveTo(toX(pts[0].x), toY(pts[0].y));
        for (let i = 1; i < pts.length; i++) ctx.lineTo(toX(pts[i].x), toY(pts[i].y));
        ctx.closePath();
        ctx.fillStyle = fill;
        ctx.fill();
        ctx.setLineDash(dash);
        ctx.strokeStyle = stroke;
        ctx.lineWidth = width;
        ctx.stroke();
        ctx.restore();
    };

    const fillColor = isDark ? "rgba(10, 132, 255, 0.15)" : "rgba(10, 98, 255, 0.12)";
    zones.forEach((zone, idx) => {
        const selected = idx === selectedIndex;
        fillPolygon(
            zone.points,
            fillColor,
            selected ? "rgba(10, 132, 255, 0.95)" : "rgba(10, 132, 255, 0.55)",
            selected ? 3 : 1.5,
        );
        if (zone.label && zone.points.length > 0) {
            const cx = zone.points.reduce((sum, p) => sum + p.x, 0) / zone.points.length;
            const cy = zone.points.reduce((sum, p) => sum + p.y, 0) / zone.points.length;
            ctx.save();
            ctx.fillStyle = isDark ? "rgba(255, 255, 255, 0.85)" : "rgba(20, 20, 20, 0.85)";
            ctx.font = "12px sans-serif";
            ctx.textAlign = "center";
            ctx.textBaseline = "middle";
            ctx.fillText(zone.label, toX(cx), toY(cy));
            ctx.restore();
        }
    });

    if (draft && draft.length >= 2) {
        fillPolygon(draft, "rgba(255, 204, 0, 0.12)", "rgba(255, 204, 0, 0.9)", 2, [4, 4]);
    }
}

// Only the selected shape's handles are drawn — matches what pickHandleAt hit-tests.
// Radii divided by `zoom` so they stay a constant screen-pixel size.
function drawVertexHandles(
    ctx: CanvasRenderingContext2D,
    proj: MapProjection,
    zoom: number,
    selectedNoGoIndex: number | null,
    selectedZoneIndex: number | null,
    lines: Point[][],
    zones: ZoneShape[],
) {
    let points: Point[] | null = null;
    let closed = false;
    if (selectedNoGoIndex != null) {
        points = lines[selectedNoGoIndex] ?? null;
    } else if (selectedZoneIndex != null) {
        points = zones[selectedZoneIndex]?.points ?? null;
        closed = true;
    }
    if (!points || points.length === 0) return;

    const { toX, toY } = proj;
    const vertexR = HANDLE_DRAW_RADIUS_PX / zoom;
    const midpointR = HANDLE_MIDPOINT_DRAW_RADIUS_PX / zoom;
    // Matches pickHandleAt's segCount rule: zones close the polygon, no-go lines don't.
    const segCount = closed ? points.length : points.length - 1;

    // Midpoints first so vertex handles paint on top at shared screen positions.
    ctx.save();
    ctx.fillStyle = "rgba(255, 255, 255, 0.45)";
    ctx.strokeStyle = "rgba(10, 132, 255, 0.55)";
    ctx.lineWidth = 1 / zoom;
    for (let i = 0; i < segCount; i++) {
        const a = points[i];
        const b = points[(i + 1) % points.length];
        ctx.beginPath();
        ctx.arc(toX((a.x + b.x) / 2), toY((a.y + b.y) / 2), midpointR, 0, Math.PI * 2);
        ctx.fill();
        ctx.stroke();
    }
    ctx.restore();

    ctx.save();
    ctx.fillStyle = "rgba(255, 255, 255, 0.95)";
    ctx.strokeStyle = "rgba(10, 132, 255, 0.95)";
    ctx.lineWidth = 1.5 / zoom;
    for (const p of points) {
        ctx.beginPath();
        ctx.arc(toX(p.x), toY(p.y), vertexR, 0, Math.PI * 2);
        ctx.fill();
        ctx.stroke();
    }
    ctx.restore();
}

// Draws the animated robot sprite: a filled circle with a small nose pointing
// in the heading direction. Theta is in degrees, 0 = +X axis (canvas right),
// increasing counter-clockwise in world coordinates, so we flip the sign when
// applying it to screen coordinates (Y axis is inverted by toY).
function drawRobotSprite(ctx: CanvasRenderingContext2D, x: number, y: number, thetaDeg: number) {
    const radius = 7;
    const screenAngle = -(thetaDeg * Math.PI) / 180;

    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(screenAngle);

    // Heading wedge behind the body so it reads as a direction arrow
    ctx.beginPath();
    ctx.moveTo(radius + 5, 0);
    ctx.lineTo(radius * 0.6, -radius * 0.7);
    ctx.lineTo(radius * 0.6, radius * 0.7);
    ctx.closePath();
    ctx.fillStyle = "rgba(52, 199, 89, 0.95)";
    ctx.fill();

    // Body
    ctx.shadowColor = "rgba(52, 199, 89, 0.6)";
    ctx.shadowBlur = 10;
    ctx.beginPath();
    ctx.arc(0, 0, radius, 0, Math.PI * 2);
    ctx.fillStyle = "rgba(52, 199, 89, 0.95)";
    ctx.fill();
    ctx.shadowBlur = 0;

    // Inner dot for contrast
    ctx.beginPath();
    ctx.arc(0, 0, radius * 0.35, 0, Math.PI * 2);
    ctx.fillStyle = "rgba(255, 255, 255, 0.9)";
    ctx.fill();

    ctx.restore();
}
