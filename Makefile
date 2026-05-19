CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
CPPFLAGS ?= -Isrc

COMMON_OBJS = \
	build/quadtree_node.o \
	build/quadtree_multi_render.o \
	build/quadtree_multi_cover.o \
	build/quadtree_multi_validate.o \
	build/quadtree_multi_edge1x1.o \
	build/quadtree_multi_generate.o \
	build/quadtree_io.o \
	build/quadtree_rank.o \
	build/quadtree_constrained.o \
	build/u256.o

GUI_OBJS = \
	build/quadtree_gui.o \
	build/quadtree_gui_config.o \
	build/quadtree_gui_viewer.o \
	build/quadtree_gui_shaders.o \
	build/quadtree_gui_png.o \
	build/quadtree_gui_geometry.o \
	build/quadtree_gui_path_graph.o \
	build/noise_field.o

GUI_LIBS ?= -lGL -lGLEW -lglfw -lpng

.PHONY: all clean

all: quadtree diag_k5 tile_size_histogram

quadtree: build/quadtree_main.o $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

diag_k5: build/diag_k5.o $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

tile_size_histogram: build/tile_size_histogram.o $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

quadtree_parity_dump: build/quadtree_parity_dump.o \
	build/quadtree_gui_geometry.o build/quadtree_gui_path_graph.o build/noise_field.o \
	$(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

quadtree_gui: $(GUI_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GUI_LIBS) -o $@

build/%.o: src/%.cpp src/quadtree.h src/quadtree_internal.h src/quadtree_multi_internal.h | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# GUI translation units additionally depend on their own headers.
build/quadtree_gui.o:          src/quadtree_gui_config.h src/quadtree_gui_viewer.h
build/quadtree_gui_config.o:   src/quadtree_gui_config.h src/quadtree_gui_geometry.h
build/quadtree_gui_viewer.o:   src/quadtree_gui_viewer.h src/quadtree_gui_config.h \
                               src/quadtree_gui_shaders.h src/quadtree_gui_png.h \
                               src/quadtree_gui_geometry.h
build/quadtree_gui_shaders.o:  src/quadtree_gui_shaders.h
build/quadtree_gui_png.o:      src/quadtree_gui_png.h
build/quadtree_gui_geometry.o:   src/quadtree_gui_geometry.h src/quadtree_gui_path_graph.h
build/quadtree_gui_path_graph.o: src/quadtree_gui_path_graph.h src/quadtree_gui_geometry.h

# noise_field is self-contained: it depends only on its own header.
build/noise_field.o: src/noise_field.cpp src/noise_field.h | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build quadtree diag_k5 quadtree_gui
