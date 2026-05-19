#include "quadtree_multi_internal.h"

using namespace quadtree_multi_internal;

namespace {

void collect_tiles_rec(const QNode& node, int x, int y, int size, int root_index,
                       std::vector<RenderTile>& out) {
    if (node.is_leaf()) {
        out.push_back({x, y, size, root_index});
        return;
    }
    int half = size / 2;
    collect_tiles_rec(*node.children[0], x,        y,        half, root_index, out);
    collect_tiles_rec(*node.children[1], x + half, y,        half, root_index, out);
    collect_tiles_rec(*node.children[2], x,        y + half, half, root_index, out);
    collect_tiles_rec(*node.children[3], x + half, y + half, half, root_index, out);
}

}  // namespace

std::vector<RenderTile> render_tiles(const MultiQuadtreeTiling& tiling) {
    std::vector<RenderTile> out;
    for (size_t i = 0; i < tiling.roots.size(); i++) {
        const PlacedQuadtreeRoot& root = tiling.roots[i];
        if (!root.tree) continue;
        int side = root_side(root.spec);
        if (side <= 0) continue;
        collect_tiles_rec(*root.tree, root.spec.x, root.spec.y, side, (int)i, out);
    }
    return out;
}

std::vector<std::vector<int>> render_grid(const MultiQuadtreeTiling& tiling) {
    std::vector<std::vector<int>> grid(
        tiling.height, std::vector<int>(tiling.width, 0));
    for (const RenderTile& tile : render_tiles(tiling)) {
        if (tile.x < 0 || tile.y < 0 || tile.size <= 0
         || tile.x + tile.size > tiling.width || tile.y + tile.size > tiling.height)
            continue;
        for (int y = tile.y; y < tile.y + tile.size; y++)
            for (int x = tile.x; x < tile.x + tile.size; x++)
                grid[y][x] = tile.size;
    }
    return grid;
}
