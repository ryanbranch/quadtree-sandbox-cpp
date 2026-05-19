#include "quadtree_gui_png.h"

#include <png.h>
#include <cstdio>
#include <csetjmp>
#include <iostream>

void savePNG(const std::string& filename,
             const std::vector<unsigned char>& pixels, int width, int height) {
    FILE* fp = fopen(filename.c_str(), "wb");
    if (!fp) { std::cerr << "Cannot open " << filename << " for writing\n"; return; }

    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop   info = png_create_info_struct(png);

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        std::cerr << "PNG write error\n";
        return;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<const png_byte*> rows(height);
    for (int y = 0; y < height; y++)
        rows[y] = pixels.data() + (height - 1 - y) * width * 4; // flip Y (OpenGL is bottom-up)

    png_write_image(png, const_cast<png_bytepp>(rows.data()));
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(fp);

    std::cout << "Saved " << filename << "\n" << std::flush;
}
