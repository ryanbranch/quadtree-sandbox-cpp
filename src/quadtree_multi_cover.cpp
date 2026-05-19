#include "quadtree_multi_internal.h"

#include <algorithm>
#include <random>
#include <vector>

using namespace quadtree_multi_internal;

// Returns the maximum allowed k for a root at (x0,y0) under outer-edge-1x1.
// A root's footprint is (x0..x0+side-1, y0..y0+side-1) where side=1<<k.
// Rule: min distance from footprint to grid border determines allowed k:
//   dist=0 (footprint touches border)      → only k=0 allowed
//   dist=1 (footprint one step from border) → k≤1 allowed
//   dist≥2                                  → any k allowed
// We find the largest k (0..29) satisfying both the grid bounds and this rule.
static int outer_edge_max_k(int x0, int y0, int width, int height) {
    for (int k = 29; k >= 0; k--) {
        int side = 1 << k;
        if (x0 + side > width || y0 + side > height) continue;
        // min distance from any footprint cell to grid border
        int min_dist = std::min({x0, y0, width - x0 - side, height - y0 - side});
        if (min_dist == 0) return 0; // footprint on border → must be k=0, return immediately
        if (min_dist == 1) return std::min(k, 1); // adjacent to border → k≤1
        return k; // interior → unrestricted
    }
    return 0;
}

std::vector<RootSpec> random_root_cover(int width, int height, uint64_t seed,
                                        const RootSpec* seed_root,
                                        bool outer_edge_1x1) {
    if (width <= 0 || height <= 0) return {};

    std::mt19937_64 rng(seed);
    std::vector<std::vector<uint8_t>> used(height, std::vector<uint8_t>(width, 0));
    int remaining = width * height;
    std::vector<RootSpec> roots;

    if (seed_root) {
        int side = 1 << seed_root->k;
        roots.push_back(*seed_root);
        for (int y = seed_root->y; y < seed_root->y + side; y++)
            for (int x = seed_root->x; x < seed_root->x + side; x++)
                if (!used[y][x]) { used[y][x] = 1; remaining--; }
    }

    while (remaining > 0) {
        int x0 = -1, y0 = -1;
        for (int y = 0; y < height && y0 < 0; y++) {
            for (int x = 0; x < width; x++) {
                if (!used[y][x]) {
                    x0 = x;
                    y0 = y;
                    break;
                }
            }
        }
        std::vector<int> candidates;
        int max_k = outer_edge_1x1 ? outer_edge_max_k(x0, y0, width, height) : 29;
        for (int k = 0; k < 30; k++) {
            int side = 1 << k;
            if (x0 + side > width || y0 + side > height) break;
            if (k > max_k) break;
            bool clear = true;
            for (int y = y0; y < y0 + side && clear; y++)
                for (int x = x0; x < x0 + side; x++)
                    if (used[y][x]) { clear = false; break; }
            if (clear) candidates.push_back(k);
        }
        if (candidates.empty()) candidates.push_back(0);

        int k = candidates[(size_t)(rng() % candidates.size())];
        int side = 1 << k;
        roots.push_back({x0, y0, k});
        for (int y = y0; y < y0 + side; y++) {
            for (int x = x0; x < x0 + side; x++) {
                if (!used[y][x]) {
                    used[y][x] = 1;
                    remaining--;
                }
            }
        }
    }

    std::sort(roots.begin(), roots.end(), [](const RootSpec& a, const RootSpec& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.x != b.x) return a.x < b.x;
        return a.k > b.k;
    });
    return roots;
}

// Deterministic row-major largest-fitting-square cover.
// Scans in row-major order; at each uncovered cell places the largest power-of-2
// square that fits within the grid and doesn't overlap any already-placed tile.
// Produces the fewest possible tiles for the given grid dimensions.
std::vector<RootSpec> greedy_root_cover(int width, int height, bool outer_edge_1x1) {
    if (width <= 0 || height <= 0) return {};

    std::vector<std::vector<uint8_t>> used(height, std::vector<uint8_t>(width, 0));
    int remaining = width * height;
    std::vector<RootSpec> roots;

    while (remaining > 0) {
        int x0 = -1, y0 = -1;
        for (int y = 0; y < height && y0 < 0; y++) {
            for (int x = 0; x < width; x++) {
                if (!used[y][x]) { x0 = x; y0 = y; break; }
            }
        }

        int max_k = outer_edge_1x1 ? outer_edge_max_k(x0, y0, width, height) : 29;
        int best_k = 0;
        for (int k = 1; k < 30; k++) {
            int side = 1 << k;
            if (x0 + side > width || y0 + side > height) break;
            if (k > max_k) break;
            bool clear = true;
            for (int y = y0; y < y0 + side && clear; y++)
                for (int x = x0; x < x0 + side; x++)
                    if (used[y][x]) { clear = false; break; }
            if (clear) best_k = k; else break;
        }

        int side = 1 << best_k;
        roots.push_back({x0, y0, best_k});
        for (int y = y0; y < y0 + side; y++)
            for (int x = x0; x < x0 + side; x++)
                if (!used[y][x]) { used[y][x] = 1; remaining--; }
    }

    return roots;
}
