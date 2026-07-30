#include "web_server.h"
#include "web_assets.h"
#include "neato_serial.h"
#include "data_logger.h"
#include "system_manager.h"
#include "settings_manager.h"
#include "firmware_manager.h"
#include "manual_clean_manager.h"
#include "notification_manager.h"
#include "cleaning_history.h"
#include "wifi_manager.h"
#include "zones_manager.h"
#include "navigation_poc.h"
#include "navigation_manager.h"
#include "maintenance_tracker.h"
#include "whole_house_timer.h"
#include <SPIFFS.h>

unsigned long WebServer::lastApiActivity = 0;

WebServer::WebServer(AsyncWebServer& server, NeatoSerial& neato, DataLogger& logger, SystemManager& sys,
                     FirmwareManager& fw, SettingsManager& settings, ManualCleanManager& manual,
                     NotificationManager& notif, CleaningHistory& history, WiFiManager& wifi, ZonesManager& zones,
                     NavigationPoc& navPoc, NavigationManager& navMgr, MaintenanceTracker& maint,
                     WholeHouseTimer& wholeHouseTimer) :
    server(server), neato(neato), logger(logger), sysMgr(sys), fwMgr(fw), settingsMgr(settings), manualMgr(manual),
    notifMgr(notif), historyMgr(history), wifiMgr(wifi), zonesMgr(zones), navPoc(navPoc), navMgr(navMgr),
    maintMgr(maint), wholeHouseTimer(wholeHouseTimer) {}

void WebServer::loggedRoute(const char *path, WebRequestMethodComposite httpMethod, SyncHandler handler) {
    server.on(path, httpMethod, [this, handler](AsyncWebServerRequest *request) {
        lastApiActivity = millis();
        unsigned long startMs = lastApiActivity;
        int status = handler(request);
        logger.logRequest(request->method(), request->url().c_str(), status, millis() - startMs);
    });
}

void WebServer::runBodyHandler(AsyncWebServerRequest *request, const BodyHandler& handler, uint8_t *data, size_t len) {
    lastApiActivity = millis();
    unsigned long startMs = lastApiActivity;
    int status = handler(request, data, len);
    logger.logRequest(request->method(), request->url().c_str(), status, millis() - startMs);
}

void WebServer::loggedBodyRoute(const char *path, WebRequestMethodComposite httpMethod, BodyHandler handler) {
    server.on(
            path, httpMethod, [](AsyncWebServerRequest *request) { /* handled in body callback */ }, nullptr,
            [this, handler](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
                // Body callback fires once per TCP segment; accumulate across fragments and
                // invoke the handler once, on the last fragment.

                // Fast path: whole body arrived in one segment — no buffering.
                if (index == 0 && len == total) {
                    runBodyHandler(request, handler, data, len);
                    return;
                }

                // Bound memory before allocating. Per-route handlers still apply
                // their own (smaller) caps once the body is assembled.
                if (total > WEB_BODY_MAX_BYTES) {
                    if (index == 0)
                        sendError(request, 413, "request body too large");
                    return;
                }

                // malloc (not new) so the library's free(_tempObject) cleanup can't mis-free it.
                if (index == 0)
                    request->_tempObject = malloc(total);
                uint8_t *buf = static_cast<uint8_t *>(request->_tempObject);
                if (buf == nullptr) {
                    if (index + len >= total)
                        sendError(request, 500, "out of memory");
                    return;
                }
                memcpy(buf + index, data, len);
                if (index + len < total)
                    return; // wait for the remaining segments

                runBodyHandler(request, handler, buf, total);
                free(buf);
                request->_tempObject = nullptr;
            });
}

void WebServer::sendGzipAsset(AsyncWebServerRequest *request, const uint8_t *data, size_t len,
                              const char *contentType) {
    AsyncWebServerResponse *response = request->beginResponse(200, contentType, data, len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void WebServer::sendError(AsyncWebServerRequest *request, int code, const String& msg) {
    request->send(code, "application/json", fieldsToJson({{"error", msg, FIELD_STRING}}));
}

void WebServer::sendOk(AsyncWebServerRequest *request) {
    request->send(200, "application/json", fieldsToJson({{"ok", "true", FIELD_BOOL}}));
}

void WebServer::begin() {
    // Register all embedded frontend assets from the auto-generated registry
    for (size_t i = 0; i < WEB_ASSETS_COUNT; i++) {
        const WebAsset& asset = WEB_ASSETS[i];
        server.on(asset.path, HTTP_GET, [&asset](AsyncWebServerRequest *request) {
            sendGzipAsset(request, asset.data, asset.length, asset.contentType);
        });
    }

    LOG("WEB", "Registered %u embedded assets", WEB_ASSETS_COUNT);

    registerApiRoutes();
    registerManualRoutes();
    registerLogRoutes();
    registerSystemRoutes();
    registerSettingsRoutes();
    registerFirmwareRoutes();
    registerMapRoutes();
    registerWiFiRoutes();
    registerNavigationRoutes();
    registerMaintenanceRoutes();

    LOG("WEB", "Frontend and API routes registered");
}

void WebServer::registerApiRoutes() {
    // -- Sensor query endpoints ----------------------------------------------

    registerGetRoute("/api/version", neato, &NeatoSerial::getVersion, {});
    registerGetRoute("/api/charger", neato, &NeatoSerial::getCharger, {});
    registerGetRoute("/api/analog", neato, &NeatoSerial::getBatteryAnalog, {});
    registerGetRoute("/api/warranty", neato, &NeatoSerial::getBatteryWarranty, {});
    registerGetRoute(
            "/api/motors", neato,
            static_cast<void (NeatoSerial::*)(std::function<void(bool, const MotorData&)>)>(&NeatoSerial::getMotors),
            {});
    registerGetRoute("/api/state", neato, &NeatoSerial::getState, {});
    registerGetRoute("/api/error", neato, &NeatoSerial::getErr, {});
    registerGetRoute("/api/lidar", neato, &NeatoSerial::getLdsScan, {});
    registerGetRoute("/api/user-settings", neato, &NeatoSerial::getUserSettings, {});

    // -- Action endpoints ----------------------------------------------------
    // All parameterized actions use query strings: resource URL identifies the
    // command, query params carry arguments (mirrors Neato serial protocol).

    registerPostRoute("/api/clean", neato, &NeatoSerial::clean, {"action", "width", "height"});
    registerPostRoute("/api/sound", neato, &NeatoSerial::playSound, {"id"});
    registerPostRoute("/api/power", neato, &NeatoSerial::powerControl, {"action"});
    registerPostRoute("/api/lidar/rotate", neato, &NeatoSerial::setLdsRotation, {"enable"});
    registerPostRoute("/api/user-settings", neato, &NeatoSerial::setUserSetting, {"key", "value"});
    registerPostRoute("/api/clear-errors", neato, &NeatoSerial::clearErrors, {});
    registerPostRoute("/api/battery/new", neato, &NeatoSerial::newBattery, {});

    // Whole-house early-return timer -- local state only, no serial I/O, so hand-written
    // rather than registerPostRoute/registerGetRoute (both built around NeatoSerial callbacks).
    // POST /api/clean-timer?action=arm&minutes=<n> -- arms the timer for n minutes.
    // POST /api/clean-timer?action=cancel -- disarms it (the dashboard calls this before
    // dispatching any other clean action, so a stale timer never outlives the run it was set for).
    loggedRoute("/api/clean-timer", HTTP_POST, [this](AsyncWebServerRequest *request) -> int {
        if (!request->hasParam("action")) {
            sendError(request, 400, "missing action");
            return 400;
        }
        String action = request->getParam("action")->value();
        if (action == "cancel") {
            wholeHouseTimer.cancel();
            sendOk(request);
            return 200;
        }
        if (action == "arm") {
            if (!request->hasParam("minutes")) {
                sendError(request, 400, "missing minutes");
                return 400;
            }
            int minutes = request->getParam("minutes")->value().toInt();
            // Upper bound matters, not just lower: durationMinutes * 60000UL overflows the 32-bit
            // unsigned long deadline math above ~71583 minutes (see clean_timer_policy.h), so an
            // unbounded large value can silently wrap into a deadline seconds away, not days away.
            if (minutes <= 0 || minutes > CLEAN_TIMER_MAX_MINUTES) {
                sendError(request, 400, "minutes must be between 1 and " + String(CLEAN_TIMER_MAX_MINUTES));
                return 400;
            }
            wholeHouseTimer.arm(static_cast<unsigned long>(minutes));
            sendOk(request);
            return 200;
        }
        sendError(request, 400, "invalid action");
        return 400;
    });

    // GET /api/clean-timer -- whole-house early-return timer status (no serial I/O)
    loggedRoute("/api/clean-timer", HTTP_GET, [this](AsyncWebServerRequest *request) -> int {
        request->send(200, "application/json",
                      fieldsToJson({{"armed", wholeHouseTimer.isArmed() ? "true" : "false", FIELD_BOOL},
                                    {"remainingSec", String(wholeHouseTimer.remainingSec()), FIELD_INT}}));
        return 200;
    });

    // Serial endpoint — send arbitrary serial command, returns raw response.
    // Always available (no debug gate — useful for diagnostics without enabling verbose logging).
    // Excluded from public API docs (diagnostics-only passthrough).
    server.on("/api/serial", HTTP_POST, [this](AsyncWebServerRequest *request) {
        lastApiActivity = millis();
        unsigned long startMs = lastApiActivity;

        if (!request->hasParam("cmd")) {
            logger.logRequest(HTTP_POST, "/api/serial", 400, millis() - startMs);
            sendError(request, 400, "missing cmd");
            return;
        }
        String cmd = request->getParam("cmd")->value();
        if (cmd.isEmpty()) {
            logger.logRequest(HTTP_POST, "/api/serial", 400, millis() - startMs);
            sendError(request, 400, "empty cmd");
            return;
        }

        auto weak = request->pause();
        bool ok = neato.sendRaw(cmd, [this, weak, startMs](bool /*success*/, const String& response) {
            if (auto req = weak.lock()) {
                unsigned long elapsed = millis() - startMs;
                logger.logRequest(HTTP_POST, "/api/serial", 200, elapsed);
                req->send(200, "text/plain", response);
            }
        });
        if (!ok) {
            logger.logRequest(HTTP_POST, "/api/serial", 503, millis() - startMs);
            sendError(request, 503, "unavailable");
        }
    });

    LOG("WEB", "API routes registered");
}

// -- Manual clean endpoints ---------------------------------------------------

void WebServer::registerManualRoutes() {
    // Register longer paths first — ESPAsyncWebServer matches routes by prefix,
    // so /api/manual would swallow /api/manual/move and /api/manual/motors.

    loggedRoute("/api/manual/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        request->send(200, "application/json", manualMgr.getStatusJson());
        return 200;
    });
    registerPostRoute("/api/manual/move", manualMgr, &ManualCleanManager::move, {"left", "right", "speed"});
    registerPostRoute("/api/manual/motors", manualMgr, &ManualCleanManager::setMotors,
                      {"brush", "vacuum", "sideBrush"});
    registerPostRoute("/api/manual", manualMgr, &ManualCleanManager::enable, {"enable"});

    LOG("WEB", "Manual clean routes registered");
}

// -- Log file endpoints ------------------------------------------------------

// Strip .hs extension so browser saves a plain .jsonl file
static String downloadName(const String& filename) {
    if (filename.endsWith(".hs"))
        return filename.substring(0, filename.length() - 3);
    return filename;
}

// Validates /zones and /pin sub-routes so they 404 on a made-up session name.
static bool sessionFileExists(const String& name) {
    return SPIFFS.exists(String(HISTORY_DIR) + "/" + name);
}

static String logListJson(const std::vector<LogFileInfo>& files) {
    String json = "[";
    for (size_t i = 0; i < files.size(); i++) {
        if (i > 0)
            json += ",";
        json += files[i].toJson();
    }
    json += "]";
    return json;
}

void WebServer::registerLogRoutes() {

    // GET /api/logs[/filename] — list logs or download a specific file
    // A single BackwardCompatible handler matches both "/api/logs" and "/api/logs/..."
    // This route uses server.on() directly instead of loggedRoute() because
    // compressed log downloads use chunked streaming (beginChunkedResponse) which
    // must not block — loggedRoute's sync wrapper would block until completion.
    server.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest *request) {
        unsigned long startMs = millis();
        String filename = request->url().substring(String("/api/logs/").length());

        if (filename.isEmpty()) {
            String json = logListJson(logger.listLogs());
            logger.logRequest(HTTP_GET, "/api/logs", 200, millis() - startMs);
            request->send(200, "application/json", json);
            return;
        }

        // Open log via DataLogger — handles path resolution and transparent decompression
        auto reader = logger.readLog(filename);
        if (!reader) {
            logger.logRequest(HTTP_GET, request->url().c_str(), 404, millis() - startMs);
            sendError(request, 404, "log not found");
            return;
        }

        logger.logRequest(HTTP_GET, request->url().c_str(), 200, millis() - startMs);

        // Stream log content via chunked response — reader handles decompression
        AsyncWebServerResponse *response = request->beginChunkedResponse(
                "application/x-ndjson",
                [reader](uint8_t *buffer, size_t maxLen, size_t) -> size_t { return reader->read(buffer, maxLen); });

        response->addHeader("Content-Disposition", "attachment; filename=\"" + downloadName(filename) + "\"");

        request->send(response);
    });

    // DELETE /api/logs[/filename] — delete all logs or a specific file
    loggedRoute("/api/logs", HTTP_DELETE, [this](AsyncWebServerRequest *request) -> int {
        String filename = request->url().substring(String("/api/logs/").length());

        if (filename.isEmpty()) {
            logger.deleteAllLogs();
            sendOk(request);
            return 200;
        }

        if (logger.deleteLog(filename)) {
            sendOk(request);
            return 200;
        }

        sendError(request, 404, "log not found");
        return 404;
    });

    LOG("WEB", "Log routes registered");
}

// -- System health endpoint ---------------------------------------------------

void WebServer::registerSystemRoutes() {

    // GET /api/system — live system health (heap, uptime, RSSI, storage, NTP)
    loggedRoute("/api/system", HTTP_GET, [this](AsyncWebServerRequest *request) -> int {
        request->send(200, "application/json", sysMgr.getSystemHealth(settingsMgr.get().tz).toJson());
        return 200;
    });

    // POST /api/system/restart — deferred restart
    loggedRoute("/api/system/restart", HTTP_POST, [this](AsyncWebServerRequest *request) -> int {
        sendOk(request);
        sysMgr.restart();
        return 200;
    });

    // POST /api/system/reset — factory reset (clears NVS + WiFi, then restarts)
    loggedRoute("/api/system/reset", HTTP_POST, [this](AsyncWebServerRequest *request) -> int {
        sendOk(request);
        sysMgr.factoryReset();
        return 200;
    });

    // POST /api/system/format-fs — format filesystem (erases logs + map data, then restarts)
    loggedRoute("/api/system/format-fs", HTTP_POST, [this](AsyncWebServerRequest *request) -> int {
        sendOk(request);
        sysMgr.formatFs();
        return 200;
    });

    LOG("WEB", "System routes registered");
}

// -- Settings endpoint -------------------------------------------------------

void WebServer::registerSettingsRoutes() {

    // GET /api/settings — all user-configurable settings
    loggedRoute("/api/settings", HTTP_GET, [this](AsyncWebServerRequest *request) -> int {
        request->send(200, "application/json", settingsMgr.get().toJson());
        return 200;
    });

    // PUT /api/settings — partial update (only fields present are written)
    loggedBodyRoute("/api/settings", HTTP_PUT,
                    [this](AsyncWebServerRequest *request, uint8_t *data, size_t len) -> int {
                        String body = String(reinterpret_cast<const char *>(data), len);
                        ApplyResult result = settingsMgr.apply(body);
                        if (result == APPLY_INVALID) {
                            sendError(request, 400, "Invalid settings");
                            return 400;
                        }
                        if (result == APPLY_CHANGED) {
                            // Push manual clean settings to manager (no reboot needed)
                            const auto& s = settingsMgr.get();
                            manualMgr.setStallThreshold(s.stallThreshold);
                            manualMgr.setBrushRpm(s.brushRpm);
                            manualMgr.setVacuumSpeed(s.vacuumSpeed);
                            manualMgr.setSideBrushPower(s.sideBrushPower);
                        }
                        request->send(200, "application/json", settingsMgr.get().toJson());
                        return 200;
                    });

    // POST /api/notifications/test?topic=<topic> — send a test notification
    loggedRoute("/api/notifications/test", HTTP_POST, [this](AsyncWebServerRequest *request) -> int {
        if (!request->hasParam("topic")) {
            sendError(request, 400, "missing topic");
            return 400;
        }
        String topic = request->getParam("topic")->value();
        if (topic.isEmpty()) {
            sendError(request, 400, "topic cannot be empty");
            return 400;
        }
        notifMgr.sendTestNotification(topic);
        sendOk(request);
        return 200;
    });

    LOG("WEB", "Settings routes registered");
}

// -- Firmware endpoints -------------------------------------------------------

void WebServer::registerFirmwareRoutes() {

    // GET /api/firmware/version — current ESP32 firmware version + chip model + robot support status
    loggedRoute("/api/firmware/version", HTTP_GET, [this](AsyncWebServerRequest *request) -> int {
        std::vector<Field> fields = {
                {"name", "OpenNeato", FIELD_STRING},
                {"version", fwMgr.getFirmwareVersion(), FIELD_STRING},
                {"chip", fwMgr.getChipModel(), FIELD_STRING},
                {"model", neato.getModelName(), FIELD_STRING},
                {"hostname", settingsMgr.get().hostname, FIELD_STRING},
                {"supported", isSupportedModel(neato.getModelName()) ? "true" : "false", FIELD_BOOL},
                {"identifying", neato.isIdentifying() ? "true" : "false", FIELD_BOOL},
                {"repositoryUrl", "https://github.com/renjfk/OpenNeato", FIELD_STRING},
                {"license", "MIT", FIELD_STRING},
        };
        request->send(200, "application/json", fieldsToJson(fields));
        return 200;
    });

    // POST /api/firmware/update?hash=<md5> — single-request firmware upload
    server.on(
            "/api/firmware/update", HTTP_POST,
            // Response handler (called after upload completes)
            [this](AsyncWebServerRequest *request) {
                unsigned long startMs = millis();
                bool ok = fwMgr.getError().isEmpty();

                if (ok) {
                    ok = fwMgr.endUpdate();
                }

                if (!ok) {
                    logger.logRequest(HTTP_POST, "/api/firmware/update", 400, millis() - startMs);
                    sendError(request, 400, fwMgr.getError());
                } else {
                    logger.logRequest(HTTP_POST, "/api/firmware/update", 200, millis() - startMs);
                    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "OK");
                    response->addHeader("Connection", "close");
                    request->send(response);
                }
            },
            // Upload handler (called per chunk)
            [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len,
                   bool final) {
                // First chunk: initialize update session
                if (!index) {
                    String md5 = request->hasParam("hash") ? request->getParam("hash")->value() : "";
                    if (!fwMgr.beginUpdate(md5)) {
                        return;
                    }
                }

                if (len) {
                    fwMgr.writeChunk(data, len);

                    // Report progress at most once per second
                    static unsigned long lastProgressMs = 0;
                    unsigned long now = millis();
                    if (now - lastProgressMs >= 1000) {
                        auto percent = static_cast<uint8_t>(
                                request->contentLength() > 0 ? static_cast<float>(fwMgr.getProgress()) * 100.0f /
                                                                       static_cast<float>(request->contentLength())
                                                             : 0);
                        LOG("FW", "Progress: %u%% (%zu/%zu bytes)", percent, fwMgr.getProgress(),
                            request->contentLength());
                        lastProgressMs = now;
                    }
                }
            });

    LOG("WEB", "Firmware routes registered");
}

// -- Map data endpoints -------------------------------------------------------

void WebServer::registerMapRoutes() {

    // GET /api/history[/filename] — list sessions, collection status, or download a specific file
    // Also handles GET /api/history/<name>/zones (fetch stored no-go/zone geometry).
    server.on("/api/history", HTTP_GET, [this](AsyncWebServerRequest *request) {
        lastApiActivity = millis();
        unsigned long startMs = lastApiActivity;
        String suffix = request->url().substring(String("/api/history/").length());

        if (suffix.isEmpty()) {
            // List all session files with embedded session/summary metadata
            auto sessions = historyMgr.listSessions();
            String json = "[";
            for (size_t i = 0; i < sessions.size(); i++) {
                if (i > 0)
                    json += ",";
                const auto& s = sessions[i];
                json += R"({"name":")" + s.name + R"(","size":)" + String(static_cast<unsigned long>(s.size)) +
                        R"(,"compressed":)" + String(s.compressed ? "true" : "false") + R"(,"recording":)" +
                        String(s.recording ? "true" : "false") + R"(,"pinned":)" +
                        String(zonesMgr.isPinned(s.name) ? "true" : "false");
                if (s.session.length() > 0) {
                    json += ",\"session\":" + s.session;
                } else {
                    json += ",\"session\":null";
                }
                if (s.summary.length() > 0) {
                    json += ",\"summary\":" + s.summary;
                } else {
                    json += ",\"summary\":null";
                }
                json += "}";
            }
            json += "]";
            logger.logRequest(HTTP_GET, "/api/history", 200, millis() - startMs);
            request->send(200, "application/json", json);
            return;
        }

        // GET /api/history/<name>/zones — fetch stored no-go/zone geometry (verbatim JSON)
        if (suffix.endsWith("/zones")) {
            String name = suffix.substring(0, suffix.length() - String("/zones").length());
            if (!zonesMgr.hasZones(name)) {
                logger.logRequest(HTTP_GET, request->url().c_str(), 404, millis() - startMs);
                sendError(request, 404, "no zones for session");
                return;
            }
            logger.logRequest(HTTP_GET, request->url().c_str(), 200, millis() - startMs);
            request->send(200, "application/json", zonesMgr.getZones(name));
            return;
        }

        // Download specific session
        auto reader = historyMgr.readSession(suffix);
        if (!reader) {
            logger.logRequest(HTTP_GET, request->url().c_str(), 404, millis() - startMs);
            sendError(request, 404, "session not found");
            return;
        }

        logger.logRequest(HTTP_GET, request->url().c_str(), 200, millis() - startMs);

        AsyncWebServerResponse *response = request->beginChunkedResponse(
                "application/x-ndjson",
                [reader](uint8_t *buffer, size_t maxLen, size_t) -> size_t { return reader->read(buffer, maxLen); });

        response->addHeader("Content-Disposition", "attachment; filename=\"" + downloadName(suffix) + "\"");

        request->send(response);
    });

    // PUT /api/history/<name>/zones — store no-go/zone geometry (verbatim JSON, size-capped)
    loggedBodyRoute("/api/history", HTTP_PUT, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len) -> int {
        String suffix = request->url().substring(String("/api/history/").length());
        if (!suffix.endsWith("/zones")) {
            sendError(request, 404, "not found");
            return 404;
        }
        String name = suffix.substring(0, suffix.length() - String("/zones").length());
        if (!sessionFileExists(name)) {
            sendError(request, 404, "session not found");
            return 404;
        }
        // Fail fast on oversized declared bodies (defense-in-depth; setZones also
        // caps). Multi-segment bodies are assembled by loggedBodyRoute first.
        if (request->contentLength() > ZONES_MAX_BYTES) {
            sendError(request, 413, "zones payload too large");
            return 413;
        }
        String body = String(reinterpret_cast<const char *>(data), len);
        String error;
        if (!zonesMgr.setZones(name, body, error)) {
            sendError(request, 400, error);
            return 400;
        }
        // Echo the persisted blob, not {ok:true} — saveZones() stores the response as the
        // new draft; {ok:true} made zonesDraft.noGoLines undefined and crashed the map view.
        request->send(200, "application/json", body);
        return 200;
    });

    // DELETE /api/history[/filename] — delete one or all sessions.
    // Also handles DELETE /api/history/<name>/zones and /api/history/<name>/pin.
    loggedRoute("/api/history", HTTP_DELETE, [this](AsyncWebServerRequest *request) -> int {
        String suffix = request->url().substring(String("/api/history/").length());

        if (suffix.isEmpty()) {
            historyMgr.deleteAllSessions();
            zonesMgr.deleteAll();
            sendOk(request);
            return 200;
        }

        if (suffix.endsWith("/zones")) {
            String name = suffix.substring(0, suffix.length() - String("/zones").length());
            if (!zonesMgr.deleteZones(name)) {
                sendError(request, 404, "no zones for session");
                return 404;
            }
            sendOk(request);
            return 200;
        }

        if (suffix.endsWith("/pin")) {
            String name = suffix.substring(0, suffix.length() - String("/pin").length());
            if (!sessionFileExists(name)) {
                sendError(request, 404, "session not found");
                return 404;
            }
            zonesMgr.unpin(name);
            sendOk(request);
            return 200;
        }

        if (historyMgr.deleteSession(suffix)) {
            // Clean up any zones/pin state so it doesn't linger as orphaned storage.
            zonesMgr.deleteZones(suffix);
            zonesMgr.unpin(suffix);
            sendOk(request);
            return 200;
        }

        sendError(request, 404, "session not found");
        return 404;
    });

    // POST /api/history/import — upload a .jsonl session file, compress and store
    server.on(
            "/api/history/import", HTTP_POST,
            // Response handler (called after upload completes)
            [this](AsyncWebServerRequest *request) {
                unsigned long startMs = millis();
                bool ok = historyMgr.getImportError().isEmpty();

                if (ok) {
                    ok = historyMgr.endImport();
                }

                if (!ok) {
                    logger.logRequest(HTTP_POST, "/api/history/import", 400, millis() - startMs);
                    sendError(request, 400, historyMgr.getImportError());
                } else {
                    logger.logRequest(HTTP_POST, "/api/history/import", 200, millis() - startMs);
                    sendOk(request);
                }
            },
            // Upload handler (called per chunk)
            [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len,
                   bool final) {
                // First chunk: initialize import session
                if (!index) {
                    if (!historyMgr.beginImport(filename)) {
                        return;
                    }
                }

                if (len && historyMgr.isImporting()) {
                    historyMgr.writeImportChunk(data, len);
                }
            });

    // POST /api/history/<name>/pin — mark a session as pinned (protected from cleanup eviction).
    // Registered after /api/history/import so that more specific route keeps matching first
    // (ESPAsyncWebServer matches by prefix in registration order — see registerManualRoutes).
    loggedRoute("/api/history", HTTP_POST, [this](AsyncWebServerRequest *request) -> int {
        String suffix = request->url().substring(String("/api/history/").length());
        if (!suffix.endsWith("/pin")) {
            sendError(request, 404, "not found");
            return 404;
        }
        String name = suffix.substring(0, suffix.length() - String("/pin").length());
        if (!sessionFileExists(name)) {
            sendError(request, 404, "session not found");
            return 404;
        }
        zonesMgr.pin(name);
        sendOk(request);
        return 200;
    });

    LOG("WEB", "History routes registered");
}

// -- WiFi management endpoints -----------------------------------------------

void WebServer::registerWiFiRoutes() {
    // GET /api/wifi/status , STA + fallback AP snapshot
    registerGetRoute("/api/wifi/status", wifiMgr, &WiFiManager::getStatus, {});

    // GET /api/wifi/scan , list nearby networks
    registerGetRoute("/api/wifi/scan", wifiMgr, &WiFiManager::scanNetworks, {});

    // POST /api/wifi/connect?ssid=&password= , save credentials and connect.
    // On success the device reboots into normal STA mode; on failure the
    // fallback AP stays up so the user can retry.
    registerPostRoute("/api/wifi/connect", wifiMgr, &WiFiManager::connect, {"ssid", "password"});

    // POST /api/wifi/disconnect , clear credentials and drop the connection
    registerPostRoute("/api/wifi/disconnect", wifiMgr, &WiFiManager::disconnect, {});

    LOG("WEB", "WiFi routes registered");
}

// -- Navigation endpoints (#70 POC, #71 Guided Clean) -------------------------

void WebServer::registerNavigationRoutes() {
    // Register longer paths first — ESPAsyncWebServer matches by prefix (see registerManualRoutes).

    // GET /api/navigate/status — #70 POC state (no serial I/O)
    loggedRoute("/api/navigate/status", HTTP_GET, [this](AsyncWebServerRequest *request) -> int {
        request->send(200, "application/json", navPoc.getStatusJson());
        return 200;
    });

    // POST /api/navigate/stop — abort the #70 POC drive, idempotent
    loggedRoute("/api/navigate/stop", HTTP_POST, [this](AsyncWebServerRequest *request) -> int {
        navPoc.stop();
        sendOk(request);
        return 200;
    });

    // POST /api/navigate — start the #70 POC drive. Body: JSON point array
    // (point_array_json.h format), e.g. [{"x":1.2,"y":3.4},{"x":2,"y":2}].
    loggedBodyRoute("/api/navigate", HTTP_POST,
                    [this](AsyncWebServerRequest *request, uint8_t *data, size_t len) -> int {
                        String body = String(reinterpret_cast<const char *>(data), len);
                        String error;
                        if (!navPoc.start(body, error)) {
                            sendError(request, 409, error);
                            return 409;
                        }
                        sendOk(request);
                        return 200;
                    });

    // GET /api/guided/status — #71 Guided Clean state machine status (no serial I/O).
    // Kept live even though start is retired below: NavigationManager itself isn't deleted
    // (dormant, not gone), so a way to observe it stays available.
    loggedRoute("/api/guided/status", HTTP_GET, [this](AsyncWebServerRequest *request) -> int {
        request->send(200, "application/json", navMgr.getStatusJson());
        return 200;
    });

    // POST /api/guided?action=stop — abort immediately, idempotent. Deliberately kept working
    // even though start is retired: removing a stop path is the opposite of safety, and this
    // is a no-cost brake to keep in case anything (a future re-enable, a residual code path)
    // ever gets navMgr into a running state again.
    // POST /api/guided?action=start — retired. Guided Clean's navigation is unvalidated
    // (unproven infeasible; uncalibrated cliff-sensor backstop) and must not be startable from
    // any surface. Refuses explicitly (409) rather than a silent no-op, so a caller — this was
    // reachable before retirement via a pinned session name — knows the request was rejected,
    // not just dropped.
    loggedRoute("/api/guided", HTTP_POST, [this](AsyncWebServerRequest *request) -> int {
        if (!request->hasParam("action")) {
            sendError(request, 400, "missing action");
            return 400;
        }
        String action = request->getParam("action")->value();

        if (action == "stop") {
            navMgr.stop();
            sendOk(request);
            return 200;
        }

        if (action == "start") {
            sendError(request, 409, "Guided Clean is retired and cannot be started");
            return 409;
        }

        sendError(request, 400, "invalid action");
        return 400;
    });

    LOG("WEB", "Navigation routes registered");
}

// -- Maintenance/consumable tracking endpoints --------------------------------

void WebServer::registerMaintenanceRoutes() {
    // Synchronous, local (no serial I/O), so loggedRoute directly rather than
    // registerGetRoute/registerPostRoute (built around async serial callbacks).

    // GET /api/maintenance — hours-used and replacement interval per consumable
    loggedRoute("/api/maintenance", HTTP_GET, [this](AsyncWebServerRequest *request) -> int {
        request->send(200, "application/json", maintMgr.get().toJson());
        return 200;
    });

    // POST /api/maintenance/reset?item=brush|filter|sideBrush|sensors
    loggedRoute("/api/maintenance/reset", HTTP_POST, [this](AsyncWebServerRequest *request) -> int {
        String itemName = request->hasParam("item") ? request->getParam("item")->value() : "";
        MaintenanceItem item = parseMaintenanceItem(itemName);
        if (item == MAINT_ITEM_INVALID) {
            sendError(request, 400, "invalid item");
            return 400;
        }
        maintMgr.reset(item);
        sendOk(request);
        return 200;
    });

    LOG("WEB", "Maintenance routes registered");
}
