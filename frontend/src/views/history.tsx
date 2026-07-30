import { useCallback, useEffect, useMemo, useState } from "preact/hooks";
import { api, ResponseParseError } from "../api";
import backSvg from "../assets/icons/back.svg?raw";
import { ConfirmDialog } from "../components/confirm-dialog";
import { ErrorBanner, ErrorBannerStack, useErrorStack } from "../components/error-banner";
import { Icon } from "../components/icon";
import { useNavigate, usePath } from "../components/router";
import { useDirtyGuard } from "../hooks/use-dirty-guard";
import { usePoll } from "../hooks/use-poll";
import type { PollResult } from "../hooks/use-polling";
import { T, useI18n } from "../i18n";
import type { ErrorData, HistoryFileInfo, MapData, ZonesBlob } from "../types";
import { deriveRobotError, normalizeError } from "../utils";
import type { Point } from "./history/geometry";
import type { ZoneShape } from "./history/helpers";
import { HistoryItemView } from "./history/item";
import { HistoryListView } from "./history/list";

// Reference session's enforced zone/no-go shapes for a just-started guided run.
interface GuidedZoneOverlay {
    zones: ZoneShape[];
    noGoLines: Point[][];
}

const RECOVERY_GUIDE_URL =
    "https://github.com/renjfk/OpenNeato/blob/main/docs/user-guide.md#recovering-corrupted-cleaning-history";

interface HistoryViewProps {
    error: PollResult<ErrorData>;
}

export function HistoryView({ error }: HistoryViewProps) {
    const { t } = useI18n();
    const navigate = useNavigate();
    const path = usePath();
    const [errors, errorStack] = useErrorStack();
    const [files, setFiles] = useState<HistoryFileInfo[]>([]);
    const [loading, setLoading] = useState(true);
    const [selectedMap, setSelectedMap] = useState<MapData | null>(null);
    const [mapEmpty, setMapEmpty] = useState(false);
    const [deleting, setDeleting] = useState(false);
    // Set when the list response is unparseable (e.g. one session's metadata
    // contains malformed JSON that breaks the surrounding response). Triggers
    // the recovery panel instead of the normal list view.
    const [listCorrupted, setListCorrupted] = useState(false);
    const [confirmReset, setConfirmReset] = useState(false);

    // Derive view from URL: /history = list, /history/<name> = a session's detail view.
    const selectedName = path.startsWith("/history/") ? decodeURIComponent(path.slice(9)) : null;
    const selectedFile = useMemo(
        () => (selectedName ? (files.find((f) => f.name === selectedName) ?? null) : null),
        [selectedName, files],
    );
    const selectedRecording = selectedFile?.recording === true;
    const robotError = deriveRobotError(error);
    const hasRecording = files.some((f) => f.recording);
    // A guided recording still in flight from before this update (map viewing/live overlay
    // only — nothing left in the UI can start a new one).
    const activeGuidedFile = useMemo(
        () => files.find((f) => f.recording && f.session?.mode === "guided") ?? null,
        [files],
    );
    // Zone/no-go shapes enforced by the guided run this flow just started (only known here).
    const [guidedOverlay, setGuidedOverlay] = useState<GuidedZoneOverlay | null>(null);

    // `zonesServer` is the last API-confirmed value; `zonesDraft` is the locally edited copy
    // the map editor mutates. Both live here (not HistoryItemView) so dirty tracking and the
    // discard-on-navigate guard can compare them.
    const [zonesServer, setZonesServer] = useState<ZonesBlob | null>(null);
    const [zonesDraft, setZonesDraft] = useState<ZonesBlob | null>(null);
    const [savingZones, setSavingZones] = useState(false);

    const zonesDirty =
        zonesDraft !== null && zonesServer !== null && JSON.stringify(zonesDraft) !== JSON.stringify(zonesServer);

    const { guardedNavigate, showDiscardConfirm, setShowDiscardConfirm, handleDiscard } = useDirtyGuard(zonesDirty);

    // Sort sessions by date descending (newest first)
    const sortByDateDesc = (list: HistoryFileInfo[]) =>
        list.sort((a, b) => (b.session?.time ?? 0) - (a.session?.time ?? 0));

    // Load file list only (no full session data)
    useEffect(() => {
        setLoading(true);
        setListCorrupted(false);
        api.getHistoryList()
            .then((fileList) => setFiles(sortByDateDesc(fileList)))
            .catch((e: unknown) => {
                if (e instanceof ResponseParseError) {
                    setListCorrupted(true);
                } else {
                    errorStack.push(normalizeError(e, "Failed to load map data"));
                }
            })
            .finally(() => setLoading(false));
    }, []); // eslint-disable-line react-hooks/exhaustive-deps

    // Poll list + active recording session map (every 5s while recording)
    usePoll(
        async () => {
            const fileList = await api.getHistoryList();
            setFiles(sortByDateDesc(fileList));

            if (selectedName) {
                const file = fileList.find((f) => f.name === selectedName);
                if (file?.recording) {
                    const maps = await api.getHistorySession(file.name);
                    if (maps.length > 0) setSelectedMap(maps[0]);
                }
            }
        },
        5000,
        hasRecording,
    );

    // Fetch full session data when URL points to a file
    useEffect(() => {
        if (!selectedName) {
            setSelectedMap(null);
            setMapEmpty(false);
            return;
        }
        setSelectedMap(null);
        setMapEmpty(false);
        const isRecording = files.find((f) => f.name === selectedName)?.recording === true;
        api.getHistorySession(selectedName)
            .then((maps) => {
                if (maps.length > 0) {
                    setSelectedMap(maps[0]);
                } else if (!isRecording) {
                    setMapEmpty(true);
                }
            })
            .catch((e: unknown) => {
                errorStack.push(normalizeError(e, "Failed to load session"));
            });
    }, [selectedName, errorStack]);

    // Fetch saved zones/no-go lines for the selected session, independent of
    // the (potentially large, streamed) session map fetch above.
    useEffect(() => {
        if (!selectedName) {
            setZonesServer(null);
            setZonesDraft(null);
            return;
        }
        setZonesServer(null);
        setZonesDraft(null);
        api.getZones(selectedName)
            .then((zones) => {
                setZonesServer(zones);
                setZonesDraft(zones);
            })
            .catch((e: unknown) => {
                errorStack.push(normalizeError(e, "Failed to load zones"));
            });
    }, [selectedName, errorStack]);

    // Clear stale errors once we're back on the list view (mirrors the old
    // handleBack behavior, now decoupled from navigation since that's
    // guarded and may not fire immediately when edits are unsaved).
    useEffect(() => {
        if (!selectedName) errorStack.clear();
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [selectedName]);

    // Drop the overlay once its session stops being the active guided recording.
    useEffect(() => {
        if (!activeGuidedFile) setGuidedOverlay(null);
    }, [activeGuidedFile]);

    const handleSelect = useCallback(
        (idx: number) => {
            const file = files[idx];
            if (!file) return;
            guardedNavigate(`/history/${file.name}`);
        },
        [files, guardedNavigate],
    );

    const handleBack = useCallback(() => {
        guardedNavigate(selectedName ? "/history" : "/");
    }, [selectedName, guardedNavigate]);

    const handleTogglePin = useCallback(
        (idx: number) => {
            const file = files[idx];
            if (!file) return;
            const wasPinned = file.pinned === true;
            (wasPinned ? api.unpinSession(file.name) : api.pinSession(file.name))
                .then(() => api.getHistoryList())
                .then((fileList) => setFiles(sortByDateDesc(fileList)))
                .catch((e: unknown) => {
                    errorStack.push(normalizeError(e, wasPinned ? "Failed to unpin session" : "Failed to pin session"));
                });
        },
        [files, errorStack],
    );

    const handleAddNoGoLine = useCallback((points: Point[]) => {
        setZonesDraft((cur) => (cur ? { ...cur, noGoLines: [...cur.noGoLines, { points }] } : cur));
    }, []);

    const handleDeleteNoGoLine = useCallback((index: number) => {
        setZonesDraft((cur) => (cur ? { ...cur, noGoLines: cur.noGoLines.filter((_, i) => i !== index) } : cur));
    }, []);

    const handleAddZone = useCallback((points: Point[]) => {
        setZonesDraft((cur) =>
            cur ? { ...cur, zones: [...cur.zones, { points, label: `Zone ${cur.zones.length + 1}` }] } : cur,
        );
    }, []);

    const handleDeleteZone = useCallback((index: number) => {
        setZonesDraft((cur) => (cur ? { ...cur, zones: cur.zones.filter((_, i) => i !== index) } : cur));
    }, []);

    const handleRenameZone = useCallback((index: number, label: string) => {
        setZonesDraft((cur) =>
            cur ? { ...cur, zones: cur.zones.map((z, i) => (i === index ? { ...z, label } : z)) } : cur,
        );
    }, []);

    // Vertex move / midpoint-insert / vertex-delete all collapse to the same
    // operation — replace this shape's point array at index — mirroring the
    // immutable-update pattern every other draft mutation above already uses.
    const handleUpdateNoGoLine = useCallback((index: number, points: Point[]) => {
        setZonesDraft((cur) =>
            cur ? { ...cur, noGoLines: cur.noGoLines.map((l, i) => (i === index ? { points } : l)) } : cur,
        );
    }, []);

    const handleUpdateZone = useCallback((index: number, points: Point[]) => {
        setZonesDraft((cur) =>
            cur ? { ...cur, zones: cur.zones.map((z, i) => (i === index ? { ...z, points } : z)) } : cur,
        );
    }, []);

    const handleSaveZones = useCallback(() => {
        if (!selectedName || !zonesDraft) return;
        setSavingZones(true);
        api.saveZones(selectedName, zonesDraft)
            .then((saved) => {
                setZonesServer(saved);
                setZonesDraft(saved);
            })
            .catch((e: unknown) => {
                errorStack.push(normalizeError(e, "Failed to save zones"));
            })
            .finally(() => setSavingZones(false));
    }, [selectedName, zonesDraft, errorStack]);

    const handleDiscardZones = useCallback(() => {
        setZonesDraft(zonesServer);
    }, [zonesServer]);

    const handleDeleteSession = useCallback(
        (idx: number) => {
            const file = files[idx];
            if (!file) return;
            setDeleting(true);
            api.deleteHistorySession(file.name)
                .then(() => api.getHistoryList())
                .then((fileList) => {
                    setFiles(sortByDateDesc(fileList));
                    if (selectedName === file.name) navigate("/history");
                })
                .catch((e: unknown) => {
                    errorStack.push(normalizeError(e, "Failed to delete"));
                })
                .finally(() => setDeleting(false));
        },
        [files, selectedName, navigate, errorStack],
    );

    const handleDeleteAll = useCallback(() => {
        setDeleting(true);
        api.deleteAllHistory()
            .then(() => {
                setFiles([]);
                setListCorrupted(false);
                if (selectedName) navigate("/history");
            })
            .catch((e: unknown) => {
                errorStack.push(normalizeError(e, "Failed to delete"));
            })
            .finally(() => setDeleting(false));
    }, [selectedName, navigate, errorStack]);

    const handleImported = useCallback(() => {
        api.getHistoryList()
            .then((fileList) => setFiles(sortByDateDesc(fileList)))
            .catch((e: unknown) => {
                errorStack.push(normalizeError(e, "Failed to refresh list"));
            });
    }, [errorStack]);

    const showDetail = selectedName !== null && selectedFile !== null;

    const nogoLines = useMemo(() => zonesDraft?.noGoLines.map((l) => l.points) ?? [], [zonesDraft]);
    const zoneShapes = useMemo(
        () => zonesDraft?.zones.map((z) => ({ points: z.points, label: z.label })) ?? [],
        [zonesDraft],
    );

    // Live guided session has no saved zones of its own yet - show the enforced ones instead.
    const showingGuidedOverlay = selectedRecording && activeGuidedFile?.name === selectedName;
    const displayNogoLines = showingGuidedOverlay && guidedOverlay ? guidedOverlay.noGoLines : nogoLines;
    const displayZones = showingGuidedOverlay && guidedOverlay ? guidedOverlay.zones : zoneShapes;

    return (
        <>
            <div class="header">
                <button type="button" class="header-back-btn" onClick={handleBack} aria-label={t("Back")}>
                    <Icon svg={backSvg} />
                </button>
                <h1>{t(showDetail ? "Clean Map" : "Cleaning History")}</h1>
                <div class="header-right-spacer" />
            </div>

            {selectedRecording && robotError && (
                <ErrorBanner
                    title={t(robotError.title)}
                    message={robotError.message}
                    hint={robotError.hint}
                    variant={robotError.kind}
                />
            )}
            {selectedRecording && !error.data && error.error && (
                <ErrorBanner title={t("Warning")} message={error.error} />
            )}

            <ErrorBannerStack errors={errors} />

            <div class="history-page">
                {loading && (
                    <div class="history-empty">
                        <T>Loading...</T>
                    </div>
                )}

                {!loading && listCorrupted && !showDetail && (
                    <div class="history-recovery">
                        <h2 class="history-recovery-title">
                            <T>Cleaning history is corrupted</T>
                        </h2>
                        <p class="history-recovery-msg">
                            <T>
                                One of the stored sessions contains malformed data and is preventing the list from
                                loading. The recovery guide explains how to identify and remove the bad session(s)
                                without losing the rest. If you'd rather not investigate, you can wipe everything in one
                                go.
                            </T>
                        </p>
                        <div class="history-recovery-actions">
                            <a
                                class="history-recovery-link"
                                href={RECOVERY_GUIDE_URL}
                                target="_blank"
                                rel="noopener noreferrer"
                            >
                                <T>Open recovery guide</T>
                            </a>
                            <button
                                type="button"
                                class={`history-delete-all-btn${deleting ? " pending" : ""}`}
                                onClick={() => setConfirmReset(true)}
                                disabled={deleting}
                            >
                                <T>Delete all history</T>
                            </button>
                        </div>
                    </div>
                )}

                {!loading && !listCorrupted && files.length === 0 && !showDetail && (
                    <HistoryListView
                        files={files}
                        hasRecording={false}
                        deleting={false}
                        onSelect={handleSelect}
                        onDeleteSession={handleDeleteSession}
                        onDeleteAll={handleDeleteAll}
                        onImported={handleImported}
                        onError={errorStack.push}
                        onTogglePin={handleTogglePin}
                    />
                )}

                {!loading && !listCorrupted && files.length > 0 && !showDetail && (
                    <HistoryListView
                        files={files}
                        hasRecording={hasRecording}
                        deleting={deleting}
                        onSelect={handleSelect}
                        onDeleteSession={handleDeleteSession}
                        onDeleteAll={handleDeleteAll}
                        onImported={handleImported}
                        onError={errorStack.push}
                        onTogglePin={handleTogglePin}
                    />
                )}

                {!loading && showDetail && (
                    <HistoryItemView
                        file={selectedFile}
                        map={selectedMap}
                        mapEmpty={mapEmpty}
                        recording={selectedRecording}
                        zonesReady={zonesDraft !== null}
                        nogoLines={displayNogoLines}
                        zones={displayZones}
                        zonesDirty={zonesDirty}
                        savingZones={savingZones}
                        onAddNoGoLine={handleAddNoGoLine}
                        onDeleteNoGoLine={handleDeleteNoGoLine}
                        onAddZone={handleAddZone}
                        onDeleteZone={handleDeleteZone}
                        onUpdateNoGoLine={handleUpdateNoGoLine}
                        onUpdateZone={handleUpdateZone}
                        onRenameZone={handleRenameZone}
                        onSaveZones={handleSaveZones}
                        onDiscardZones={handleDiscardZones}
                    />
                )}

                {confirmReset && (
                    <ConfirmDialog
                        message={t("Delete all map data?")}
                        confirmLabel={t("Delete")}
                        disabled={deleting}
                        onConfirm={() => {
                            setConfirmReset(false);
                            handleDeleteAll();
                        }}
                        onCancel={() => setConfirmReset(false)}
                    />
                )}

                {showDiscardConfirm && (
                    <ConfirmDialog
                        message={t("You have unsaved no-go line and zone edits. Discard them?")}
                        confirmLabel={t("Discard")}
                        onConfirm={handleDiscard}
                        onCancel={() => setShowDiscardConfirm(false)}
                    />
                )}
            </div>
        </>
    );
}
