#pragma once

#include <Arduino.h>
#include <FS.h>

enum class ImageTransfer { Direct, Dropoff };

struct VisionSettings {
    String apiKey;
    String apiUrl = "https://api.mistral.ai/v1/chat/completions";
    String apiModel = "mistral-small-latest";
    ImageTransfer transfer = ImageTransfer::Direct;
    String uploadHost = "example.com";
    String uploadPath = "/neurosnap/upload.php";
    String dropoffPath = "/neurosnap/dropoff.php";
};

// Returns an empty error on success.
String analyzePhoto(const VisionSettings &settings, const String &prompt,
                    const uint8_t *jpeg, size_t length, String &answer);
String uploadGalleryPhoto(const VisionSettings &settings, File &photo, File &caption,
                          const String &filename);
