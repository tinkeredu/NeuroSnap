#include "CameraSettings.h"

#include <SD_MMC.h>

namespace {

constexpr char settingsPath[] = "/settings.json";
constexpr char backupPath[] = "/settings.bak";
constexpr char temporaryPath[] = "/settings.tmp";
constexpr size_t settingsSizeLimit = 24576; // Includes JSON escaping.

String readLine(File &file) {
    String line = file.readStringUntil('\n');
    line.trim();
    return line;
}

String readTextFile(const char *path) {
    File file = SD_MMC.open(path);
    String value;
    if (file) {
        value = file.readString();
        value.trim();
    }
    return value;
}

bool loadJson(CameraSettings &settings, const char *path) {
    File file = SD_MMC.open(path);
    if (!file || file.size() > settingsSizeLimit) return false;
    JsonDocument doc;
    if (deserializeJson(doc, file)) return false;

    CameraSettings candidate;
    if (candidate.applyJson(doc.as<JsonVariantConst>()).length()) return false;
    settings = candidate;
    return true;
}

bool readField(JsonVariantConst json, const char *key, String &field,
               unsigned limit, bool multiline = false) {
    const JsonVariantConst fieldJson = json[key];
    if (fieldJson.isUnbound()) return true;
    const JsonString value = fieldJson.as<JsonString>();
    if (!value || value.size() > limit) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const uint8_t c = value.c_str()[i];
        if (c == 127 || (c < 32 && !(multiline && (c == '\n' || c == '\r' || c == '\t')))) {
            return false;
        }
    }
    field = value.c_str();
    return field.length() == value.size();
}

bool validHost(const String &host) {
    if (!host.length() || host[0] == '.' || host.endsWith(".")) return false;
    for (unsigned i = 0; i < host.length(); ++i) {
        const char c = host[i];
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.')) return false;
    }
    return true;
}

bool validPath(const String &path) {
    return path.startsWith("/") && !path.startsWith("//") && path.indexOf(' ') < 0 &&
           path.indexOf('#') < 0 && path.indexOf('\\') < 0;
}

} // namespace

String CameraSettings::applyJson(JsonVariantConst json) {
    if (!json.is<JsonObjectConst>()) return "Einstellungen muessen ein JSON-Objekt sein.";
    if (!readField(json, "prompt", prompt, 3000, true) ||
        !readField(json, "header", header, 512, true) ||
        !readField(json, "footer", footer, 512, true)) {
        return "Prompt oder Drucktext ist zu lang oder enthaelt ungueltige Zeichen.";
    }
    if (!readField(json, "api_key", vision.apiKey, 256) ||
        !readField(json, "api_url", vision.apiUrl, 256) ||
        !readField(json, "api_model", vision.apiModel, 96) ||
        !readField(json, "upload_host", vision.uploadHost, 253) ||
        !readField(json, "upload_path", vision.uploadPath, 256) ||
        !readField(json, "dropoff_path", vision.dropoffPath, 256) ||
        !vision.apiUrl.startsWith("https://") || !vision.apiModel.length() ||
        vision.apiKey.indexOf(' ') >= 0 || !validHost(vision.uploadHost) ||
        !validPath(vision.uploadPath) || !validPath(vision.dropoffPath)) {
        return "Ungueltige KI- oder Server-Einstellungen. Die API benoetigt eine HTTPS-URL.";
    }
    if (!readField(json, "admin_password", adminPassword, 63) || !adminPassword.length()) {
        return "Das Admin-Passwort darf 1 bis 63 UTF-8-Bytes lang sein.";
    }
    if (!json["mode"].isUnbound()) {
        const String value = json["mode"] | "";
        if (value == "ai_text") mode = CaptureMode::AiText;
        else if (value == "photo") mode = CaptureMode::Photo;
        else if (value == "photo_ai") mode = CaptureMode::PhotoAi;
        else return "Unbekannter Aufnahmemodus.";
    }
    if (!json["photo_protocol"].isUnbound()) {
        const String value = json["photo_protocol"] | "";
        if (value == "esc_pos") photoProtocol = PhotoProtocol::EscPos;
        else if (value == "adafruit") photoProtocol = PhotoProtocol::Adafruit;
        else return "Unbekanntes Fotodruck-Protokoll.";
    }
    if (!json["transfer"].isUnbound()) {
        const String value = json["transfer"] | "";
        if (value == "direct") vision.transfer = ImageTransfer::Direct;
        else if (value == "dropoff") vision.transfer = ImageTransfer::Dropoff;
        else return "Unbekannter KI-Bildversand.";
    }
    if (json["logging_enabled"].is<bool>()) logging = json["logging_enabled"].as<bool>();
    if (json["guided_mode"].is<bool>()) guided = json["guided_mode"].as<bool>();
    return "";
}

void CameraSettings::toJson(JsonDocument &doc, bool includeSecrets) const {
    switch (mode) {
        case CaptureMode::AiText: doc["mode"] = "ai_text"; break;
        case CaptureMode::Photo: doc["mode"] = "photo"; break;
        case CaptureMode::PhotoAi: doc["mode"] = "photo_ai"; break;
    }
    doc["prompt"] = prompt;
    doc["header"] = header;
    doc["footer"] = footer;
    doc["photo_protocol"] = photoProtocol == PhotoProtocol::EscPos ? "esc_pos" : "adafruit";
    doc["api_url"] = vision.apiUrl;
    doc["api_model"] = vision.apiModel;
    doc["transfer"] = vision.transfer == ImageTransfer::Direct ? "direct" : "dropoff";
    doc["upload_host"] = vision.uploadHost;
    doc["upload_path"] = vision.uploadPath;
    doc["dropoff_path"] = vision.dropoffPath;
    doc["logging_enabled"] = logging;
    doc["guided_mode"] = guided;
    if (includeSecrets) {
        doc["api_key"] = vision.apiKey;
        doc["admin_password"] = adminPassword;
    } else {
        doc["api_key_configured"] = vision.apiKey.length() > 0;
    }
}

void CameraSettings::loadFromSD() {
    if (loadJson(*this, settingsPath)) return;
    for (const char *recovery : {backupPath, temporaryPath}) {
        if (loadJson(*this, recovery)) {
            if (SD_MMC.exists(settingsPath)) SD_MMC.remove(settingsPath);
            SD_MMC.rename(recovery, settingsPath);
            Serial.println("SD-Einstellungen wiederhergestellt.");
            return;
        }
    }
    if (SD_MMC.exists(settingsPath)) Serial.println("Ungueltige settings.json. Lade Altkonfiguration.");

    File legacy = SD_MMC.open("/config.txt");
    if (legacy) {
        vision.apiKey = readLine(legacy);
        if (vision.apiKey == "DEIN_MISTRAL_API_KEY") vision.apiKey = "";
        const String url = readLine(legacy);
        const String model = readLine(legacy);
        const String host = readLine(legacy);
        const String upload = readLine(legacy);
        logging = readLine(legacy) == "1";
        const String dropoff = readLine(legacy);
        guided = readLine(legacy) == "1";
        if (url.startsWith("https://")) vision.apiUrl = url;
        if (model.length()) vision.apiModel = model;
        if (validHost(host)) vision.uploadHost = host;
        if (validPath(upload)) vision.uploadPath = upload;
        if (validPath(dropoff)) vision.dropoffPath = dropoff;
        // Preserve a previously configured relay. New installations use direct upload.
        if (vision.apiKey.length() && vision.uploadHost != "example.com" &&
            vision.uploadHost != "dein-server.de") {
            vision.transfer = ImageTransfer::Dropoff;
        }
    }
    const String password = readTextFile("/password.txt");
    if (password.length()) adminPassword = password;
    else if (!SD_MMC.exists("/password.txt") && legacy) adminPassword = "admin";
    const String savedPrompt = readTextFile("/prompt.txt");
    if (savedPrompt.length()) prompt = savedPrompt;
}

bool CameraSettings::saveToSD() const {
    JsonDocument doc;
    toJson(doc, true);
    String json;
    if (doc.overflowed() || serializeJson(doc, json) != measureJson(doc) ||
        json.length() > settingsSizeLimit) return false;

    File file = SD_MMC.open(temporaryPath, FILE_WRITE);
    if (!file) return false;
    const bool written = file.print(json) == json.length();
    file.close();
    if (!written) {
        SD_MMC.remove(temporaryPath);
        return false;
    }
    if (SD_MMC.exists(settingsPath)) {
        if (SD_MMC.exists(backupPath) && !SD_MMC.remove(backupPath)) return false;
        if (!SD_MMC.rename(settingsPath, backupPath)) return false;
    }
    if (!SD_MMC.rename(temporaryPath, settingsPath)) {
        if (SD_MMC.exists(backupPath)) SD_MMC.rename(backupPath, settingsPath);
        return false;
    }
    if (SD_MMC.exists(backupPath)) SD_MMC.remove(backupPath);
    return true;
}
