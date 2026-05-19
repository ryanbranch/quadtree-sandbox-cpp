#include "quadtree_gui_viewer.h"

#include "quadtree_gui_shaders.h"
#include "quadtree_gui_png.h"
#include "quadtree_gui_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

const int   WINDOW_SIZE      = 1024;
const float LINE_PX_CONSTANT = 128.0f; // base / 2^k = line total-width in px per source-tile grid unit
const float ZOOM_STEP        = 1.41421356237f; // sqrt(2): one geometric step per discrete key press
const float SPEED_STEP       = 1.41421356237f; // sqrt(2): geometric step for [ and ] speed adjustment
const int   RECORD_FPS       = 60;

constexpr GLsizei STRIDE = 6 * sizeof(float);

// Build a "YYYYMMDDhhmmssMMM" timestamp string (same format as art-project-src).
std::string timestampString() {
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char tsbuf[16];
    std::strftime(tsbuf, sizeof(tsbuf), "%Y%m%d%H%M%S", std::localtime(&t));
    char full[24];
    std::snprintf(full, sizeof(full), "%s%03lld", tsbuf, (long long)ms.count());
    return std::string(full);
}

// Ensure the outputs/ directory exists and return its path.
std::filesystem::path outputsDir() {
    namespace fs = std::filesystem;
    fs::path out = fs::current_path() / "outputs";
    fs::create_directories(out);
    return out;
}

} // namespace

GuiViewer::GuiViewer(const GuiConfig& cfg_)
    : cfg(cfg_), rng(cfg_.seed), haveIndex(cfg_.haveIndex),
      noOuterFlip(cfg_.noOuterFlip) {
    // ---- grid / window geometry ----
    gridW = cfg.multiMode ? cfg.multiW : (1 << cfg.k);
    gridH = cfg.multiMode ? cfg.multiH : (1 << cfg.k);
    // FNV noise field for per-tile chirality / hFlip / vFlip bits.
    noise = std::make_unique<NoiseField>(gridW, gridH, noiseChunkX, noiseChunkY);
    const int maxDim = std::max(gridW, gridH);
    // cellPx: how many screen pixels represent one grid cell.
    // Derived so the long axis of the window is ~WINDOW_SIZE pixels.
    cellPx = std::max(1, WINDOW_SIZE / maxDim);
    linePxPerCell = LINE_PX_CONSTANT / (float)maxDim;
    // Window matches grid aspect ratio: long axis = cellPx * maxDim (~WINDOW_SIZE
    // after integer truncation), short axis scales by minDim/maxDim. For square
    // grids (all single-quadtree modes) this collapses to a square window.
    windowW = gridW * cellPx;
    windowH = gridH * cellPx;
    // libx264 requires even dimensions; round the recording size down to even.
    recordW = windowW - (windowW & 1);
    recordH = windowH - (windowH & 1);

    colorSeed = rng();
    pathColorSeed = rng();

    // ---- GLFW / OpenGL init ----
    if (!glfwInit()) throw std::runtime_error("glfwInit failed");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window = glfwCreateWindow(windowW, windowH, "Quadtree Viewer", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        throw std::runtime_error("glfwCreateWindow failed");
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        glfwDestroyWindow(window);
        glfwTerminate();
        throw std::runtime_error("glewInit failed");
    }

    glViewport(0, 0, windowW, windowH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glfwSwapBuffers(window);
    glfwPollEvents();

    // ---- first tiling load ----
    // Done before GPU resource allocation so a bad config (invalid k, missing
    // cache, etc.) fails fast without leaking GL objects.
    try {
        if (cfg.multiMode) loadMulti();
        else               loadSingle();
    } catch (...) {
        glfwDestroyWindow(window);
        glfwTerminate();
        throw;
    }

    // ---- GPU resources ----
    prog = buildProgram();

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glGenBuffers(1, &vbo);
    glGenBuffers(1, &lineVbo);
    uploadTileGeometry();
    uploadConnectionGeometry();

    // ---- camera ----
    viewCX = (float)(gridW * cellPx) / 2.0f;
    viewCY = (float)(gridH * cellPx) / 2.0f;
    zoom   = 1.0f;

    updateTitle();
    draw();
}

GuiViewer::~GuiViewer() {
    if (recording) stopRecording();
    if (vbo)     glDeleteBuffers(1, &vbo);
    if (lineVbo) glDeleteBuffers(1, &lineVbo);
    if (vao)     glDeleteVertexArrays(1, &vao);
    if (prog)    glDeleteProgram(prog);
    if (window)  glfwDestroyWindow(window);
    glfwTerminate();
}

// Build a uniform u256 from 4 mt19937_64 calls, reduced mod `total`.
u256 GuiViewer::randIndex(u256 total) {
    u256 r;
    r.limbs[0] = rng();
    r.limbs[1] = rng();
    r.limbs[2] = rng();
    r.limbs[3] = rng();
    return r % total;
}

// Convert a u256 rank fraction to [0, 360) hue. Works by dividing numerator
// and denominator by a common power of 2 until both fit in u64, then doing a
// double-precision divide. Loses precision but gives a smooth visual.
float GuiViewer::rankToHue(const u256& num, const u256& den) const {
    if (den.is_zero()) return 0.0f;
    u256 a = num, b = den;
    while (!(b.limbs[1] == 0 && b.limbs[2] == 0 && b.limbs[3] == 0)) {
        b.limbs[0] = (b.limbs[0] >> 1) | (b.limbs[1] << 63);
        b.limbs[1] = (b.limbs[1] >> 1) | (b.limbs[2] << 63);
        b.limbs[2] = (b.limbs[2] >> 1) | (b.limbs[3] << 63);
        b.limbs[3] = (b.limbs[3] >> 1);
        a.limbs[0] = (a.limbs[0] >> 1) | (a.limbs[1] << 63);
        a.limbs[1] = (a.limbs[1] >> 1) | (a.limbs[2] << 63);
        a.limbs[2] = (a.limbs[2] >> 1) | (a.limbs[3] << 63);
        a.limbs[3] = (a.limbs[3] >> 1);
    }
    double dn = (double)a.limbs[0]
              + (double)a.limbs[1] * 18446744073709551616.0
              + (double)a.limbs[2] * 18446744073709551616.0 * 18446744073709551616.0;
    double dd = (double)b.limbs[0];
    if (dd <= 0.0) return 0.0f;
    double frac = dn / dd;
    if (frac < 0.0) frac = 0.0;
    if (frac >= 1.0) frac = std::fmod(frac, 1.0);
    return (float)(frac * 360.0);
}

void GuiViewer::recomputeTileStats() {
    maxTileSize = 1;
    for (const RenderTile& t : tiles)
        if (t.size > maxTileSize) maxTileSize = t.size;
    if (!cfg.multiMode)
        globalHue = rankToHue(n, total);
    // In multi mode, globalHue is set by loadMulti() before this call.
}

void GuiViewer::loadSingle() {
    if (!idx) idx = std::make_unique<QuadtreeIndex>(cfg.k);
    total = idx->total();
    if (haveIndex) {
        if (cfg.userIndex >= total)
            throw std::runtime_error("--index out of range");
        n = cfg.userIndex;
    } else {
        n = randIndex(total);
    }
    auto tree = idx->unrank(n);
    MultiQuadtreeTiling wrapper;
    wrapper.width = gridW;
    wrapper.height = gridH;
    wrapper.roots.push_back({{0, 0, cfg.k}, tree});
    tiles = render_tiles(wrapper);
    recomputeTileStats();
    std::cout << "Rendering quadtree #" << u256_to_string(n)
              << " (k=" << cfg.k << ", " << gridW << "x" << gridH << " grid)\n"
              << std::flush;
    if (cfg.printGrid) print_grid(render_grid(*tree, cfg.k));
}

void GuiViewer::loadMulti() {
    // rng() advances the RNG for the seed passed to generate_multi_quadtree so
    // that Shift-regenerate produces a different tiling each time.
    std::string err;
    bool unlimited = (cfg.attempts < 0);
    auto progress = [unlimited](int attempt) {
        std::cout << "\r    attempt " << attempt
                  << (unlimited ? " (no limit, Ctrl-C to abort)..." : "...")
                  << "    " << std::flush;
    };
    RootSpec seedRootSpec{0, 0, cfg.firstK};
    currentMulti = generate_multi_quadtree(gridW, gridH, cfg.layoutSpecs, rng(),
        cfg.attempts, &err, cfg.hasFirstRank ? &cfg.firstRank : nullptr,
        cfg.outerEdge1x1, cfg.skip1x1Precomputation, cfg.greedyCover, progress,
        cfg.hasFirstK ? &seedRootSpec : nullptr);
    std::cout << "\r" << std::string(60, ' ') << "\r" << std::flush;
    std::vector<std::string> errors;
    bool ok = validate_multi_quadtree(currentMulti, &errors);
    if (cfg.outerEdge1x1)
        ok = validate_outer_edges_1x1(currentMulti, &errors) && ok;
    if (!ok) {
        std::ostringstream os;
        os << "generated invalid multi-quadtree";
        if (!errors.empty()) os << ": " << errors.front();
        throw std::runtime_error(os.str());
    }
    tiles = render_tiles(currentMulti);
    // Compute globalHue as the circular mean of each root's by-rank hue.
    {
        double sinSum = 0.0, cosSum = 0.0;
        for (const PlacedQuadtreeRoot& root : currentMulti.roots) {
            int k = root.spec.k;
            multiIdx.try_emplace(k, k);
            QuadtreeIndex& kidx = multiIdx.at(k);
            u256 rootRank = kidx.rank(*root.tree);
            float hue = rankToHue(rootRank, kidx.total());
            double rad = hue * M_PI / 180.0;
            sinSum += std::sin(rad);
            cosSum += std::cos(rad);
        }
        double meanRad = std::atan2(sinSum, cosSum);
        if (meanRad < 0.0) meanRad += 2.0 * M_PI;
        globalHue = (float)(meanRad * 180.0 / M_PI);
    }
    n = 0;
    recomputeTileStats();
    std::cout << "Rendering multi-quadtree "
              << gridW << "x" << gridH
              << " roots=" << currentMulti.roots.size()
              << " seed=" << cfg.seed << "\n" << std::flush;
    if (cfg.printGrid) print_grid(render_grid(currentMulti));
}

void GuiViewer::uploadTileGeometry() {
    buf = buildGeometry(tiles, cellPx, cfg.colorMode, colorSeed, globalHue, maxTileSize);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(buf.size() * sizeof(float)),
                 buf.data(), GL_DYNAMIC_DRAW);
    vertCount = (GLsizei)(buf.size() / 6);
}

void GuiViewer::uploadConnectionGeometry() {
    lineBuf = buildConnections(tiles, gridW, gridH, cellPx, linePxPerCell,
                                chiralEnabled ? noise.get() : nullptr,
                                flipRule, pathColorMode, pathColorSeed, noOuterFlip);
    if (showBitIndicators)
        appendTileBitIndicators(lineBuf, tiles, cellPx, noise.get());
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(lineBuf.size() * sizeof(float)),
                 lineBuf.data(), GL_DYNAMIC_DRAW);
    lineVertCount = (GLsizei)(lineBuf.size() / 6);
}

// Regenerate a fresh random tree/layout and rebuild both VBOs, then redraw.
void GuiViewer::reloadTree() {
    try {
        haveIndex = false;
        if (cfg.multiMode) loadMulti();
        else               loadSingle();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return;
    }
    uploadTileGeometry();
    uploadConnectionGeometry();
    updateTitle();
    draw();
}

// Core render pass: clear, set uniforms, draw VBOs into the currently-bound
// framebuffer. Called with the window framebuffer by draw(), and with an
// offscreen FBO + scaled zoom by saveHighResPNG — same geometry, different
// resolution and zoom level.
void GuiViewer::renderScene(float screenW, float screenH, float zoomOverride) {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glUniform1f(glGetUniformLocation(prog, "uViewCenterX"), viewCX);
    glUniform1f(glGetUniformLocation(prog, "uViewCenterY"), viewCY);
    glUniform1f(glGetUniformLocation(prog, "uZoom"),        zoomOverride);
    glUniform1f(glGetUniformLocation(prog, "uScreenW"),     screenW);
    glUniform1f(glGetUniformLocation(prog, "uScreenH"),     screenH);

    if (showTiles) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, STRIDE, (void*)0);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, STRIDE, (void*)(2 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, vertCount);
    }

    if (showPaths) {
        glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, STRIDE, (void*)0);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, STRIDE, (void*)(2 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, lineVertCount);
    }
}

void GuiViewer::draw() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowW, windowH);
    renderScene((float)windowW, (float)windowH, zoom);
    glfwSwapBuffers(window);
}

void GuiViewer::updateTitle() {
    std::ostringstream ts;
    ts << "Quadtree Viewer | ";
    if (cfg.multiMode) {
        ts << "multi " << gridW << "x" << gridH
           << " | roots=" << currentMulti.roots.size()
           << " | seed=" << cfg.seed;
    } else {
        ts << "k=" << cfg.k
           << " | #" << u256_to_string(n)
           << " | " << gridW << "x" << gridH;
    }
    ts << " | zoom=" << std::fixed;
    ts.precision(2);
    ts << zoom << "x";
    ts << " | " << (smoothMode ? "smooth" : "discrete")
       << " " << speedMultiplier << "x";
    ts << " | flips+chiral=" << (flipRule == FR_OFF ? "off" : "on");
    ts << " | paths=" << (pathColorMode == PCM_AXIS ? "axis" : "component");
    if (showBitIndicators) ts << " [bits]";
    if (noOuterFlip) ts << " [no-outer-flip]";
    if (recording) ts << " [RECORDING]";
    glfwSetWindowTitle(window, ts.str().c_str());
}

// Save a standard-resolution PNG of the current framebuffer with timing/vertex info.
void GuiViewer::saveStandardPNG() {
    using Clock = std::chrono::high_resolution_clock;
    auto t0 = Clock::now();
    std::vector<unsigned char> pixels((size_t)windowW * windowH * 4);
    glReadPixels(0, 0, windowW, windowH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    auto t1 = Clock::now();
    std::string ts = timestampString();
    std::string basename = ts + "-quadtree";
    std::string filename = (outputsDir() / (basename + ".png")).string();
    savePNG(filename, pixels, windowW, windowH);
    auto t2 = Clock::now();
    double readMs  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double writeMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::cout << "  resolution: " << windowW << "x" << windowH
              << "  verts: tiles=" << vertCount << " lines=" << lineVertCount
              << "  read=" << readMs << "ms"
              << "  write=" << writeMs << "ms\n" << std::flush;
    saveStateJSON(basename);
}

// Save a high-resolution PNG by rendering into an offscreen FBO at windowW/H *
// scale. Reuses the same camera (viewCX, viewCY, zoom), so what you see is what
// you get, just at higher pixel density. Preserves the window's aspect ratio.
void GuiViewer::saveHighResPNG(float scale) {
    using Clock = std::chrono::high_resolution_clock;
    int outW = std::max(1, (int)std::round(windowW * scale));
    int outH = std::max(1, (int)std::round(windowH * scale));

    auto t0 = Clock::now();
    GLuint fbo = 0, colorTex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "FBO incomplete; high-res PNG aborted\n";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &colorTex);
        glDeleteFramebuffers(1, &fbo);
        return;
    }

    glViewport(0, 0, outW, outH);
    // Scale zoom by `scale` so the visible region matches the on-screen view.
    renderScene((float)outW, (float)outH, zoom * scale);
    auto t1 = Clock::now();

    std::vector<unsigned char> pixels((size_t)outW * outH * 4);
    glReadPixels(0, 0, outW, outH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    auto t2 = Clock::now();

    // Restore default framebuffer + viewport so subsequent redraws work.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowW, windowH);
    glDeleteTextures(1, &colorTex);
    glDeleteFramebuffers(1, &fbo);

    std::string basename = timestampString() + "-quadtree";
    std::string filename = (outputsDir() / (basename + ".png")).string();
    savePNG(filename, pixels, outW, outH);
    auto t3 = Clock::now();

    double renderMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double readMs   = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double writeMs  = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::cout << "  resolution: " << outW << "x" << outH << " (scale " << scale << "x)"
              << "  verts: tiles=" << vertCount << " lines=" << lineVertCount
              << "  render=" << renderMs << "ms"
              << "  read=" << readMs << "ms"
              << "  write=" << writeMs << "ms\n" << std::flush;
    saveStateJSON(basename);
}

void GuiViewer::startRecording() {
    std::string basename = timestampString() + "-quadtree";
    std::string filename = (outputsDir() / (basename + ".mp4")).string();
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f rawvideo -pixel_format rgba -video_size %dx%d -framerate %d -i pipe:0 "
        "-vf vflip -c:v libx264 -pix_fmt yuv420p -crf 18 \"%s\" 2>/dev/null",
        recordW, recordH, RECORD_FPS, filename.c_str());
    ffmpegPipe = popen(cmd, "w");
    if (!ffmpegPipe) {
        std::cerr << "Failed to launch ffmpeg\n";
        return;
    }
    recording = true;
    recordFrameCount = 0;
    lastRecordFrame = std::chrono::high_resolution_clock::now();
    std::cout << "Recording started: " << filename << "\n" << std::flush;
    saveStateJSON(basename);
    updateTitle();
}

void GuiViewer::stopRecording() {
    if (!recording || !ffmpegPipe) return;
    pclose(ffmpegPipe);
    ffmpegPipe = nullptr;
    recording = false;
    std::cout << "Recording stopped. " << recordFrameCount << " frames captured.\n" << std::flush;
    updateTitle();
}

// Emit a JSON blob capturing all parameters needed to reproduce this exact
// artwork. Written to outputs/<basename>.json and also printed to stdout so
// it can be copied without touching the filesystem.
void GuiViewer::saveStateJSON(const std::string& basename) {
    // helpers for enum -> string
    auto colorModeStr = [](ColorMode m) -> const char* {
        switch (m) {
            case CM_NONE:    return "none";
            case CM_RANDOM:  return "random";
            case CM_BY_SIZE: return "by-size";
            case CM_BY_RANK: return "by-rank";
        }
        return "none";
    };
    auto pathColorModeStr = [](PathColorMode m) -> const char* {
        return m == PCM_COMPONENT ? "component" : "axis";
    };
    auto boolStr = [](bool b) -> const char* { return b ? "true" : "false"; };

    std::ostringstream j;
    j << "{\n";

    // -- config: launch-time parameters --
    j << "  \"config\": {\n";
    j << "    \"seed\": "            << cfg.seed            << ",\n";
    j << "    \"k\": "               << cfg.k               << ",\n";
    j << "    \"multiMode\": "       << boolStr(cfg.multiMode) << ",\n";
    if (cfg.multiMode) {
        j << "    \"multiW\": "      << cfg.multiW          << ",\n";
        j << "    \"multiH\": "      << cfg.multiH          << ",\n";
    }
    if (cfg.haveIndex || haveIndex) {
        j << "    \"index\": \""     << u256_to_string(n)   << "\",\n";
    }
    j << "    \"attempts\": "        << cfg.attempts        << ",\n";
    j << "    \"outerEdge1x1\": "    << boolStr(cfg.outerEdge1x1)          << ",\n";
    j << "    \"greedyCover\": "     << boolStr(cfg.greedyCover)           << ",\n";
    j << "    \"colorMode\": \""     << colorModeStr(cfg.colorMode)        << "\",\n";
    j << "    \"resolutionScale\": " << cfg.resolutionScale << ",\n";
    if (cfg.hasFirstRank) {
        j << "    \"firstRank\": \"" << u256_to_string(cfg.firstRank) << "\",\n";
    }
    if (cfg.hasFirstK) {
        j << "    \"firstK\": "      << cfg.firstK          << ",\n";
    }
    if (!cfg.layoutFile.empty()) {
        // Inline layout specs so the JSON is self-contained even if the file moves.
        j << "    \"layoutFile\": \"" << cfg.layoutFile     << "\",\n";
        j << "    \"layoutSpecs\": [\n";
        for (size_t i = 0; i < cfg.layoutSpecs.size(); ++i) {
            const auto& rs = cfg.layoutSpecs[i];
            j << "      { \"x\": " << rs.x << ", \"y\": " << rs.y << ", \"k\": " << rs.k << " }";
            if (i + 1 < cfg.layoutSpecs.size()) j << ",";
            j << "\n";
        }
        j << "    ],\n";
    }
    // Remove trailing comma from last config field (resolutionScale or firstK/firstRank).
    // Easier to just emit a sentinel field that always comes last with no comma.
    j << "    \"windowW\": "         << windowW             << ",\n";
    j << "    \"windowH\": "         << windowH             << "\n";
    j << "  },\n";

    // -- state: runtime toggles and seeds --
    j << "  \"state\": {\n";
    j << "    \"colorSeed\": "       << colorSeed           << ",\n";
    j << "    \"pathColorSeed\": "   << pathColorSeed       << ",\n";
    j << "    \"pathColorMode\": \"" << pathColorModeStr(pathColorMode) << "\",\n";
    j << "    \"flipRule\": "        << (int)flipRule        << ",\n";
    j << "    \"chiralEnabled\": "   << boolStr(chiralEnabled) << ",\n";
    j << "    \"noiseChunkX\": "     << noiseChunkX         << ",\n";
    j << "    \"noiseChunkY\": "     << noiseChunkY         << ",\n";
    j << "    \"showTiles\": "       << boolStr(showTiles)  << ",\n";
    j << "    \"showPaths\": "       << boolStr(showPaths)  << ",\n";
    j << "    \"showBitIndicators\": " << boolStr(showBitIndicators) << ",\n";
    j << "    \"smoothMode\": "      << boolStr(smoothMode) << ",\n";
    j << "    \"speedMultiplier\": " << speedMultiplier     << "\n";
    j << "  },\n";

    // -- camera --
    j << "  \"camera\": {\n";
    j << "    \"viewCX\": " << viewCX << ",\n";
    j << "    \"viewCY\": " << viewCY << ",\n";
    j << "    \"zoom\": "   << zoom   << "\n";
    j << "  }\n";

    j << "}\n";

    std::string json = j.str();

    // Write sidecar file.
    std::string filepath = (outputsDir() / (basename + ".json")).string();
    if (FILE* f = std::fopen(filepath.c_str(), "w")) {
        std::fwrite(json.c_str(), 1, json.size(), f);
        std::fclose(f);
    } else {
        std::cerr << "  warning: could not write state JSON to " << filepath << "\n";
    }

    // Also print to stdout so it can be copied directly.
    std::cout << "  state JSON (" << filepath << "):\n" << json << std::flush;
}

void GuiViewer::run() {
    // Draw once so the initial frame is on-screen before entering the loop.
    draw();

    // Edge-triggered "was down" flags for keys we want one-shot, not repeat.
    // These are loop-local: they describe input state across frames, not
    // viewer state, so they stay out of the class.
    bool enterWasDown    = false;
    bool shiftWasDown    = false;
    bool tabWasDown      = false;
    bool backslashWasDown = false;
    bool spaceWasDown    = false;
    bool rWasDown        = false;
    bool tWasDown        = false;
    bool pWasDown        = false;
    bool cWasDown        = false;
    bool fWasDown        = false;
    bool dWasDown        = false;
    bool oWasDown        = false;
    bool equalWasDown    = false;
    bool minusWasDown    = false;
    bool upWasDown       = false;
    bool downWasDown     = false;
    bool leftWasDown     = false;
    bool rightWasDown    = false;
    bool key1WasDown     = false;
    bool bracketLWasDown = false;
    bool bracketRWasDown = false;

    // glfwPollEvents (not glfwWaitEvents) keeps the loop spinning so smooth-mode
    // zoom/pan and per-frame MP4 capture work without any external event trigger.
    using Clock = std::chrono::high_resolution_clock;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, GLFW_TRUE);

        bool dirty = false;

        // Shift: regenerate tiling.
        bool shiftDown = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS
                      || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        if (shiftDown && !shiftWasDown) { reloadTree(); }
        shiftWasDown = shiftDown;

        // Tab: jump to a new position in the noise field, regenerating chirality/flip bits.
        bool tabDown = glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS;
        if (tabDown && !tabWasDown) {
            noiseChunkX = (int)(rng() & 0xFFFF);
            noiseChunkY = (int)(rng() & 0xFFFF);
            noise = std::make_unique<NoiseField>(gridW, gridH, noiseChunkX, noiseChunkY);
            uploadConnectionGeometry();
            updateTitle();
            dirty = true;
        }
        tabWasDown = tabDown;

        // R: regenerate random colors (only meaningful for CM_RANDOM, but cheap otherwise).
        bool rDown = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
        if (rDown && !rWasDown) {
            colorSeed = rng();
            uploadTileGeometry();
            dirty = true;
        }
        rWasDown = rDown;

        // T: toggle tile rendering.
        bool tDown = glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS;
        if (tDown && !tWasDown) { showTiles = !showTiles; dirty = true; }
        tWasDown = tDown;

        // P: toggle path (connection-line) rendering.
        bool pDown = glfwGetKey(window, GLFW_KEY_P) == GLFW_PRESS;
        if (pDown && !pWasDown) { showPaths = !showPaths; dirty = true; }
        pWasDown = pDown;

        // C: cycle path color mode (axis colors <-> connected-component colors).
        bool cDown = glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS;
        if (cDown && !cWasDown) {
            pathColorMode = (pathColorMode == PCM_AXIS) ? PCM_COMPONENT : PCM_AXIS;
            pathColorSeed = rng();
            uploadConnectionGeometry();
            updateTitle();
            dirty = true;
        }
        cWasDown = cDown;

        // F: toggle flip-driven diagonal routing and chirality together.
        // Rebuilds the connection VBO so the change is visible immediately.
        bool fDown = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
        if (fDown && !fWasDown) {
            bool nowOn = (flipRule == FR_OFF);
            flipRule = nowOn ? FR_ON : FR_OFF;
            chiralEnabled = nowOn;
            uploadConnectionGeometry();
            updateTitle();
            dirty = true;
        }
        fWasDown = fDown;

        // D: toggle per-tile hFlip/vFlip/chiral bit-indicator squares.
        bool dDown = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
        if (dDown && !dWasDown) {
            showBitIndicators = !showBitIndicators;
            uploadConnectionGeometry();
            updateTitle();
            dirty = true;
        }
        dWasDown = dDown;

        // O: toggle --no-outer-flip (force hFlip=vFlip=0 on all border tiles).
        bool oDown = glfwGetKey(window, GLFW_KEY_O) == GLFW_PRESS;
        if (oDown && !oWasDown) {
            noOuterFlip = !noOuterFlip;
            uploadConnectionGeometry();
            updateTitle();
            dirty = true;
        }
        oWasDown = oDown;

        // 1: toggle smooth/discrete movement mode.
        bool key1Down = glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS;
        if (key1Down && !key1WasDown) {
            smoothMode = !smoothMode;
            updateTitle();
        }
        key1WasDown = key1Down;

        // [ / ] : geometric speed adjustment (affects smooth mode + discrete pan step).
        bool bracketLDown = glfwGetKey(window, GLFW_KEY_LEFT_BRACKET)  == GLFW_PRESS;
        bool bracketRDown = glfwGetKey(window, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS;
        if (bracketLDown && !bracketLWasDown) { speedMultiplier /= SPEED_STEP; updateTitle(); }
        if (bracketRDown && !bracketRWasDown) { speedMultiplier *= SPEED_STEP; updateTitle(); }
        bracketLWasDown = bracketLDown;
        bracketRWasDown = bracketRDown;

        // =/- zoom.
        //   smooth mode  : every frame the key is held, zoom by ZOOM_STEP^speedMultiplier
        //   discrete mode: one ZOOM_STEP per press
        bool equalDown = glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS;
        bool minusDown = glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS;
        float zoomFactor = smoothMode ? std::pow(ZOOM_STEP, speedMultiplier) : ZOOM_STEP;
        bool zoomInTrigger  = smoothMode ? equalDown : (equalDown && !equalWasDown);
        bool zoomOutTrigger = smoothMode ? minusDown : (minusDown && !minusWasDown);
        if (zoomInTrigger)  { zoom *= zoomFactor; dirty = true; }
        if (zoomOutTrigger) { zoom /= zoomFactor; dirty = true; }
        equalWasDown = equalDown;
        minusWasDown = minusDown;

        // Arrow keys pan; step is cellPx * speedMultiplier in world units, divided
        // by zoom so the visual pan distance stays consistent at any zoom.
        // Smooth mode pans every held frame; discrete mode pans once per press.
        float panStep = (float)cellPx * speedMultiplier / zoom;
        bool upDown    = glfwGetKey(window, GLFW_KEY_UP)    == GLFW_PRESS;
        bool downDown  = glfwGetKey(window, GLFW_KEY_DOWN)  == GLFW_PRESS;
        bool leftDown  = glfwGetKey(window, GLFW_KEY_LEFT)  == GLFW_PRESS;
        bool rightDown = glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
        bool upTrigger    = smoothMode ? upDown    : (upDown    && !upWasDown);
        bool downTrigger  = smoothMode ? downDown  : (downDown  && !downWasDown);
        bool leftTrigger  = smoothMode ? leftDown  : (leftDown  && !leftWasDown);
        bool rightTrigger = smoothMode ? rightDown : (rightDown && !rightWasDown);
        if (upTrigger)    { viewCY -= panStep; dirty = true; }
        if (downTrigger)  { viewCY += panStep; dirty = true; }
        if (leftTrigger)  { viewCX -= panStep; dirty = true; }
        if (rightTrigger) { viewCX += panStep; dirty = true; }
        upWasDown = upDown; downWasDown = downDown;
        leftWasDown = leftDown; rightWasDown = rightDown;

        // Enter: standard-resolution PNG.
        bool enterDown = glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS;
        if (enterDown && !enterWasDown) saveStandardPNG();
        enterWasDown = enterDown;

        // Backslash: high-resolution PNG (scale = resolutionScale).
        bool backslashDown = glfwGetKey(window, GLFW_KEY_BACKSLASH) == GLFW_PRESS;
        if (backslashDown && !backslashWasDown) saveHighResPNG(cfg.resolutionScale);
        backslashWasDown = backslashDown;

        // Space: toggle MP4 recording.
        bool spaceDown = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (spaceDown && !spaceWasDown) {
            if (recording) stopRecording();
            else           startRecording();
        }
        spaceWasDown = spaceDown;

        // Collapse all per-key dirty flags into one draw per loop iteration.
        if (dirty) {
            updateTitle();
            draw();
        }

        // Capture a recording frame at fixed FPS.
        if (recording && ffmpegPipe) {
            auto now = Clock::now();
            double elapsed = std::chrono::duration<double>(now - lastRecordFrame).count();
            if (elapsed >= 1.0 / RECORD_FPS) {
                lastRecordFrame = now;
                if (!dirty) draw(); // make sure latest frame is in the back buffer
                // Read exactly recordW x recordH so the buffer matches the ffmpeg
                // video_size parameter, dropping any odd-pixel right column / bottom row.
                std::vector<unsigned char> pixels((size_t)recordW * recordH * 4);
                glReadPixels(0, 0, recordW, recordH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                std::fwrite(pixels.data(), 1, pixels.size(), ffmpegPipe);
                recordFrameCount++;
            }
        }
    }

    if (recording) stopRecording();
}

