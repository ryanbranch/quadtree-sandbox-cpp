#include "quadtree_gui_config.h"
#include "quadtree_gui_viewer.h"

#include <exception>
#include <iostream>
#include <random>

int main(int argc, char** argv) {
    GuiConfig cfg;
    if (!parseArgs(argc, argv, std::mt19937_64(std::random_device{}())(), cfg))
        return 1;

    try {
        GuiViewer viewer(cfg);
        viewer.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
