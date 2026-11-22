#include "VisionUpload.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_tls.h>
#include <mbedtls/base64.h>
#include <mbedtls/ssl.h>
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
static_assert(UPLOAD_BLOCK <= MBEDTLS_SSL_OUT_CONTENT_LEN, "Upload writes must fit a TLS record");
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
        config.event_handler = onHttpEvent;
        config.user_data = this;
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
        errno = 0;
        openResult = esp_http_client_open(handle, length);
        if (openResult == ESP_OK) return true;
        const int openErrno = errno;
        socketError = esp_http_client_get_errno(handle);
        if (socketError <= 0) socketError = openErrno;
        return false;
    }

    String openError(const char *service) const {
        if (!handle) return String(service) + ": HTTPS-Client konnte nicht initialisiert werden";
        String error = String(service) + ": HTTPS-Verbindung fehlgeschlagen (" + esp_err_to_name(openResult);
        if (tlsResult != ESP_OK) error += String(", Transport ") + esp_err_to_name(tlsResult);
        if (tlsCode) error += ", mbedTLS 0x" + String(tlsCode, HEX);
        if (tlsCertFlags) error += ", Zertifikatsflags 0x" + String(tlsCertFlags, HEX);
        if (socketError) error += ", errno " + String(socketError);
        return error + ")";
    }

    bool write(const uint8_t *data, size_t length) {
        while (length) {
            // Limit each call to one TLS record so no earlier record in the call can be lost from the byte count.
            const auto *buffer = reinterpret_cast<const char *>(data);
            const size_t count = min(length, UPLOAD_BLOCK);
            errno = 0;
            int sent = esp_http_client_write(handle, buffer, count);
            if (sent == MBEDTLS_ERR_SSL_WANT_WRITE) {
                // mbedTLS may still have this record pending and requires identical arguments.
                ++writeRetries;
                errno = 0;
                sent = esp_http_client_write(handle, buffer, count);
            }
            if (sent <= 0) {
                const int writeErrno = errno;
                socketError = esp_http_client_get_errno(handle);
                if (socketError <= 0) socketError = writeErrno;
                writeResult = sent;
                // The failed call may have transmitted up to count more bytes.
                retryableReset = sent < 0 && socketError == ECONNRESET &&
                                 bytesWritten + count < bodyLength;
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
               ", Schreibcode " + String(writeResult) + ", errno " + String(socketError) +
               ", Schreibwiederholungen " + String(writeRetries) + ")";
    }

    bool canRetryAfterReset() const { return retryableReset; }

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
    static esp_err_t onHttpEvent(esp_http_client_event_t *event) {
        // ESP-IDF 4.4 passes the transport's TLS error handle in this event.
        if (event->event_id == HTTP_EVENT_ERROR && event->data) {
            auto &post = *static_cast<HttpsPost *>(event->user_data);
            post.tlsResult = esp_tls_get_and_clear_last_error(
                static_cast<esp_tls_error_handle_t>(event->data), &post.tlsCode, &post.tlsCertFlags);
        }
        return ESP_OK;
    }

    esp_http_client_handle_t handle = nullptr;
    esp_err_t openResult = ESP_OK;
    esp_err_t tlsResult = ESP_OK;
    int tlsCode = 0;
    int tlsCertFlags = 0;
    size_t bodyLength = 0;
    size_t bytesWritten = 0;
    size_t writeRetries = 0;
    int writeResult = 0;
    int socketError = 0;
    bool retryableReset = false;
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

String sendMistralBody(HttpsPost &post, const String &head, const String &tail,
                       const uint8_t *jpeg, size_t length) {
    if (!post.write(head)) return post.uploadError("Mistral");
    if (length) {
        const String error = sendBase64(post, jpeg, length);
        if (error.length()) return error;
    }
    if (!post.write(tail)) return post.uploadError("Mistral");
    return "";
}

bool appendJsonString(String &body, const String &value) {
    JsonDocument literal;
    if (!literal.set(value)) return false;
    // ArduinoJson clears an Arduino String destination before serializing.
    String encoded;
    serializeJson(literal, encoded);
    return encoded.length() == measureJson(literal) && body.concat(encoded);
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
    String head;
    if (!head.concat("{\"max_tokens\":320,\"model\":") ||
        !appendJsonString(head, settings.apiModel) ||
        !head.concat(",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":") ||
        !appendJsonString(head, prompt) ||
        !head.concat("},{\"type\":\"image_url\",\"image_url\":") ||
        !(direct ? head.concat("\"data:image/jpeg;base64,") : appendJsonString(head, imageUrl))) {
        return "Mistral: JSON-Anfrage konnte nicht erstellt werden";
    }
    const String tail = direct ? "\"}]}]}" : "}]}]}";
    const size_t jpegLength = direct ? length : 0;
    const size_t requestLength = head.length() + 4 * ((jpegLength + 2) / 3) + tail.length();

    String body;
    for (bool retry = true; ; retry = false) {
        HttpsPost post(settings.apiUrl, "application/json", 60000, settings.apiKey);
        if (!post.open(requestLength)) return post.openError("Mistral");
        const String sendError = sendMistralBody(post, head, tail, jpeg, jpegLength);
        if (sendError.length()) {
            if (retry && post.canRetryAfterReset()) continue;
            return sendError;
        }
        const String responseError = post.readResponse("Mistral", body, 16384);
        if (responseError.length()) return responseError;
        break;
    }

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
