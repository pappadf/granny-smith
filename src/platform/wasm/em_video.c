// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_video.c
// Video subsystem for Emscripten platform - implements WebGL2 rendering for Macintosh Plus framebuffer

// ============================================================================
// Includes
// ============================================================================

#include "em.h"

#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "system.h"

// ============================================================================
// Constants
// ============================================================================

// Framebuffer dimensions
#define WIDTH   512
#define HEIGHT  342
#define FB_SIZE (WIDTH * HEIGHT / 8) // 21,924 bytes
#define TEX_W   (WIDTH / 8) // 64
#define TEX_H   HEIGHT // 342

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Local framebuffer copy (compared against emulator's RAM for change detection)
static uint8_t framebuffer[FB_SIZE];

// WebGL resources
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = 0;
static GLuint prog = 0;
static GLuint tex = 0;
static GLuint vbo = 0;
static GLint loc_pos = -1;

// ============================================================================
// Shader Source Code
// ============================================================================

// Vertex shader (WebGL 2) - full-screen quad with texture coordinates
static const char *vs_src = "#version 300 es\n"
                            "in  vec2 a_position;\n"
                            "out vec2 v_texCoord;\n"
                            "\n"
                            "const float FB_WIDTH  = 512.0;\n"
                            "const float FB_HEIGHT = 342.0;\n"
                            "\n"
                            "void main() {\n"
                            "    v_texCoord = vec2((a_position.x + 1.0) * 0.5,\n"
                            "                      1.0 - (a_position.y + 1.0) * 0.5);\n"
                            "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
                            "}\n";

// Fragment shader (WebGL 2) - unpack one bit from the red channel
static const char *fs_src = "#version 300 es\n"
                            "precision mediump float;\n"
                            "\n"
                            "uniform sampler2D u_texture;\n"
                            "in  vec2 v_texCoord;\n"
                            "out vec4 fragColor;\n"
                            "\n"
                            "const float FB_WIDTH        = 512.0;\n"
                            "const float FB_HEIGHT       = 342.0;\n"
                            "const float BYTES_PER_ROW   = FB_WIDTH / 8.0;\n"
                            "\n"
                            "void main() {\n"
                            "    vec2  px        = v_texCoord * vec2(FB_WIDTH, FB_HEIGHT);\n"
                            "    float x         = floor(px.x);\n"
                            "    float y         = floor(px.y);\n"
                            "    float byteIndex = floor(x / 8.0);\n"
                            "    float bitPos    = mod(x, 8.0);\n"
                            "\n"
                            "    vec2 tc = vec2((byteIndex + 0.5) / BYTES_PER_ROW,\n"
                            "                   (y + 0.5)       / FB_HEIGHT);\n"
                            "\n"
                            "    float byteVal = floor(texture(u_texture, tc).r * 255.0 + 0.5);\n"
                            "    float mask    = exp2(7.0 - bitPos);\n"
                            "    float pixel   = step(mask, mod(byteVal, mask * 2.0));\n"
                            "    float col     = 1.0 - pixel;\n"
                            "    fragColor     = vec4(col, col, col, 1.0);\n"
                            "}\n";

// ============================================================================
// Static Helpers
// ============================================================================

// Compile a shader and check for errors
static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);

    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        printf("Shader error: %s\n", log);
        emscripten_cancel_main_loop();
    }
    return s;
}

// Initialize WebGL2 context and rendering resources
static void init_gl(void) {
    // Create WebGL2 context
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.majorVersion = 2;
    attr.minorVersion = 0;
    ctx = emscripten_webgl_create_context("#screen", &attr);
    if (ctx <= 0) {
        printf("WebGL 2 not supported—cannot run.");
        return;
    }
    emscripten_webgl_make_context_current(ctx);

    glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // Compile and link shader program
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_position");
    glLinkProgram(prog);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        printf("Link error: %s\n", log);
        emscripten_cancel_main_loop();
    }
    glUseProgram(prog);

    // Create full-screen quad
    const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    loc_pos = glGetAttribLocation(prog, "a_position");
    glEnableVertexAttribArray(loc_pos);
    glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);

    // Create red-channel texture for framebuffer data
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, TEX_W, TEX_H, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(glGetUniformLocation(prog, "u_texture"), 0);

    glViewport(0, 0, WIDTH, HEIGHT);
}

// Update canvas by uploading framebuffer to texture and drawing
static void update_canvas(void) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TEX_W, TEX_H, GL_RED, GL_UNSIGNED_BYTE, framebuffer);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        printf("glTexSubImage2D error: %d\n", err);
    }

    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// ============================================================================
// Operations (Public API)
// ============================================================================

// Initialize video subsystem
void em_video_init(void) {
    init_gl();
}

// Update video from emulator's video buffer (if changed)
void em_video_update(void) {
    uint8_t *vbuf = system_framebuffer();
    if (!vbuf)
        return;

    int size = 512 * 342 / 8; // Fixed size for the framebuffer
    if (memcmp(framebuffer, vbuf, size) != 0) {
        memcpy(framebuffer, vbuf, size);
        update_canvas();
    }
}

// Force a one-shot redraw from the emulator's current video buffer
// Used after restoring a checkpoint when the emulation may be paused
void em_video_force_redraw(void) {
    uint8_t *vbuf = system_framebuffer();
    if (!vbuf)
        return;

    const int size = 512 * 342 / 8;
    memcpy(framebuffer, vbuf, size);
    update_canvas();
}

// Get pointer to framebuffer (for external access if needed)
uint8_t *em_video_get_framebuffer(void) {
    return framebuffer;
}

// Platform interface implementation for frontend redraw
void frontend_force_redraw(void) {
    em_video_force_redraw();
}
