#pragma once

// Pure frame comparison helpers used by rfbvice. They have no dependency on
// LibVNCServer so that they can be unit tested in isolation.
//
// All images are 32-bit pixels (uint32_t) addressed as px[y * stride + x].
// Only the low 24 bits (the color channels) are compared; the top byte is
// ignored because the program does not guarantee its contents.

#include <cstdint>
#include <vector>

namespace rfbvice {

constexpr uint32_t ColorMask = 0x00FFFFFFu;

struct Rect {
    int x1, y1, x2, y2; // half-open: [x1, x2) x [y1, y2)
};

// A detected vertical scroll: rows [y1, y2) (columns [x1, x2)) of the new
// frame are equal to rows [y1 - dy, y2 - dy) of the old frame.
struct ScrollMatch {
    bool found = false;
    int dy = 0;
    int x1 = 0, x2 = 0;
    int y1 = 0, y2 = 0;
};

// Detects the dominant vertical scroll between two frames of the same size.
// Columns at the right edge (excludeRight pixels, e.g. a scrollbar whose thumb
// moves on every scroll) are ignored so that they do not break row matching;
// they will be picked up by the regular tile diff instead.
ScrollMatch detectVerticalScroll(
    const uint32_t* oldPx,
    const uint32_t* newPx,
    int width,
    int height,
    int stride,
    int excludeRight
);

// Applies a scroll match to an image in place, moving rows exactly as a VNC
// CopyRect with the same parameters would.
void applyScroll(uint32_t* px, int stride, const ScrollMatch& match);

// Appends the tiles (tileSize x tileSize, clipped to the image) that differ
// between a and b to out. Horizontally adjacent changed tiles in the same tile
// row are merged into one rectangle.
void findChangedTiles(
    const uint32_t* a,
    const uint32_t* b,
    int width,
    int height,
    int stride,
    int tileSize,
    std::vector<Rect>& out
);

}
