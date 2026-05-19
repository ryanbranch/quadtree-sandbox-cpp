#include "quadtree_gui_geometry.h"
#include "quadtree_gui_path_graph.h"
#include "noise_field.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <random>
#include <utility>
#include <vector>

static const int BORDER_PX = 1;

// Convert HSV (h in [0, 360), s/v in [0, 1]) to RGB ([0, 1] each).
static void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
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

// Push one axis-aligned quad as two CCW triangles.
// Vertex format: x, y, r, g, b, a  (6 floats per vertex, 12 per triangle, 36 per quad).
static void pushQuad(std::vector<float>& buf,
                     float x0, float y0, float x1, float y1,
                     float r, float g, float b) {
    auto v = [&](float x, float y) {
        buf.push_back(x); buf.push_back(y);
        buf.push_back(r); buf.push_back(g); buf.push_back(b); buf.push_back(1.0f);
    };
    // Triangle 1: top-left, bottom-left, bottom-right
    v(x0, y0); v(x0, y1); v(x1, y1);
    // Triangle 2: top-left, bottom-right, top-right
    v(x0, y0); v(x1, y1); v(x1, y0);
}

std::vector<float> buildGeometry(const std::vector<RenderTile>& tiles, int cellPx,
                                 ColorMode colorMode, uint64_t colorSeed,
                                 float globalHue, int maxTileSize,
                                 const NoiseField* /*noise*/) {
    std::vector<float> buf;
    std::mt19937_64 colorRng(colorSeed);

    for (const RenderTile& tile : tiles) {
        int s = tile.size;
        float px0 = (float)(tile.x * cellPx);
        float py0 = (float)(tile.y * cellPx);
        float px1 = px0 + (float)(s * cellPx);
        float py1 = py0 + (float)(s * cellPx);
        float b   = (float)BORDER_PX;

        float fr = 0.0f, fg = 0.0f, fb = 0.0f;
        if (colorMode == CM_RANDOM) {
            float h = (float)(colorRng() % 360000) / 1000.0f; // [0, 360)
            hsvToRgb(h, 0.7f, 0.85f, fr, fg, fb);
        } else if (colorMode == CM_BY_SIZE) {
            // log2(size) / log2(maxTileSize) -> [0, 1], mapped onto hue [0, 300)
            // (avoid wrapping back to red at 360)
            float t = 0.0f;
            if (maxTileSize > 1)
                t = std::log2((float)s) / std::log2((float)maxTileSize);
            hsvToRgb(t * 300.0f, 0.7f, 0.85f, fr, fg, fb);
        } else if (colorMode == CM_BY_RANK) {
            hsvToRgb(globalHue, 0.7f, 0.85f, fr, fg, fb);
        }

        pushQuad(buf, px0, py0, px1, py1, fr, fg, fb);
        pushQuad(buf, px0, py0,     px1, py0 + b, 1.0f, 1.0f, 1.0f);
        pushQuad(buf, px0, py1 - b, px1, py1,     1.0f, 1.0f, 1.0f);
        pushQuad(buf, px0,     py0 + b, px0 + b, py1 - b, 1.0f, 1.0f, 1.0f);
        pushQuad(buf, px1 - b, py0 + b, px1,     py1 - b, 1.0f, 1.0f, 1.0f);
    }

    return buf;
}

// Append per-tile bit-indicator quads to an existing geometry buffer (intended
// to be the connection-geometry buffer so indicators draw ON TOP of path
// strokes). RGB encodes (hFlip, vFlip, chiral) sampled at each tile's UL cell.
//   black=none, red=H, green=V, blue=chiral, yellow=H+V,
//   magenta=H+chiral, cyan=V+chiral, white=all three.
//
// The indicator has TWO concentric borders so it stays visible against ANY
// tile fill color, including pure black (the bits=000 case under --color none,
// where a single black border on black would render the whole indicator
// invisible) and pure white. From outside in:
//   - 1px black ring (anchors against light/white tiles)
//   - 1px white ring (anchors against dark/black tiles)
//   - inner colored square (the actual RGB-encoded bits)
void appendTileBitIndicators(std::vector<float>& buf,
                             const std::vector<RenderTile>& tiles, int cellPx,
                             const NoiseField* noise) {
    if (!noise) return;
    const float b = (float)BORDER_PX;
    for (const RenderTile& tile : tiles) {
        if (tile.x < 0 || tile.y < 0 ||
            tile.x >= noise->width() || tile.y >= noise->height()) continue;
        float px0 = (float)(tile.x * cellPx);
        float py0 = (float)(tile.y * cellPx);
        TileBits bits = noise->sample(tile.x, tile.y);
        float ir = bits.hFlip  ? 1.0f : 0.0f;
        float ig = bits.vFlip  ? 1.0f : 0.0f;
        float ib = bits.chiral ? 1.0f : 0.0f;
        float sq = 0.25f * (float)cellPx;
        float sx0 = px0 + b + 2.0f;
        float sy0 = py0 + b + 2.0f;
        // Outer black ring (visible against light/white tile fills).
        pushQuad(buf, sx0 - 2.0f, sy0 - 2.0f, sx0 + sq + 2.0f, sy0 + sq + 2.0f,
                 0.0f, 0.0f, 0.0f);
        // Inner white ring (visible against dark/black tile fills, including
        // when the indicator's own RGB is 0,0,0).
        pushQuad(buf, sx0 - 1.0f, sy0 - 1.0f, sx0 + sq + 1.0f, sy0 + sq + 1.0f,
                 1.0f, 1.0f, 1.0f);
        // Colored interior (the actual hFlip/vFlip/chiral bits).
        pushQuad(buf, sx0, sy0, sx0 + sq, sy0 + sq, ir, ig, ib);
    }
}

// ---------------------------------------------------------------- tessellation
//
// Turn the symbolic PathSegment list into triangles. A LINE segment is one
// quad (centerline extruded +/-halfW). An ARC segment is sampled into a fan of
// short quads -- enough to keep the curve visually smooth at screen scale.
//
// Every path is drawn TWICE: first a slightly fatter white copy (the outline),
// then the normal-width colored copy on top. The result is a colored path with
// a white border. Doing both passes here -- all white, then all color -- keeps
// the layering correct even where neighboring paths overlap at junctions: the
// colored pass always lands on top of every white outline.
//
// PATH_OUTLINE_FRAC scales the white border relative to cellPx (the base cell
// size in pixels), so the border stays visually consistent as the grid zooms.
// 0.125 * 32px (default k=5 cell) = 4px, matching the old hard-coded constant.
static const float PATH_OUTLINE_FRAC = 0.0625f;

// Emit one tessellated segment (white outline if `outline`, else colored fill)
// into buf. Per-component tessellation (see buildConnections) calls this twice
// per component so each path's white+color is emitted as a unit -- which is
// what keeps both paths' white outlines visible at crossings between components.
static void emitSegInto(std::vector<float>& buf, const PathSegment& s,
                        bool outline, float outlinePx) {
    auto v = [&](float x, float y, float r, float g, float b) {
        buf.push_back(x); buf.push_back(y);
        buf.push_back(r); buf.push_back(g); buf.push_back(b); buf.push_back(1.0f);
    };
    auto quad = [&](float x0, float y0, float x1, float y1,
                    float x2, float y2, float x3, float y3,
                    float r, float g, float b) {
        v(x0, y0, r, g, b); v(x1, y1, r, g, b); v(x2, y2, r, g, b);
        v(x0, y0, r, g, b); v(x2, y2, r, g, b); v(x3, y3, r, g, b);
    };
    float halfW = outline ? s.halfW + outlinePx * 0.5f : s.halfW;
    float r = outline ? 1.0f : s.r;
    float g = outline ? 1.0f : s.g;
    float b = outline ? 1.0f : s.b;
    if (s.kind == SEG_LINE) {
        float dx = s.bx - s.ax, dy = s.by - s.ay;
        float len = std::sqrt(dx*dx + dy*dy);
        if (len < 1e-6f) return;
        float nx = -dy / len, ny = dx / len;
        float ox = nx * halfW, oy = ny * halfW;
        quad(s.ax + ox, s.ay + oy, s.ax - ox, s.ay - oy,
             s.bx - ox, s.by - oy, s.bx + ox, s.by + oy,
             r, g, b);
    } else { // SEG_ARC
        float cx = s.ax, cy = s.ay;
        float da = s.a1 - s.a0;
        const float VERTS_PER_2PI = 60.0f;
        int steps = (int)std::ceil(std::fabs(da) *
                                   (VERTS_PER_2PI / (2.0f * (float)M_PI)));
        if (steps < 1) steps = 1;
        float rIn  = s.radius - halfW;
        float rOut = s.radius + halfW;
        float prevAng = s.a0;
        for (int i = 1; i <= steps; i++) {
            float ang = s.a0 + da * ((float)i / (float)steps);
            float c0 = std::cos(prevAng), s0 = std::sin(prevAng);
            float c1 = std::cos(ang),     s1 = std::sin(ang);
            quad(cx + rOut*c0, cy + rOut*s0, cx + rIn*c0, cy + rIn*s0,
                 cx + rIn*c1,  cy + rIn*s1,  cx + rOut*c1, cy + rOut*s1,
                 r, g, b);
            prevAng = ang;
        }
    }
}



// Path model:
//
// Every tile carries one HORIZONTAL path and one VERTICAL path, each crossing
// the tile's center at the tile's natural width (size * linePxPerCell). A
// tile's path on a given axis is built as two independent halves (center ->
// each of the two relevant sides). Each half is one of:
//
//   * STRAIGHT  - the neighbor across that side is the same size (or this is
//     the smaller side of a 2:1, in which case the half is still just a
//     straight stub at this tile's own width). Emitted center -> edge.
//
//   * SPLIT WEDGE - this tile is the LARGER side of a 2:1 transition: its edge
//     is shared by two half-as-wide neighbors. The full-width path resolves
//     INSIDE this tile into two half-width sub-paths within the outer-quarter
//     wedge band. The full-width portion butts into a perpendicular connector
//     bar; two sub-lanes peel off the bar's ends and run to the edge. All four
//     corners of that T-junction are rounded with tangent circular-arc fillets
//     whose centerline radius equals the stroke thickness.
//
// The larger tile always owns the junction; the two small neighbors just emit
// ordinary straight stubs that meet the wedge's thin ends at the shared edge.
// Emit every tile's path segments and run the path-graph pass to produce a
// fully-populated ConnectionBuild. Internal callers go through buildConnections
// for tessellation; parity tools use buildConnectionModel to inspect this
// renderer-agnostic model directly.
static ConnectionBuild buildConnectionSegments(const std::vector<RenderTile>& tiles,
                                               int width, int height,
                                               int cellPx, float linePxPerCell,
                                               const NoiseField* noise,
                                               FlipRule flipRule,
                                               uint64_t pathColorSeed,
                                               bool noOuterFlip = false) {
    ConnectionBuild result;
    std::vector<PathSegment>& segs = result.segs;
    // Flat row-major tile-id grid: tileIdAt(x, y) = tile index covering that
    // cell, or -1 if off-grid / uncovered. Flat layout outperforms the previous
    // vector<vector<int>> which incurred a pointer chase per access.
    std::vector<int> tileIdGrid((size_t)width * (size_t)height, -1);
    auto tileIdAt = [&](int x, int y) -> int {
        return tileIdGrid[(size_t)y * (size_t)width + (size_t)x];
    };
    for (size_t i = 0; i < tiles.size(); i++) {
        const RenderTile& tile = tiles[i];
        int y1 = std::min(tile.y + tile.size, height);
        int x1 = std::min(tile.x + tile.size, width);
        int y0 = std::max(tile.y, 0);
        int x0 = std::max(tile.x, 0);
        for (int y = y0; y < y1; y++) {
            int* row = tileIdGrid.data() + (size_t)y * (size_t)width;
            for (int x = x0; x < x1; x++) row[x] = (int)i;
        }
    }

    // ---- pixel-space helpers -------------------------------------------
    auto px = [&](int gx) { return (float)(gx * cellPx); };

    // Emit a straight constant-width stroke (centerline a -> b).
    auto pushLine = [&](float ax, float ay, float bx, float by,
                        float halfW, float r, float g, float b) {
        PathSegment s;
        s.kind = SEG_LINE;
        s.ax = ax; s.ay = ay; s.bx = bx; s.by = by;
        s.radius = s.a0 = s.a1 = 0.0f;
        s.halfW = halfW; s.r = r; s.g = g; s.b = b;
        segs.push_back(s);
    };
    // Emit a circular-arc constant-width stroke: centerline arc of `radius`
    // about (cx,cy), swept from angle a0 to a1.
    auto pushArc = [&](float cx, float cy, float radius, float a0, float a1,
                       float halfW, float r, float g, float b) {
        PathSegment s;
        s.kind = SEG_ARC;
        s.ax = cx; s.ay = cy; s.bx = 0.0f; s.by = 0.0f;
        s.radius = radius; s.a0 = a0; s.a1 = a1;
        s.halfW = halfW; s.r = r; s.g = g; s.b = b;
        segs.push_back(s);
    };

    // Both axes render black; the white outlines around each stroke still
    // provide the per-axis "carve" at crossings (see buildConnections).
    const float hR = 0.0f, hG = 0.0f, hB = 0.0f;
    const float vR = 0.0f, vG = 0.0f, vB = 0.0f;

    // A tile side's path either runs straight to the edge, or RESOLVES into
    // two half-width sub-lanes within this tile.
    //
    // Key fact: BOTH neighbours across a resolving edge split, and their
    // sub-lanes meet STRAIGHT across the edge -- quadtree balancing guarantees
    // that this tile's sub-lanes (at center +/- s/4) line up with whatever is
    // on the other side, whether that is a smaller 2:1 tile or an offset
    // same-size partner. So there is no bending/jogging: a side simply either
    // stays full-width or splits into two straight sub-lanes. The split is
    // needed exactly when the edge faces two DIFFERENT neighbours (one per
    // half); a single neighbour spanning the whole edge stays full-width.
    //   SK_STRAIGHT : full-width straight to the edge.
    //   SK_BORDER   : outer screen edge; extend straight out.
    //   SK_RESOLVE  : the path splits into two straight half-width sub-lanes.
    enum SideKind { SK_STRAIGHT, SK_BORDER, SK_RESOLVE };

    // For tile `ti`, inspect one side. `axisH` true => horizontal path, so the
    // sides are LEFT/RIGHT (varying x); `posSide` true => the +x (right) or +y
    // (bottom) side.
    auto classifySide = [&](int ti, bool axisH, bool posSide) -> SideKind {
        const RenderTile& t = tiles[ti];
        int s = t.size;
        // Coordinate of the cell column/row just across the side.
        int edgeGrid;
        if (axisH) edgeGrid = posSide ? t.x + s : t.x;
        else       edgeGrid = posSide ? t.y + s : t.y;
        int nbCell = posSide ? edgeGrid : edgeGrid - 1; // cell across the side
        if (axisH) { if (nbCell < 0 || nbCell >= width)  return SK_BORDER; }
        else       { if (nbCell < 0 || nbCell >= height) return SK_BORDER; }
        // The neighbour covering each half of the edge.
        int lo = axisH ? t.y : t.x;
        auto nbAt = [&](int k) -> int {
            int gx, gy;
            if (axisH) { gx = nbCell; gy = lo + k; }
            else       { gx = lo + k; gy = nbCell; }
            if (gx < 0 || gy < 0 || gx >= width || gy >= height) return -1;
            int nb = tileIdAt(gx, gy);
            return (nb == ti) ? -1 : nb;
        };
        int loHalfNb = nbAt(0);          // -cross half
        int hiHalfNb = nbAt(s - 1);      // +cross half
        if (loHalfNb < 0 || hiHalfNb < 0)
            return SK_STRAIGHT;          // ragged / off-grid: leave straight
        // Two different neighbours, one per half => the path resolves into
        // two straight sub-lanes. One shared neighbour => full-width straight.
        return (loHalfNb != hiHalfNb) ? SK_RESOLVE : SK_STRAIGHT;
    };

    // Side geometry helpers ------------------------------------------------
    //
    // sideEdgeOut: returns (edgeC, outC) for a tile/side -- edge coord at the
    // tile boundary, plus the "extend out" coord at the screen edge.
    auto sideEdgeOut = [&](const RenderTile& t, bool axisH, bool posSide)
                       -> std::pair<float,float> {
        float edgeC, outC;
        if (axisH) {
            edgeC = posSide ? px(t.x + t.size) : px(t.x);
            outC  = posSide ? (float)(width  * cellPx) : 0.0f;
        } else {
            edgeC = posSide ? px(t.y + t.size) : px(t.y);
            outC  = posSide ? (float)(height * cellPx) : 0.0f;
        }
        return {edgeC, outC};
    };

    // Emit JUST the SK_RESOLVE split machinery (lane stubs + corner fillets +
    // connector bar) for a tile's side, in a given color. Does NOT emit the
    // full-width portion -- the caller is responsible for whatever connects
    // the tile center / chiral arc to the bar.
    // Returns aBar (the along-coord of the connector bar's centerline) so the
    // caller knows where to terminate its full-width portion / chiral arc.
    auto emitSplitMachinery = [&](int ti, bool axisH, bool posSide,
                                  float r, float g, float b) -> float {
        const RenderTile& t = tiles[ti];
        int s = t.size;
        float cx = px(t.x) + s * cellPx * 0.5f;
        float cy = px(t.y) + s * cellPx * 0.5f;
        float hw = s * linePxPerCell * 0.5f;
        float edgeC = sideEdgeOut(t, axisH, posSide).first;

        float aDir    = (edgeC > (axisH ? cx : cy)) ? 1.0f : -1.0f;
        float cCenter = axisH ? cy : cx;
        float laneOff = s * 0.25f * cellPx;
        float subHw   = hw * 0.5f;
        auto acXY = [&](float a, float c) -> std::pair<float,float> {
            return axisH ? std::make_pair(a, c) : std::make_pair(c, a);
        };
        float aLane = edgeC - aDir * hw;
        float aBar  = edgeC - aDir * 2.0f * hw;

        // Connector bar.
        {
            auto [x0, y0] = acXY(aBar, cCenter - (laneOff - hw));
            auto [x1, y1] = acXY(aBar, cCenter + (laneOff - hw));
            pushLine(x0, y0, x1, y1, subHw, r, g, b);
        }
        // Two sub-lanes + corner fillets.
        for (int lane = 0; lane < 2; lane++) {
            float cSign = (lane == 0) ? -1.0f : 1.0f;
            float cLane = cCenter + cSign * laneOff;
            // Lane stub.
            {
                auto [x0, y0] = acXY(aLane, cLane);
                auto [x1, y1] = acXY(edgeC, cLane);
                pushLine(x0, y0, x1, y1, subHw, r, g, b);
            }
            // Annular-arc fillet (centerline radius = hw).
            float arcCa = aLane;
            float arcCc = cLane - cSign * hw;
            auto [arcCx, arcCy]   = acXY(arcCa, arcCc);
            auto [barEx, barEy]   = acXY(aBar,  cLane - cSign * hw);
            auto [laneEx, laneEy] = acXY(aLane, cLane);
            float a0 = std::atan2(barEy  - arcCy, barEx  - arcCx);
            float a1 = std::atan2(laneEy - arcCy, laneEx - arcCx);
            while (a1 - a0 >  (float)M_PI) a1 -= 2.0f * (float)M_PI;
            while (a1 - a0 < -(float)M_PI) a1 += 2.0f * (float)M_PI;
            pushArc(arcCx, arcCy, hw, a0, a1, subHw, r, g, b);
        }
        return aBar;
    };

    // Emit one half of a tile's path on a given axis: from the tile center to
    // one side. In SK_RESOLVE, the split machinery is delegated to
    // emitSplitMachinery, and the full-width portion runs from tile center to
    // the bar.
    auto emitHalf = [&](int ti, bool axisH, bool posSide) {
        const RenderTile& t = tiles[ti];
        int s = t.size;
        float cx = px(t.x) + s * cellPx * 0.5f;
        float cy = px(t.y) + s * cellPx * 0.5f;
        float hw = s * linePxPerCell * 0.5f;
        SideKind kind = classifySide(ti, axisH, posSide);

        float r = axisH ? hR : vR;
        float g = axisH ? hG : vG;
        float b = axisH ? hB : vB;

        auto [edgeC, outC] = sideEdgeOut(t, axisH, posSide);

        if (kind == SK_STRAIGHT || kind == SK_BORDER) {
            float endC = (kind == SK_BORDER) ? outC : edgeC;
            if (axisH) pushLine(cx, cy, endC, cy, hw, r, g, b);
            else       pushLine(cx, cy, cx, endC, hw, r, g, b);
            return;
        }

        // SK_RESOLVE: emit the split machinery, then the full-width portion
        // from the tile center to the bar's along-coord (returned by the
        // helper). Pull the tip back by subHw (= hw/2) to the bar's inner
        // colored edge, so the full-width stroke's perpendicular white
        // outline strips don't extend along-axis into the bar's colored
        // body (which is wider perpendicularly than the full-width line
        // whenever 2*hw < laneOff). Otherwise those white strips appear
        // *inside* the bar's color, covering part of it.
        float aBar = emitSplitMachinery(ti, axisH, posSide, r, g, b);
        float aDir = (sideEdgeOut(t, axisH, posSide).first > (axisH ? cx : cy))
                     ? 1.0f : -1.0f;
        float aTip = aBar - aDir * hw * 0.5f;
        if (axisH) pushLine(cx, cy, aTip, cy, hw, r, g, b);
        else       pushLine(cx, cy, cx, aTip, hw, r, g, b);
    };

    // Chiral colors: NE arc and SW arc each get their own distinct hue, so they
    // are visually identifiable separately from the H/V (currently both black)
    // bodies.
    const float neR = 0.0f / 255.0f, neG = 191.0f / 255.0f, neB = 191.0f / 255.0f;  // cyan
    const float swR = 191.0f / 255.0f, swG = 0.0f / 255.0f, swB = 191.0f / 255.0f;  // magenta

    // ---- flip-driven diagonal routing (size-1 tiles only, phase 1) -------
    //
    // When a size-1 tile has any of its 8 corner-flip bits set, one or both
    // endpoints of its H or V path get diverted from a cardinal edge midpoint
    // to a diagonal corner. The path is then a single curve from its `in`
    // endpoint to its `out` endpoint -- straight when the two endpoints lie
    // on a common tangent line, otherwise a single circular arc tangent to
    // the outward direction at each endpoint. (Phase 1 ignores cross-tile
    // routing -- adjacent tiles' paths may not align across cell corners.)

    // Map a compass neighbour index (1=N, 2=NW, 3=W, 4=SW, 5=S, 6=SE, 7=E,
    // 8=NE) to its boundary position on a size-1 tile and the outward unit
    // tangent direction at that position.
    struct EndpointInfo { float px, py; float tx, ty; };
    auto endpointFor = [&](const RenderTile& t, int idx) -> EndpointInfo {
        float x0 = px(t.x), x1 = px(t.x + t.size);
        float y0 = px(t.y), y1 = px(t.y + t.size);
        float xm = 0.5f * (x0 + x1), ym = 0.5f * (y0 + y1);
        const float diag = 0.70710678f;   // 1/sqrt(2)
        switch (idx) {
            case 1: return { xm, y0,  0.0f, -1.0f       };  // N
            case 2: return { x0, y0, -diag, -diag       };  // NW
            case 3: return { x0, ym, -1.0f,  0.0f       };  // W
            case 4: return { x0, y1, -diag,  diag       };  // SW
            case 5: return { xm, y1,  0.0f,  1.0f       };  // S
            case 6: return { x1, y1,  diag,  diag       };  // SE
            case 7: return { x1, ym,  1.0f,  0.0f       };  // E
            case 8: return { x1, y0,  diag, -diag       };  // NE
            default: return { xm, ym, 0.0f, 0.0f };
        }
    };

    // Whether a tile's 4-corner-flip bits leave it in the all-cardinal default
    // (hIn=W=3, hOut=E=7, vIn=N=1, vOut=S=5). The standard non-flipped
    // straight-crossing case is rendered by the existing emitHalf path.
    auto isDefaultEndpoints = [](const PathEndpoints& e) -> bool {
        return e.hIn == 3 && e.hOut == 7 && e.vIn == 1 && e.vOut == 5;
    };

    // Emit an H or V path stroke for a size-1 flipped tile, using a
    // straight-leg + arc + straight-leg construction. This guarantees the
    // path enters tangent at P0 AND exits tangent at P1 in their respective
    // outward directions, by absorbing any geometric mismatch into the
    // straight legs.
    //
    // Algorithm: extend the path direction line forward from P0 and backward
    // from P1; they meet at the "miter" point M. Inscribe a circle of radius
    // R in the corner at M; the tangent points are T0 (on the P0-line) and
    // T1 (on the P1-line). The path is: P0 -> T0 (line), T0 -> T1 (arc),
    // T1 -> P1 (line). Straight cases (parallel directions) fall through to
    // a single pushLine.
    // Returns true if a diagonal endpoint (idx in {2,4,6,8}) on tile `t` sits
    // exactly on the screen boundary, meaning one edge of the stroke would
    // otherwise fall outside the visible area.
    // Returns true if a diagonal endpoint (idx in {2,4,6,8}) on tile `t` has
    // its corner touching any screen boundary — not just the image corners.
    // A NW endpoint at a left-edge tile or a top-edge tile both need extending,
    // since the stroke's outer edge would exit the visible area in that axis.
    auto diagEndpointOnScreenEdge = [&](const RenderTile& t, int idx) -> bool {
        switch (idx) {
            case 2: return t.x == 0               || t.y == 0;               // NW
            case 4: return t.x == 0               || t.y + t.size == height; // SW
            case 6: return t.x + t.size == width  || t.y + t.size == height; // SE
            case 8: return t.x + t.size == width  || t.y == 0;               // NE
            default: return false;
        }
    };

    auto emitFlipPath = [&](const RenderTile& t, int inIdx, int outIdx,
                            float halfW, float r, float g, float b,
                            bool resolveAtIn = false,
                            bool resolveAtOut = false) {
        EndpointInfo P0 = endpointFor(t, inIdx);
        EndpointInfo P1 = endpointFor(t, outIdx);
        // If a side resolves, the path's endpoint on that side terminates at
        // the bar's INNER COLORED edge (2*hw + subHw = 2.5*hw inward along the
        // path direction) instead of at the cell boundary. The split machinery
        // owns the band between there and the edge. The extra subHw pullback
        // (vs. ending at the bar centerline) keeps the full-width stroke's
        // perpendicular white outline strips from sitting inside the bar's
        // colored body and visibly covering part of it.
        float fullHw = (float)t.size * linePxPerCell * 0.5f;
        float resolvePullback = 2.5f * fullHw;
        if (resolveAtIn) {
            P0.px += resolvePullback * (-P0.tx);
            P0.py += resolvePullback * (-P0.ty);
        }
        if (resolveAtOut) {
            P1.px += resolvePullback * (-P1.tx);
            P1.py += resolvePullback * (-P1.ty);
        }
        // Diagonal endpoints on the screen boundary: extend outward by
        // halfW + outlinePx/2 so the FULL fattened stroke (centerline
        // half-thickness PLUS the white outline that sits half of
        // outlinePx further out on each side) reaches the screen edge.
        // Without the outline term the colored fill makes it but its white
        // border falls short of the edge by a couple of pixels.
        float outlinePx = PATH_OUTLINE_FRAC * (float)cellPx;
        float diagExt = halfW + outlinePx * 0.5f;
        if (!resolveAtIn  && diagEndpointOnScreenEdge(t, inIdx)) {
            P0.px += diagExt * P0.tx;
            P0.py += diagExt * P0.ty;
        }
        if (!resolveAtOut && diagEndpointOnScreenEdge(t, outIdx)) {
            P1.px += diagExt * P1.tx;
            P1.py += diagExt * P1.ty;
        }
        // Path directions: into the cell at P0, out of the cell at P1.
        float D0x = -P0.tx, D0y = -P0.ty;
        float D1x =  P1.tx, D1y =  P1.ty;

        // Solve P0 + s*D0 = P1 - u*D1 for (s, u). System: D0*s + D1*u = P1-P0.
        float det = D0x * D1y - D0y * D1x;
        if (std::fabs(det) < 1e-6f) {
            // Parallel directions: straight if collinear, skip otherwise.
            float dx = P1.px - P0.px, dy = P1.py - P0.py;
            float perp = dx * D0y - dy * D0x;
            if (std::fabs(perp) < 1e-3f) {
                pushLine(P0.px, P0.py, P1.px, P1.py, halfW, r, g, b);
            }
            return;
        }
        float rhsX = P1.px - P0.px, rhsY = P1.py - P0.py;
        float sParam = (rhsX * D1y - rhsY * D1x) / det;
        float uParam = (D0x * rhsY - D0y * rhsX) / det;
        if (sParam <= 0.0f || uParam <= 0.0f) {
            // Miter is on the wrong side (would require an S-curve). Skip.
            return;
        }
        // Turn angle: angle from D0 to D1, measured by their dot/cross.
        float cosT = D0x * D1x + D0y * D1y;
        float sinT = D0x * D1y - D0y * D1x;
        float theta = std::atan2(std::fabs(sinT), cosT);   // [0, pi]
        float halfT = theta * 0.5f;
        if (halfT < 1e-4f) {
            // Effectively straight after all.
            pushLine(P0.px, P0.py, P1.px, P1.py, halfW, r, g, b);
            return;
        }

        // Choose arc radius. Scales with tile size so larger tiles get
        // proportionally larger turns. The 0.5 coefficient (half the tile
        // side) was tuned visually; larger arcs feel more like swept curves
        // than corner fillets. Clamped so the tangent-point distance fits
        // both legs. Tangent distance from the miter corner to each tangent
        // point is R * tan(theta/2), where theta is the turn angle (the
        // corner's half-angle is (pi - theta) / 2, and tan of that is
        // cot(theta/2)).
        float R = (float)cellPx * (float)t.size * 0.5f;
        float tanDist = R * std::tan(halfT);
        float maxTanDist = std::min(sParam, uParam);
        if (tanDist > maxTanDist) {
            tanDist = maxTanDist;
            R = (std::tan(halfT) > 1e-6f) ? tanDist / std::tan(halfT) : R;
        }

        float T0x = P0.px + (sParam - tanDist) * D0x;
        float T0y = P0.py + (sParam - tanDist) * D0y;
        float T1x = P1.px - (uParam - tanDist) * D1x;
        float T1y = P1.py - (uParam - tanDist) * D1y;

        // Arc center: perpendicular to D0 at T0, offset by R on the inside of
        // the turn. Sign of sinT tells us the turn direction (left vs right).
        float turnSign = (sinT >= 0.0f) ? 1.0f : -1.0f;
        // Inward normal of D0 (90deg toward the turn side).
        float N0x = -D0y * turnSign;
        float N0y =  D0x * turnSign;
        float Cx = T0x + R * N0x;
        float Cy = T0y + R * N0y;

        // P0 -> T0 leg.
        if ((sParam - tanDist) > 1e-3f)
            pushLine(P0.px, P0.py, T0x, T0y, halfW, r, g, b);
        // Arc T0 -> T1, sweep = theta with sign = turnSign.
        float a0 = std::atan2(T0y - Cy, T0x - Cx);
        float a1 = std::atan2(T1y - Cy, T1x - Cx);
        float da = a1 - a0;
        if (turnSign > 0.0f) {
            while (da <= 0.0f)              da += 2.0f * (float)M_PI;
            while (da >  2.0f * (float)M_PI) da -= 2.0f * (float)M_PI;
        } else {
            while (da >= 0.0f)              da -= 2.0f * (float)M_PI;
            while (da < -2.0f * (float)M_PI) da += 2.0f * (float)M_PI;
        }
        pushArc(Cx, Cy, R, a0, a0 + da, halfW, r, g, b);
        // T1 -> P1 leg.
        if ((uParam - tanDist) > 1e-3f)
            pushLine(T1x, T1y, P1.px, P1.py, halfW, r, g, b);
    };

    // ---- flip tiering, generalized to any tile size --------------------
    //
    // The flip system was originally size-1-only; this generalizes it. The
    // STRICT-ALIGNMENT rule for "neighbour" means: a size-N tile T at
    // (tx, ty) has 8 neighbour slots at offsets (+/- N, +/- N) etc. Each
    // slot's tile (call it M) qualifies as a same-size neighbour iff M is
    // size N AND M's upper-left grid coord lands exactly on T's expected
    // slot position. Off-grid slots are treated as same-size (per the
    // outer-edge convention).
    //
    // Helper: does the cell at grid coord (x, y) belong to a size-N tile
    // anchored exactly at (x, y)? (I.e. the tile whose UL corner lands here.)
    auto tileOfSizeAt = [&](int x, int y, int N) -> bool {
        if (x < 0 || y < 0 || x >= width || y >= height) return true;  // off-grid OK
        int nti = tileIdAt(x, y);
        if (nti < 0) return true;
        const RenderTile& nt = tiles[nti];
        // Strict alignment: the tile must be size N AND its UL corner must
        // sit exactly at (x, y).
        return nt.size == N && nt.x == x && nt.y == y;
    };

    // Does a size-N tile at (tx, ty) have all 8 strict-alignment neighbours
    // of the same size?
    auto tileHasAll8SameSizeNeighbors = [&](int tx, int ty, int N) -> bool {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                if (!tileOfSizeAt(tx + dx * N, ty + dy * N, N)) return false;
            }
        return true;
    };

    // Tile classification (2-tier).
    //   FT_OUTER       : default rendering (no flip-driven routing).
    //   FT_TRANSITION  : tile whose 3x3 same-size neighbourhood is NOT
    //                    complete. Renders mixed cardinal/diagonal endpoints
    //                    based on which corners face interior tiles. This
    //                    naturally includes larger tiles that border smaller
    //                    INTERIOR blocks -- they don't have all-same-size
    //                    neighbours either, so they qualify here, and the
    //                    cornerFacesInterior check (which now considers
    //                    half-size interiors too) decides whether their flip
    //                    bits actually fire.
    //   FT_INTERIOR    : tile whose 3x3 same-size neighbourhood IS complete.
    //                    Renders full flip routing.
    //
    // Operates on a tile (referred to by its UL grid coord and size). For
    // off-grid or non-existent tile slots: not classifiable, returns OUTER.
    enum FlipTier { FT_OUTER = 0, FT_TRANSITION = 1, FT_INTERIOR = 2 };
    auto tileTier = [&](int tx, int ty, int N) -> FlipTier {
        if (!tileOfSizeAt(tx, ty, N)) return FT_OUTER;
        return tileHasAll8SameSizeNeighbors(tx, ty, N) ? FT_INTERIOR
                                                       : FT_TRANSITION;
    };

    // For a tile T (size N at (tx, ty)), does its corner at offset
    // (cornerDx, cornerDy in {0, 1}: 0=upper/left, 1=lower/right) face an
    // INTERIOR neighbour? The neighbour can be either:
    //   * A SAME-SIZE (N) interior tile -- the standard case for size-N
    //     transition cells in a same-size block.
    //   * A HALF-SIZE (N/2) interior tile -- enables larger tiles to act as
    //     transition cells for blocks of smaller (N/2) cells. Strict 2:1
    //     ratio only.
    //
    // For same-size: check the 3 strict-aligned same-size neighbour slots at
    // offsets (sx*N, sy*N), (sx*N, 0), (0, sy*N) where sx,sy = +/-1.
    // For half-size: check the 3 size-N/2 cells touching this corner that
    // are NOT inside T -- positions (tx + cornerDx*N + (sx-1)/2 * N/2, ...).
    //
    auto cornerFacesSameSizeInterior = [&](int tx, int ty, int N,
                                           int cornerDx, int cornerDy) -> bool {
        int sx = cornerDx * 2 - 1;
        int sy = cornerDy * 2 - 1;
        if (tileTier(tx + sx * N, ty + sy * N, N) == FT_INTERIOR) return true;
        if (tileTier(tx + sx * N, ty,          N) == FT_INTERIOR) return true;
        if (tileTier(tx,          ty + sy * N, N) == FT_INTERIOR) return true;
        return false;
    };
    // Relaxed-INTERIOR test: a size-`sz` cell at (cx, cy) qualifies as an
    // INTERIOR-equivalent for the half-size transition role even if some of
    // its 8 size-sz neighbours are blocked by being inside the larger tile T
    // at (tx, ty) size `tN`. Positions covered by T are treated as
    // "compatible" rather than disqualifying -- this is the relaxation that
    // makes the half-size code path actually trigger (a size-1 cell adjacent
    // to a size-2 tile can never be strictly INTERIOR, because one of its
    // neighbour positions is always inside the size-2 tile).
    auto isInsideTile = [&](int x, int y, int tx, int ty, int N) -> bool {
        return x >= tx && x < tx + N && y >= ty && y < ty + N;
    };
    auto cellRelaxedInteriorFor = [&](int cx, int cy, int sz,
                                      int tx, int ty, int tN) -> bool {
        // The cell itself must actually be a size-sz tile at (cx, cy).
        if (!tileOfSizeAt(cx, cy, sz)) return false;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                int nx = cx + dx * sz;
                int ny = cy + dy * sz;
                // Skip neighbour positions that fall inside the larger tile T.
                if (isInsideTile(nx, ny, tx, ty, tN)) continue;
                if (!tileOfSizeAt(nx, ny, sz)) return false;
            }
        return true;
    };

    auto cornerFacesHalfSizeInterior = [&](int tx, int ty, int N,
                                           int cornerDx, int cornerDy) -> bool {
        if (N < 2 || (N % 2 != 0)) return false;
        int half = N / 2;
        int cx = tx + cornerDx * N;
        int cy = ty + cornerDy * N;
        for (int dy = -1; dy <= 0; dy++)
            for (int dx = -1; dx <= 0; dx++) {
                int innerDx = -cornerDx;
                int innerDy = -cornerDy;
                if (dx == innerDx && dy == innerDy) continue;
                int nx = cx + dx * half;
                int ny = cy + dy * half;
                if (cellRelaxedInteriorFor(nx, ny, half, tx, ty, N)) return true;
            }
        return false;
    };
    // Per-tile decision: from THIS tile's local neighbourhood, does the given
    // corner face an interior? This is the original asymmetric rule -- kept as
    // a helper so the symmetric wrapper below can OR it across all tiles
    // touching the corner.
    auto cornerFacesInteriorLocal = [&](int tx, int ty, int N,
                                        int cornerDx, int cornerDy) -> bool {
        if (cornerFacesSameSizeInterior(tx, ty, N, cornerDx, cornerDy)) return true;
        if (cornerFacesHalfSizeInterior(tx, ty, N, cornerDx, cornerDy)) return true;
        return false;
    };
    // Symmetric corner-facing rule. A shared corner is "interior-facing" if
    // ANY of the SAME-SIZE tiles touching the corner would say yes under the
    // local rule. This guarantees that two same-size tiles meeting at a shared
    // corner reach the SAME verdict -- they then both honor (or both suppress)
    // the shared noise bit and their flip endpoints land on the same physical
    // point, avoiding the visible discontinuity that asymmetric verdicts
    // produced (e.g. one side demoting to a cardinal midpoint while the
    // neighbor pinned its endpoint to the corner).
    //
    // IMPORTANT: we restrict the OR to same-size tiles. Promoting "yes" from
    // a different-size neighbor breaks SK_RESOLVE split junctions: a small
    // tile next to a larger split-resolving tile must keep its endpoint
    // cardinal on the shared side so the larger tile's sub-lane has something
    // to meet. A larger tile may say "yes" at its corner via the half-size
    // path; if we propagated that to the small tile, the small tile would
    // divert its endpoint to the corner and leave the sub-lane dangling.
    //
    // Corner position in grid coords: (tx + cornerDx*N, ty + cornerDy*N).
    // The 4 cells touching that corner are at (cornerGx-1, cornerGy-1),
    // (cornerGx, cornerGy-1), (cornerGx-1, cornerGy), (cornerGx, cornerGy).
    auto cornerFacesInterior = [&](int tx, int ty, int N,
                                   int cornerDx, int cornerDy) -> bool {
        int cornerGx = tx + cornerDx * N;
        int cornerGy = ty + cornerDy * N;
        if (cornerFacesInteriorLocal(tx, ty, N, cornerDx, cornerDy)) return true;
        // Now OR across the other up-to-3 SAME-SIZE tiles touching the same corner.
        for (int dy = -1; dy <= 0; dy++) {
            for (int dx = -1; dx <= 0; dx++) {
                int cx = cornerGx + dx;
                int cy = cornerGy + dy;
                if (cx < 0 || cy < 0 || cx >= width || cy >= height) continue;
                int nti = tileIdAt(cx, cy);
                if (nti < 0) continue;
                const RenderTile& nt = tiles[nti];
                // Skip the calling tile (already handled above).
                if (nt.x == tx && nt.y == ty && nt.size == N) continue;
                // Same-size only: cross-size verdict propagation would break
                // SK_RESOLVE machinery (see header comment above).
                if (nt.size != N) continue;
                // Which of nt's four corners touches the shared point?
                int ndx = (cornerGx == nt.x)            ? 0
                        : (cornerGx == nt.x + nt.size)  ? 1
                                                        : -1;
                int ndy = (cornerGy == nt.y)            ? 0
                        : (cornerGy == nt.y + nt.size)  ? 1
                                                        : -1;
                if (ndx < 0 || ndy < 0) continue;  // corner doesn't land on nt's corner
                if (cornerFacesInteriorLocal(nt.x, nt.y, nt.size, ndx, ndy))
                    return true;
            }
        }
        return false;
    };


    // Compute path endpoints for a transition tile using the rule:
    //   diagonal iff flip bit set AND that corner faces an INTERIOR neighbour;
    //   otherwise cardinal (default direction).
    // Mirrors computeEndpoints()'s priority order (UL bit takes precedence
    // over LL for hIn, etc.).
    auto transitionEndpoints = [&](int tx, int ty, int N,
                                   const TileCornerBits& bits)
                               -> PathEndpoints {
        PathEndpoints e;
        // hIn: UL (corner 0,0) -> diagonal 2 (NW), LL (corner 0,1) -> diag 4 (SW), default 3 (W).
        e.hIn  = (bits.hFlip[0] && cornerFacesInterior(tx, ty, N, 0, 0)) ? 2
               : (bits.hFlip[2] && cornerFacesInterior(tx, ty, N, 0, 1)) ? 4
                                                                          : 3;
        // hOut: UR (1,0) -> 8 (NE), LR (1,1) -> 6 (SE), default 7 (E).
        e.hOut = (bits.hFlip[1] && cornerFacesInterior(tx, ty, N, 1, 0)) ? 8
               : (bits.hFlip[3] && cornerFacesInterior(tx, ty, N, 1, 1)) ? 6
                                                                          : 7;
        // vIn: UL -> 2 (NW), UR -> 8 (NE), default 1 (N).
        e.vIn  = (bits.vFlip[0] && cornerFacesInterior(tx, ty, N, 0, 0)) ? 2
               : (bits.vFlip[1] && cornerFacesInterior(tx, ty, N, 1, 0)) ? 8
                                                                          : 1;
        // vOut: LL -> 4 (SW), LR -> 6 (SE), default 5 (S).
        e.vOut = (bits.vFlip[2] && cornerFacesInterior(tx, ty, N, 0, 1)) ? 4
               : (bits.vFlip[3] && cornerFacesInterior(tx, ty, N, 1, 1)) ? 6
                                                                          : 5;
        return e;
    };

    // Helper: for a flipped tile, decide whether each side should keep its
    // SK_RESOLVE split machinery (and thus suppress the flip on that endpoint).
    // Also handles SK_BORDER (no resolve, no machinery, endpoint stays where
    // the flip puts it). Returns the side kind so the caller can emit the
    // split machinery on resolving sides itself.
    auto sideOverridesFlip = [&](int ti, bool axisH, bool posSide,
                                 PathEndpoints& e, float r, float g, float b) -> SideKind {
        SideKind k = classifySide(ti, axisH, posSide);
        if (k == SK_RESOLVE) {
            // Force the affected endpoint back to its cardinal default and
            // emit the standard split machinery so neighbours connect.
            if (axisH) {
                if (!posSide) e.hIn  = 3;   // W
                else          e.hOut = 7;   // E
            } else {
                if (!posSide) e.vIn  = 1;   // N
                else          e.vOut = 5;   // S
            }
            emitSplitMachinery(ti, axisH, posSide, r, g, b);
        }
        return k;
    };

    // ---- per-tile caches ------------------------------------------------
    // The original code recomputed noise->sampleCorners, tileTier and
    // effectiveEndpoints up to 6+ times per tile across the V/H/chiral
    // emission and the atom-linking passes. Precompute once.
    int nTiles = (int)tiles.size();
    bool noisePresent = (noise != nullptr);
    std::vector<unsigned char> noiseValid(nTiles, 0);
    std::vector<TileCornerBits> cornerBitsCache(nTiles);
    std::vector<PathEndpoints> endpointsCache(nTiles, {3, 7, 1, 5});
    std::vector<unsigned char> chiralCache(nTiles, 0);
    std::vector<unsigned char> qualifiesCache(nTiles, 0);
    for (int ti = 0; ti < nTiles; ti++) {
        const RenderTile& t = tiles[ti];
        if (noisePresent &&
            t.x >= 0 && t.y >= 0 &&
            t.x < noise->width() && t.y < noise->height()) {
            noiseValid[ti] = 1;
            cornerBitsCache[ti] = noise->sampleCorners(t.x, t.y, t.size);
            if (noOuterFlip) {
                TileCornerBits& cb = cornerBitsCache[ti];
                // corners: [0]=UL [1]=UR [2]=LL [3]=LR
                // Zero a corner if it touches any outer edge of the grid.
                bool onN = (t.y == 0);
                bool onS = (t.y + t.size == height);
                bool onW = (t.x == 0);
                bool onE = (t.x + t.size == width);
                if (onN || onW) { cb.hFlip[0] = cb.vFlip[0] = false; }
                if (onN || onE) { cb.hFlip[1] = cb.vFlip[1] = false; }
                if (onS || onW) { cb.hFlip[2] = cb.vFlip[2] = false; }
                if (onS || onE) { cb.hFlip[3] = cb.vFlip[3] = false; }
            }
        }
    }
    for (int ti = 0; ti < nTiles; ti++) {
        const RenderTile& t = tiles[ti];
        if (flipRule != FR_OFF) {
            FlipTier ft = tileTier(t.x, t.y, t.size);
            qualifiesCache[ti] = (ft == FT_INTERIOR || ft == FT_TRANSITION) ? 1 : 0;
        }
        if (noiseValid[ti]) {
            const TileCornerBits& corners = cornerBitsCache[ti];
            if (tileTier(t.x, t.y, t.size) == FT_TRANSITION)
                endpointsCache[ti] = transitionEndpoints(t.x, t.y, t.size, corners);
            else
                endpointsCache[ti] = computeEndpoints(corners);
            if (noise->sample(t.x, t.y).chiral && canBeChiral(corners))
                chiralCache[ti] = 1;
        }
    }

    auto tileQualifiesForFlips = [&](int ti) -> bool {
        return qualifiesCache[ti] != 0;
    };
    auto isChiral = [&](int ti) -> bool {
        return chiralCache[ti] != 0;
    };

    auto emitFlippedH = [&](int ti) -> bool {
        if (!tileQualifiesForFlips(ti) || !noiseValid[ti]) return false;
        const RenderTile& t = tiles[ti];
        PathEndpoints e = endpointsCache[ti];
        SideKind westKind = sideOverridesFlip(ti, true, false, e, hR, hG, hB);
        SideKind eastKind = sideOverridesFlip(ti, true, true,  e, hR, hG, hB);
        bool westResolves = (westKind == SK_RESOLVE);
        bool eastResolves = (eastKind == SK_RESOLVE);
        // If after overrides the path is fully default cardinal AND no side
        // resolves, fall through so the standard emitHalf renders it.
        if (isDefaultEndpoints(e) && !westResolves && !eastResolves) return false;
        float hw = t.size * linePxPerCell * 0.5f;
        emitFlipPath(t, e.hIn, e.hOut, hw, hR, hG, hB,
                     westResolves, eastResolves);
        return true;
    };
    auto emitFlippedV = [&](int ti) -> bool {
        if (!tileQualifiesForFlips(ti) || !noiseValid[ti]) return false;
        const RenderTile& t = tiles[ti];
        PathEndpoints e = endpointsCache[ti];
        SideKind northKind = sideOverridesFlip(ti, false, false, e, vR, vG, vB);
        SideKind southKind = sideOverridesFlip(ti, false, true,  e, vR, vG, vB);
        bool northResolves = (northKind == SK_RESOLVE);
        bool southResolves = (southKind == SK_RESOLVE);
        if (isDefaultEndpoints(e) && !northResolves && !southResolves) return false;
        float hw = t.size * linePxPerCell * 0.5f;
        emitFlipPath(t, e.vIn, e.vOut, hw, vR, vG, vB,
                     northResolves, southResolves);
        return true;
    };

    // ---- emit every tile's vertical path, then every tile's horizontal path,
    // then every chiral tile's two chiral arcs. The V/H grouping lets the
    // caller tessellate the two axes separately (white-V, color-V, white-H,
    // color-H draw order) -- so horizontal white outlines visibly carve
    // through vertical bodies at crossings. Chiral arcs land in a third group
    // (after horizontal), each colored distinctly.

    // Per-tile segment ranges for the connected-component coloring pass.
    // segRangeV[ti]  = [begin, end) in segs[] for tile ti's V-path (or fallback V).
    // segRangeH[ti]  = [begin, end) for tile ti's H-path (or fallback H).
    // segRangeC0[ti] = [begin, end) for chiral path 0 (cyan); empty if not chiral.
    // segRangeC1[ti] = [begin, end) for chiral path 1 (magenta); empty if not chiral.
    std::vector<SegRange> segRangeV(nTiles), segRangeH(nTiles),
                          segRangeC0(nTiles), segRangeC1(nTiles);

    for (int ti = 0; ti < nTiles; ti++) {
        segRangeV[ti].begin = segs.size();
        if (isChiral(ti)) {
            // chiral tiles emit nothing on the V pass
        } else if (!emitFlippedV(ti)) {
            emitHalf(ti, false, false);
            emitHalf(ti, false, true);
        }
        segRangeV[ti].end = segs.size();
    }
    for (int ti = 0; ti < nTiles; ti++) {
        segRangeH[ti].begin = segs.size();
        if (isChiral(ti)) {
            // chiral tiles emit nothing on the H pass
        } else if (!emitFlippedH(ti)) {
            emitHalf(ti, true,  false);
            emitHalf(ti, true,  true);
        }
        segRangeH[ti].end = segs.size();
    }
    // Chiral pass: emit the two chiral paths for every chiral tile.
    //
    // Chirality swaps hOut <-> vOut after the flip bits set the endpoints
    // (matching art-project genAtoms()). The resulting two paths are:
    //   path 0 (cyan):    hIn  -> hOut_swapped  (was vOut)
    //   path 1 (magenta): vIn  -> vOut_swapped  (was hOut)
    //
    // SK_RESOLVE side handling: after the swap the endpoints no longer align
    // with their original H/V axes, so we cannot blindly pass (axisH=true,
    // west/east) or (axisH=false, north/south) to sideOverridesFlip. Instead
    // we map each endpoint index to its physical side(s) and call classifySide
    // + emitSplitMachinery directly, forcing the endpoint back to its cardinal
    // default when a side resolves. Corner endpoints (NW/NE/SW/SE) touch two
    // sides; if either resolves the endpoint falls back to cardinal on that axis.
    //
    // Also re-validate canBeChiral against the tier-aware endpoints: a
    // TRANSITION tile may have had some flip bits suppressed, making the
    // post-transition endpoints safe (or not) differently from the raw bits.

    // For one endpoint of a chiral path: check the one side that this endpoint
    // owns (determined by which axis and direction applies after the chiral
    // swap), emit split machinery if it resolves, and return the (possibly
    // overridden) endpoint index and whether it resolved.
    //
    // After the hOut<->vOut swap the side ownership is:
    //   path 0 hIn  : W side  (axisH=true,  posSide=false, cardinal=3)
    //   path 0 hOut : S side  (axisH=false, posSide=true,  cardinal=5)
    //   path 1 vIn  : N side  (axisH=false, posSide=false, cardinal=1)
    //   path 1 vOut : E side  (axisH=true,  posSide=true,  cardinal=7)
    auto resolveChiralEndpoint = [&](int ti, int idx,
                                     bool axisH, bool posSide, int cardinalDef,
                                     float r, float g, float b)
                                 -> std::pair<int, bool> {
        SideKind k = classifySide(ti, axisH, posSide);
        if (k == SK_RESOLVE) {
            emitSplitMachinery(ti, axisH, posSide, r, g, b);
            return { cardinalDef, true };
        }
        return { idx, false };
    };

    for (int ti = 0; ti < (int)tiles.size(); ti++) {
        if (!isChiral(ti)) continue;
        const RenderTile& t = tiles[ti];
        if (!noiseValid[ti]) continue;
        // Tier-aware endpoints (cached). canBeChiral was already validated on
        // the raw corner bits in the cache pass.
        PathEndpoints e = endpointsCache[ti];

        // Fall back to standard V+H emission for tiles whose chiral state was
        // marked OK by canBeChiral but where the tier-aware endpoints would
        // produce a degenerate path. Called by both the post-tier downgrade
        // check and the post-swap U-turn check.
        auto fallbackToStandard = [&]() {
            segRangeV[ti].begin = segs.size();
            if (!emitFlippedV(ti)) {
                emitHalf(ti, false, false);
                emitHalf(ti, false, true);
            }
            segRangeV[ti].end = segs.size();
            segRangeH[ti].begin = segs.size();
            if (!emitFlippedH(ti)) {
                emitHalf(ti, true, false);
                emitHalf(ti, true, true);
            }
            segRangeH[ti].end = segs.size();
        };

        // canBeChiral checks pre-swap endpoints: hIn!=vOut and vIn!=hOut.
        // Use the tier-aware e rather than raw computeEndpoints so transition
        // tiles that had endpoints forced to cardinal are evaluated correctly.
        // IMPORTANT: if the downgrade fires here, isChiral() already returned
        // true (using raw computeEndpoints), so the tile was skipped in both
        // the V and H passes. We must fall back to emitting the standard paths
        // rather than skipping again -- otherwise the tile renders nothing.
        if ((e.hIn == e.vOut) || (e.vIn == e.hOut)) {
            fallbackToStandard();
            continue;
        }
        // Apply the chiral swap: hOut <-> vOut.
        std::swap(e.hOut, e.vOut);

        // Post-swap U-turn check: if either chiral path would have both
        // endpoints on the same edge face of the tile, the resulting arc is
        // non-connectable and we fall back to standard paths. Edge faces:
        //   path 0 (hIn->hOut): hIn on W-face {2,3,4}, hOut on S-face {4,5,6}.
        //     Shared face only when both lie on the S-face (hIn=4 AND hOut on S).
        //   path 1 (vIn->vOut): vIn on N-face {8,1,2}, vOut on E-face {6,7,8}.
        //     Shared face only when both lie on the E-face (vIn=8 AND vOut on E).
        auto onSFace = [](int idx) { return idx == 4 || idx == 5 || idx == 6; };
        auto onEFace = [](int idx) { return idx == 6 || idx == 7 || idx == 8; };
        if ((onSFace(e.hIn) && onSFace(e.hOut)) ||
            (onEFace(e.vIn) && onEFace(e.vOut))) {
            fallbackToStandard();
            continue;
        }

        float hw = t.size * linePxPerCell * 0.5f;

        // Path 0 (cyan): hIn -> hOut (hOut is the swapped vOut, exits south).
        //   hIn  owns W side (axisH=true,  posSide=false, cardinal=3=W)
        //   hOut owns S side (axisH=false, posSide=true,  cardinal=5=S)
        segRangeC0[ti].begin = segs.size();
        {
            auto [inIdx,  inResolved]  = resolveChiralEndpoint(ti, e.hIn,  true,  false, 3, neR, neG, neB);
            auto [outIdx, outResolved] = resolveChiralEndpoint(ti, e.hOut, false, true,  5, neR, neG, neB);
            emitFlipPath(t, inIdx, outIdx, hw, neR, neG, neB, inResolved, outResolved);
        }
        segRangeC0[ti].end = segs.size();
        // Path 1 (magenta): vIn -> vOut (vOut is the swapped hOut, exits east).
        //   vIn  owns N side (axisH=false, posSide=false, cardinal=1=N)
        //   vOut owns E side (axisH=true,  posSide=true,  cardinal=7=E)
        segRangeC1[ti].begin = segs.size();
        {
            auto [inIdx,  inResolved]  = resolveChiralEndpoint(ti, e.vIn,  false, false, 1, swR, swG, swB);
            auto [outIdx, outResolved] = resolveChiralEndpoint(ti, e.vOut, true,  true,  7, swR, swG, swB);
            emitFlipPath(t, inIdx, outIdx, hw, swR, swG, swB, inResolved, outResolved);
        }
        segRangeC1[ti].end = segs.size();
    }

    // ---- Hand off to the path-graph pass --------------------------------
    //
    // Segment emission above already produced everything the path-graph pass
    // needs: per-tile segment ranges (segRangeV/H/C0/C1), per-tile noise /
    // qualifies / endpoints / chiral caches, and the row-major tileIdGrid.
    // Package them up and delegate.
    SegmentEmitRanges segRanges;
    segRanges.v  = std::move(segRangeV);
    segRanges.h  = std::move(segRangeH);
    segRanges.c0 = std::move(segRangeC0);
    segRanges.c1 = std::move(segRangeC1);
    PathGraphCaches caches;
    caches.noiseValid     = std::move(noiseValid);
    caches.qualifiesCache = std::move(qualifiesCache);
    caches.chiralCache    = std::move(chiralCache);
    caches.endpointsCache = std::move(endpointsCache);
    buildPathGraph(tiles, width, height, tileIdGrid,
                   segRanges, caches, pathColorSeed, result);
    return result;
}


ConnectionBuild buildConnectionModel(const std::vector<RenderTile>& tiles,
                                     int width, int height,
                                     int cellPx, float linePxPerCell,
                                     const NoiseField* noise,
                                     FlipRule flipRule,
                                     uint64_t pathColorSeed,
                                     bool noOuterFlip) {
    return buildConnectionSegments(tiles, width, height, cellPx, linePxPerCell,
                                   noise, flipRule, pathColorSeed, noOuterFlip);
}


// Convenience: build + tessellate in one call.
//
// Tessellation is two global passes: ALL white outlines first (every
// component), then ALL color fills (every component). This ensures every white
// outline sits on top of every color fill, so path borders stay visible
// wherever two differently-colored components cross each other.
std::vector<float> buildConnections(const std::vector<RenderTile>& tiles,
                                    int width, int height,
                                    int cellPx, float linePxPerCell,
                                    const NoiseField* noise,
                                    FlipRule flipRule,
                                    PathColorMode pathColorMode,
                                    uint64_t pathColorSeed,
                                    bool noOuterFlip) {
    ConnectionBuild cb = buildConnectionSegments(tiles, width, height, cellPx, linePxPerCell,
                                                 noise, flipRule, pathColorSeed, noOuterFlip);

    float outlinePx = PATH_OUTLINE_FRAC * (float)cellPx;

    // BFS over atoms, following connectivity, so each segment is drawn
    // immediately after its neighbors — giving white outlines the best chance
    // of landing on top of adjacent segments' colors. Every segment in cb.segs
    // is owned by exactly one atom's SegRange, so iterating atoms covers them
    // all.
    int nAtoms = (int)cb.atoms.size();
    std::vector<bool> visited(nAtoms, false);

    std::vector<float> result;

    // One BFS pass per connected component (each component has its own root).
    // Within a component, expand each atom's neighbors in queue order. Reuse
    // a single deque-backed queue across roots to avoid per-component
    // allocator churn.
    std::queue<int> q;
    for (int root = 0; root < nAtoms; root++) {
        if (visited[root]) continue;
        visited[root] = true;
        q.push(root);
        while (!q.empty()) {
            int ai = q.front(); q.pop();
            const Atom& atom = cb.atoms[ai];
            int c = atom.component;

            // Emit this atom's segments interleaved (white then color each).
            for (size_t si = atom.segs.begin; si < atom.segs.end; si++) {
                PathSegment s = cb.segs[si];
                if (pathColorMode == PCM_COMPONENT && c >= 0 && c < cb.numComp) {
                    s.r = cb.compR[c]; s.g = cb.compG[c]; s.b = cb.compB[c];
                }
                emitSegInto(result, s, true,  outlinePx);   // white outline
                emitSegInto(result, s, false, outlinePx);   // colored fill
            }

            // Enqueue all unvisited neighbors (treating edges as undirected).
            auto enqueue = [&](int ni) {
                if (ni < 0 || ni >= nAtoms || visited[ni]) return;
                visited[ni] = true;
                q.push(ni);
            };
            for (int k = 0; k < atom.outCount; k++) enqueue(atom.outNeighbors[k]);
            for (int k = 0; k < atom.inCount;  k++) enqueue(atom.inNeighbors[k]);
        }
    }
    return result;
}
