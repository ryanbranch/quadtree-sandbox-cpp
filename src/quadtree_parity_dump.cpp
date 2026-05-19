#include "noise_field.h"
#include "quadtree.h"
#include "quadtree_gui_geometry.h"
#include "quadtree_gui_path_graph.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

uint64_t parse_u64_arg(const char* s, const char* name) {
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (!s[0] || (end && *end)) {
        throw std::runtime_error(std::string("bad ") + name + ": " + s);
    }
    return (uint64_t)v;
}

const char* atom_kind_name(AtomKind k) {
    switch (k) {
        case AK_V: return "v";
        case AK_H: return "h";
        case AK_C0: return "c0";
        case AK_C1: return "c1";
    }
    return "unknown";
}

void usage(const char* argv0) {
    std::cerr
        << "usage: " << argv0 << " --k K --rank N [--chunk-x X] [--chunk-y Y]\n"
        << "       [--cell-px N] [--line-px-per-cell N] [--flips on|off]\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        int k = 3;
        uint64_t rank = 0;
        bool haveRank = false;
        int cellPx = 128;
        float linePxPerCell = 16.0f;
        int chunkX = 0;
        int chunkY = 0;
        bool flips = true;

        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            auto need = [&](const char* name) -> const char* {
                if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
                return argv[++i];
            };
            if (a == "--k") {
                k = (int)parse_u64_arg(need("--k"), "--k");
            } else if (a == "--rank") {
                rank = parse_u64_arg(need("--rank"), "--rank");
                haveRank = true;
            } else if (a == "--cell-px") {
                cellPx = (int)parse_u64_arg(need("--cell-px"), "--cell-px");
            } else if (a == "--line-px-per-cell") {
                linePxPerCell = std::stof(need("--line-px-per-cell"));
            } else if (a == "--chunk-x") {
                chunkX = (int)parse_u64_arg(need("--chunk-x"), "--chunk-x");
            } else if (a == "--chunk-y") {
                chunkY = (int)parse_u64_arg(need("--chunk-y"), "--chunk-y");
            } else if (a == "--flips") {
                std::string v = need("--flips");
                flips = (v == "on" || v == "true" || v == "1");
            } else if (a == "--help" || a == "-h") {
                usage(argv[0]);
                return 0;
            } else {
                throw std::runtime_error("unknown argument: " + a);
            }
        }
        if (!haveRank) throw std::runtime_error("missing --rank");
        if (k < 0 || k > 3) throw std::runtime_error("parity dump is intended for k=0..3");

        std::shared_ptr<QNode> tree;
        {
            // QuadtreeIndex may print cache/index status. Keep stdout pure JSON.
            std::ostringstream sink;
            std::streambuf* old = std::cout.rdbuf(sink.rdbuf());
            QuadtreeIndex idx(k);
            tree = idx.unrank_direct(u256(rank));
            std::cout.rdbuf(old);
        }

        int width = 1 << k;
        int height = 1 << k;
        MultiQuadtreeTiling tiling;
        tiling.width = width;
        tiling.height = height;
        tiling.roots.push_back({ RootSpec{0, 0, k}, tree });
        auto tiles = render_tiles(tiling);
        NoiseField noise(width, height, chunkX, chunkY);
        ConnectionBuild model = buildConnectionModel(
            tiles, width, height, cellPx, linePxPerCell,
            &noise, flips ? FR_ON : FR_OFF, 1);

        std::cout << std::fixed << std::setprecision(9);
        std::cout << "{";
        std::cout << "\"schema\":1";
        std::cout << ",\"k\":" << k;
        std::cout << ",\"rank\":\"" << rank << "\"";
        std::cout << ",\"width\":" << width << ",\"height\":" << height;
        std::cout << ",\"cellPx\":" << cellPx;
        std::cout << ",\"linePxPerCell\":" << linePxPerCell;
        std::cout << ",\"tiles\":[";
        for (size_t i = 0; i < tiles.size(); i++) {
            const RenderTile& t = tiles[i];
            if (i) std::cout << ",";
            std::cout << "{\"x\":" << t.x << ",\"y\":" << t.y
                      << ",\"size\":" << t.size
                      << ",\"rootIndex\":" << t.root_index << "}";
        }
        std::cout << "]";

        std::cout << ",\"segments\":[";
        for (size_t i = 0; i < model.segs.size(); i++) {
            const PathSegment& s = model.segs[i];
            if (i) std::cout << ",";
            std::cout << "{";
            std::cout << "\"kind\":\"" << (s.kind == SEG_LINE ? "line" : "arc") << "\"";
            std::cout << ",\"ax\":" << s.ax << ",\"ay\":" << s.ay;
            if (s.kind == SEG_LINE) {
                std::cout << ",\"bx\":" << s.bx << ",\"by\":" << s.by;
            } else {
                std::cout << ",\"radius\":" << s.radius
                          << ",\"a0\":" << s.a0 << ",\"a1\":" << s.a1;
            }
            std::cout << ",\"halfW\":" << s.halfW;
            std::cout << ",\"r\":" << s.r << ",\"g\":" << s.g << ",\"b\":" << s.b;
            std::cout << "}";
        }
        std::cout << "]";

        std::cout << ",\"atoms\":[";
        for (size_t i = 0; i < model.atoms.size(); i++) {
            const Atom& a = model.atoms[i];
            if (i) std::cout << ",";
            std::cout << "{\"tileIndex\":" << a.tileIdx
                      << ",\"kind\":\"" << atom_kind_name(a.kind) << "\""
                      << ",\"component\":" << a.component;
            std::cout << ",\"segments\":[";
            for (size_t si = a.segs.begin; si < a.segs.end; si++) {
                if (si != a.segs.begin) std::cout << ",";
                std::cout << si;
            }
            std::cout << "],\"in\":[";
            for (int n = 0; n < a.inCount; n++) {
                if (n) std::cout << ",";
                std::cout << a.inNeighbors[n];
            }
            std::cout << "],\"out\":[";
            for (int n = 0; n < a.outCount; n++) {
                if (n) std::cout << ",";
                std::cout << a.outNeighbors[n];
            }
            std::cout << "]}";
        }
        std::cout << "]";
        std::cout << ",\"numComponents\":" << model.numComp;
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "quadtree_parity_dump: " << e.what() << "\n";
        usage(argv[0]);
        return 1;
    }
}
