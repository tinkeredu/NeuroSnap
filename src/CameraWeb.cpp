#include "CameraWeb.h"

#include <SD_MMC.h>
#include <Update.h>
#include <WebServer.h>

#include "CameraWebPage.h"

namespace {

WebServer server(80);
constexpr char TEXT[] = "text/plain; charset=utf-8";
constexpr char REQUEST_HEADER[] = "X-NeuroSnap-Request";
unsigned long wifiPortalAt = 0;
unsigned long restartAt = 0;
void (*startWifiPortal)() = nullptr;
bool updateReady = false;

bool isAuthorized(const CameraSettings &settings) {
    return server.authenticate("admin", settings.adminPassword.c_str());
}

bool authorize(const CameraSettings &settings, bool write = false) {
    server.sendHeader("Cache-Control", "no-store");
    if (!isAuthorized(settings)) {
        server.send(401, TEXT, "Falsches Admin-Passwort.");
        return false;
    }
    if (write && server.header(REQUEST_HEADER) != "1") {
        server.send(403, TEXT, "Ungültiger Zugriff.");
        return false;
    }
    return true;
}

void sendFile(const String &path, const char *type) {
    File file = SD_MMC.open(path);
    if (file && !file.isDirectory()) server.streamFile(file, type);
    else server.send(404, TEXT, "Datei nicht gefunden.");
}

} // namespace

bool isCaptureName(const String &name, const char *extension) {
    if (!name.startsWith("bild_") || !name.endsWith(extension) ||
        name.length() < 10 || name.length() > 19) return false;
    for (unsigned i = 5; i < name.length() - 4; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

void configureCameraWeb(CameraSettings &settings, bool sdReady, const bool &captureActive,
                        void (*openWifiPortal)()) {
    startWifiPortal = openWifiPortal;
    const char *headers[] = {REQUEST_HEADER};
    server.collectHeaders(headers, 1);
    server.on("/", HTTP_GET, [] {
        server.send_P(200, "text/html; charset=utf-8", CAMERA_WEB_PAGE);
    });
    server.on("/list", HTTP_GET, [sdReady] {
        File root = sdReady ? SD_MMC.open("/") : File();
        if (!root) {
            server.send(200, "application/json", "[]");
            return;
        }
        // Stream the list so the number of photos does not determine peak heap use.
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "application/json", "");
        server.sendContent("[");
        bool first = true;
        File file;
        while ((file = root.openNextFile())) {
            const String name = file.name();
            if (!file.isDirectory() && isCaptureName(name, ".jpg")) {
                server.sendContent(String(first ? "" : ",") + "{\"name\":\"/" + name + "\"}");
                first = false;
            }
        }
        server.sendContent("]");
        server.sendContent("");
    });
    server.onNotFound([] {
        const String uri = server.uri();
        if (server.method() == HTTP_GET && uri.startsWith("/sd/")) {
            const String name = uri.substring(4);
            if (isCaptureName(name, ".jpg")) return sendFile("/" + name, "image/jpeg");
            if (isCaptureName(name, ".txt")) return sendFile("/" + name, TEXT);
        }
        server.send(404, TEXT, "Seite nicht gefunden.");
    });
    server.on("/delete", HTTP_POST, [&settings] {
        if (!authorize(settings, true)) return;
        const String name = server.arg("plain");
        if (!isCaptureName(name, ".jpg")) return server.send(400, TEXT, "Ungültiger Bildname.");
        if (!SD_MMC.remove("/" + name)) return server.send(500, TEXT, "Bild konnte nicht gelöscht werden.");
        const String caption = "/" + name.substring(0, name.length() - 4) + ".txt";
        if (SD_MMC.exists(caption)) SD_MMC.remove(caption);
        server.send(200, TEXT, "Bild gelöscht.");
    });
    server.on("/settings", HTTP_GET, [&settings, sdReady] {
        if (!authorize(settings)) return;
        JsonDocument doc;
        settings.toJson(doc, false);
        doc["sd_ready"] = sdReady;
        String body;
        if (doc.overflowed() || serializeJson(doc, body) != measureJson(doc)) {
            return server.send(500, TEXT, "Einstellungen konnten nicht geladen werden.");
        }
        server.send(200, "application/json", body);
    });
    server.on("/settings", HTTP_POST, [&settings, sdReady] {
        if (!authorize(settings, true)) return;
        if (!sdReady) return server.send(503, TEXT, "Zum Speichern ist eine SD-Karte erforderlich.");
        const String body = server.arg("plain");
        if (body.length() > 24576) return server.send(413, TEXT, "Einstellungen sind zu groß.");
        JsonDocument doc;
        if (deserializeJson(doc, body)) return server.send(400, TEXT, "Ungültiges JSON.");
        CameraSettings updated = settings;
        const String error = updated.applyJson(doc.as<JsonVariantConst>());
        if (error.length()) return server.send(400, TEXT, error);
        if (!updated.saveToSD()) return server.send(500, TEXT, "Speichern auf der SD-Karte fehlgeschlagen.");
        settings = updated;
        server.send(200, TEXT, "Einstellungen gespeichert.");
    });
    server.on("/getLog", HTTP_GET, [&settings, sdReady] {
        if (!authorize(settings)) return;
        if (sdReady && SD_MMC.exists("/system.log")) sendFile("/system.log", TEXT);
        else server.send(200, TEXT, "Kein Log vorhanden.");
    });
    server.on("/deleteLog", HTTP_POST, [&settings] {
        if (!authorize(settings, true)) return;
        if (SD_MMC.exists("/system.log") && !SD_MMC.remove("/system.log")) {
            return server.send(500, TEXT, "Log konnte nicht gelöscht werden.");
        }
        server.send(200, TEXT, "Log gelöscht.");
    });
    server.on("/wifi", HTTP_POST, [&settings] {
        if (!authorize(settings, true)) return;
        server.send(200, TEXT, "Verbinde dich mit NeuroSnap_Setup (Passwort neurosnap) und öffne http://192.168.4.1.");
        wifiPortalAt = millis() + 500;
    });
    server.on("/update", HTTP_POST, [&settings, &captureActive] {
        if (!authorize(settings, true)) return;
        if (captureActive) {
            return server.send(409, TEXT, "Warte mit dem Firmware-Update, bis die Aufnahme abgeschlossen ist.");
        }
        const bool success = updateReady && Update.isRunning() && Update.end(true);
        updateReady = false;
        if (!success) {
            if (Update.isRunning()) Update.abort();
            return server.send(400, TEXT, "Firmware-Update fehlgeschlagen.");
        }
        server.send(200, TEXT, "Firmware aktualisiert. Die Kamera startet neu.");
        restartAt = millis() + 500;
    }, [&settings, &captureActive] {
        if (!isAuthorized(settings) || server.header(REQUEST_HEADER) != "1" || restartAt || captureActive) return;
        HTTPUpload &upload = server.upload();
        switch (upload.status) {
            case UPLOAD_FILE_START:
                if (Update.isRunning()) Update.abort();
                updateReady = false;
                if (upload.name == "firmware" && upload.filename.endsWith(".bin")) {
                    Update.begin(UPDATE_SIZE_UNKNOWN);
                }
                break;
            case UPLOAD_FILE_WRITE:
                if (Update.isRunning() && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    Update.abort();
                }
                break;
            case UPLOAD_FILE_END:
                updateReady = upload.totalSize > 0 && Update.isRunning();
                break;
            case UPLOAD_FILE_ABORTED:
                updateReady = false;
                if (Update.isRunning()) Update.abort();
                break;
        }
    });
}

bool cameraWebRestartPending() {
    return restartAt != 0;
}

void beginCameraWeb() {
    server.begin();
}

void serviceCameraWeb() {
    server.handleClient();
    if (restartAt && int32_t(millis() - restartAt) >= 0) ESP.restart();
    if (wifiPortalAt && int32_t(millis() - wifiPortalAt) >= 0) {
        wifiPortalAt = 0;
        startWifiPortal();
    }
}

void stopCameraWeb() {
    server.stop();
}
