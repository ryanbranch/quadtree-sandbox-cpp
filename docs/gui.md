# GUI Viewer (`quadtree_gui`)

A real-time OpenGL window that renders a balanced quadtree tiling together with a center-crossing **path graph** through every tile. Interactive controls let you zoom, pan, switch tile and path coloring modes, toggle chirality-driven path routing, and export PNG snapshots or MP4 recordings.

## Building

```bash
make quadtree_gui
```

Requires GLEW, GLFW3, libpng, and (only for MP4 recording) ffmpeg. The link line is `-lGL -lGLEW -lglfw -lpng`.

## Usage

```bash
# Single quadtree
./quadtree_gui [--k K] [--index N] [--color MODE] [--resolution N] [--print-grid]

# Multi-quadtree tiling
./quadtree_gui --multi M N [--layout FILE] [--seed S]
               [--attempts N | --max-attempts N | --no-limit]
               [--greedy-cover] [--outer-edge-1x1] [--skip-1x1-precomputation]
               [--first-rank N [--first-k K]]
               [--color MODE] [--resolution N] [--print-grid]
```

### Common flags

| Flag | Default | Description |
|------|---------|-------------|
| `--k K` | `5` | Grid size: renders a 2^K × 2^K tiling (e.g. k=5 → 32×32) |
| `--index N` | random | Rank index of the specific tree to render |
| `--color MODE` | `none` | Tile fill color mode (see [Tile color modes](#tile-color-modes)) |
| `--resolution N` | `2.0` | Scale factor for backslash-key high-res PNG export |
| `--print-grid` | off | Print the depth grid to stdout on load |

### Multi-quadtree flags

| Flag | Default | Description |
|------|---------|-------------|
| `--multi M N` | — | Tile an M×N rectangle with multiple quadtree roots |
| `--layout FILE` | — | Load exact root layout from a file instead of generating one. Each line is `x y k`; the first line may additionally end with a rank `r` to pin that root to a specific tree |
| `--seed S` | random | RNG seed for cover generation and tree sampling |
| `--attempts N` | `128` | Stop after N failed tree-assignment attempts. `--max-attempts N` is an alias |
| `--no-limit` | off | Keep retrying tree assignment indefinitely (Ctrl-C to abort). Equivalent to an unbounded `--attempts` |
| `--greedy-cover` | off | Use the deterministic largest-fits-first cover instead of the random cover. Ignored if `--layout` is also given |
| `--outer-edge-1x1` | off | Force all outer-perimeter cells to be 1×1 tiles. Requires the edge-1×1 precompute cache (see `quadtree --precompute-edge-1x1`) unless `--skip-1x1-precomputation` is also given |
| `--skip-1x1-precomputation` | off | Skip the cache check; fall back to on-the-fly constraint evaluation (much slower) |
| `--first-rank N` | — | Pin the *first* root's tree to rank N. Requires `--layout` or `--first-k K` |
| `--first-k K` | — | When `--first-rank N` is set without `--layout`, place a single root of size 2ᴷ at the origin and fill the rest randomly |

### Examples

```bash
./quadtree_gui                              # random tree, k=5
./quadtree_gui --k 3                        # random tree, k=3 (8×8 grid)
./quadtree_gui --index 42                   # tree #42, k=5
./quadtree_gui --k 4 --index 1000           # tree #1000, k=4
./quadtree_gui --color by-size              # color tiles by size
./quadtree_gui --multi 12 8 --color random  # random-colored 12×8 multi-tiling
./quadtree_gui --multi 16 12 --greedy-cover # deterministic largest-first cover
./quadtree_gui --multi 8 8 --first-rank 0 --first-k 2   # pin a k=2 root at origin to rank 0
./quadtree_gui --resolution 4               # backslash exports 4× resolution PNG
```

## Window sizing

The window dimensions match the grid's aspect ratio. For square grids (all single-quadtree modes) this produces a square window. For non-square multi-quadtree grids the window is `(gridW × cellPx) × (gridH × cellPx)` pixels, so the long axis is approximately `WINDOW_SIZE` (1024) pixels and the short axis scales proportionally.

## Tile color modes

Tile fill color is controlled by `--color MODE` at launch and can be re-randomized at runtime with `R`. Path stroke color is controlled separately — see [Path color modes](#path-color-modes).

| Mode | Description |
|------|-------------|
| `none` | All tiles black (default) |
| `random` | Each tile a random hue, deterministic from `colorSeed`; press R to re-seed |
| `by-size` | Hue derived from `log2(tile.size) / log2(maxTileSize)`, mapped to [0°, 300°) — smaller tiles red, larger tiles blue |
| `by-rank` | All tiles the same hue derived from the current tree's rank as a fraction of total |

All non-black modes use HSV saturation 0.7 and value 0.85.

## Keyboard controls

### Navigation

| Key | Action |
|-----|--------|
| Arrow keys | Pan the view |
| `=` | Zoom in |
| `-` | Zoom out |
| `1` | Toggle smooth / discrete movement mode |
| `[` | Halve movement speed (÷√2) |
| `]` | Double movement speed (×√2) |

**Smooth mode** (default): zoom and pan respond every frame while a key is held, scaled by the speed multiplier. **Discrete mode**: each key press fires exactly one step regardless of hold duration.

### Rendering toggles

| Key | Action |
|-----|--------|
| `T` | Toggle tile rendering on/off |
| `P` | Toggle path rendering on/off |
| `R` | Reseed random tile colors (meaningful for `--color random`) |
| `C` | Cycle path color mode: axis colors ↔ connected-component colors (also reseeds the component palette) |
| `F` | Toggle flip-driven diagonal routing and chirality on/off together (see [Paths, chirality, and flips](#paths-chirality-and-flips)) |
| `D` | Toggle per-tile bit-indicator squares showing each tile's hFlip / vFlip / chiral bits in its upper-left corner |
| `Tab` | Jump to a fresh position in the noise field — regenerates chirality and flip bits while keeping the tiling itself unchanged |

### Export

| Key | Action |
|-----|--------|
| `Enter` | Save a standard-resolution PNG to `outputs/` |
| `\` (backslash) | Save a high-resolution PNG at `--resolution` scale (default 2×) to `outputs/` |
| `Space` | Toggle MP4 recording (60 fps, libx264, written to `outputs/`) |

PNG filenames are timestamped: `YYYYMMDDHHmmssMMM-quadtree.png`. MP4 filenames use the same format.

The high-resolution PNG renders into an offscreen FBO at the scaled resolution, then restores the on-screen framebuffer. The zoom and pan state is preserved — the export captures exactly what is on screen, at higher pixel density.

MP4 recording pipes raw RGBA frames to ffmpeg (`libx264`, CRF 18, `vflip` to correct OpenGL's bottom-up pixel order). Pressing Space again stops the recording and closes the ffmpeg pipe.

### Other

| Key | Action |
|-----|--------|
| `Shift` | Regenerate a new random quadtree (left or right Shift) |
| `Escape` | Close the window |

## Window title

The window title updates in real time and shows, separated by ` | `:

- Mode — for single mode: `k=K | #rank | WxH`; for multi mode: `multi WxH | roots=N | seed=S`
- Current zoom level (e.g. `zoom=1.25x`)
- Movement mode and speed multiplier (e.g. `smooth 0.1x` or `discrete 0.2x`)
- Flip/chirality state (`flips+chiral=on` or `off`)
- Path color mode (`paths=axis` or `paths=component`)
- `[bits]` tag while the `D` bit-indicator overlay is on
- `[RECORDING]` tag while MP4 capture is active

## What is rendered

**Tiles** — each leaf node of the quadtree is drawn as a filled square (color per the active tile color mode) with a 1 px white border on the inside of each edge. Larger tiles correspond to shallower nodes; 1×1 cells are the deepest leaves. All tiles together partition the full grid with no gaps or overlaps.

**Paths** — every tile carries one **horizontal** path and one **vertical** path, each crossing the tile's center at a stroke width proportional to the tile's size. Paths from adjacent tiles meet at the shared edge so the union forms a continuous network across the whole grid. Each stroke is drawn as a colored centerline with a white outline, so crossings between perpendicular paths show a clean white "carve" rather than a muddied junction. See [Paths, chirality, and flips](#paths-chirality-and-flips) for the full geometry rules.

### Path geometry at 2:1 transitions

When a tile's neighbor across one edge is half its size (a 2:1 transition), the larger tile owns the junction: its full-width path **splits** inside the tile into two half-width sub-lanes that meet a perpendicular connector bar, with all four corners rounded by tangent circular-arc fillets. The two small neighbors on the other side of the edge simply emit ordinary straight stubs that meet the sub-lane ends — quadtree balancing guarantees the sub-lanes always line up with whatever is on the other side.

When both neighbors across an edge are the same size as the tile, the path stays full-width and straight all the way through.

## Path color modes

Path stroke color is controlled by `C` at runtime (it cycles between two modes):

| Mode | Description |
|------|-------------|
| `axis` (default) | Both axes draw in black. The white outlines around each stroke handle the per-axis "carve" at crossings. Chiral arcs (when enabled — see below) override the axis color with cyan (W→S) and magenta (N→E) hues so the two chiral routes are visually distinguishable |
| `component` | The path network is flood-filled into connected components; each component is assigned a random hue and every stroke that belongs to it inherits that hue. The component palette reseeds every time you press `C` |

### How components are detected: atoms, continuity, coloring

The `component` color mode is the user-visible face of an internal three-stage pipeline that turns the painted strokes into a connectivity graph:

1. **Segment emission** (`quadtree_gui_geometry.cpp`) — for every tile, emit a flat list of `PathSegment`s (constant-width LINE / ARC strokes) for its H path, V path, or — if chiral — its two arcs. The per-tile `[begin, end)` ranges into that flat list are remembered.

2. **Path graph** (`quadtree_gui_path_graph.cpp`) — wrap each non-empty range in an **Atom**. An atom is the unit of path connectivity: one path-piece inside one tile, with a "natural in" end (W for H, N for V, arc-entry for C0/C1) and a "natural out" end (E / S / arc-exit). Each tile contributes up to two atoms — the standard `AK_V` + `AK_H` pair, or for chiral tiles the swapped `AK_C0` (cyan, W→S) + `AK_C1` (magenta, N→E) pair, or any mix when a chiral tile falls back to standard paths on one axis.

   Atoms are then wired together by walking the tile grid:

   - **Cardinal continuity** — for each tile and each of its four cardinal sides, look at every neighbor cell along that shared edge. If our atom has an endpoint on this side and the neighbor has the matching endpoint on the same side, link them. A directional rule (link "out" → "in") preserves path orientation. Edges are deduplicated so wiring the same boundary from both sides is harmless.

   - **Diagonal continuity** — for diagonal endpoints (the corner endpoints produced by `hFlip` / `vFlip`), the matching partner sits at the shared corner. The two endpoints must be anti-parallel (NW↔SE or NE↔SW) to count as a straight-line continuation through the corner. A same-axis preference (V↔V, H↔H) prevents parallel-overlapping strokes through a shared corner from cross-linking.

3. **Components and palette** — a BFS over the atom graph assigns a component ID to every atom. One random hue is then sampled per component (HSV saturation 0.85, value 0.95). In `component` color mode, every stroke inherits its atom's component hue; in `axis` mode the components are still computed but the colors are ignored.

`SK_RESOLVE` (the 2:1 split-junction case) gets special handling: when a tile's side resolves into split sub-lanes, the segment-emission pass forces the endpoint on that side back to its cardinal default and emits the split machinery. The path-graph pass mirrors that override so the wired endpoints match what's physically on the screen.

## Paths, chirality, and flips

Beyond the basic "one H + one V path per tile" model, the GUI supports an optional **flip-and-chirality** routing system, toggled by the `F` key (on by default at launch).

When the system is on, three bits sampled from a deterministic noise field (see [Noise field](#noise-field)) decide how each tile's two paths are routed:

- **chiral** — when set on a qualifying size-1 tile, the tile's horizontal and vertical paths are *swapped* into two big quarter-circle arcs:
  - **C0** (cyan) — an arc from the W edge to the S edge
  - **C1** (magenta) — an arc from the N edge to the E edge

  Both axes are diverted into arcs that bend through opposite corners instead of crossing through the tile's center.

- **hFlip / vFlip** — when set, divert one endpoint of one path to an adjacent diagonal corner of the tile, producing the "transition" routing described below. These are sampled at every cell corner (not just the upper-left), so the four corners of a single tile see four independent flip bits.

### Qualifying rule (the 2-tier system)

Only size-1 tiles participate in the flip/chirality system. Larger tiles always render plain straight/split paths. Within the size-1 tiles, there are two tiers:

- **INTERIOR** — a size-1 cell whose full 3×3 neighborhood is also size-1. Renders the *full* flip routing using all eight flip bits across its four corners.
- **TRANSITION** — a size-1 cell that is not interior (it has at least one non-size-1 neighbor or is on the grid border). Renders a *mixed* routing: cardinal endpoints by default, with diagonal endpoints **only** where (a) the flip bit at that corner is set AND (b) the corner faces an interior cell.

The off-grid region beyond the screen edge is treated as same-size for qualifying purposes, so border 1×1 tiles can still be interior.

### Bit indicators (`D` key)

Pressing `D` draws a small RGB-encoded square in the upper-left corner of every tile, showing the three bits sampled at the tile's UL cell:

| Color | Bits |
|-------|------|
| Black | none |
| Red | hFlip only |
| Green | vFlip only |
| Blue | chiral only |
| Yellow | hFlip + vFlip |
| Magenta | hFlip + chiral |
| Cyan | vFlip + chiral |
| White | all three |

The indicator has two concentric rings — outer black, inner white — so it stays visible against any tile fill color, including pure black (the `--color none` default) and pure white.

## Noise field

The flip and chirality bits are not stored per-tile; they are sampled on demand from a `NoiseField` (`src/noise_field.h` / `.cpp`) that is regenerated whenever the grid dimensions change or `Tab` is pressed.

The field ports an **FNV-1a hash + no-consecutive-1s transform** from a sibling art project. The FNV is computed over the tuple `(z, UNIVERSE_Z_CONSTANT)` with z ∈ {0, 1, 2}, seeded by a per-cell hash of `(chunkX, chunkY, x, y)`. For each grid vertex it produces:

- **chiral** (z = 0) — raw FNV bool, used as-is
- **hFlip** (z = 1) — FNV bool, run through the no-consecutive-1s digit transform across the whole grid
- **vFlip** (z = 2) — same as hFlip but on the transposed axis

Two chunk coordinates `(chunkX, chunkY)` feed the FNV hash as offsets into the field. At launch they are both `0` (reproducing the art project's chunk `(0, 0)` behaviour). Each `Tab` press picks a new random `(chunkX, chunkY)` and rebuilds the field — the tiling itself stays exactly the same, but every flip/chiral bit is freshly resampled, so the path routing changes wholesale.

Unlike the source art project (which tiles fixed 2×2-cell chunks for seamless infinite-universe rendering), the GUI treats the entire `width × height` grid as a single chunk — there is no cross-grid boundary stitching.

## Source file map

The GUI is split into eight translation units:

| File | Contents |
|------|----------|
| `quadtree_gui.cpp` | Entry point: calls `parseArgs` → constructs `GuiViewer` → calls `run()` |
| `quadtree_gui_config.h/.cpp` | `GuiConfig` struct + `parseArgs`; all command-line flag parsing |
| `quadtree_gui_viewer.h/.cpp` | `GuiViewer` class: GLFW/GL init, tiling load, noise field, camera, event loop, export |
| `quadtree_gui_geometry.h/.cpp` | `buildGeometry` (tile VBO), segment emission + `buildConnections` tessellation (path VBO), `appendTileBitIndicators`; `ColorMode`, `PathColorMode`, `FlipRule` enums; the `PathSegment` / `SegRange` segment-level types |
| `quadtree_gui_path_graph.h/.cpp` | `buildPathGraph`: turns the per-tile segment ranges produced by `quadtree_gui_geometry.cpp` into Atoms, wires path continuity across shared edges and diagonal corners, BFS-floods connected components, and assigns the per-component palette. Owns the `Atom` / `AtomKind` / `ConnectionBuild` types |
| `quadtree_gui_shaders.h/.cpp` | GLSL vertex/fragment shader sources; `compileShader` and `buildProgram` |
| `quadtree_gui_png.h/.cpp` | `savePNG`: libpng write with Y-flip for OpenGL's bottom-up pixel layout |
| `noise_field.h/.cpp` | `NoiseField` class plus the chirality-downgrade rule (`canBeChiral`) and endpoint mapping (`computeEndpoints`) |

## The `GuiViewer` class

`GuiViewer` owns everything that lives for the duration of the window: the GLFW window handle, the compiled shader program, the VAO and two VBOs (tiles and connection-paths), per-k `QuadtreeIndex` objects, the active `NoiseField`, camera state, recording state, and the current tiling. It is constructed from an immutable `GuiConfig` and has a single public method, `run()`, which enters the GLFW event loop.

### Construction and teardown

The constructor initializes GLFW and OpenGL, computes window and cell dimensions from the config, generates the initial tiling (via `loadSingle` or `loadMulti`), uploads both VBOs, and draws the first frame. If any step throws, GLFW is cleaned up before propagating. The destructor stops any active recording, deletes all GL objects, and terminates GLFW.

### Tiling state

Two load paths populate the `tiles` vector (a flat list of `RenderTile`):

- **`loadSingle()`** — constructs or reuses a `QuadtreeIndex` for the configured k, picks a rank (from `--index` or randomly), calls `unrank`, wraps the result in a single-root `MultiQuadtreeTiling`, and calls `render_tiles`.
- **`loadMulti()`** — calls `generate_multi_quadtree` with the configured width/height/layout/seed, validates the result, and calls `render_tiles`.

After either load, `recomputeTileStats()` scans the tile list to find `maxTileSize` and computes `globalHue` from the current rank fraction (used by the `CM_BY_RANK` color mode).

### GPU buffers

Two VBOs are kept in sync with the tile list:

- **`vbo`** (tiles): rebuilt by `uploadTileGeometry()` via `buildGeometry`. Rebuilt on every `reloadTree()` and on every `R` keypress (re-color without regenerating the tiling).
- **`lineVbo`** (paths): rebuilt by `uploadConnectionGeometry()` via `buildConnections` (plus `appendTileBitIndicators` when `D` is on). Rebuilt on `reloadTree()` *and* on any input that changes path appearance: `Tab` (new noise sample), `F` (toggle flip/chirality), `C` (toggle path color mode), `D` (toggle bit indicators).

### Rendering

`renderScene(screenW, screenH, zoomOverride)` is the core draw call: it binds the shader program, sets the five view uniforms (center, zoom, screen dimensions), then draws whichever of the two VBOs are currently enabled by `showTiles`/`showPaths`. Because `renderScene` takes the framebuffer dimensions and zoom as parameters, it can be called identically for the on-screen framebuffer (`draw()`) and for an offscreen FBO at a different resolution (`saveHighResPNG`).

### Event loop

`run()` uses `glfwPollEvents` (not `glfwWaitEvents`) so that smooth-mode zoom/pan and MP4 frame capture can fire on every loop iteration. Each key is tracked with an edge-triggered `wasDown` flag so toggles and one-shot actions (PNG save, speed change) fire exactly once per press. Zoom and pan in smooth mode scale by `ZOOM_STEP^speedMultiplier` per frame; in discrete mode they fire `ZOOM_STEP` once per press. A `dirty` flag defers the `draw()` call to once per frame even if multiple keys fire in the same iteration.
