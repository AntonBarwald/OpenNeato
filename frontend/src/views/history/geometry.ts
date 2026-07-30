// Pure geometry helpers for the map editor (Guided Clean — zones & no-go lines):
// canvasToWorld (inverse of renderMap's projection) and the segment/polygon predicates the
// firmware replay engine mirrors for no-go-line skipping and zone filtering.

import type { MapProjection } from "./helpers";

export interface Point {
    x: number;
    y: number;
}

// Inverse of renderMap's project -> pan/zoom -> rotate pipeline, undone in reverse order.
// devicePixelRatio doesn't appear here: pointer inputs are already in CSS px, the same
// space renderMap draws in before its ctx.scale(dpr).
export function canvasToWorld(
    sx: number,
    sy: number,
    proj: MapProjection,
    tf: { panX: number; panY: number; zoom: number },
    rotationDeg: number,
    displayW: number,
    displayH: number,
): Point {
    // 1. Undo rotation about the canvas center (inverse rotation is by -theta).
    const cx = displayW / 2;
    const cy = displayH / 2;
    const theta = (-rotationDeg * Math.PI) / 180;
    const cos = Math.cos(theta);
    const sin = Math.sin(theta);
    const dx = sx - cx;
    const dy = sy - cy;
    const qx = cx + cos * dx - sin * dy;
    const qy = cy + sin * dx + cos * dy;

    // 2. Undo pan/zoom (screen = pan + zoom * projected).
    const px = (qx - tf.panX) / tf.zoom;
    const py = (qy - tf.panY) / tf.zoom;

    // 3. Undo the world projection.
    const wx = proj.minX + (px - proj.offX) / proj.scale;
    const wy = proj.maxY - (py - proj.offY) / proj.scale;
    return { x: wx, y: wy };
}

// 2D cross product of (b-a) x (c-a). Sign gives orientation of a->b->c.
function cross(a: Point, b: Point, c: Point): number {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// True when point q lies on segment a-b, assuming the three are collinear.
function onSegment(a: Point, b: Point, q: Point): boolean {
    return (
        Math.min(a.x, b.x) <= q.x && q.x <= Math.max(a.x, b.x) && Math.min(a.y, b.y) <= q.y && q.y <= Math.max(a.y, b.y)
    );
}

// Exact predicate the firmware replay engine uses to decide if a path hop crosses a
// no-go line — keep the two implementations in lockstep.
export function segmentsIntersect(a1: Point, a2: Point, b1: Point, b2: Point): boolean {
    const d1 = cross(b1, b2, a1);
    const d2 = cross(b1, b2, a2);
    const d3 = cross(a1, a2, b1);
    const d4 = cross(a1, a2, b2);

    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
        return true;
    }
    // Collinear / endpoint-touching cases.
    if (d1 === 0 && onSegment(b1, b2, a1)) return true;
    if (d2 === 0 && onSegment(b1, b2, a2)) return true;
    if (d3 === 0 && onSegment(a1, a2, b1)) return true;
    if (d4 === 0 && onSegment(a1, a2, b2)) return true;
    return false;
}

// True when a polyline (open no-go line) crosses the segment a-b anywhere.
export function polylineCrossesSegment(polyline: Point[], a: Point, b: Point): boolean {
    for (let i = 0; i + 1 < polyline.length; i++) {
        if (segmentsIntersect(a, b, polyline[i], polyline[i + 1])) return true;
    }
    return false;
}

// Ray-casting point-in-polygon; a point exactly on an edge is not specially handled.
export function pointInPolygon(pt: Point, polygon: Point[]): boolean {
    let inside = false;
    for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
        const pi = polygon[i];
        const pj = polygon[j];
        const intersects = pi.y > pt.y !== pj.y > pt.y && pt.x < ((pj.x - pi.x) * (pt.y - pi.y)) / (pj.y - pi.y) + pi.x;
        if (intersects) inside = !inside;
    }
    return inside;
}

// Shortest distance from point p to segment a-b. Used for hit-testing: pick a
// no-go line or vertex when the pointer is within a screen-derived radius.
export function distPointToSegment(p: Point, a: Point, b: Point): number {
    const abx = b.x - a.x;
    const aby = b.y - a.y;
    const lenSq = abx * abx + aby * aby;
    if (lenSq === 0) return Math.hypot(p.x - a.x, p.y - a.y);
    let t = ((p.x - a.x) * abx + (p.y - a.y) * aby) / lenSq;
    t = Math.max(0, Math.min(1, t));
    return Math.hypot(p.x - (a.x + t * abx), p.y - (a.y + t * aby));
}
