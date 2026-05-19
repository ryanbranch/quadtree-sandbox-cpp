#pragma once

#include <GL/glew.h>

// Compile a single shader stage; throws std::runtime_error on failure.
GLuint compileShader(GLenum type, const char* src);

// Build and link the GUI's vertex+fragment program; throws on failure.
GLuint buildProgram();
