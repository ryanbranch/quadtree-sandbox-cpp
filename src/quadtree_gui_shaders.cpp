#include "quadtree_gui_shaders.h"

#include <stdexcept>
#include <string>

// World coords -> NDC via view-centre / zoom / screen-size uniforms.
// (Identical pattern to art-project-src/main.cpp.)
static const char* VERT_SRC = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
out vec4 vColor;
uniform float uViewCenterX;
uniform float uViewCenterY;
uniform float uZoom;
uniform float uScreenW;
uniform float uScreenH;
void main() {
    vec2 screen = (aPos - vec2(uViewCenterX, uViewCenterY)) * uZoom
                  + vec2(uScreenW, uScreenH) * 0.5;
    float nx =  screen.x / (uScreenW  * 0.5) - 1.0;
    float ny =  1.0 - screen.y / (uScreenH * 0.5);
    gl_Position = vec4(nx, ny, 0.0, 1.0);
    vColor = aColor;
}
)";

static const char* FRAG_SRC = R"(
#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        throw std::runtime_error(std::string("Shader compile error: ") + log);
    }
    return s;
}

GLuint buildProgram() {
    GLuint vert = compileShader(GL_VERTEX_SHADER,   VERT_SRC);
    GLuint frag = compileShader(GL_FRAGMENT_SHADER, FRAG_SRC);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, 512, nullptr, log);
        throw std::runtime_error(std::string("Program link error: ") + log);
    }
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}
