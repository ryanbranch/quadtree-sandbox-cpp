#include "quadtree_gui_path_graph.h"

#include <algorithm>
#include <queue>
#include <random>
#include <utility>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// HSV -> RGB. Duplicated here (and in quadtree_gui_geometry.cpp) so the two
// TUs stay independent; the function is tiny and pure.
// ---------------------------------------------------------------------------
void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    float c = v * s;
    float hh = h / 60.0f;
    float x = c * (1.0f - std::fabs(std::fmod(hh, 2.0f) - 1.0f));
    float r1 = 0, g1 = 0, b1 = 0;
    if      (hh < 1) { r1 = c; g1 = x; }
    else if (hh < 2) { r1 = x; g1 = c; }
    else if (hh < 3) { g1 = c; b1 = x; }
    else if (hh < 4) { g1 = x; b1 = c; }
    else if (hh < 5) { r1 = x; b1 = c; }
    else             { r1 = c; b1 = x; }
    float m = v - c;
    r = r1 + m; g = g1 + m; b = b1 + m;
}

// ---------------------------------------------------------------------------
// Compass direction abstraction (W, E, N, S). The path-graph code has four
// near-identical loops per atom — one for each cardinal direction. Driving
// them off a single Dir4 table collapses ~150 lines of copy-paste into a
// single loop body.
// ---------------------------------------------------------------------------
enum Dir4 { D_W = 0, D_E = 1, D_N = 2, D_S = 3 };

struct DirInfo {
    int  dx, dy;        // neighbor cell offset (per-step) from the tile's side
    int  cardinalIdx;   // compass index for that cardinal (W=3, E=7, N=1, S=5)
    bool axisH;         // true if this direction belongs to the horizontal axis
    bool posSide;       // true for E or S (the "+" side of the axis)
};

// Indexed by Dir4.
constexpr DirInfo kDirs[4] = {
    /* D_W */ { -1,  0, 3, /*axisH=*/true,  /*posSide=*/false },
    /* D_E */ { +1,  0, 7, /*axisH=*/true,  /*posSide=*/true  },
    /* D_N */ {  0, -1, 1, /*axisH=*/false, /*posSide=*/false },
    /* D_S */ {  0, +1, 5, /*axisH=*/false, /*posSide=*/true  },
};

inline bool isCardinal(int idx, Dir4 d) { return idx == kDirs[d].cardinalIdx; }
inline bool isDiagonal(int idx)         { return idx >= 2 && idx <= 8 && !(idx & 1); }

// For a tile `t` of size `s`, the starting cell at the +k offset along the
// shared edge of direction `d`. Returns the grid coord of the *neighbor* cell
// across that edge at offset `k` (0..s-1).
inline std::pair<int,int> edgeNeighborCell(const RenderTile& t, Dir4 d, int k) {
    int s = t.size;
    switch (d) {
        case D_W: return { t.x - 1, t.y + k };
        case D_E: return { t.x + s, t.y + k };
        case D_N: return { t.x + k, t.y - 1 };
        case D_S: return { t.x + k, t.y + s };
    }
    return { -1, -1 };
}

// ---------------------------------------------------------------------------
// classifySide / SideKind, duplicated minimally from geometry.cpp. This is
// enough of an oracle to know whether an endpoint should be overridden to its
// cardinal default (because the tile's side resolves into split sub-lanes,
// owned by the segment-emission pass) -- the path-graph pass needs to agree
// with what the segment pass actually emitted at SK_RESOLVE sides.
// ---------------------------------------------------------------------------
enum SideKind { SK_STRAIGHT, SK_BORDER, SK_RESOLVE };

SideKind classifySide(const std::vector<RenderTile>& tiles,
                      const std::vector<int>& tileIdGrid,
                      int width, int height,
                      int ti, Dir4 d) {
    const RenderTile& t = tiles[ti];
    int s = t.size;
    const DirInfo& info = kDirs[d];
    // The cell-column or cell-row just across the side.
    int nbCell;
    if (info.axisH) nbCell = info.posSide ? t.x + s : t.x - 1;
    else            nbCell = info.posSide ? t.y + s : t.y - 1;
    int axisLimit = info.axisH ? width : height;
    if (nbCell < 0 || nbCell >= axisLimit) return SK_BORDER;
    // Which neighbor covers each half of the edge?
    int lo = info.axisH ? t.y : t.x;
    auto nbIdAt = [&](int k) -> int {
        int gx = info.axisH ? nbCell    : lo + k;
        int gy = info.axisH ? lo + k    : nbCell;
        if (gx < 0 || gy < 0 || gx >= width || gy >= height) return -1;
        int nb = tileIdGrid[(size_t)gy * (size_t)width + (size_t)gx];
        return (nb == ti) ? -1 : nb;
    };
    int loHalf = nbIdAt(0);
    int hiHalf = nbIdAt(s - 1);
    if (loHalf < 0 || hiHalf < 0) return SK_STRAIGHT;
    return (loHalf != hiHalf) ? SK_RESOLVE : SK_STRAIGHT;
}

// ---------------------------------------------------------------------------
// Endpoint helpers shared by cardinal wiring and diagonal continuity detection.
// ---------------------------------------------------------------------------

// Resolve overrides: if a side classifies as SK_RESOLVE, the segment pass has
// already forced that endpoint to its cardinal default. The path-graph pass
// must agree, so applyResolveOverrides() snaps the relevant endpoints back to
// their cardinal defaults whenever a side resolves.
//
// `chiralSwap` controls whether the four "owned by" sides are the standard
// V/H assignment (V owns N/S, H owns W/E) or the chiral post-swap assignment
// (C0 owns W on hIn / S on hOut, C1 owns N on vIn / E on vOut).
void applyResolveOverrides(const std::vector<RenderTile>& tiles,
                           const std::vector<int>& tileIdGrid,
                           int width, int height, int ti,
                           PathEndpoints& e, bool chiralSwap) {
    auto resolves = [&](Dir4 d) {
        return classifySide(tiles, tileIdGrid, width, height, ti, d) == SK_RESOLVE;
    };
    if (!chiralSwap) {
        if (resolves(D_W)) e.hIn  = 3;
        if (resolves(D_E)) e.hOut = 7;
        if (resolves(D_N)) e.vIn  = 1;
        if (resolves(D_S)) e.vOut = 5;
    } else {
        // After swap(e.hOut, e.vOut):
        //   C0 = hIn -> hOut (W -> S);  C1 = vIn -> vOut (N -> E).
        // So the SK_RESOLVE override targets the post-swap names.
        if (resolves(D_W)) e.hIn  = 3;
        if (resolves(D_S)) e.hOut = 5;
        if (resolves(D_N)) e.vIn  = 1;
        if (resolves(D_E)) e.vOut = 7;
    }
}

// Endpoint grid-coord (in HALF-cell units so we can talk about midpoints
// without losing precision). Same encoding as the original endpointGridXY.
inline std::pair<int,int> endpointGridXY(const RenderTile& t, int idx) {
    int x2 = 2 * t.x, y2 = 2 * t.y, s2 = 2 * t.size;
    switch (idx) {
        case 1: return { x2 + s2 / 2, y2          };  // N
        case 5: return { x2 + s2 / 2, y2 + s2     };  // S
        case 3: return { x2,          y2 + s2 / 2 };  // W
        case 7: return { x2 + s2,     y2 + s2 / 2 };  // E
        case 2: return { x2,          y2          };  // NW
        case 8: return { x2 + s2,     y2          };  // NE
        case 4: return { x2,          y2 + s2     };  // SW
        case 6: return { x2 + s2,     y2 + s2     };  // SE
        default: return { -1, -1 };
    }
}

// Diagonal-corner offset within the tile (0 = upper/left, 1 = lower/right) on
// each axis, for indices 2/4/6/8 only. Returns {-1,-1} for cardinals.
inline std::pair<int,int> cornerDelta(int idx) {
    switch (idx) {
        case 2: return { 0, 0 };
        case 8: return { 1, 0 };
        case 4: return { 0, 1 };
        case 6: return { 1, 1 };
        default: return { -1, -1 };
    }
}

// Each atom kind has an "axis identity":
//   V-axis: AK_V, AK_C1 (built from vIn -> swapped-vOut)
//   H-axis: AK_H, AK_C0 (built from hIn -> swapped-hOut)
// Same-axis pairs are preferred at diagonal corners so parallel-overlapping V
// and H strokes don't cross-link through a shared corner.
inline int axisOf(AtomKind k) { return (k == AK_V || k == AK_C1) ? 0 : 1; }

// ---------------------------------------------------------------------------
// PathGraphBuilder: encapsulates all state and helpers for one buildPathGraph
// invocation. Lives entirely in the anonymous namespace.
// ---------------------------------------------------------------------------
class PathGraphBuilder {
public:
    PathGraphBuilder(const std::vector<RenderTile>& tiles,
                     int width, int height,
                     const std::vector<int>& tileIdGrid,
                     const SegmentEmitRanges& segRanges,
                     const PathGraphCaches& caches,
                     uint64_t pathColorSeed,
                     ConnectionBuild& out)
        : tiles_(tiles), width_(width), height_(height),
          tileIdGrid_(tileIdGrid), segRanges_(segRanges),
          caches_(caches), pathColorSeed_(pathColorSeed), out_(out),
          nTiles_((int)tiles.size()) {}

    void run() {
        constructAtoms();
        computeEndpointTables();
        wireAllNeighbors();
        assignComponents();
        buildPalette();
    }

private:
    // ---- inputs ----
    const std::vector<RenderTile>& tiles_;
    int                            width_, height_;
    const std::vector<int>&        tileIdGrid_;
    const SegmentEmitRanges&       segRanges_;
    const PathGraphCaches&         caches_;
    uint64_t                       pathColorSeed_;
    ConnectionBuild&               out_;
    int                            nTiles_;

    // ---- per-tile endpoint table (post-resolve + post-chiral-swap) ----
    // Holds the SAME endpoint values that segment-emission used, so wiring is
    // consistent with the geometry on the screen.
    //
    // For a non-chiral tile, atomEnds(ti, AK_V/AK_H) returns the standard V/H
    // endpoints (cardinal defaults + tier-aware diagonals + SK_RESOLVE snap).
    //
    // For a chiral tile, atomEnds(ti, AK_C0/AK_C1) returns the post-swap chiral
    // endpoints, also snapped at SK_RESOLVE sides.
    struct EndpointPair { int in, out; };
    std::vector<PathEndpoints> tEndsVH_;       // V/H, post-resolve, no swap
    std::vector<PathEndpoints> tEndsChiral_;   // post-swap, post-resolve

    // True for tiles that have ANY of the C0 / C1 atoms.
    bool tileHasChiralAtoms(int ti) const {
        return out_.atomC0[ti] >= 0 || out_.atomC1[ti] >= 0;
    }

    EndpointPair atomEnds(int ti, AtomKind k) const {
        if (k == AK_V)  return { tEndsVH_[ti].vIn, tEndsVH_[ti].vOut };
        if (k == AK_H)  return { tEndsVH_[ti].hIn, tEndsVH_[ti].hOut };
        if (k == AK_C0) return { tEndsChiral_[ti].hIn, tEndsChiral_[ti].hOut };
        /* AK_C1 */     return { tEndsChiral_[ti].vIn, tEndsChiral_[ti].vOut };
    }

    int atomIdOfKind(int ti, AtomKind k) const {
        switch (k) {
            case AK_V:  return out_.atomV[ti];
            case AK_H:  return out_.atomH[ti];
            case AK_C0: return out_.atomC0[ti];
            case AK_C1: return out_.atomC1[ti];
        }
        return -1;
    }

    SideKind sideKind(int ti, Dir4 d) const {
        return classifySide(tiles_, tileIdGrid_, width_, height_, ti, d);
    }

    int nbTileAt(int gx, int gy, int selfTi) const {
        if (gx < 0 || gy < 0 || gx >= width_ || gy >= height_) return -1;
        int n = tileIdGrid_[(size_t)gy * (size_t)width_ + (size_t)gx];
        return (n == selfTi) ? -1 : n;
    }

    // ---- stages ----
    void constructAtoms();
    void computeEndpointTables();
    void wireAllNeighbors();
    void wireCardinalSide(int ti, Dir4 d);
    void wireDiagonalAt (int ti, int thisAtom, AtomKind thisKind,
                         int thisIdx, bool thisIsOut);
    void assignComponents();
    void buildPalette();

    // Link a directed edge a -> b. Deduplicates and respects kMaxNeighbors.
    void linkOut(int ai, int bi) {
        if (ai < 0 || bi < 0 || ai == bi) return;
        Atom& a = out_.atoms[ai];
        Atom& b = out_.atoms[bi];
        for (int i = 0; i < a.outCount; i++) if (a.outNeighbors[i] == bi) return;
        if (a.outCount < Atom::kMaxNeighbors) a.outNeighbors[a.outCount++] = bi;
        if (b.inCount  < Atom::kMaxNeighbors) b.inNeighbors [b.inCount++ ] = ai;
    }

    // When a neighbor tile is fully chiral (no V/H atoms, only C0/C1), pick
    // the chiral atom whose endpoint actually faces the side toward us.
    //   neighbor lies to our W -> we want its E-facing atom = C1
    //   neighbor lies to our E -> we want its W-facing atom = C0
    //   neighbor lies to our N -> we want its S-facing atom = C0
    //   neighbor lies to our S -> we want its N-facing atom = C1
    int chiralFallback(int nti, Dir4 nbDirRelativeToUs) const {
        int c0 = out_.atomC0[nti];
        int c1 = out_.atomC1[nti];
        bool wantC1 = (nbDirRelativeToUs == D_W || nbDirRelativeToUs == D_S);
        if (wantC1 && c1 >= 0) return c1;
        if (!wantC1 && c0 >= 0) return c0;
        return -1;
    }

    // Same-axis partner test: does tile `nti` have ANY atom of `wantAxis`
    // with an endpoint matching (corner, wantIdx)? Used to decide whether to
    // suppress a cross-axis diagonal link (we prefer V<->V and H<->H).
    bool hasSameAxisAt(int nti, std::pair<int,int> corner,
                       int wantIdx, int wantAxis) const {
        const AtomKind kinds[4] = { AK_V, AK_H, AK_C0, AK_C1 };
        const RenderTile& nt = tiles_[nti];
        for (AtomKind k : kinds) {
            int aId = atomIdOfKind(nti, k);
            if (aId < 0) continue;
            if (axisOf(k) != wantAxis) continue;
            auto [aIn, aOut] = atomEnds(nti, k);
            auto check = [&](int idx) {
                if (!isDiagonal(idx)) return false;
                if (idx != wantIdx) return false;
                return endpointGridXY(nt, idx) == corner;
            };
            if (check(aIn) || check(aOut)) return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Stage 1: build one Atom per non-empty segment range, populate atom-id
// lookup tables. Order: V, H, C0, C1 per tile -- so iteration order in the
// BFS is tile-row-major within each kind.
// ---------------------------------------------------------------------------
void PathGraphBuilder::constructAtoms() {
    out_.atomV.assign (nTiles_, -1);
    out_.atomH.assign (nTiles_, -1);
    out_.atomC0.assign(nTiles_, -1);
    out_.atomC1.assign(nTiles_, -1);

    auto makeAtom = [&](int ti, AtomKind kind, const SegRange& r) -> int {
        if (r.begin >= r.end) return -1;
        int idx = (int)out_.atoms.size();
        Atom a;
        a.tileIdx = ti;
        a.kind    = kind;
        a.segs    = r;
        for (int q = 0; q < Atom::kMaxNeighbors; q++) {
            a.inNeighbors[q]  = -1;
            a.outNeighbors[q] = -1;
        }
        a.inCount = a.outCount = 0;
        a.component = -1;
        out_.atoms.push_back(a);
        return idx;
    };

    for (int ti = 0; ti < nTiles_; ti++) {
        out_.atomV [ti] = makeAtom(ti, AK_V,  segRanges_.v [ti]);
        out_.atomH [ti] = makeAtom(ti, AK_H,  segRanges_.h [ti]);
        out_.atomC0[ti] = makeAtom(ti, AK_C0, segRanges_.c0[ti]);
        out_.atomC1[ti] = makeAtom(ti, AK_C1, segRanges_.c1[ti]);
    }
}

// ---------------------------------------------------------------------------
// Stage 2: precompute the post-resolve endpoint tables for every tile, in both
// flavors (non-chiral V/H, and chiral post-swap). This is what atomEnds()
// reads. Building both tables up-front avoids recomputing resolves at every
// neighbor lookup.
// ---------------------------------------------------------------------------
void PathGraphBuilder::computeEndpointTables() {
    const PathEndpoints cardinalDefault = { 3, 7, 1, 5 };
    tEndsVH_.assign    (nTiles_, cardinalDefault);
    tEndsChiral_.assign(nTiles_, cardinalDefault);

    for (int ti = 0; ti < nTiles_; ti++) {
        // V/H endpoints: only meaningful for tiles that qualified for flips
        // (per the segment-emission qualifiesCache); otherwise leave defaults.
        if (caches_.qualifiesCache[ti] && caches_.noiseValid[ti]) {
            PathEndpoints e = caches_.endpointsCache[ti];
            applyResolveOverrides(tiles_, tileIdGrid_, width_, height_,
                                  ti, e, /*chiralSwap=*/false);
            tEndsVH_[ti] = e;
        }
        // Chiral post-swap endpoints: meaningful for every tile (cheap to
        // compute; only the tiles that actually have C0/C1 atoms will be
        // queried, but keeping the table dense simplifies access).
        if (caches_.noiseValid[ti]) {
            PathEndpoints e = caches_.endpointsCache[ti];
            std::swap(e.hOut, e.vOut);  // chiral swap
            applyResolveOverrides(tiles_, tileIdGrid_, width_, height_,
                                  ti, e, /*chiralSwap=*/true);
            tEndsChiral_[ti] = e;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 3a: wire one cardinal side of one tile.
//
// On the W/E side we walk H atoms; on the N/S side we walk V atoms. For a
// fully chiral neighbor (no V/H atoms), we fall back to whichever chiral atom
// faces our shared edge (chiralFallback()).
//
// For chiral tiles on our side, we walk all of the tile's atoms (V, H, C0,
// C1) and link each whose endpoint touches the side under consideration.
// That collapses the old "non-chiral atoms" + "chiral atoms" code paths into
// one symmetric loop.
//
// Linking is dedup'd by linkOut(), so wiring an edge from BOTH endpoints is
// harmless (each tile sees its W/E and the other sees its E/W); we don't
// need ti-ordering guards.
// ---------------------------------------------------------------------------
void PathGraphBuilder::wireCardinalSide(int ti, Dir4 d) {
    const RenderTile& t = tiles_[ti];
    int s = t.size;
    const DirInfo& info = kDirs[d];

    // Which atom-kinds on OUR tile can own an endpoint on direction `d`?
    //   horizontal sides (W/E) -> H-axis atoms (H + C0)
    //   vertical   sides (N/S) -> V-axis atoms (V + C1)
    const AtomKind hKinds[2] = { AK_H, AK_C0 };
    const AtomKind vKinds[2] = { AK_V, AK_C1 };
    const AtomKind* ourCandKinds = info.axisH ? hKinds : vKinds;

    // The complementary atom-kind on the neighbor's tile is the same one
    // (an H endpoint matches another H endpoint across the shared edge).
    // For fully chiral neighbors we fall back to the directionally-correct
    // chiral atom.

    for (int ki = 0; ki < 2; ki++) {
        AtomKind myKind = ourCandKinds[ki];
        int myAtom = atomIdOfKind(ti, myKind);
        if (myAtom < 0) continue;
        auto [myIn, myOut] = atomEnds(ti, myKind);
        bool myEndpointOnThisSide =
            isCardinal(myIn, d) || isCardinal(myOut, d);
        if (!myEndpointOnThisSide) continue;

        for (int k = 0; k < s; k++) {
            auto [gx, gy] = edgeNeighborCell(t, d, k);
            int nti = nbTileAt(gx, gy, ti);
            if (nti < 0) continue;

            // Pick the matching neighbor atom: same axis identity first, with
            // chiralFallback for fully-chiral neighbors.
            AtomKind nbPrimary = info.axisH ? AK_H : AK_V;
            int nbAtom = atomIdOfKind(nti, nbPrimary);
            if (nbAtom < 0) {
                // Neighbor lies in direction `d` from us. From the neighbor's
                // POV, we lie in the OPPOSITE direction. chiralFallback wants
                // "which direction is the neighbor relative to us?" which is
                // exactly `d`.
                nbAtom = chiralFallback(nti, d);
            }
            if (nbAtom < 0) continue;

            // Direction-continuity: link my OUT-end to neighbor's IN-end, or
            // vice versa. If both my in and out happen to be on this side
            // (impossible for valid endpoints, but defensive), we use whichever
            // matches.
            if (isCardinal(myOut, d))     linkOut(myAtom, nbAtom);
            else /* isCardinal(myIn, d)*/ linkOut(nbAtom, myAtom);
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 3b: diagonal-corner continuity for a single endpoint.
//
// `thisIdx` is the compass index of the endpoint (2/4/6/8 = NW/SW/SE/NE).
// We check the up-to-3 other tiles that touch that corner; on each, we try
// every atom-kind and accept the link iff:
//   - the neighbor endpoint's grid position matches ours
//   - the two endpoints are anti-parallel (sums 8 or 12 -> NW<->SE or NE<->SW)
//   - cross-axis links are only allowed when no same-axis partner exists on
//     either side (parallel-overlap suppression)
// We only link out -> in (path-continuity direction).
// ---------------------------------------------------------------------------
void PathGraphBuilder::wireDiagonalAt(int ti, int thisAtom, AtomKind thisKind,
                                      int thisIdx, bool thisIsOut) {
    if (!isDiagonal(thisIdx)) return;
    const RenderTile& t = tiles_[ti];
    auto [cdx, cdy] = cornerDelta(thisIdx);
    if (cdx < 0) return;
    int cgx = t.x + cdx * t.size;
    int cgy = t.y + cdy * t.size;
    // Corner on screen border: no off-grid partner.
    if (cgx <= 0 || cgy <= 0 || cgx >= width_ || cgy >= height_) return;
    auto myXY = endpointGridXY(t, thisIdx);
    int thisAxis = axisOf(thisKind);

    const AtomKind kinds[4] = { AK_V, AK_H, AK_C0, AK_C1 };
    for (int dy = -1; dy <= 0; dy++) {
        for (int dx = -1; dx <= 0; dx++) {
            int cx = cgx + dx, cy = cgy + dy;
            if (cx < 0 || cy < 0 || cx >= width_ || cy >= height_) continue;
            int nti = tileIdGrid_[(size_t)cy * (size_t)width_ + (size_t)cx];
            if (nti < 0 || nti == ti) continue;
            const RenderTile& nt = tiles_[nti];
            for (AtomKind k : kinds) {
                int nbAtom = atomIdOfKind(nti, k);
                if (nbAtom < 0) continue;
                auto [nbIn, nbOut] = atomEnds(nti, k);
                // Match my IN -> neighbor's OUT, and my OUT -> neighbor's IN.
                int nbIdx = thisIsOut ? nbIn : nbOut;
                if (!isDiagonal(nbIdx)) continue;
                if (endpointGridXY(nt, nbIdx) != myXY) continue;
                int sum = thisIdx + nbIdx;
                if (sum != 8 && sum != 12) continue;   // not anti-parallel
                int nbAxis = axisOf(k);
                if (nbAxis != thisAxis) {
                    bool sameOnNb = hasSameAxisAt(nti, myXY, nbIdx, thisAxis);
                    bool sameOnUs = hasSameAxisAt(ti,  myXY, thisIdx, nbAxis);
                    if (sameOnNb || sameOnUs) continue;  // prefer same-axis pair
                }
                if (thisIsOut) linkOut(thisAtom, nbAtom);
                else           linkOut(nbAtom, thisAtom);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 3: wire every atom's cardinal and diagonal neighbors.
// ---------------------------------------------------------------------------
void PathGraphBuilder::wireAllNeighbors() {
    const AtomKind allKinds[4] = { AK_V, AK_H, AK_C0, AK_C1 };
    for (int ti = 0; ti < nTiles_; ti++) {
        // Cardinal wiring: one pass per direction. wireCardinalSide internally
        // walks the two same-axis kinds (H+C0 for W/E, V+C1 for N/S).
        for (int d = 0; d < 4; d++) wireCardinalSide(ti, (Dir4)d);

        // Diagonal wiring: one pass per atom-kind that exists on this tile,
        // checking both endpoints.
        for (AtomKind k : allKinds) {
            int aId = atomIdOfKind(ti, k);
            if (aId < 0) continue;
            auto [inIdx, outIdx] = atomEnds(ti, k);
            wireDiagonalAt(ti, aId, k, inIdx,  /*thisIsOut=*/false);
            wireDiagonalAt(ti, aId, k, outIdx, /*thisIsOut=*/true );
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 4: BFS over atoms[] in row-major tile order to assign component IDs.
// ---------------------------------------------------------------------------
void PathGraphBuilder::assignComponents() {
    int nAtoms = (int)out_.atoms.size();
    int numComp = 0;
    std::queue<int> q;
    for (int ai = 0; ai < nAtoms; ai++) {
        if (out_.atoms[ai].component >= 0) continue;
        out_.atoms[ai].component = numComp;
        q.push(ai);
        while (!q.empty()) {
            int cur = q.front(); q.pop();
            Atom& a = out_.atoms[cur];
            auto visit = [&](int nb) {
                if (nb >= 0 && out_.atoms[nb].component < 0) {
                    out_.atoms[nb].component = numComp;
                    q.push(nb);
                }
            };
            for (int i = 0; i < a.inCount;  i++) visit(a.inNeighbors[i]);
            for (int i = 0; i < a.outCount; i++) visit(a.outNeighbors[i]);
        }
        numComp++;
    }
    out_.numComp = numComp;
}

// ---------------------------------------------------------------------------
// Stage 5: assign one hue per component, spaced so adjacent trees are
// distinguishable.  With few trees the minimum gap is large; it shrinks
// gracefully as numComp grows.  We try up to MAX_TRIES random candidates
// per slot and keep whichever maximises the minimum angular distance to
// already-assigned hues.
// ---------------------------------------------------------------------------
static float hueAngularDist(float a, float b) {
    float d = std::fabs(a - b);
    return d > 180.0f ? 360.0f - d : d;
}

void PathGraphBuilder::buildPalette() {
    int numComp = out_.numComp;
    std::mt19937_64 crng(pathColorSeed_);
    out_.compR.assign(numComp, 0.0f);
    out_.compG.assign(numComp, 0.0f);
    out_.compB.assign(numComp, 0.0f);

    // Minimum gap scales down as more trees need to fit in 360 degrees.
    // Floor at 15 degrees so it never becomes meaningless.
    float minGap = (numComp > 1) ? std::max(15.0f, 300.0f / (float)numComp) : 0.0f;
    constexpr int MAX_TRIES = 32;

    std::vector<float> assigned;
    assigned.reserve(numComp);

    for (int c = 0; c < numComp; c++) {
        float bestH   = (float)(crng() % 360000) / 1000.0f;
        float bestMin = -1.0f;

        for (int t = 0; t < MAX_TRIES; t++) {
            float h = (float)(crng() % 360000) / 1000.0f;

            // Compute minimum distance to all already-assigned hues.
            float minDist = 360.0f;
            for (float ah : assigned)
                minDist = std::min(minDist, hueAngularDist(h, ah));

            if (assigned.empty() || minDist > bestMin) {
                bestMin = minDist;
                bestH   = h;
                if (minDist >= minGap) break; // good enough, stop early
            }
        }

        assigned.push_back(bestH);
        hsvToRgb(bestH, 0.85f, 0.95f,
                 out_.compR[c], out_.compG[c], out_.compB[c]);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry point.
// ---------------------------------------------------------------------------
void buildPathGraph(const std::vector<RenderTile>& tiles,
                    int                            width,
                    int                            height,
                    const std::vector<int>&        tileIdGrid,
                    const SegmentEmitRanges&       segRanges,
                    const PathGraphCaches&         caches,
                    uint64_t                       pathColorSeed,
                    ConnectionBuild&               out) {
    PathGraphBuilder b(tiles, width, height, tileIdGrid, segRanges, caches,
                       pathColorSeed, out);
    b.run();
}

