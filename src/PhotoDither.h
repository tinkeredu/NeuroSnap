#pragma once

#include <stdint.h>

constexpr uint16_t PHOTO_PRINT_WIDTH = 384;

enum class PhotoDither { Bayer, FloydSteinberg, Atkinson, Threshold };

class DitherRows {
public:
    void write(PhotoDither method, const uint8_t *gray, uint8_t *bitmap, int width, int row);

private:
    // Errors are stored in sixteenths of a gray level. Padding absorbs off-image pixels.
    int16_t errors[3][PHOTO_PRINT_WIDTH + 4]{};
};
