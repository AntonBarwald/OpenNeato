// Small shared utilities. Keep dependency-free and side-effect-free so this
// module can be imported from anywhere (components, hooks, plain modules)
// without pulling in Preact.
import type { PollResult } from "./hooks/use-polling";
import type { ErrorData, HistoryFileInfo } from "./types";

// Zero-pad a non-negative integer to the given width. Used for clock and
// date formatting throughout the UI.
export function pad2(n: number): string {
    return n.toString().padStart(2, "0");
}

// Format hour/minute as "HH:MM".
export function fmtTime(h: number, m: number): string {
    return `${pad2(h)}:${pad2(m)}`;
}

const TIME_RE = /^([01]\d|2[0-3]):([0-5]\d)$/;

// Parse "HH:MM" into { hour, minute }, or null if invalid.
export function parseTime(value: string): { hour: number; minute: number } | null {
    const match = value.match(TIME_RE);
    if (!match) return null;
    return { hour: Number.parseInt(match[1], 10), minute: Number.parseInt(match[2], 10) };
}

// Best-effort extraction of a human-readable message from a caught value.
// Falls back to the provided default when the value isn't an Error instance.
export function normalizeError(e: unknown, fallback = "Something went wrong"): string {
    return e instanceof Error ? e.message : fallback;
}

// The session currently being recorded, if any.
export function findRecordingSession(files: HistoryFileInfo[]): HistoryFileInfo | undefined {
    return files.find((f) => f.recording);
}

// Delay for bounded retry loops.
export function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

// Derive robot error/warning banner content from a polled /api/error result,
// shared by dashboard.tsx and history.tsx so both surfaces render it identically.
export interface RobotError {
    kind: "error" | "warning";
    title: string;
    message: string;
    hint?: string;
}

export function deriveRobotError(error: PollResult<ErrorData>): RobotError | null {
    return error.data?.hasError
        ? {
              kind: error.data.kind === "warning" ? "warning" : "error",
              title: error.data.kind === "warning" ? "Robot Notice" : "Robot Attention Needed",
              message: error.data.displayMessage || `Robot reported error ${error.data.errorCode}.`,
              hint: error.data.recoveryHint || undefined,
          }
        : null;
}
