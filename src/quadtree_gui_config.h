#pragma once

#include "quadtree.h"
#include "quadtree_gui_geometry.h"

#include <cstdint>
#include <string>
#include <vector>

// All settings derived from the command line. Populated by parseArgs and then
// handed to GuiViewer; immutable for the lifetime of the viewer.
struct GuiConfig {
    int  k          = 5;
    bool haveIndex  = false;
    u256 userIndex  = 0;
    bool multiMode  = false;
    bool printGrid  = false;
    bool outerEdge1x1 = false;
    bool skip1x1Precomputation = false;
    int  multiW     = 0;
    int  multiH     = 0;
    int  attempts   = 128;   // <0 means unlimited
    uint64_t seed   = 0;          // set from argv, or random if --seed absent
    std::string layoutFile;
    ColorMode colorMode = CM_NONE;
    float resolutionScale = 2.0f; // backslash-key high-res export factor

    // Parsed layout file contents (empty unless --layout given).
    // generate_multi_quadtree treats an empty layoutSpecs as "generate a random
    // cover", so this being empty is valid even when multiMode is true.
    std::vector<RootSpec> layoutSpecs;
    bool hasFirstRank = false;
    u256 firstRank = 0;
    bool hasFirstK = false;
    int firstK = -1;
    bool greedyCover = false;
    bool noOuterFlip = false;
};

// Parse argv into cfg. On error, prints a message/usage to stderr and returns
// false. `defaultSeed` is used when --seed is not supplied.
bool parseArgs(int argc, char** argv, uint64_t defaultSeed, GuiConfig& cfg);
