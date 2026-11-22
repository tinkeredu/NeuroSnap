#include "VisionUpload.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <mbedtls/base64.h>
#include <time.h>

// Arduino's WiFiClientSecure ships another esp_crt_bundle.h. Use the ESP-IDF
// function linked with the SDK's built-in CA bundle instead.
extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

namespace {

constexpr size_t TRANSFER_BLOCK = 768; // A multiple of three so padding occurs only at the end.

class HttpsPost {
public:
    HttpsPost(const String &url, const String &contentType, int timeoutMs,
              const String &apiKey = "") {
        esp_http_client_config_t config{};
        config.url = url.c_str();
        config.method = HTTP_METHOD_POST;
        config.timeout_ms = timeoutMs;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        handle = esp_http_client_init(&config);
        if (handle) {
            esp_http_client_set_header(handle, "Connection", "close");
            esp_http_client_set_header(handle, "Content-Type", contentType.c_str());
            if (apiKey.length()) {
                String auth = "Bearer " + apiKey;
                esp_http_client_set_header(handle, "Authorization", auth.c_str());
            }
        }
    }

    ~HttpsPost() {
        if (handle) esp_http_client_cleanup(handle);
    }

    bool open(size_t length) {
        return handle && esp_http_client_open(handle, length) == ESP_OK;
    }

    bool write(const uint8_t *data, size_t length) {
        while (length) {
            const int sent = esp_http_client_write(handle, reinterpret_cast<const char *>(data), length);
            if (sent <= 0) return false;
            data += sent;
            length -= sent;
        }
        return true;
    }

    bool write(const String &part) {
        return write(reinterpret_cast<const uint8_t *>(part.c_str()), part.length());
    }

    bool writeFile(File &file) {
        uint8_t buffer[TRANSFER_BLOCK];
        size_t remaining = file.size();
        while (remaining) {
            const size_t count = file.read(buffer, min(remaining, sizeof(buffer)));
            if (!count || !write(buffer, count)) return false;
            remaining -= count;
        }
        return true;
    }

    String readResponse(const char *service, String &body, size_t limit) {
        if (esp_http_client_fetch_headers(handle) < 0) {
            return String(service) + ": keine HTTP-Antwort";
        }
        const int status = esp_http_client_get_status_code(handle);
        if (status != 200) return String(service) + ": HTTP " + status;

        char buffer[512];
        for (;;) {
            const int count = esp_http_client_read(handle, buffer, sizeof(buffer));
            if (count < 0) return String(service) + ": Lesefehler";
            if (count == 0) break;
            if (body.length() + count > limit || !body.concat(buffer, count)) {
                return String(service) + ": Antwort zu gross";
            }
        }
        if (!esp_http_client_is_complete_data_received(handle)) {
            return String(service) + ": unvollstaendige Antwort";
        }
        return "";
    }

private:
    esp_http_client_handle_t handle = nullptr;
};

String checkNetwork() {
    if (WiFi.status() != WL_CONNECTED) return "Kein WLAN";
    if (time(nullptr) < 1700000000) return "Uhrzeit fuer HTTPS noch nicht synchronisiert";
    return "";
}

String imagePart(const String &boundary, const String &name) {
    return "--" + boundary + "\r\nContent-Disposition: form-data; name=\"imageFile\"; filename=\"" +
           name + "\"\r\nContent-Type: image/jpeg\r\n\r\n";
}

String sendToDropoff(const VisionSettings &settings, const uint8_t *jpeg, size_t length, String &url) {
    const String boundary = "NeuroSnap" + String(esp_random(), HEX);
    const String head = imagePart(boundary, "capture.jpg");
    const String tail = "\r\n--" + boundary + "--\r\n";
    HttpsPost post("https://" + settings.uploadHost + settings.dropoffPath,
                   "multipart/form-data; boundary=" + boundary, 15000);
    if (!post.open(head.length() + length + tail.length()) || !post.write(head)) {
        return "Dropoff: HTTPS-Verbindung fehlgeschlagen";
    }
    for (size_t offset = 0; offset < length; offset += TRANSFER_BLOCK) {
        if (!post.write(jpeg + offset, min(TRANSFER_BLOCK, length - offset))) {
            return "Dropoff: Bild konnte nicht gesendet werden";
        }
    }
    if (!post.write(tail)) return "Dropoff: Upload unvollstaendig";
    String error = post.readResponse("Dropoff", url, 2048);
    if (error.length()) return error;
    url.trim();
    if (!(url.startsWith("https://") || url.startsWith("http://")) ||
        url.indexOf('\n') >= 0 || url.indexOf('\r') >= 0 || url.indexOf(' ') >= 0) {
        return "Dropoff: Keine gueltige Bild-URL";
    }
    return "";
}

bool sendBase64(HttpsPost &post, const uint8_t *jpeg, size_t length) {
    uint8_t encoded[4 * (TRANSFER_BLOCK / 3) + 1];
    for (size_t offset = 0; offset < length; offset += TRANSFER_BLOCK) {
        size_t encodedLength = 0;
        if (mbedtls_base64_encode(encoded, sizeof(encoded), &encodedLength, jpeg + offset,
                                  min(TRANSFER_BLOCK, length - offset)) != 0 ||
            !post.write(encoded, encodedLength)) {
            return false;
        }
    }
    return true;
}

} // namespace

String analyzePhoto(const VisionSettings &settings, const String &prompt,
                    const uint8_t *jpeg, size_t length, String &answer) {
    if (!settings.apiKey.length()) return "Mistral-API-Schluessel fehlt";
    const String networkError = checkNetwork();
    if (networkError.length()) return networkError;

    const bool direct = settings.transfer == ImageTransfer::Direct;
    String imageUrl;
    if (!direct) {
        const String error = sendToDropoff(settings, jpeg, length, imageUrl);
        if (error.length()) return error;
    }

    // The dynamic JSON strings are escaped independently so the JPEG can be streamed into image_url.
    String head = "{\"max_tokens\":320,\"model\":";
    JsonDocument literal;
    literal.set(settings.apiModel);
    serializeJson(literal, head);
    head += ",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":";
    literal.set(prompt);
    serializeJson(literal, head);
    head += "},{\"type\":\"image_url\",\"image_url\":";
    if (direct) {
        head += "\"data:image/jpeg;base64,";
    } else {
        literal.set(imageUrl);
        serializeJson(literal, head);
    }
    const String tail = direct ? "\"}]}]}" : "}]}]}";
    const size_t imageLength = direct ? 4 * ((length + 2) / 3) : 0;

    HttpsPost post(settings.apiUrl, "application/json", 60000, settings.apiKey);
    if (!post.open(head.length() + imageLength + tail.length()) || !post.write(head)) {
        return "Mistral: HTTPS-Verbindung fehlgeschlagen";
    }
    if ((direct && !sendBase64(post, jpeg, length)) || !post.write(tail)) {
        return "Mistral: Upload unvollstaendig";
    }
    String body;
    const String error = post.readResponse("Mistral", body, 16384);
    if (error.length()) return error;

    JsonDocument response;
    JsonDocument filter;
    filter["choices"][0]["message"]["content"] = true;
    if (deserializeJson(response, body, DeserializationOption::Filter(filter))) {
        return "Mistral: ungueltige JSON-Antwort";
    }
    const JsonVariantConst content = response["choices"][0]["message"]["content"];
    answer = content | "";
    for (JsonObjectConst part : content.as<JsonArrayConst>()) {
        if (part["type"] == "text") answer += part["text"] | "";
    }
    answer.trim();
    if (!answer.length()) return "Mistral: keine Textantwort";
    return "";
}

String uploadGalleryPhoto(const VisionSettings &settings, File &photo, File &caption,
                          const String &filename) {
    const String networkError = checkNetwork();
    if (networkError.length()) return networkError;
    const String boundary = "NeuroSnap" + String(esp_random(), HEX);
    const String head = "--" + boundary +
                        "\r\nContent-Disposition: form-data; name=\"textFileContent\"\r\n\r\n";
    const String photoHead = "\r\n" + imagePart(boundary, filename);
    const String tail = "\r\n--" + boundary + "--\r\n";
    HttpsPost post("https://" + settings.uploadHost + settings.uploadPath,
                   "multipart/form-data; boundary=" + boundary, 15000);
    if (!post.open(head.length() + caption.size() + photoHead.length() + photo.size() + tail.length())) {
        return "Galerie: HTTPS-Verbindung fehlgeschlagen";
    }
    if (!post.write(head) || !post.writeFile(caption) || !post.write(photoHead) ||
        !post.writeFile(photo) || !post.write(tail)) {
        return "Galerie: Upload unvollstaendig";
    }
    String reply;
    const String error = post.readResponse("Galerie", reply, 2048);
    if (error.length()) return error;
    return reply.startsWith("Success:") ? "" : "Galerie: Upload nicht bestaetigt";
}
