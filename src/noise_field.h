#pragma once

#include <cstdint>
#include <vector>

// ---------------------------------------------------------------- noise field
//
// Ports the FNV-hash "noise field" from the art-project source so that every
// quadtree grid vertex (cell upper-left corner) can be assigned three bits:
//
//   chiral : chirality bit          (raw FNV bool at z = UNIVERSE_Z_CONSTANT+0)
//   hFlip  : horizontal flip bit    (FNV bool at z+1, run through the no-11
//                                    digit transform across the whole grid)
//   vFlip  : vertical flip bit      (FNV bool at z+2, no-11 transform, transposed)
//
// Unlike the art project (which works on fixed 2x2-cell chunks for seamless
// universe tiling), this treats the entire width x height grid as a single
// "chunk": the no-consecutive-1s transform runs once over the whole grid, with
// no cross-grid boundary stitching.

// One quadtree tile's three bits, sampled at its upper-left corner.
struct TileBits {
    bool hFlip;
    bool vFlip;
    bool chiral;
};

// The four hFlip/vFlip bits sampled at the four corners of one tile (UL, UR,
// LL, LR), packed in the order the art project's Cell expects. Used by the
// chirality-downgrade rule, which mirrors the art-project semantics of
// per-corner flip routing.
struct TileCornerBits {
    bool hFlip[4];   // [0]=UL, [1]=UR, [2]=LL, [3]=LR
    bool vFlip[4];
};

// Faithfully ports the art-project's chirality-downgrade rule. Given the 4+4
// corner-flip bits of a single cell, computes the (virtual) hIn/hOut/vIn/vOut
// neighbour indices and returns false if applying chirality would produce a
// 4-to-4 or 8-to-8 degenerate path (hIn == vOut || vIn == hOut). The cell
// retains chirality only if this returns true.
bool canBeChiral(const TileCornerBits& bits);

// The H- and V-path entry/exit neighbour indices (1-8 compass: 1=N, 2=NW,
// 3=W, 4=SW, 5=S, 6=SE, 7=E, 8=NE), derived from a cell's corner flip bits
// using the art-project's mapping. Without any flips: hIn=3 (W), hOut=7 (E),
// vIn=1 (N), vOut=5 (S) -- the standard cardinal crossings. Single corner
// flips divert one endpoint of one path to an adjacent diagonal corner.
struct PathEndpoints {
    int hIn, hOut, vIn, vOut;
};

PathEndpoints computeEndpoints(const TileCornerBits& bits);

// 32-bit FNV-1a hash over a list of ints (each masked to its low byte, matching
// the art-project hash). `seed` is the FNV offset basis by default.
uint32_t fnv32(const std::vector<int>& ints, uint32_t seed = 0x811c9dc5u);

// The noise field for a width x height grid of cell vertices. Construction runs
// the full FNV -> no-11 pipeline once; afterwards sample() is a cheap lookup.
class NoiseField {
public:
    // Build the field for a grid of `width` x `height` cells. `chunkX`/`chunkY`
    // feed the FNV hash as the (single) chunk coordinate -- vary them to shift
    // the field. Defaults reproduce the art project's chunk (0, 0) behaviour
    // with its standard seed bytes.
    NoiseField(int width, int height, int chunkX = 0, int chunkY = 0);

    int width()  const { return w_; }
    int height() const { return h_; }

    // The three bits for the tile whose upper-left corner is grid vertex (x, y).
    // (x, y) must be in [0, width) x [0, height).
    TileBits sample(int x, int y) const;

    // The 4+4 corner bits for a tile of given size at (x, y). The 4 corners are
    // the vertices (x, y), (x+size, y), (x, y+size), (x+size, y+size) -- so for
    // multi-cell tiles the corners are spread across the noise field, sampling
    // wider noise structure than a single cell would. hFlip / vFlip are
    // available at vertices in [0, width] x [0, height] inclusive.
    TileCornerBits sampleCorners(int x, int y, int size) const;

private:
    int w_, h_;
    std::vector<std::vector<bool>> chiral_;   // [x][y], size w_ x h_
    // hFlip/vFlip are stored over the FULL (w_+1) x (h_+1) vertex grid so the
    // four corners of any tile -- including the lower-right corner at vertex
    // (w_, h_) -- are accessible.
    std::vector<std::vector<bool>> hFlip_;    // [x][y], size (w_+1) x (h_+1)
    std::vector<std::vector<bool>> vFlip_;    // [x][y], size (w_+1) x (h_+1)
};
