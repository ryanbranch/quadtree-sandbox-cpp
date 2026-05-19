#pragma once

// =============================================================================
//  Path graph: atoms, continuity detection, connected components, palette
// =============================================================================
//
// The GUI's path system is built in two stages:
//
//   1. SEGMENT EMISSION (quadtree_gui_geometry.cpp) — for every tile, emit a
//      flat list of PathSegments (line / arc strokes) describing its H, V, or
//      chiral path geometry. Per-tile [begin, end) ranges into that flat list
//      are recorded so the second stage can rediscover which segments belong
//      to which tile and which "kind" (V, H, C0, C1).
//
//   2. PATH GRAPH (this TU) — wrap each non-empty segment range in an Atom,
//      detect path continuity by walking the tile grid and matching endpoints
//      across shared edges and shared diagonal corners, BFS the resulting
//      undirected graph to assign component IDs, then assign one random hue
//      per component for the PCM_COMPONENT color mode.
//
// Splitting the path graph out of geometry.cpp keeps each TU focused: geometry
// owns "tile -> pixels", this TU owns "pixels -> connectivity + color".

#include "noise_field.h"
#include "quadtree.h"
#include "quadtree_gui_geometry.h"

#include <cstdint>
#include <vector>

// Which of the (up to) four path-pieces a tile can contain.
//   AK_V : vertical path (or fallback V for chiral-downgraded tiles)
//   AK_H : horizontal path (or fallback H)
//   AK_C0: chiral path-0 (cyan arc, W -> S after the hOut<->vOut swap)
//   AK_C1: chiral path-1 (magenta arc, N -> E after the swap)
enum AtomKind { AK_V = 0, AK_H = 1, AK_C0 = 2, AK_C1 = 3 };

// One path-piece inside one tile.
//
// An Atom is the primary connectivity unit. Each tile contributes up to two
// atoms (V+H for normal tiles, C0+C1 for chiral tiles, or any mix when a tile
// fell back from chiral to standard). Two atoms that share a physical endpoint
// are linked via inNeighbors / outNeighbors so traversal never needs to
// re-derive geometry.
//
// Direction convention:
//   "in"  end = path's natural entry: N for V, W for H, arc-entry for C0/C1.
//   "out" end = path's natural exit:  S for V, E for H, arc-exit for C0/C1.
struct Atom {
    int       tileIdx;                              // index into tiles[]
    AtomKind  kind;
    SegRange  segs;                                 // slice of ConnectionBuild::segs
    static constexpr int kMaxNeighbors = 8;
    int       inNeighbors[kMaxNeighbors];           // atom indices, -1 = none
    int       outNeighbors[kMaxNeighbors];
    int       inCount;
    int       outCount;
    int       component;                            // assigned by the BFS
};

// Output of the segment-emission + path-graph pipeline. See header comment.
//
// `segs` is the flat PathSegment list. `atoms` is the explicit connectivity
// graph: each Atom owns a SegRange into segs and links to its in/out neighbors
// by index into atoms[]. The component-color palette is compR/G/B[c].
struct ConnectionBuild {
    std::vector<PathSegment> segs;
    std::vector<Atom>        atoms;

    // Per-tile atom-index lookup. atomV[ti], atomH[ti], atomC0[ti], atomC1[ti]
    // are indices into atoms[], or -1 if that tile has no atom of that kind.
    std::vector<int> atomV, atomH, atomC0, atomC1;

    int                numComp = 0;
    std::vector<float> compR, compG, compB;
};

// Per-tile segment ranges produced by the segment-emission pass. Indexed by
// tile id (parallel to `tiles`). An empty range (begin == end) means the tile
// has no atom of that kind.
struct SegmentEmitRanges {
    std::vector<SegRange> v, h, c0, c1;
};

// Per-tile caches that the segment-emission pass already produced and that the
// path-graph pass needs. Indexed by tile id.
//
// `noiseValid[ti]` is 1 iff the tile is on the noise grid (so endpointsCache
// is meaningful).
// `qualifiesCache[ti]` is 1 iff the tile qualifies for the flip system at its
// current FlipRule.
// `endpointsCache[ti]` holds the tier-aware (transition-adjusted) endpoints
// for tile `ti`, or the cardinal default {3, 7, 1, 5} for non-noise tiles.
// `chiralCache[ti]` is 1 iff the tile emitted chiral arcs (C0 + C1) rather
// than V/H -- but note that chiral tiles may fall back to V/H later, so
// `chiralCache` alone is not the source of truth for "does this tile have C0".
// The atom indices in `atomC0[ti] >= 0` are.
struct PathGraphCaches {
    std::vector<unsigned char> noiseValid;
    std::vector<unsigned char> qualifiesCache;
    std::vector<unsigned char> chiralCache;
    std::vector<PathEndpoints> endpointsCache;
};

// Build the path graph (atoms + components + palette) for an already-emitted
// segment list. Writes its results into `out` (which the caller has already
// populated with `out.segs`). The segment-emission stage in geometry.cpp owns
// the segment list and the per-tile range vectors; this function turns those
// into Atoms, wires neighbors via cardinal and diagonal endpoint matching,
// flood-fills components, and assigns the per-component palette.
//
// `tileIdGrid` is the flat row-major grid that maps each cell to its owning
// tile id (or -1). It's built once during segment emission and reused here so
// classifySide() and neighbor lookups don't have to rebuild it.
void buildPathGraph(const std::vector<RenderTile>& tiles,
                    int                            width,
                    int                            height,
                    const std::vector<int>&        tileIdGrid,
                    const SegmentEmitRanges&       segRanges,
                    const PathGraphCaches&         caches,
                    uint64_t                       pathColorSeed,
                    ConnectionBuild&               out);
