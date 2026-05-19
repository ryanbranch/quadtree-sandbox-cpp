#pragma once

#include "quadtree.h"

#include <cstdint>
#include <vector>

class NoiseField;   // src/noise_field.h -- supplies per-tile chirality bits

// Whether to render flip-driven diagonal routing on size-1 tiles. Tiles that
// don't qualify (or aren't size-1) render with the standard straight/split
// paths regardless of their flip bits. The off-grid region beyond the screen
// edge is treated as same-size for qualifying purposes.
//
// The qualifying rule is a 2-tier system:
//   INTERIOR   = size-1 cell whose 3x3 neighbourhood is all size-1. Renders
//                full flip routing using both flip bits per path.
//   TRANSITION = size-1 cell that's not interior. Renders mixed cardinal /
//                diagonal endpoints -- diagonal only where (a) the flip bit
//                is set AND (b) the corner faces an interior cell.
// Larger (non-size-1) tiles are unaffected and always render standard paths.
enum FlipRule {
    FR_OFF = 0,   // no flip-driven routing
    FR_ON  = 1,   // 2-tier system (INTERIOR + TRANSITION) as above
};

// Tile fill-color strategy for the GUI.
enum ColorMode { CM_NONE = 0, CM_RANDOM = 1, CM_BY_SIZE = 2, CM_BY_RANK = 3 };

// Path (connection-line) color strategy.
//   PCM_AXIS      : black paths with cyan/magenta chiral arcs (original behavior)
//   PCM_COMPONENT : flood-fill connected components, each component a random hue
enum PathColorMode { PCM_AXIS = 0, PCM_COMPONENT = 1 };

// Build the full tile vertex buffer from exact tile rectangles: a filled quad
// per tile (color per the active mode) plus four 1-px white inner border strips.
// Vertex format is 6 floats: x, y, r, g, b, a.
//
// Color modes:
//   CM_NONE    : every tile black
//   CM_RANDOM  : every tile a random hue, deterministic from colorSeed
//   CM_BY_SIZE : hue derived from tile.size, smaller tiles redder, larger bluer
//   CM_BY_RANK : every tile the same hue, derived from globalHue
std::vector<float> buildGeometry(const std::vector<RenderTile>& tiles, int cellPx,
                                 ColorMode colorMode, uint64_t colorSeed,
                                 float globalHue, int maxTileSize,
                                 const class NoiseField* noise = nullptr);

// Append per-tile bit-indicator quads to `buf`. Intended to be appended to the
// connection-geometry buffer so indicators draw on top of path strokes.
void appendTileBitIndicators(std::vector<float>& buf,
                             const std::vector<RenderTile>& tiles, int cellPx,
                             const class NoiseField* noise);

// ---------------------------------------------------------------- path model
//
// The connection geometry is built in three stages:
//   1. SEGMENT EMISSION (this TU) -- produce a flat list of PathSegments
//      (constant-width LINE / ARC strokes) for every tile's H, V, or chiral
//      path. Renderer-agnostic: the GL tessellator consumes it, and a future
//      SVG exporter could emit each segment natively.
//   2. PATH GRAPH (quadtree_gui_path_graph.cpp) -- wrap each segment range in
//      an Atom, wire atoms across shared edges / corners, BFS for components,
//      assign per-component palette.
//   3. TESSELLATION (buildConnections, this TU) -- triangulate every segment,
//      walking atoms in BFS order so component coloring is contiguous.
//
// The Atom / ConnectionBuild types live in quadtree_gui_path_graph.h; this
// header only defines the segment-level types that both TUs need.

enum SegKind { SEG_LINE = 0, SEG_ARC = 1 };

// One constant-width stroke. For SEG_LINE: the centerline runs (ax,ay)->(bx,by).
// For SEG_ARC: the centerline is the arc of radius `radius` about (cx,cy),
// sweeping from angle `a0` to `a1` (radians; sign of (a1-a0) gives direction).
// `halfW` is the half-thickness; the stroke is the centerline extruded +/-halfW
// along its normal. r,g,b is the fill color.
struct PathSegment {
    SegKind kind;
    // LINE endpoints / ARC center.
    float ax, ay, bx, by;   // LINE: endpoints.   ARC: ax,ay = center (cx,cy).
    float radius, a0, a1;   // ARC only (centerline radius and sweep angles).
    float halfW;
    float r, g, b;
};

// Per-tile [begin, end) range into the flat PathSegment list. Used both by
// the segment-emission pass (to remember which tile owns which range) and by
// the path-graph pass (to attach each range to an Atom).
struct SegRange { size_t begin = 0, end = 0; };

struct ConnectionBuild;

// Audit/debug entry point: build the renderer-agnostic path model before final
// tessellation. This is used by parity tools to compare JS and C++ behavior
// without depending on OpenGL or screenshot diffs.
ConnectionBuild buildConnectionModel(const std::vector<RenderTile>& tiles,
                                     int width, int height,
                                     int cellPx, float linePxPerCell,
                                     const NoiseField* noise = nullptr,
                                     FlipRule flipRule = FR_OFF,
                                     uint64_t pathColorSeed = 0,
                                     bool noOuterFlip = false);

// Build + tessellate the path network in one call. Vertex format matches
// buildGeometry. This is the only public path entry point; the
// segment-emission + path-graph + tessellation pipeline is internal.
std::vector<float> buildConnections(const std::vector<RenderTile>& tiles,
                                    int width, int height,
                                    int cellPx, float linePxPerCell,
                                    const NoiseField* noise = nullptr,
                                    FlipRule flipRule = FR_OFF,
                                    PathColorMode pathColorMode = PCM_AXIS,
                                    uint64_t pathColorSeed = 0,
                                    bool noOuterFlip = false);
