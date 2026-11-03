#include "frame_diff.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace rfbvice {

namespace {

uint64_t hashRow(const uint32_t* row, int count) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for(int x = 0; x < count; ++x) {
        h ^= (uint64_t)(row[x] & ColorMask);
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

bool spanEqual(const uint32_t* a, const uint32_t* b, int count) {
    // Fast path: most unchanged spans are bytewise identical.
    if(memcmp(a, b, sizeof(uint32_t) * (size_t)count) == 0) {
        return true;
    }
    for(int i = 0; i < count; ++i) {
        if((a[i] ^ b[i]) & ColorMask) {
            return false;
        }
    }
    return true;
}

}

ScrollMatch detectVerticalScroll(
    const uint32_t* oldPx,
    const uint32_t* newPx,
    int width,
    int height,
    int stride,
    int excludeRight
) {
    ScrollMatch none;

    // Too small to be worth it.
    if(width < 64 || height < 64) {
        return none;
    }
    int hashWidth = width;
    if(excludeRight > 0 && excludeRight * 4 < width) {
        hashWidth = width - excludeRight;
    }

    std::vector<uint64_t> oldHash((size_t)height);
    std::vector<uint64_t> newHash((size_t)height);
    for(int y = 0; y < height; ++y) {
        oldHash[(size_t)y] = hashRow(oldPx + (size_t)y * (size_t)stride, hashWidth);
        newHash[(size_t)y] = hashRow(newPx + (size_t)y * (size_t)stride, hashWidth);
    }

    int changedRows = 0;
    for(int y = 0; y < height; ++y) {
        if(oldHash[(size_t)y] != newHash[(size_t)y]) {
            ++changedRows;
        }
    }
    if(changedRows < 8) {
        return none;
    }

    // Position of each old row hash, or -1 if the hash is not unique (blank
    // lines, repeated borders etc. would give ambiguous votes).
    std::unordered_map<uint64_t, int> oldPos;
    oldPos.reserve((size_t)height * 2);
    for(int y = 0; y < height; ++y) {
        auto it = oldPos.find(oldHash[(size_t)y]);
        if(it == oldPos.end()) {
            oldPos.emplace(oldHash[(size_t)y], y);
        } else {
            it->second = -1;
        }
    }

    // Each changed new row whose content is found exactly once in the old
    // frame votes for the offset between the two positions.
    std::unordered_map<int, int> votes;
    for(int y = 0; y < height; ++y) {
        if(oldHash[(size_t)y] == newHash[(size_t)y]) {
            continue;
        }
        auto it = oldPos.find(newHash[(size_t)y]);
        if(it != oldPos.end() && it->second >= 0 && it->second != y) {
            ++votes[y - it->second];
        }
    }

    int bestDy = 0;
    int bestVotes = 0;
    for(const auto& v : votes) {
        if(v.second > bestVotes || (v.second == bestVotes && abs(v.first) < abs(bestDy))) {
            bestDy = v.first;
            bestVotes = v.second;
        }
    }
    if(bestVotes < 3 || bestDy == 0) {
        return none;
    }

    // Find the longest run of rows that match under the chosen offset.
    int yStart = std::max(0, bestDy);
    int yEnd = std::min(height, height + bestDy);
    int runStart = -1;
    int bestStart = 0, bestEnd = 0, bestGain = 0;
    auto closeRun = [&](int runEnd) {
        if(runStart < 0) {
            return;
        }
        // Gain = rows in the run that actually changed, i.e. rows that the
        // copy saves us from sending.
        int gain = 0;
        for(int y = runStart; y < runEnd; ++y) {
            if(oldHash[(size_t)y] != newHash[(size_t)y]) {
                ++gain;
            }
        }
        if(gain > bestGain) {
            bestGain = gain;
            bestStart = runStart;
            bestEnd = runEnd;
        }
        runStart = -1;
    };
    for(int y = yStart; y < yEnd; ++y) {
        bool match = newHash[(size_t)y] == oldHash[(size_t)(y - bestDy)];
        if(match) {
            if(runStart < 0) {
                runStart = y;
            }
        } else {
            closeRun(y);
        }
    }
    closeRun(yEnd);

    if(bestEnd - bestStart < 24 || bestGain < 8) {
        return none;
    }

    // Hash collisions are possible in theory; verify the band exactly before
    // trusting it, since a wrong CopyRect would corrupt the client's screen.
    for(int y = bestStart; y < bestEnd; ++y) {
        if(!spanEqual(
            newPx + (size_t)y * (size_t)stride,
            oldPx + (size_t)(y - bestDy) * (size_t)stride,
            hashWidth
        )) {
            return none;
        }
    }

    ScrollMatch match;
    match.found = true;
    match.dy = bestDy;
    match.x1 = 0;
    match.x2 = hashWidth;
    match.y1 = bestStart;
    match.y2 = bestEnd;
    return match;
}

void applyScroll(uint32_t* px, int stride, const ScrollMatch& match) {
    if(!match.found) {
        return;
    }
    size_t count = (size_t)(match.x2 - match.x1);
    auto copyRow = [&](int y) {
        memmove(
            px + (size_t)y * (size_t)stride + (size_t)match.x1,
            px + (size_t)(y - match.dy) * (size_t)stride + (size_t)match.x1,
            count * sizeof(uint32_t)
        );
    };
    if(match.dy > 0) {
        // Source rows are above destination rows: copy bottom-up.
        for(int y = match.y2 - 1; y >= match.y1; --y) {
            copyRow(y);
        }
    } else {
        for(int y = match.y1; y < match.y2; ++y) {
            copyRow(y);
        }
    }
}

void findChangedTiles(
    const uint32_t* a,
    const uint32_t* b,
    int width,
    int height,
    int stride,
    int tileSize,
    std::vector<Rect>& out
) {
    for(int ty = 0; ty < height; ty += tileSize) {
        int tyEnd = std::min(height, ty + tileSize);
        int runX = -1;
        for(int tx = 0; tx < width; tx += tileSize) {
            int txEnd = std::min(width, tx + tileSize);
            bool changed = false;
            for(int y = ty; y < tyEnd && !changed; ++y) {
                size_t off = (size_t)y * (size_t)stride + (size_t)tx;
                if(!spanEqual(a + off, b + off, txEnd - tx)) {
                    changed = true;
                }
            }
            if(changed) {
                if(runX < 0) {
                    runX = tx;
                }
            } else if(runX >= 0) {
                out.push_back({runX, ty, tx, tyEnd});
                runX = -1;
            }
        }
        if(runX >= 0) {
            out.push_back({runX, ty, width, tyEnd});
        }
    }
}

}
