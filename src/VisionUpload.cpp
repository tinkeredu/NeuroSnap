#include "VisionUpload.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <cerrno>
#include <memory>
#include <new>

// Arduino's WiFiClientSecure ships another esp_crt_bundle.h. Use the ESP-IDF
// function linked with the SDK's built-in CA bundle instead.
extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

namespace {

// Fit a TLS record in the ESP32's 5760-byte TCP send buffer.
constexpr size_t UPLOAD_BLOCK = 4096;
constexpr size_t BASE64_INPUT_BLOCK = (UPLOAD_BLOCK / 4) * 3; // Padding only in the final block.

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
        bodyLength = length;
        if (!handle) return false;
        openResult = esp_http_client_open(handle, length);
        return openResult == ESP_OK;
    }

    String openError(const char *service) const {
        if (!handle) return String(service) + ": HTTPS-Client konnte nicht initialisiert werden";
        return String(service) + ": HTTPS-Verbindung fehlgeschlagen (" +
               esp_err_to_name(openResult) + ")";
    }

    bool write(const uint8_t *data, size_t length) {
        while (length) {
            errno = 0;
            const int sent = esp_http_client_write(handle, reinterpret_cast<const char *>(data), length);
            if (sent <= 0) {
                const int writeErrno = errno;
                socketError = esp_http_client_get_errno(handle);
                if (socketError <= 0) socketError = writeErrno;
                writeResult = sent;
                return false;
            }
            bytesWritten += sent;
            data += sent;
            length -= sent;
        }
        return true;
    }

    bool write(const String &part) {
        return write(reinterpret_cast<const uint8_t *>(part.c_str()), part.length());
    }

    bool writeFile(File &file) {
        size_t remaining = file.size();
        if (!remaining) return true;
        const size_t capacity = min(remaining, UPLOAD_BLOCK);
        std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[capacity]);
        if (!buffer) {
            localWriteError = "Kein Speicher fuer den Upload-Puffer";
            return false;
        }
        while (remaining) {
            const size_t count = file.read(buffer.get(), min(remaining, capacity));
            if (!count) {
                localWriteError = "Upload-Datei konnte nicht vollstaendig gelesen werden";
                return false;
            }
            if (!write(buffer.get(), count)) return false;
            remaining -= count;
        }
        return true;
    }

    String uploadError(const char *service) const {
        if (localWriteError) return String(service) + ": " + localWriteError;
        return String(service) + ": Upload unvollstaendig (HTTP-Body " +
               String(bytesWritten) + "/" + String(bodyLength) + " Bytes" +
               ", Schreibcode " + String(writeResult) + ", errno " + String(socketError) + ")";
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
    esp_err_t openResult = ESP_OK;
    size_t bodyLength = 0;
    size_t bytesWritten = 0;
    int writeResult = 0;
    int socketError = 0;
    const char *localWriteError = nullptr;
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
    if (!post.open(head.length() + length + tail.length())) return post.openError("Dropoff");
    if (!post.write(head)) return post.uploadError("Dropoff");
    for (size_t offset = 0; offset < length; offset += UPLOAD_BLOCK) {
        if (!post.write(jpeg + offset, min(UPLOAD_BLOCK, length - offset))) {
            return post.uploadError("Dropoff");
        }
    }
    if (!post.write(tail)) return post.uploadError("Dropoff");
    String error = post.readResponse("Dropoff", url, 2048);
    if (error.length()) return error;
    url.trim();
    if (!(url.startsWith("https://") || url.startsWith("http://")) ||
        url.indexOf('\n') >= 0 || url.indexOf('\r') >= 0 || url.indexOf(' ') >= 0) {
        return "Dropoff: Keine gueltige Bild-URL";
    }
    return "";
}

String sendBase64(HttpsPost &post, const uint8_t *jpeg, size_t length) {
    // mbedTLS also writes a NUL terminator. Neither it nor the JPEG needs a full base64 copy.
    std::unique_ptr<uint8_t[]> encoded(new (std::nothrow) uint8_t[UPLOAD_BLOCK + 1]);
    if (!encoded) return "Mistral: Kein Speicher fuer den Base64-Puffer";
    for (size_t offset = 0; offset < length; offset += BASE64_INPUT_BLOCK) {
        size_t encodedLength = 0;
        if (mbedtls_base64_encode(encoded.get(), UPLOAD_BLOCK + 1, &encodedLength, jpeg + offset,
                                  min(BASE64_INPUT_BLOCK, length - offset)) != 0) {
            return "Mistral: Base64-Kodierung fehlgeschlagen";
        }
        if (!post.write(encoded.get(), encodedLength)) return post.uploadError("Mistral");
    }
    return "";
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
    if (!post.open(head.length() + imageLength + tail.length())) return post.openError("Mistral");
    if (!post.write(head)) return post.uploadError("Mistral");
    if (direct) {
        const String error = sendBase64(post, jpeg, length);
        if (error.length()) return error;
    }
    if (!post.write(tail)) return post.uploadError("Mistral");
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
        return post.openError("Galerie");
    }
    if (!post.write(head) || !post.writeFile(caption) || !post.write(photoHead) ||
        !post.writeFile(photo) || !post.write(tail)) {
        return post.uploadError("Galerie");
    }
    String reply;
    const String error = post.readResponse("Galerie", reply, 2048);
    if (error.length()) return error;
    return reply.startsWith("Success:") ? "" : "Galerie: Upload nicht bestaetigt";
}
