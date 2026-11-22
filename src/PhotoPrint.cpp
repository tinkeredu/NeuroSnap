#include "PhotoPrint.h"

#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>

namespace {

// TJpg_Decoder calls back synchronously with small RGB565 tiles.
PhotoRaster *raster = nullptr;
uint16_t decodedWidth;
uint16_t decodedHeight;

bool copyTile(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t *pixels) {
    static constexpr uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };
    const uint16_t stride = (raster->width + 7) / 8;
    // Invert the nearest-neighbor mapping to find the output area of this tile.
    const int left = (x * raster->width + decodedWidth - 1) / decodedWidth;
    const int right = ((x + width) * raster->width + decodedWidth - 1) / decodedWidth;
    const int top = (y * raster->height + decodedHeight - 1) / decodedHeight;
    const int bottom = ((y + height) * raster->height + decodedHeight - 1) / decodedHeight;
    for (int row = top; row < bottom; ++row) {
        const uint16_t *source = pixels + (row * decodedHeight / raster->height - y) * width;
        uint8_t *output = raster->data + row * stride;
        for (int col = left; col < right; ++col) {
            const uint16_t rgb = source[col * decodedWidth / raster->width - x];
            const int gray = ((rgb >> 11) * 8 * 77 + ((rgb >> 5) & 63) * 4 * 150 +
                              (rgb & 31) * 8 * 29) >> 8;
            if (gray < bayer[row % 4][col % 4] * 16 + 8) {
                output[col / 8] |= 0x80 >> (col % 8);
            }
        }
    }
    return true;
}

} // namespace

bool rasterizePhoto(const uint8_t *jpeg, size_t length, PhotoRaster &photo) {
    uint16_t width = 0;
    uint16_t height = 0;
    if (TJpgDec.getJpgSize(&width, &height, jpeg, length) != JDR_OK || !width || !height) {
        return false;
    }
    // Limit height as well so unusual aspect ratios cannot exhaust memory or paper.
    const uint16_t printHeightLimit = PHOTO_PRINT_WIDTH * 2;
    const uint32_t widthAtHeightLimit = uint32_t(width) * printHeightLimit / height;
    photo.width = min(uint32_t(PHOTO_PRINT_WIDTH), min(uint32_t(width), widthAtHeightLimit));
    photo.height = uint32_t(height) * photo.width / width;
    if (!photo.width || !photo.height) return false;
    const size_t bytes = ((photo.width + 7) / 8) * photo.height;
    photo.data = static_cast<uint8_t *>(heap_caps_calloc(bytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!photo.data) photo.data = static_cast<uint8_t *>(calloc(bytes, 1));
    if (!photo.data) return false;

    uint8_t scale = 1;
    while (scale < 8 && width / (2 * scale) >= photo.width &&
           height / (2 * scale) >= photo.height) {
        scale *= 2;
    }
    TJpgDec.setJpgScale(scale);
    TJpgDec.setCallback(copyTile);
    decodedWidth = width / scale;
    decodedHeight = height / scale;
    raster = &photo;
    const JRESULT result = TJpgDec.drawJpg(0, 0, jpeg, length);
    raster = nullptr;
    return result == JDR_OK;
}

void printEscPosRaster(HardwareSerial &serial, const PhotoRaster &photo) {
    const uint16_t stride = (photo.width + 7) / 8;
    // A full-width block including its header fits in a 256-byte receive buffer.
    constexpr uint16_t rowsPerBlock = 4;
    constexpr unsigned printTimePerRowMs = 30;
    for (uint16_t row = 0; row < photo.height; row += rowsPerBlock) {
        const uint16_t rows = min(rowsPerBlock, uint16_t(photo.height - row));
        const uint8_t command[] = {
            0x1d, 'v', '0', 0,
            uint8_t(stride), uint8_t(stride >> 8), uint8_t(rows), uint8_t(rows >> 8)
        };
        serial.write(command, sizeof(command));
        serial.write(photo.data + size_t(row) * stride, size_t(rows) * stride);
        // Wait for transmission, then allow the printer to finish the block.
        serial.flush();
        delay(rows * printTimePerRowMs);
    }
}

void printText(Adafruit_Thermal &printer, String text) {
    if (!text.length()) return;
    text.replace("Ä", "Ae"); text.replace("ä", "ae");
    text.replace("Ö", "Oe"); text.replace("ö", "oe");
    text.replace("Ü", "Ue"); text.replace("ü", "ue");
    text.replace("ß", "ss");
    text.replace("–", "-"); text.replace("—", "-");
    text.replace("„", "\""); text.replace("“", "\""); text.replace("”", "\"");
    text.replace("’", "'");
    text.replace("\r", "");
    text.replace("\t", " ");

    constexpr unsigned lineWidth = 32;
    String line;
    for (unsigned start = 0; start < text.length();) {
        const int newline = text.indexOf('\n', start);
        const unsigned end = newline < 0 ? text.length() : newline;
        // Keep UTF-8 continuation bytes and control codes off the printer's command stream.
        line = "";
        for (unsigned i = start; i < end; ++i) {
            const uint8_t c = text[i];
            if (c >= 32 && c <= 126) line += char(c);
            else if (c >= 192) line += '?';
        }
        line.trim();
        while (line.length() > lineWidth) {
            const int space = line.lastIndexOf(' ', lineWidth);
            const unsigned split = space > 0 ? space : lineWidth;
            printer.println(line.substring(0, split));
            line.remove(0, split);
            line.trim();
        }
        printer.println(line);
        start = end + 1;
    }
}
