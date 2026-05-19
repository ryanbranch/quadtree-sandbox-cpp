#pragma once

#include <string>
#include <vector>

// Write an RGBA pixel buffer to a PNG file. Pixels are assumed bottom-up
// (OpenGL convention) and are flipped vertically on write. Errors are
// reported to stderr; the function returns without throwing.
void savePNG(const std::string& filename,
             const std::vector<unsigned char>& pixels, int width, int height);
