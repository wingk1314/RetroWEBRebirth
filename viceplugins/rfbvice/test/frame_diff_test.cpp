// Unit tests for frame_diff.cpp: scroll detection, scroll application and
// changed tile search, on synthetic "web page" images.

#include "frame_diff.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace rfbvice;

static int failures = 0;

#define CHECK(cond) do { \
    if(!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while(0)

// A tall document with pseudo-random "text" lines separated by blank lines,
// like a typical article.
static std::vector<uint32_t> makeDocument(int width, int height, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<uint32_t> doc((size_t)width * (size_t)height, 0x00FFFFFFu);
    for(int y = 0; y < height; ++y) {
        if(y % 18 >= 12) continue; // line spacing
        for(int x = 8; x < width - 8; ++x) {
            if(rng() % 3 == 0) {
                doc[(size_t)y * (size_t)width + (size_t)x] = 0x00202020u + (rng() % 64);
            }
        }
    }
    return doc;
}

// Screen = static header of headerH rows + a window into doc at offset, with
// a scrollbar thumb in the rightmost 12 columns that moves with the offset.
static std::vector<uint32_t> makeScreen(
    const std::vector<uint32_t>& doc, int width, int docHeight,
    int height, int headerH, int offset
) {
    std::vector<uint32_t> s((size_t)width * (size_t)height, 0);
    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            uint32_t v;
            if(y < headerH) {
                v = 0x00C0C0C0u ^ (uint32_t)((x * 7 + y * 13) & 0x3F);
            } else {
                int dy = y - headerH + offset;
                v = doc[(size_t)dy * (size_t)width + (size_t)x];
            }
            s[(size_t)y * (size_t)width + (size_t)x] = v;
        }
        // Scrollbar.
        int thumbY = headerH + offset * (height - headerH) / docHeight;
        for(int x = width - 12; x < width; ++x) {
            bool thumb = y >= thumbY && y < thumbY + 40;
            s[(size_t)y * (size_t)width + (size_t)x] = thumb ? 0x00808080u : 0x00EEEEEEu;
        }
    }
    return s;
}

// Simulates what the plugin does: scroll, then patch changed tiles. Returns
// the number of pixels sent as tiles. The result must equal next exactly.
static long simulate(std::vector<uint32_t>& shadow, const std::vector<uint32_t>& next,
                     int width, int height, bool useScroll, ScrollMatch* matchOut) {
    ScrollMatch m;
    if(useScroll) {
        m = detectVerticalScroll(shadow.data(), next.data(), width, height, width, 24);
        applyScroll(shadow.data(), width, m);
    }
    if(matchOut) *matchOut = m;
    std::vector<Rect> tiles;
    findChangedTiles(shadow.data(), next.data(), width, height, width, 32, tiles);
    long pixels = 0;
    for(const Rect& r : tiles) {
        CHECK(r.x1 >= 0 && r.x2 <= width && r.y1 >= 0 && r.y2 <= height);
        CHECK(r.x1 < r.x2 && r.y1 < r.y2);
        for(int y = r.y1; y < r.y2; ++y) {
            memcpy(&shadow[(size_t)y * (size_t)width + (size_t)r.x1],
                   &next[(size_t)y * (size_t)width + (size_t)r.x1],
                   sizeof(uint32_t) * (size_t)(r.x2 - r.x1));
        }
        pixels += (long)(r.x2 - r.x1) * (r.y2 - r.y1);
    }
    CHECK(memcmp(shadow.data(), next.data(), shadow.size() * 4) == 0);
    return pixels;
}

static void testScrollDown() {
    const int W = 1024, H = 768, header = 64, docH = 6000;
    auto doc = makeDocument(W, docH, 1);
    for(int step : {1, 3, 17, 40, 120, 300}) {
        auto a = makeScreen(doc, W, docH, H, header, 500);
        auto b = makeScreen(doc, W, docH, H, header, 500 + step);
        ScrollMatch m;
        auto shadowScroll = a;
        long withScroll = simulate(shadowScroll, b, W, H, true, &m);
        auto shadowPlain = a;
        long without = simulate(shadowPlain, b, W, H, false, nullptr);
        CHECK(m.found);
        CHECK(m.dy == -step);
        CHECK(m.y1 >= header);
        printf("scroll down %3d px: tiles %7ld px with scroll detection, %7ld px without (%.1f%%)\n",
               step, withScroll, without, 100.0 * (double)withScroll / (double)without);
        // Only the newly revealed strip (plus tile rounding and the
        // scrollbar column) should need new pixels.
        CHECK(withScroll <= (long)(step + 96) * W);
    }
}

static void testScrollUp() {
    const int W = 800, H = 600, header = 40, docH = 4000;
    auto doc = makeDocument(W, docH, 2);
    auto a = makeScreen(doc, W, docH, H, header, 900);
    auto b = makeScreen(doc, W, docH, H, header, 900 - 55);
    ScrollMatch m;
    auto shadow = a;
    simulate(shadow, b, W, H, true, &m);
    CHECK(m.found);
    CHECK(m.dy == 55);
}

static void testNoScrollOnLocalChange() {
    const int W = 640, H = 480;
    auto doc = makeDocument(W, 2000, 3);
    auto a = makeScreen(doc, W, 2000, H, 30, 100);
    auto b = a;
    // A hover effect: one small box changes.
    for(int y = 200; y < 220; ++y)
        for(int x = 100; x < 180; ++x)
            b[(size_t)y * W + (size_t)x] ^= 0x00FF0000u;
    ScrollMatch m;
    auto shadow = a;
    long px = simulate(shadow, b, W, H, true, &m);
    CHECK(!m.found);
    CHECK(px <= 32 * 32 * 6);
}

static void testIdentical() {
    const int W = 320, H = 240;
    auto doc = makeDocument(W, 1000, 4);
    auto a = makeScreen(doc, W, 1000, H, 20, 10);
    auto shadow = a;
    ScrollMatch m;
    CHECK(simulate(shadow, a, W, H, true, &m) == 0);
    CHECK(!m.found);
}

static void testIgnoresAlphaByte() {
    const int W = 128, H = 64;
    std::vector<uint32_t> a((size_t)W * H, 0x00123456u), b = a;
    for(auto& p : b) p |= 0xFF000000u;
    std::vector<Rect> tiles;
    findChangedTiles(a.data(), b.data(), W, H, W, 32, tiles);
    CHECK(tiles.empty());
}

static void testOddSizes() {
    // Sizes that are not multiples of the tile size.
    const int W = 333, H = 251;
    auto doc = makeDocument(W, 3000, 5);
    auto a = makeScreen(doc, W, 3000, H, 25, 700);
    auto b = makeScreen(doc, W, 3000, H, 25, 731);
    auto shadow = a;
    ScrollMatch m;
    simulate(shadow, b, W, H, true, &m);
    CHECK(m.found && m.dy == -31);
}

static void testRandomNoise() {
    // Completely unrelated frames must not produce a bogus scroll.
    const int W = 400, H = 300;
    std::mt19937 rng(9);
    std::vector<uint32_t> a((size_t)W * H), b((size_t)W * H);
    for(auto& p : a) p = rng() & 0xFFFFFF;
    for(auto& p : b) p = rng() & 0xFFFFFF;
    auto shadow = a;
    ScrollMatch m;
    simulate(shadow, b, W, H, true, &m);
    CHECK(!m.found);
}

int main() {
    testScrollDown();
    testScrollUp();
    testNoScrollOnLocalChange();
    testIdentical();
    testIgnoresAlphaByte();
    testOddSizes();
    testRandomNoise();
    if(failures) {
        fprintf(stderr, "frame_diff_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("frame_diff_test: all tests passed\n");
    return 0;
}
