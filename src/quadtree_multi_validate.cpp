#include "quadtree_multi_internal.h"

#include <algorithm>
#include <set>
#include <sstream>

using namespace quadtree_multi_internal;

// --------------------------------------------------------------------------
// TileMaps construction

bool quadtree_multi_internal::build_tile_maps(
    const MultiQuadtreeTiling& tiling, bool require_full,
    TileMaps& maps, std::vector<std::string>* errors)
{
    if (tiling.width <= 0 || tiling.height <= 0) {
        add_error(errors, "tiling dimensions must be positive");
        return false;
    }

    maps.tiles = render_tiles(tiling);
    maps.tile_id.assign(tiling.height, std::vector<int>(tiling.width, -1));
    maps.root_id.assign(tiling.height, std::vector<int>(tiling.width, -1));

    bool ok = true;
    for (size_t i = 0; i < maps.tiles.size(); i++) {
        const RenderTile& t = maps.tiles[i];
        if (t.x < 0 || t.y < 0 || t.size <= 0
         || t.x + t.size > tiling.width || t.y + t.size > tiling.height) {
            std::ostringstream os;
            os << "tile out of bounds at root " << t.root_index
               << " tile (" << t.x << "," << t.y << "," << t.size << ")";
            add_error(errors, os.str());
            ok = false;
            continue;
        }
        for (int y = t.y; y < t.y + t.size; y++) {
            for (int x = t.x; x < t.x + t.size; x++) {
                if (maps.tile_id[y][x] != -1) {
                    std::ostringstream os;
                    os << "overlap at cell (" << x << "," << y << ")";
                    add_error(errors, os.str());
                    ok = false;
                }
                maps.tile_id[y][x] = (int)i;
                maps.root_id[y][x] = t.root_index;
            }
        }
    }

    if (require_full) {
        for (int y = 0; y < tiling.height; y++) {
            for (int x = 0; x < tiling.width; x++) {
                if (maps.tile_id[y][x] == -1) {
                    std::ostringstream os;
                    os << "uncovered cell (" << x << "," << y << ")";
                    add_error(errors, os.str());
                    ok = false;
                }
            }
        }
    }

    return ok;
}

// --------------------------------------------------------------------------
// Edge-contact geometry helpers

static bool intervals_overlap(int a0, int a1, int b0, int b1) {
    return std::max(a0, b0) < std::min(a1, b1);
}

static bool allowed_edge_contact(const RenderTile& a, const RenderTile& b,
                                 std::string* reason) {
    bool vertical = false;
    bool horizontal = false;
    int a0 = 0, b0 = 0, overlap = 0;

    if ((a.x + a.size == b.x || b.x + b.size == a.x)
     && intervals_overlap(a.y, a.y + a.size, b.y, b.y + b.size)) {
        vertical = true;
        a0 = a.y;
        b0 = b.y;
        overlap = std::min(a.y + a.size, b.y + b.size) - std::max(a.y, b.y);
    }
    if ((a.y + a.size == b.y || b.y + b.size == a.y)
     && intervals_overlap(a.x, a.x + a.size, b.x, b.x + b.size)) {
        horizontal = true;
        a0 = a.x;
        b0 = b.x;
        overlap = std::min(a.x + a.size, b.x + b.size) - std::max(a.x, b.x);
    }
    if (vertical == horizontal) {
        if (reason) *reason = "tiles do not share exactly one edge";
        return false;
    }

    int s = a.size;
    int t = b.size;
    if (s == t) {
        int offset = std::abs(a0 - b0);
        if (offset == 0 && overlap == s) return true;
        if (s % 2 == 0 && offset == s / 2 && overlap == s / 2) return true;
        if (reason) *reason = "equal-size tiles are not aligned or half-offset";
        return false;
    }

    int small = std::min(s, t);
    int large = std::max(s, t);
    if (large != 2 * small) {
        if (reason) *reason = "tile sizes differ by more than 2:1";
        return false;
    }

    int large0 = (s > t) ? a0 : b0;
    int small0 = (s > t) ? b0 : a0;
    if (overlap == small && (small0 == large0 || small0 == large0 + small))
        return true;

    if (reason) *reason = "2:1 contact does not occupy an aligned half";
    return false;
}

static bool validate_edges_from_maps(const MultiQuadtreeTiling& tiling, const TileMaps& maps,
                                     std::vector<std::string>* errors) {
    bool ok = true;
    std::set<std::pair<int,int>> checked;

    auto check_pair = [&](int a, int b) {
        if (a < 0 || b < 0 || a == b) return;
        const RenderTile& ta = maps.tiles[a];
        const RenderTile& tb = maps.tiles[b];
        if (ta.root_index == tb.root_index) return;
        auto key = std::minmax(a, b);
        if (!checked.insert(key).second) return;
        std::string reason;
        if (!allowed_edge_contact(ta, tb, &reason)) {
            std::ostringstream os;
            os << "invalid inter-root contact between root " << ta.root_index
               << " tile (" << ta.x << "," << ta.y << "," << ta.size << ")"
               << " and root " << tb.root_index
               << " tile (" << tb.x << "," << tb.y << "," << tb.size << "): "
               << reason;
            add_error(errors, os.str());
            ok = false;
        }
    };

    for (int y = 0; y < tiling.height; y++) {
        for (int x = 1; x < tiling.width; x++)
            check_pair(maps.tile_id[y][x - 1], maps.tile_id[y][x]);
    }
    for (int y = 1; y < tiling.height; y++) {
        for (int x = 0; x < tiling.width; x++)
            check_pair(maps.tile_id[y - 1][x], maps.tile_id[y][x]);
    }

    return ok;
}

bool quadtree_multi_internal::validate_partial_multi(
    const MultiQuadtreeTiling& tiling, std::vector<std::string>* errors)
{
    TileMaps maps;
    if (!build_tile_maps(tiling, false, maps, errors)) return false;
    return validate_edges_from_maps(tiling, maps, errors);
}

// --------------------------------------------------------------------------
// Public validation API

bool validate_root_cover(int width, int height,
                         const std::vector<RootSpec>& roots,
                         std::vector<std::string>* errors) {
    bool ok = true;
    if (width <= 0 || height <= 0) {
        add_error(errors, "cover dimensions must be positive");
        return false;
    }

    std::vector<std::vector<int>> owner(height, std::vector<int>(width, -1));
    for (size_t i = 0; i < roots.size(); i++) {
        const RootSpec& r = roots[i];
        int side = root_side(r);
        if (side <= 0) {
            std::ostringstream os;
            os << "root " << i << " has invalid k=" << r.k;
            add_error(errors, os.str());
            ok = false;
            continue;
        }
        if (r.x < 0 || r.y < 0 || r.x + side > width || r.y + side > height) {
            std::ostringstream os;
            os << "root " << i << " out of bounds: "
               << r.x << " " << r.y << " " << r.k;
            add_error(errors, os.str());
            ok = false;
            continue;
        }
        for (int y = r.y; y < r.y + side; y++) {
            for (int x = r.x; x < r.x + side; x++) {
                if (owner[y][x] != -1) {
                    std::ostringstream os;
                    os << "root " << i << " overlaps root " << owner[y][x]
                       << " at cell (" << x << "," << y << ")";
                    add_error(errors, os.str());
                    ok = false;
                }
                owner[y][x] = (int)i;
            }
        }
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (owner[y][x] == -1) {
                std::ostringstream os;
                os << "uncovered cell (" << x << "," << y << ")";
                add_error(errors, os.str());
                ok = false;
            }
        }
    }

    return ok;
}

bool validate_multi_quadtree(const MultiQuadtreeTiling& tiling,
                             std::vector<std::string>* errors) {
    std::vector<RootSpec> specs;
    specs.reserve(tiling.roots.size());
    for (size_t i = 0; i < tiling.roots.size(); i++) {
        specs.push_back(tiling.roots[i].spec);
        if (!tiling.roots[i].tree) {
            std::ostringstream os;
            os << "root " << i << " has no tree";
            add_error(errors, os.str());
        }
    }

    bool ok = validate_root_cover(tiling.width, tiling.height, specs, errors);
    TileMaps maps;
    ok = build_tile_maps(tiling, true, maps, errors) && ok;
    ok = validate_edges_from_maps(tiling, maps, errors) && ok;
    return ok && (!errors || errors->empty());
}

bool validate_outer_edges_1x1(const MultiQuadtreeTiling& tiling,
                              std::vector<std::string>* errors) {
    auto grid = render_grid(tiling);
    if ((int)grid.size() != tiling.height || tiling.height <= 0 || tiling.width <= 0) {
        add_error(errors, "cannot validate outer edges on invalid grid");
        return false;
    }

    bool ok = true;
    auto check = [&](int x, int y) {
        if (grid[y][x] != 1) {
            std::ostringstream os;
            os << "outer-edge cell (" << x << "," << y
               << ") belongs to a " << grid[y][x] << "x" << grid[y][x]
               << " tile, expected 1x1";
            add_error(errors, os.str());
            ok = false;
        }
    };

    for (int x = 0; x < tiling.width; x++) {
        check(x, 0);
        if (tiling.height > 1) check(x, tiling.height - 1);
    }
    for (int y = 1; y + 1 < tiling.height; y++) {
        check(0, y);
        if (tiling.width > 1) check(tiling.width - 1, y);
    }

    return ok;
}
