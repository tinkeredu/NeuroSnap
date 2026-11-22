#pragma once

#include <Arduino.h>
#include <Adafruit_Thermal.h>

constexpr uint16_t PHOTO_PRINT_WIDTH = 384;

struct PhotoRaster {
    uint8_t *data = nullptr;
    uint16_t width = 0;
    uint16_t height = 0;

    PhotoRaster() = default;
    PhotoRaster(const PhotoRaster &) = delete;
    PhotoRaster &operator=(const PhotoRaster &) = delete;
    ~PhotoRaster() { free(data); }
};

bool rasterizePhoto(const uint8_t *jpeg, size_t length, PhotoRaster &photo);
void printText(Adafruit_Thermal &printer, String text);
