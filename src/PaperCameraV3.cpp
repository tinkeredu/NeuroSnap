#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_camera.h>
#include <esp_system.h>
#include <atomic>
#include <memory>
#include <new>

#include "CameraSettings.h"
#include "CameraWeb.h"
#include "PhotoPrint.h"

// NeuroSnap (ESP32-S3), licensed under CC BY-NC-SA 4.0.
namespace {

constexpr int BUTTON_PIN = 1;
constexpr int PRINTER_RX_PIN = 2;
constexpr int PRINTER_TX_PIN = 3;
constexpr int LED_PIN = 21;
constexpr char SETUP_SSID[] = "NeuroSnap_Setup";

HardwareSerial printerSerial(2);
Adafruit_Thermal printer(&printerSerial);
Adafruit_NeoPixel pixels(1, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiManager wifiManager;
CameraSettings settings;
bool cameraReady = false;
bool sdReady = false;
bool connected = false;
bool connectingAtBoot = true;
bool webRunning = false;
unsigned long connectionStarted;
uint32_t nextPhotoNumber = 0;
bool captureActive = false;
String pendingIpReceipt;

struct CaptureJob {
    camera_fb_t *frame = nullptr;
    TaskHandle_t worker = nullptr;
    CaptureMode mode;
    PhotoProtocol protocol;
    PhotoDither dither;
    VisionSettings vision;
    String name, header, footer, prompt, answer, error;
    String uploadDiagnostics;
    bool photoReady = true;
};

std::atomic<CaptureJob *> pendingAiResult{nullptr};
std::atomic<CaptureJob *> finishedCapture{nullptr};

void setStatus(uint8_t red, uint8_t green, uint8_t blue) {
    pixels.setPixelColor(0, pixels.Color(red, green, blue));
    pixels.show();
}

void readyStatus() {
    if (!cameraReady) setStatus(255, 0, 0);
    else if (wifiManager.getConfigPortalActive()) setStatus(0, 255, 255);
    else if (captureActive) setStatus(255, 255, 0);
    else setStatus(0, 0, 255);
}

void sysLog(const String &message) {
    Serial.println(message);
    if (sdReady && settings.logging) {
        File log = SD_MMC.open("/system.log", FILE_APPEND);
        if (log) log.println(String(millis() / 1000) + "s: " + message);
    }
}

bool initCamera() {
    camera_config_t config{};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    // ESP32-S3 EYE camera wiring on the NeuroSnap PCB.
    config.pin_d0 = 11; config.pin_d1 = 9; config.pin_d2 = 8; config.pin_d3 = 10;
    config.pin_d4 = 12; config.pin_d5 = 18; config.pin_d6 = 17; config.pin_d7 = 16;
    config.pin_xclk = 15; config.pin_pclk = 13;
    config.pin_vsync = 6; config.pin_href = 7;
    config.pin_sccb_sda = 4; config.pin_sccb_scl = 5;
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.xclk_freq_hz = 10000000;
    config.pixel_format = PIXFORMAT_JPEG;

    const bool hasPsram = psramFound();
    config.frame_size = hasPsram ? FRAMESIZE_UXGA : FRAMESIZE_VGA;
    config.jpeg_quality = hasPsram ? 12 : 15;
    config.fb_count = 1;
    config.fb_location = hasPsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    const esp_err_t result = esp_camera_init(&config);
    if (result != ESP_OK) {
        sysLog("Kamerastart fehlgeschlagen: " + String(esp_err_to_name(result)));
        return false;
    }
    if (sensor_t *sensor = esp_camera_sensor_get()) {
        sensor->set_vflip(sensor, 0);
        sensor->set_brightness(sensor, 1);
        sensor->set_saturation(sensor, 0);
    }
    return true;
}

String savePhoto(const camera_fb_t &frame) {
    if (!sdReady) return "";
    String name;
    do {
        name = "/bild_" + String(nextPhotoNumber++);
    } while (SD_MMC.exists(name + ".jpg") || SD_MMC.exists(name + ".txt"));
    const String path = name + ".jpg";
    File file = SD_MMC.open(path, FILE_WRITE);
    if (!file || file.write(frame.buf, frame.len) != frame.len) {
        file.close();
        if (SD_MMC.exists(path)) SD_MMC.remove(path);
        sysLog("Bild konnte nicht auf die SD-Karte geschrieben werden.");
        return "";
    }
    return name;
}

void saveCaption(const String &name, const String &prompt, const String &answer) {
    // The gallery may have deleted the JPEG while Mistral was processing it.
    if (!name.length() || !SD_MMC.exists(name + ".jpg")) return;
    const String path = name + ".txt";
    const String content = "PROMPT: " + prompt + "\n----------------\nAI:\n" + answer;
    File file = SD_MMC.open(path, FILE_WRITE);
    if (!file || file.print(content) != content.length()) {
        file.close();
        if (SD_MMC.exists(path)) SD_MMC.remove(path);
        sysLog("KI-Text konnte nicht auf die SD-Karte geschrieben werden.");
    }
}

void processCapture(CaptureJob &job) {
    const bool printPhoto = job.mode != CaptureMode::AiText;
    const bool useAi = job.mode != CaptureMode::Photo;
    PhotoRaster raster;
    if (printPhoto) {
        job.photoReady = rasterizePhoto(job.frame->buf, job.frame->len, job.dither, raster);
    }
    if (useAi) {
        job.uploadDiagnostics = "Vor KI-Upload: JPEG " + String(job.frame->len) +
                                " Bytes, freier Heap " + String(ESP.getFreeHeap()) +
                                ", groesster Heap-Block " + String(ESP.getMaxAllocHeap()) +
                                ", freier PSRAM " + String(ESP.getFreePsram());
        job.error = analyzePhoto(job.vision, job.prompt, job.frame->buf, job.frame->len, job.answer);
    }
    esp_camera_fb_return(job.frame);
    if (useAi) {
        // Let the loop save the caption before printing can draw power.
        job.worker = xTaskGetCurrentTaskHandle();
        pendingAiResult.store(&job);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    if (printPhoto) {
        printText(printer, job.header);
        if (job.photoReady) {
            if (job.protocol == PhotoProtocol::EscPos) {
                printer.timeoutWait();
                printEscPosRaster(printerSerial, raster);
            } else {
                printer.printBitmap(raster.width, raster.height, raster.data, false);
            }
        } else {
            printer.println(F("Fotodruck fehlgeschlagen."));
        }
    }
    if (job.answer.length()) {
        if (printPhoto) printer.feed(1);
        printer.boldOn();
        printer.println(F("Mistral AI:"));
        printer.boldOff();
        printText(printer, job.answer);
    } else if (useAi && !printPhoto) {
        printText(printer, job.error);
    }
    if (printPhoto && job.footer.length()) {
        printer.feed(1);
        printText(printer, job.footer);
    }
    printer.feed(3);
    printer.timeoutWait();
    printerSerial.flush();
}

void captureTask(void *context) {
    auto *job = static_cast<CaptureJob *>(context);
    processCapture(*job);
    finishedCapture.store(job);
    vTaskDelete(nullptr);
}

void capturePhoto() {
    if (!cameraReady || captureActive || cameraWebRestartPending()) return;
    setStatus(255, 255, 0);
    // With one buffer, the queued frame may predate the button press.
    if (camera_fb_t *queued = esp_camera_fb_get()) esp_camera_fb_return(queued);
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        sysLog("Fotoaufnahme fehlgeschlagen.");
        setStatus(255, 0, 0);
        return;
    }
    const String name = savePhoto(*frame);
    auto *job = new (std::nothrow) CaptureJob;
    if (job) {
        job->frame = frame;
        job->name = name;
        job->mode = settings.mode;
        job->protocol = settings.photoProtocol;
        job->dither = settings.photoDither;
        job->header = settings.header;
        job->footer = settings.footer;
        if (job->mode != CaptureMode::Photo) {
            job->vision = settings.vision;
            job->prompt = settings.prompt;
        }
        captureActive = true;
        // The printer library polls in yield() loops. Keep that work off the Wi-Fi core.
        if (xTaskCreatePinnedToCore(captureTask, "capture", 8192, job, 1, nullptr,
                                    ARDUINO_RUNNING_CORE) == pdPASS) return;
        delete job;
        captureActive = false;
    }
    esp_camera_fb_return(frame);
    sysLog("Nicht genug Speicher fuer die Verarbeitung der Aufnahme.");
    setStatus(255, 0, 0);
}

void serviceCapture() {
    if (const auto *result = pendingAiResult.exchange(nullptr)) {
        sysLog(result->uploadDiagnostics);
        if (result->error.length()) sysLog(result->error);
        else saveCaption(result->name, result->prompt, result->answer);
        xTaskNotifyGive(result->worker);
    }

    std::unique_ptr<CaptureJob> job(finishedCapture.exchange(nullptr));
    if (!job) return;
    captureActive = false;

    if (!job->photoReady) {
        sysLog("JPEG konnte nicht fuer den Druck aufbereitet werden.");
        setStatus(255, 0, 0);
    } else if (job->error.length()) setStatus(255, 69, 0);
    else readyStatus();
    sysLog("Verarbeitung der Aufnahme beendet.");
}

void uploadGallery() {
    if (settings.mode == CaptureMode::Photo || !sdReady) return;
    if (WiFi.status() != WL_CONNECTED) {
        sysLog("Galerie-Upload: Kein WLAN");
        setStatus(255, 69, 0);
        return;
    }
    setStatus(100, 0, 255);
    File root = SD_MMC.open("/");
    File entry;
    bool failed = false;
    while ((entry = root.openNextFile())) {
        const String name = entry.name();
        const bool valid = !entry.isDirectory() && isCaptureName(name, ".jpg");
        entry.close();
        if (!valid) continue;
        const String imagePath = "/" + name;
        const String textPath = imagePath.substring(0, imagePath.length() - 4) + ".txt";
        File caption = SD_MMC.open(textPath);
        if (!caption) continue; // Photo-only captures stay on the device.
        File image = SD_MMC.open(imagePath);
        if (!image) continue;
        const String error = uploadGalleryPhoto(settings.vision, image, caption, name);
        image.close();
        caption.close();
        if (error.length()) {
            sysLog(error);
            failed = true;
            break; // Keep unsent pairs on the SD card for the next manual upload.
        }
        SD_MMC.remove(imagePath);
        SD_MMC.remove(textPath);
        sysLog("Galerie-Upload erfolgreich: " + name);
    }
    if (failed) setStatus(255, 69, 0);
    else readyStatus();
}

void openWifiPortal() {
    connectingAtBoot = false;
    if (wifiManager.getConfigPortalActive()) return;
    // The portal uses the same port as the camera interface.
    if (webRunning) {
        stopCameraWeb();
        webRunning = false;
    }
    wifiManager.startConfigPortal(SETUP_SSID, "neurosnap");
    if (wifiManager.getConfigPortalActive()) {
        setStatus(0, 255, 255);
        sysLog("WLAN-Setup: NeuroSnap_Setup, http://192.168.4.1");
    } else {
        sysLog("WLAN-Portal konnte nicht gestartet werden.");
    }
}

void serviceWifi() {
    if (wifiManager.getConfigPortalActive()) {
        wifiManager.process();
        if (!wifiManager.getConfigPortalActive()) readyStatus();
    }
    const bool online = WiFi.status() == WL_CONNECTED;
    if (!online && webRunning) {
        stopCameraWeb();
        webRunning = false;
        pendingIpReceipt = "";
        sysLog("WLAN-Verbindung verloren.");
    }
    // WebServer opens an lwIP socket. Wait for station bring-up and leave port 80 to the portal.
    if (online && !wifiManager.getConfigPortalActive() && !webRunning) {
        sysLog("Starte Webserver.");
        beginCameraWeb();
        webRunning = true;
    }
    if (online && !connected) {
        connectingAtBoot = false;
        WiFi.setSleep(false);
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
        pendingIpReceipt = WiFi.localIP().toString();
        sysLog("Weboberflaeche: http://" + pendingIpReceipt);
    }
    if (webRunning && !captureActive && pendingIpReceipt.length()) {
        printer.println(F("IP-Adresse:"));
        printText(printer, pendingIpReceipt);
        printer.feed(1);
        pendingIpReceipt = "";
    }
    connected = online;
    if (connectingAtBoot && millis() - connectionStarted >= 12000) openWifiPortal();
}

void serviceButton() {
    static int raw = HIGH;
    static int stable = HIGH;
    static unsigned long changedAt = 0;
    static unsigned long pressedAt = 0;
    const unsigned long now = millis();
    const int level = digitalRead(BUTTON_PIN);
    if (level != raw) {
        raw = level;
        changedAt = now;
    }
    if (raw == stable || now - changedAt < 30) return;
    stable = raw;
    if (stable == LOW) {
        pressedAt = now;
        return;
    }
    const unsigned long duration = now - pressedAt;
    if (duration < 2000) capturePhoto();
    else if (duration >= 10000) openWifiPortal();
    else if (duration >= 5000 && !captureActive) uploadGallery();
}

} // namespace

void setup() {
    Serial.begin(115200);
    pixels.begin();
    pixels.setBrightness(50);
    setStatus(255, 165, 0);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    SD_MMC.setPins(39, 38, 40);
    sdReady = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5);
    if (sdReady) settings.loadFromSD();
    else sysLog("Keine SD-Karte: Speichern und Galerie sind deaktiviert.");

    printerSerial.begin(9600, SERIAL_8N1, PRINTER_RX_PIN, PRINTER_TX_PIN);
    printer.begin();
    printer.setCodePage(CODEPAGE_CP437);

    const esp_reset_reason_t resetReason = esp_reset_reason();
    sysLog("NeuroSnap Start, ESP32-Resetgrund: " + String(static_cast<int>(resetReason)));
    sysLog("Initialisiere Kamera.");
    cameraReady = initCamera();
    configureCameraWeb(settings, sdReady, captureActive, openWifiPortal);

    wifiManager.setDebugOutput(false);
    wifiManager.setConfigPortalBlocking(false);
    wifiManager.setConfigPortalTimeout(180);
    wifiManager.setConnectTimeout(12);
    wifiManager.setConnectRetries(1);
    sysLog("Initialisiere WLAN.");
    if (WiFi.mode(WIFI_STA)) {
        WiFi.setAutoReconnect(true);
        WiFi.begin();
    } else {
        sysLog("WLAN-Initialisierung fehlgeschlagen. Starte anschliessend das Setup-Portal.");
    }
    connectionStarted = millis();

    if (settings.guided) {
        if (settings.mode != CaptureMode::Photo) {
            printText(printer, "Die KI-Aufnahmemodi senden Fotos zur Analyse an Mistral AI.");
        }
        printText(printer, "Papier zum Abreissen ueber die Kante ziehen. Mit der schwarzen Druckertaste Papier vorschieben.");
        printer.feed(3);
    }

    readyStatus();
}

void loop() {
    serviceCapture();
    serviceWifi();
    serviceCameraWeb(webRunning);
    serviceButton();
    delay(5);
}
