#pragma once

#include "VisionUpload.h"
#include <ArduinoJson.h>

enum class CaptureMode { AiText, Photo, PhotoAi };

struct CameraSettings {
    VisionSettings vision;
    CaptureMode mode = CaptureMode::AiText;
    String prompt = "Beschreibe dieses Bild.";
    String header;
    String footer;
    String adminPassword = "neurosnap";
    bool logging = false;
    bool guided = false;

    void loadFromSD();
    bool saveToSD() const;
    void toJson(JsonDocument &doc, bool includeSecrets) const;
    // Missing keys keep their current values. Returns an empty error on success.
    String applyJson(JsonVariantConst json);
};
