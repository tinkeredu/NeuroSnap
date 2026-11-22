#include "PhotoDither.h"

#include <algorithm>
#include <cstring>

void DitherRows::write(PhotoDither method, const uint8_t *gray, uint8_t *bitmap, int width, int row) {
    static constexpr uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };
    if (method == PhotoDither::Threshold || method == PhotoDither::Bayer) {
        for (int x = 0; x < width; ++x) {
            const int threshold = method == PhotoDither::Bayer ? bayer[row % 4][x % 4] * 16 + 8 : 128;
            if (gray[x] < threshold) bitmap[x / 8] |= 0x80 >> (x % 8);
        }
        return;
    }

    auto *current = errors[row % 3] + 2;
    auto *next = errors[(row + 1) % 3] + 2;
    auto *after = errors[(row + 2) % 3] + 2;
    constexpr int white = 255 * 16;
    // Mirror the diffusion kernel on alternate rows.
    const int direction = row % 2 ? -1 : 1;
    for (int x = direction == 1 ? 0 : width - 1; x >= 0 && x < width; x += direction) {
        const int value = std::max(0, std::min(white, gray[x] * 16 + current[x]));
        const bool black = value < 128 * 16;
        if (black) bitmap[x / 8] |= 0x80 >> (x % 8);
        const int error = value - (black ? 0 : white);

        if (method == PhotoDither::FloydSteinberg) {
            current[x + direction] += error * 7 / 16;
            next[x - direction] += error * 3 / 16;
            next[x] += error * 5 / 16;
            next[x + direction] += error / 16;
        } else {
            const int share = error / 8;
            current[x + direction] += share;
            current[x + direction * 2] += share;
            next[x - direction] += share;
            next[x] += share;
            next[x + direction] += share;
            after[x] += share;
        }
    }
    std::memset(errors[row % 3], 0, sizeof(errors[0]));
}
