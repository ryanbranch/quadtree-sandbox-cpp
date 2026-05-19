#include "noise_field.h"

// ---- art-project constants (carried over from art-project-src/config.h) ----
//
// Only the handful of constants that actually feed the bit-generation pipeline
// are reproduced here; the path/curve machinery is not needed.
namespace {
constexpr int    UNIVERSE_Z_CONSTANT = 28;   // base virtual Z of the 2D field
constexpr int    Z0_COUNT            = 8;    // unique Z coordinates computed
constexpr int    Z_BOOL_DENOMINATOR  = 79;   // Z-hash floor-divided by this; low bit kept
const std::vector<int> FNV_SEED_BYTES = {131, 113};
}

// ---- fnv32 ------------------------------------------------------------------

uint32_t fnv32(const std::vector<int>& ints, uint32_t seed) {
    uint32_t h = seed;
    for (int val : ints) {
        h ^= (uint32_t)(val & 255);
        h += (h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24);
    }
    return h;
}

// ---- NoiseField -------------------------------------------------------------

NoiseField::NoiseField(int width, int height, int chunkX, int chunkY)
    : w_(width), h_(height) {

    const uint32_t hashSeed = fnv32(FNV_SEED_BYTES);

    // ---- stage 1: genHashes ------------------------------------------------
    //
    // Build a (w_+1) x (h_+1) grid of vertex hashes; the extra row/column is
    // the boundary the no-11 transform needs. The boundary row/column hash
    // with local index 0 (instead of w_/h_) so they match the "next chunk"'s
    // first row/column -- faithful to the art source even though we only build
    // one chunk here.
    //
    // boolXYZ3D[x][y][z] holds the per-vertex bit for each Z coordinate.
    const int xCount = w_ + 1;
    const int yCount = h_ + 1;

    std::vector<std::vector<std::vector<bool>>> boolXYZ3D(
        xCount, std::vector<std::vector<bool>>(yCount));

    for (int x = 0; x < xCount; x++) {
        // The boundary column (x == w_) belongs to chunk (chunkX+1); the rest
        // to chunkX. Its local index wraps to 0 so the hash stitches.
        int xChunk = (x == w_) ? chunkX + 1 : chunkX;
        int xLocal = (x == w_) ? 0          : x;
        uint32_t xHash = fnv32({xLocal, xChunk}, hashSeed);

        for (int y = 0; y < yCount; y++) {
            int yChunk = (y == h_) ? chunkY + 1 : chunkY;
            int yLocal = (y == h_) ? 0          : y;
            uint32_t xyHash = fnv32({yLocal, yChunk}, xHash);

            boolXYZ3D[x][y].resize(Z0_COUNT);
            for (int z = 0; z < Z0_COUNT; z++) {
                uint32_t zHash = fnv32({z, UNIVERSE_Z_CONSTANT}, xyHash);
                boolXYZ3D[x][y][z] =
                    ((zHash / (uint32_t)Z_BOOL_DENOMINATOR) & 1u) == 1u;
            }
        }
    }

    // chirality is the raw z=0 bit, taken directly over the interior grid.
    chiral_.assign(w_, std::vector<bool>(h_));
    for (int x = 0; x < w_; x++)
        for (int y = 0; y < h_; y++)
            chiral_[x][y] = boolXYZ3D[x][y][0];

    // ---- stage 2: genBinaryNo11 -------------------------------------------
    //
    // The "no consecutive 1s" digit transform, ported from the art source and
    // generalized from fixed 2-cell chunks to the whole grid. Two bit-planes
    // are produced:
    //   hFlip : built per column-index n, iterating digit index d  (z = 1)
    //   vFlip : built the same way on the transposed axis           (z = 2)
    //
    // For each number index n we lay down digits: when the raw bool is true we
    // emit a 1 followed by the mandatory 0, otherwise just a 0 -- so a 1 is
    // always followed by a 0 ("no 11"). The digit string is trimmed to exactly
    // `totalCount` digits. The last digit is then forced to the raw bool of the
    // boundary row/column (the art project's seamless-tiling rule); if that
    // forced digit is 1, the digit before it is cleared to preserve "no 11".

    auto buildNo11 = [](int numberCount, int digitCount,
                        // rawBit(n, d): the raw FNV bool feeding number n, digit d
                        auto rawBit) -> std::vector<std::vector<bool>> {
        int totalCount = digitCount + 1;   // includes the boundary digit
        int baseCount  = digitCount;       // index of the boundary digit
        std::vector<std::vector<bool>> out(numberCount + 1);

        for (int n = 0; n <= numberCount; n++) {
            std::vector<bool>& digits = out[n];
            for (int d = 0; d <= digitCount; d++) {
                if ((int)digits.size() < totalCount) {
                    if (rawBit(n, d)) digits.push_back(true);
                    digits.push_back(false);
                }
                if ((int)digits.size() > totalCount) digits.pop_back();
            }
            // Force the boundary digit to the raw boundary bool, then apply the
            // no-11 rule to the digit just before it.
            bool demote = rawBit(n, baseCount);
            digits[baseCount] = demote;
            if (demote) digits[baseCount - 1] = false;
        }
        return out;
    };

    // hFlip: number index = x (column), digit index = y (row). z = 1.
    std::vector<std::vector<bool>> gridFlipH =
        buildNo11(w_, h_, [&](int n, int d) {
            return boolXYZ3D[n][d][1];
        });

    // vFlip: built on the transposed axis -- number index = y, digit index = x,
    // z = 2 -- then transposed so it indexes [x][y] like the others.
    std::vector<std::vector<bool>> tempFlipV =
        buildNo11(h_, w_, [&](int n, int d) {
            return boolXYZ3D[d][n][2];
        });

    // Copy the FULL (w+1) x (h+1) vertex grid for hFlip/vFlip -- this is the
    // size that buildNo11 produces, and we keep the whole thing so callers can
    // sample the 4 corners of any tile (including tiles at the grid's far edge,
    // whose lower-right corner sits at vertex (w_, h_)).
    hFlip_.assign(w_ + 1, std::vector<bool>(h_ + 1));
    vFlip_.assign(w_ + 1, std::vector<bool>(h_ + 1));
    for (int x = 0; x <= w_; x++) {
        for (int y = 0; y <= h_; y++) {
            hFlip_[x][y] = gridFlipH[x][y];
            vFlip_[x][y] = tempFlipV[y][x];   // transpose
        }
    }
}

TileBits NoiseField::sample(int x, int y) const {
    return TileBits{ hFlip_[x][y], vFlip_[x][y], chiral_[x][y] };
}

TileCornerBits NoiseField::sampleCorners(int x, int y, int size) const {
    // 4 tile corners in art-project corner order: UL, UR, LL, LR.
    const int xs[4] = { x,        x + size, x,        x + size };
    const int ys[4] = { y,        y,        y + size, y + size };

    // For size > 1, the raw corner coordinates step by `size` through the no-11
    // array, which can land two adjacent tiles on consecutive 1s (violating the
    // no-adjacent-flips invariant). Apply the stride-remapping transform so that
    // a step of `size` in tile space maps to a step of 1 in the no-11 array,
    // regardless of the tile's position within a stride-`size` phase.
    //
    // The grid has (w_+1) vertices (0..w_). For stride s, phase p = c%s contains
    // vertices p, p+s, p+2s, ... The counts are: count(p) = q+1 if p<=r, else q,
    // where q = w_/s and r = w_%s. The phase offset (prefix sum) is:
    //   offset(p) = p*(q+1)      if p <= r
    //   offset(p) = p*q + r      if p >  r
    // Transformed index = offset(p) + floor(c/s).
    auto transformCoord = [](int c, int dim, int s) -> int {
        int p = c % s;
        int k = c / s;
        int q = dim / s;
        int r = dim % s;
        int offset = (p <= r) ? p * (q + 1) : p * q + r;
        return offset + k;
    };

    TileCornerBits out;
    for (int i = 0; i < 4; i++) {
        int mapped_x = (size == 1) ? xs[i] : transformCoord(xs[i], w_, size);
        int mapped_y = (size == 1) ? ys[i] : transformCoord(ys[i], h_, size);
        out.hFlip[i] = hFlip_[mapped_x][mapped_y];
        out.vFlip[i] = vFlip_[mapped_x][mapped_y];
    }
    return out;
}

// ---- canBeChiral ------------------------------------------------------------
//
// Ported directly from art-project-src/cell.cpp's genAtoms() logic, sans
// rendering. Computes the virtual hIn/hOut/vIn/vOut neighbour indices that the
// path system would produce from the corner flip bits, then checks the
// art-project's chirality-downgrade condition: chirality is allowed only when
// swapping hOut and vOut would NOT yield a degenerate hIn==vOut or vIn==hOut
// (4-to-4 / 8-to-8 path).
PathEndpoints computeEndpoints(const TileCornerBits& bits) {
    const bool* hFlip = bits.hFlip;
    const bool* vFlip = bits.vFlip;
    PathEndpoints e;
    e.hIn  = (hFlip[0] ? 2 : (hFlip[2] ? 4 : 3));
    e.hOut = (hFlip[1] ? 8 : (hFlip[3] ? 6 : 7));
    e.vIn  = (vFlip[0] ? 2 : (vFlip[1] ? 8 : 1));
    e.vOut = (vFlip[2] ? 4 : (vFlip[3] ? 6 : 5));
    return e;
}

bool canBeChiral(const TileCornerBits& bits) {
    PathEndpoints e = computeEndpoints(bits);
    return (e.hIn != e.vOut) && (e.vIn != e.hOut);
}
