#pragma once

#include "quadtree.h"
#include "quadtree_gui_config.h"
#include "quadtree_gui_geometry.h"
#include "noise_field.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <vector>

// Owns the GLFW window, the GL resources, and all interactive viewer state.
// Construct from a parsed GuiConfig, then call run() to enter the event loop.
// The constructor throws std::runtime_error if init or the first tiling load
// fails; the destructor tears down all GL/GLFW resources.
class GuiViewer {
  public:
    explicit GuiViewer(const GuiConfig& cfg);
    ~GuiViewer();

    GuiViewer(const GuiViewer&) = delete;
    GuiViewer& operator=(const GuiViewer&) = delete;

    // Run the interactive event loop until the window is closed.
    void run();

  private:
    // ---- tiling load / stats ----
    u256 randIndex(u256 total);
    float rankToHue(const u256& num, const u256& den) const;
    void recomputeTileStats();
    void loadSingle();   // populate `tiles` from a single-quadtree rank
    void loadMulti();    // populate `tiles` from a generated multi-quadtree

    // ---- GPU buffer (re)builds ----
    void uploadTileGeometry();        // (re)fill the tile VBO from `tiles`
    void uploadConnectionGeometry();  // (re)fill the line VBO from `tiles`
    void reloadTree();                // regenerate tiling + both VBOs + redraw

    // ---- rendering ----
    void renderScene(float screenW, float screenH, float zoomOverride);
    void draw();                      // render to the window + swap buffers
    void updateTitle();

    // ---- export ----
    void saveStandardPNG();
    void saveHighResPNG(float scale);
    void startRecording();
    void stopRecording();
    void saveStateJSON(const std::string& basename); // console + sidecar JSON of all reproducible state

    // ============================ state ============================
    const GuiConfig& cfg;
    std::mt19937_64  rng;

    // ---- grid / window geometry (fixed once at construction) ----
    int   gridW = 0, gridH = 0;
    int   cellPx = 1;
    float linePxPerCell = 0.0f;
    int   windowW = 0, windowH = 0;
    int   recordW = 0, recordH = 0; // even-rounded dimensions for ffmpeg

    // ---- tiling state ----
    std::unique_ptr<QuadtreeIndex> idx;
    std::map<int, QuadtreeIndex>   multiIdx; // per-k indexes for multi mode
    u256 total = 0;
    u256 n = 0;
    MultiQuadtreeTiling currentMulti;
    std::vector<RenderTile> tiles;
    // Mirrors cfg.haveIndex on construction; cleared to false after the first
    // Shift-regenerate so subsequent reloads pick random indices instead of
    // replaying the same --index value.
    bool haveIndex = false;
    int  maxTileSize = 1;
    uint64_t colorSeed = 0;
    float globalHue = 0.0f;

    // FNV-driven noise field providing per-tile chirality / hFlip / vFlip bits.
    // Rebuilt whenever the grid dimensions change. Null until first build.
    std::unique_ptr<NoiseField> noise;

    // Chunk coordinates used to seed the noise field. Tab randomizes these so
    // chirality/hFlip/vFlip are regenerated from a completely different position.
    int noiseChunkX = 0;
    int noiseChunkY = 0;

    // Active flip-routing rule: toggled live via the F key together with chiralEnabled.
    FlipRule flipRule = FR_ON;

    // Whether chirality is enabled; toggled in sync with flipRule via the F key.
    bool chiralEnabled = true;

    // Path coloring mode: cycled via the C key (axis colors -> component colors).
    PathColorMode pathColorMode = PCM_AXIS;
    uint64_t pathColorSeed = 0;

    // When true, per-tile bit-indicator squares (hFlip/vFlip/chiral) are drawn
    // in the upper-left corner of each leaf tile. Toggled via the D key.
    bool showBitIndicators = false;

    // When true, hFlip and vFlip are forced to 0 for tiles on the south and east
    // borders of the grid. Toggled via the O key.
    bool noOuterFlip = false;

    // ---- GLFW / GL resources ----
    GLFWwindow* window = nullptr;
    GLuint prog = 0;
    GLuint vao = 0, vbo = 0, lineVbo = 0;
    // CPU-side mirrors of the VBO contents; kept as members to avoid
    // reallocating on every upload (capacity is reused across reloads).
    std::vector<float> buf, lineBuf;
    GLsizei vertCount = 0, lineVertCount = 0;

    // ---- camera ----
    float viewCX = 0.0f, viewCY = 0.0f;
    float zoom = 1.0f;

    // ---- movement mode ----
    // smoothMode=true  : zoom/pan respond to held keys every frame, scaled by speedMultiplier
    // smoothMode=false : zoom/pan fire once per key press (edge-triggered)
    bool  smoothMode = true;
    float speedMultiplier = 0.1f;   // matches art-project-src default

    // ---- display toggles ----
    bool showTiles = true;
    bool showPaths = true;

    // ---- recording ----
    bool  recording = false;
    FILE* ffmpegPipe = nullptr;
    int   recordFrameCount = 0;
    std::chrono::high_resolution_clock::time_point lastRecordFrame;
};
