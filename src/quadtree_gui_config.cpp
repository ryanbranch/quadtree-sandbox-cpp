#include "quadtree_gui_config.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

static bool parseU256Gui(const std::string& s, u256& out) {
    if (s.empty()) return false;
    u256 v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (unsigned)(c - '0');
    }
    out = v;
    return true;
}

static bool loadRootSpecsFile(const std::string& path, std::vector<RootSpec>& out,
                              bool& hasFirstRank, u256& firstRank) {
    std::ifstream is(path);
    if (!is) { std::cerr << "cannot open layout file: " << path << "\n"; return false; }
    out.clear();
    hasFirstRank = false;
    std::string line;
    int lineNo = 0;
    int rootNo = 0;
    while (std::getline(is, line)) {
        lineNo++;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        RootSpec spec;
        if (!(ls >> spec.x >> spec.y >> spec.k)) {
            std::cerr << "bad layout line " << lineNo << ": expected x y k [rank on first root only]\n";
            return false;
        }
        std::string extra;
        if (ls >> extra) {
            if (rootNo != 0) {
                std::cerr << "bad layout line " << lineNo
                          << ": rank may only be specified on the first root\n";
                return false;
            }
            if (!parseU256Gui(extra, firstRank)) {
                std::cerr << "bad layout line " << lineNo << ": invalid first-root rank\n";
                return false;
            }
            std::string trailing;
            if (ls >> trailing) {
                std::cerr << "bad layout line " << lineNo << ": too many fields\n";
                return false;
            }
            hasFirstRank = true;
        }
        out.push_back(spec);
        rootNo++;
    }
    return true;
}

bool parseArgs(int argc, char** argv, uint64_t defaultSeed, GuiConfig& cfg) {
    cfg.seed = defaultSeed;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--multi" && i + 2 < argc) {
            cfg.multiMode = true;
            cfg.multiW = std::atoi(argv[++i]);
            cfg.multiH = std::atoi(argv[++i]);
            if (cfg.multiW <= 0 || cfg.multiH <= 0) {
                std::cerr << "--multi dimensions must be positive\n"; return false;
            }
        } else if (a == "--layout" && i + 1 < argc) {
            cfg.layoutFile = argv[++i];
        } else if (a == "--seed" && i + 1 < argc) {
            cfg.seed = (uint64_t)std::strtoull(argv[++i], nullptr, 10);
        } else if ((a == "--attempts" || a == "--max-attempts") && i + 1 < argc) {
            cfg.attempts = std::atoi(argv[++i]);
        } else if (a == "--no-limit") {
            cfg.attempts = -1;
        } else if (a == "--k" && i + 1 < argc) {
            cfg.k = std::atoi(argv[++i]);
            if (cfg.k < 0) { std::cerr << "--k must be >= 0\n"; return false; }
        } else if (a == "--index" && i + 1 < argc) {
            // Parse as u256 via the same decimal loop used in quadtree_main.cpp
            std::string s = argv[++i];
            u256 v = 0;
            for (char c : s) {
                if (c < '0' || c > '9') { std::cerr << "--index must be a non-negative integer\n"; return false; }
                v = v * 10 + (unsigned)(c - '0');
            }
            cfg.userIndex = v;
            cfg.haveIndex = true;
        } else if (a == "--print-grid") {
            cfg.printGrid = true;
        } else if (a == "--outer-edge-1x1") {
            cfg.outerEdge1x1 = true;
        } else if (a == "----no-outer-flip") {
            cfg.noOuterFlip = true;
        } else if (a == "--skip-1x1-precomputation") {
            cfg.skip1x1Precomputation = true;
        } else if (a == "--greedy-cover") {
            cfg.greedyCover = true;
        } else if (a == "--first-rank" && i + 1 < argc) {
            if (!parseU256Gui(argv[++i], cfg.firstRank)) {
                std::cerr << "bad --first-rank N\n"; return false;
            }
            cfg.hasFirstRank = true;
        } else if (a == "--first-k" && i + 1 < argc) {
            cfg.firstK = std::atoi(argv[++i]);
            if (cfg.firstK < 0) { std::cerr << "bad --first-k K\n"; return false; }
            cfg.hasFirstK = true;
        } else if (a == "--color" && i + 1 < argc) {
            std::string m = argv[++i];
            if      (m == "none")    cfg.colorMode = CM_NONE;
            else if (m == "random")  cfg.colorMode = CM_RANDOM;
            else if (m == "by-size") cfg.colorMode = CM_BY_SIZE;
            else if (m == "by-rank") cfg.colorMode = CM_BY_RANK;
            else { std::cerr << "--color must be one of: none, random, by-size, by-rank\n"; return false; }
        } else if (a == "--resolution" && i + 1 < argc) {
            cfg.resolutionScale = (float)std::atof(argv[++i]);
            if (cfg.resolutionScale <= 0.0f) { std::cerr << "--resolution must be > 0\n"; return false; }
        } else {
            std::cerr << "Usage: quadtree_gui [--k K] [--index N] [--print-grid] [--color MODE] [--resolution N]\n"
                      << "       quadtree_gui --multi M N [--layout FILE] [--seed S] [--attempts N] [--max-attempts N] [--no-limit] [--print-grid] [--outer-edge-1x1] [--skip-1x1-precomputation] [--greedy-cover] [--color MODE] [--resolution N] [--first-rank N [--first-k K]]\n"
                      << "       --attempts N / --max-attempts N: stop after N failed attempts (default 128)\n"
                      << "       --no-limit: keep searching indefinitely until success (Ctrl-C to abort)\n"
                      << "       --first-rank N: fix the first root's quadtree to rank N\n"
                      << "         requires --layout or --first-k K (placed at origin, rest filled randomly)\n"
                      << "       --color MODE is one of: none, random, by-size, by-rank\n"
                      << "       --resolution N is a float scaling factor for backslash-key high-res PNG export (default 2.0)\n"
                      << "       layout lines are: x y k; first line may be: x y k rank\n"
                      << "       Keys: arrows pan, =/- zoom, 1 toggle smooth/discrete, [ / ] speed down/up, T toggle tiles, P toggle paths, Shift regenerate, Enter PNG, \\ high-res PNG, Space MP4 record, R recolor, Esc quit\n";
            return false;
        }
    }

    if (!cfg.layoutFile.empty()) {
        bool fileHasFirstRank = false;
        u256 fileFirstRank = 0;
        if (!loadRootSpecsFile(cfg.layoutFile, cfg.layoutSpecs, fileHasFirstRank, fileFirstRank))
            return false;
        if (fileHasFirstRank && cfg.hasFirstRank) {
            std::cerr << "--first-rank conflicts with rank specified in layout file\n";
            return false;
        }
        if (fileHasFirstRank) {
            cfg.hasFirstRank = true;
            cfg.firstRank = fileFirstRank;
        }
    }

    if (cfg.hasFirstRank && cfg.layoutFile.empty() && !cfg.hasFirstK) {
        std::cerr << "--first-rank requires --layout or --first-k K\n";
        return false;
    }
    if (cfg.hasFirstK && cfg.layoutFile.empty() && !cfg.hasFirstRank) {
        std::cerr << "--first-k requires --first-rank N\n";
        return false;
    }
    if (cfg.hasFirstK && !cfg.layoutFile.empty()) {
        std::cerr << "--first-k cannot be combined with --layout\n";
        return false;
    }
    if (cfg.hasFirstK) {
        int side = 1 << cfg.firstK;
        if (side > cfg.multiW || side > cfg.multiH) {
            std::cerr << "--first-k " << cfg.firstK << " gives size " << side
                      << " which exceeds grid " << cfg.multiW << "x" << cfg.multiH << "\n";
            return false;
        }
    }

    return true;
}
