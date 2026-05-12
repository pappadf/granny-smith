// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_video.c
// WebGL2 renderer for the emulator's display.  Step 5 of the IIcx/IIx
// proposal (§3.3.3): format-aware shader pipeline + variable canvas.
// Reads the active `display_t` from system_display() each frame and
// consumes its dirty flags:
//
//   * shape_dirty: rebind the fragment-shader program for the current
//     format, reallocate the framebuffer texture, resize the canvas,
//     and re-upload pixels
//   * fb_dirty (and not shape_dirty): re-upload pixels into the
//     existing texture
//   * clut_dirty: re-upload the CLUT texture (indexed formats only)
//   * response_dirty: re-upload the per-channel CRT response LUT
//
// v1 ships full pixel paths for PIXEL_1BPP_MSB and PIXEL_8BPP (the
// shipping cards land on these); 16-bpp / 32-bpp programs are stubs
// that draw the framebuffer as grayscale luminance so an unfinished
// JMFB driver doesn't render garbage.  Step 6's JMFB driver fills
// those out as it lights up colour modes.

#include "em.h"

#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "system.h"

// ============================================================================
// Internal state
// ============================================================================

// Maximum supported framebuffer area.  Chosen to fit the largest mode the
// proposal scopes (1152x870 = 1MP).  We still allocate per-display, but the
// scratch upload buffer is sized once.
#define MAX_FB_BYTES (1152u * 870u * 4u)

// WebGL resources
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE s_ctx = 0;
static GLuint s_vbo = 0;
static GLuint s_fb_tex = 0; // framebuffer texture (format depends on pixel_format)
static GLuint s_clut_tex = 0; // 256x1 RGBA texture for indexed-format CLUTs
static GLuint s_response_tex = 0; // 256x3 R8 texture — per-channel CRT response LUT

// Per-format shader programs.  Indexed by pixel_format_t.  A NULL slot means
// "no program for this format yet" — fall back to the 1bpp program with
// black output to keep the renderer alive.
#define NUM_FORMATS 6
static GLuint s_progs[NUM_FORMATS] = {0};

// Per-program uniform locations, queried at link time.  -1 if the uniform
// isn't present in that program (e.g. u_clut on direct-colour formats).
typedef struct prog_uniforms {
    GLint u_fb_size; // vec2 (width, height) in pixels
    GLint u_stride; // float — bytes per row in the framebuffer texture
    GLint u_texture; // sampler2D (framebuffer)
    GLint u_clut; // sampler2D (CLUT, optional)
    GLint u_response; // sampler2D (256x3 R8 CRT response LUT, optional)
} prog_uniforms_t;

static prog_uniforms_t s_uniforms[NUM_FORMATS] = {0};

// Scratch upload buffer for the framebuffer texture.
static uint8_t s_upload_scratch[MAX_FB_BYTES];

// ============================================================================
// Shader sources
// ============================================================================

// Vertex shader is shared across all programs — full-screen quad, flips Y
// so the texture's row 0 is the top of the canvas.
static const char *VS_SHARED = "#version 300 es\n"
                               "in  vec2 a_position;\n"
                               "out vec2 v_uv;\n"
                               "void main() {\n"
                               "    v_uv = vec2((a_position.x + 1.0) * 0.5,\n"
                               "                1.0 - (a_position.y + 1.0) * 0.5);\n"
                               "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
                               "}\n";

// 1 bpp packed, MSB-first.  Texture stores raw framebuffer bytes in
// red channel (R8); shader unpacks the bit at the pixel column.  Output is
// inverted (1 = black, 0 = white) to match Mac display convention.
static const char *FS_1BPP = "#version 300 es\n"
                             "precision mediump float;\n"
                             "uniform sampler2D u_texture;\n"
                             "uniform vec2 u_fb_size;\n"
                             "uniform float u_stride;\n"
                             "in  vec2 v_uv;\n"
                             "out vec4 fragColor;\n"
                             "void main() {\n"
                             "    vec2 px        = v_uv * u_fb_size;\n"
                             "    float x        = floor(px.x);\n"
                             "    float y        = floor(px.y);\n"
                             "    float byteIdx  = floor(x / 8.0);\n"
                             "    float bitPos   = mod(x, 8.0);\n"
                             "    vec2 tc        = vec2((byteIdx + 0.5) / u_stride,\n"
                             "                          (y + 0.5) / u_fb_size.y);\n"
                             "    float byteVal  = floor(texture(u_texture, tc).r * 255.0 + 0.5);\n"
                             "    float mask     = exp2(7.0 - bitPos);\n"
                             "    float pixel    = step(mask, mod(byteVal, mask * 2.0));\n"
                             "    float col      = 1.0 - pixel;\n"
                             "    fragColor      = vec4(col, col, col, 1.0);\n"
                             "}\n";

// 8 bpp indexed.  Framebuffer texture stores raw bytes in red channel;
// CLUT texture is 256x1 RGBA8.  Sample the byte, scale to [0,1], lookup
// in the CLUT, then route each component through the per-channel CRT
// response LUT (`u_response`, 256x3 R8: row 0 = R, row 1 = G, row 2 = B).
// The response LUT models the monitor's physical gamma response — see
// display_t::crt_response.  For monitors with near-identity gamma
// (12"/13" RGB, Portrait), the LUT is filled with identity so this
// pass is a no-op; for Kong it un-does Apple's gamma pre-correction
// and yields a neutral image.
static const char *FS_8BPP = "#version 300 es\n"
                             "precision mediump float;\n"
                             "uniform sampler2D u_texture;\n"
                             "uniform sampler2D u_clut;\n"
                             "uniform sampler2D u_response;\n"
                             "uniform vec2 u_fb_size;\n"
                             "uniform float u_stride;\n"
                             "in  vec2 v_uv;\n"
                             "out vec4 fragColor;\n"
                             "void main() {\n"
                             "    vec2 px       = v_uv * u_fb_size;\n"
                             "    float x       = floor(px.x);\n"
                             "    float y       = floor(px.y);\n"
                             "    vec2 tc       = vec2((x + 0.5) / u_stride,\n"
                             "                         (y + 0.5) / u_fb_size.y);\n"
                             "    float idx     = floor(texture(u_texture, tc).r * 255.0 + 0.5);\n"
                             "    vec4 entry    = texture(u_clut, vec2((idx + 0.5) / 256.0, 0.5));\n"
                             "    float r_out   = texture(u_response, vec2(entry.r, 0.5 / 3.0)).r;\n"
                             "    float g_out   = texture(u_response, vec2(entry.g, 1.5 / 3.0)).r;\n"
                             "    float b_out   = texture(u_response, vec2(entry.b, 2.5 / 3.0)).r;\n"
                             "    fragColor     = vec4(r_out, g_out, b_out, 1.0);\n"
                             "}\n";

// Stubs for 2/4 bpp indexed: same shape as 8bpp but unpacking a 2- or
// 4-bit field per pixel from packed bytes.  v1 ships them as fallback
// programs that emit grey so the canvas isn't blank when an
// unimplemented format hits the pipeline.
static const char *FS_GRAY_STUB = "#version 300 es\n"
                                  "precision mediump float;\n"
                                  "in  vec2 v_uv;\n"
                                  "out vec4 fragColor;\n"
                                  "void main() { fragColor = vec4(0.5, 0.5, 0.5, 1.0); }\n";

// ============================================================================
// Static helpers
// ============================================================================

// Compile a shader, panic on error.  Returns 0 if compilation failed.
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
        return 0;
    }
    return s;
}

// Link a vertex+fragment pair into a program; stash uniform locations.
static GLuint link_program(const char *vs_src, const char *fs_src, prog_uniforms_t *u_out) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return 0;
    GLuint prog = glCreateProgram();
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
        return 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (u_out) {
        u_out->u_fb_size = glGetUniformLocation(prog, "u_fb_size");
        u_out->u_stride = glGetUniformLocation(prog, "u_stride");
        u_out->u_texture = glGetUniformLocation(prog, "u_texture");
        u_out->u_clut = glGetUniformLocation(prog, "u_clut");
        u_out->u_response = glGetUniformLocation(prog, "u_response");
    }
    return prog;
}

// Build the per-format program table.  Programs that share a fragment
// shader (the gray stubs) get distinct GL programs to keep uniform
// management simple.
static void init_programs(void) {
    s_progs[PIXEL_1BPP_MSB] = link_program(VS_SHARED, FS_1BPP, &s_uniforms[PIXEL_1BPP_MSB]);
    s_progs[PIXEL_2BPP_MSB] = link_program(VS_SHARED, FS_GRAY_STUB, &s_uniforms[PIXEL_2BPP_MSB]);
    s_progs[PIXEL_4BPP_MSB] = link_program(VS_SHARED, FS_GRAY_STUB, &s_uniforms[PIXEL_4BPP_MSB]);
    s_progs[PIXEL_8BPP] = link_program(VS_SHARED, FS_8BPP, &s_uniforms[PIXEL_8BPP]);
    s_progs[PIXEL_16BPP_555] = link_program(VS_SHARED, FS_GRAY_STUB, &s_uniforms[PIXEL_16BPP_555]);
    s_progs[PIXEL_32BPP_XRGB] = link_program(VS_SHARED, FS_GRAY_STUB, &s_uniforms[PIXEL_32BPP_XRGB]);
}

// Pick the GL program that best matches `format`, with a 1bpp fallback
// when the slot is empty (compile failure during init).
static GLuint program_for(pixel_format_t format) {
    if (format >= 0 && (int)format < NUM_FORMATS && s_progs[format])
        return s_progs[format];
    return s_progs[PIXEL_1BPP_MSB];
}

static const prog_uniforms_t *uniforms_for(pixel_format_t format) {
    if (format >= 0 && (int)format < NUM_FORMATS && s_progs[format])
        return &s_uniforms[format];
    return &s_uniforms[PIXEL_1BPP_MSB];
}

// (Re)allocate the framebuffer texture for the given format and stride.
// Each format picks the texture width / internal format that matches how
// the framebuffer bytes pack into pixels:
//   * 1bpp / 2bpp / 4bpp / 8bpp:  width = stride bytes, R8 internal
//   * 16bpp:                      width = stride/2,     R16UI (mediump)
//   * 24bpp packed:               width = stride/3,     RGB8
// The shader's u_stride uniform feeds the byte-to-pixel index math.
static void allocate_fb_texture(pixel_format_t format, uint32_t stride, uint32_t height) {
    GLenum internal = GL_R8;
    GLenum src_fmt = GL_RED;
    GLenum src_type = GL_UNSIGNED_BYTE;
    uint32_t tex_width = stride;
    switch (format) {
    case PIXEL_1BPP_MSB:
    case PIXEL_2BPP_MSB:
    case PIXEL_4BPP_MSB:
    case PIXEL_8BPP:
        internal = GL_R8;
        src_fmt = GL_RED;
        break;
    case PIXEL_16BPP_555:
        internal = GL_R8; // step 6 / JMFB driver replaces this with RGB565
        src_fmt = GL_RED;
        tex_width = stride;
        break;
    case PIXEL_32BPP_XRGB:
        internal = GL_RGBA8;
        src_fmt = GL_RGBA;
        tex_width = stride / 4;
        break;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_fb_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, tex_width, height, 0, src_fmt, src_type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// (Re)upload the live framebuffer bytes into the existing texture.  Caller
// must have called allocate_fb_texture first if the shape changed.
//
// Explicitly selects texture unit 0 before binding so this routine doesn't
// clobber the s_clut_tex / s_response_tex bindings on units 1 / 2 by
// rebinding s_fb_tex on whichever unit happened to be active.  Same fix
// applies to upload_clut (unit 1) and upload_response (unit 2).
static void upload_fb(const display_t *d) {
    GLenum src_fmt = (d->format == PIXEL_32BPP_XRGB) ? GL_RGBA : GL_RED;
    uint32_t tex_width = (d->format == PIXEL_32BPP_XRGB) ? d->stride / 4 : d->stride;

    size_t bytes = (size_t)d->stride * d->height;
    if (bytes > sizeof(s_upload_scratch))
        bytes = sizeof(s_upload_scratch);
    memcpy(s_upload_scratch, d->bits, bytes);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_fb_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_width, d->height, src_fmt, GL_UNSIGNED_BYTE, s_upload_scratch);
}

// Upload `clut` (clut_len entries) into the 256x1 CLUT texture.  Pads
// unused entries with black so an out-of-range pixel index doesn't sample
// stale colour from a previous palette.
static void upload_clut(const display_t *d) {
    uint8_t buf[256 * 4] = {0};
    uint32_t n = d->clut_len > 256 ? 256 : d->clut_len;
    for (uint32_t i = 0; i < n; i++) {
        buf[i * 4 + 0] = d->clut[i].r;
        buf[i * 4 + 1] = d->clut[i].g;
        buf[i * 4 + 2] = d->clut[i].b;
        buf[i * 4 + 3] = 255;
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_clut_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, buf);
}

// Resize the canvas element + viewport to match the live display.  The
// CSS scale (zoom) is applied JS-side via the screen-wrapper element; here
// we set the *intrinsic* canvas resolution so 1 canvas pixel == 1 emulator
// pixel.
//
// Notifies the JS layout layer via Module.onScreenResize(width, height)
// on every transition so the page can reflow its screen-wrapper element
// to match the new aspect ratio / size.  Without this the wrapper stays
// at the previous size — the canvas's intrinsic resolution updates but
// CSS-driven display dimensions don't, and the page shows a stretched /
// letterboxed framebuffer (this surfaced for the IIcx when the JMFB
// driver flipped from the SE/30-default 512×342 to 640×480).  Mirrors
// the change-only pattern used by em_main_tick's onRunStateChange push.
static void resize_canvas(uint32_t width, uint32_t height) {
    emscripten_set_canvas_element_size("#screen", (int)width, (int)height);
    glViewport(0, 0, (int)width, (int)height);
    static uint32_t last_w = 0, last_h = 0;
    if (width != last_w || height != last_h) {
        last_w = width;
        last_h = height;
        // clang-format off
        MAIN_THREAD_ASYNC_EM_ASM(
            { if (typeof Module.onScreenResize === 'function') Module.onScreenResize($0, $1); },
            (int)width, (int)height);
        // clang-format on
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

static void init_gl(void) {
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.majorVersion = 2;
    attr.minorVersion = 0;
    s_ctx = emscripten_webgl_create_context("#screen", &attr);
    if (s_ctx <= 0) {
        printf("WebGL 2 not supported—cannot run.");
        return;
    }
    emscripten_webgl_make_context_current(s_ctx);

    glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    init_programs();

    // Full-screen quad (position-only attribute @ location 0).
    const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    // Framebuffer + CLUT textures.  Sizes set by the first allocate_fb_texture
    // / upload_clut call once the active display becomes known.
    glGenTextures(1, &s_fb_tex);
    glGenTextures(1, &s_clut_tex);
    glBindTexture(GL_TEXTURE_2D, s_clut_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // CRT response LUT — 256x3 R8 texture.  Initialised to identity (every
    // row 0..255) so display sources whose monitor has no per-channel
    // response (`display.crt_response == NULL`) render through a pass-
    // through path.  Non-identity responses (e.g. Kong's blue-boost
    // table) get uploaded by upload_response when the display source
    // assigns its monitor's table.
    glGenTextures(1, &s_response_tex);
    glBindTexture(GL_TEXTURE_2D, s_response_tex);
    uint8_t identity[3 * 256];
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++)
            identity[c * 256 + v] = (uint8_t)v;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 3, 0, GL_RED, GL_UNSIGNED_BYTE, identity);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// Upload the per-channel CRT response LUT.  If the display source has a
// non-NULL crt_response pointer, upload its 3x256 bytes; otherwise upload
// the identity table so the response pass is a no-op for monitors whose
// gamma is near-identity (12"/13" RGB, Portrait B&W).
static void upload_response(const display_t *d) {
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s_response_tex);
    if (d->crt_response) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 3, GL_RED, GL_UNSIGNED_BYTE, d->crt_response);
    } else {
        uint8_t identity[3 * 256];
        for (int c = 0; c < 3; c++)
            for (int v = 0; v < 256; v++)
                identity[c * 256 + v] = (uint8_t)v;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 3, GL_RED, GL_UNSIGNED_BYTE, identity);
    }
}

// Consume the display's dirty flags and re-bind / re-upload exactly what
// the producer marked changed.  Returns false if nothing to draw (no
// display).  shape_dirty implies the framebuffer texture must be
// reallocated and its pixels re-uploaded, so we treat it as fb-implying.
static bool refresh_from_display(display_t *d, bool force_full) {
    if (!d || !d->bits)
        return false;

    bool shape = force_full || d->shape_dirty;
    bool fb = force_full || d->fb_dirty || shape;
    bool clut = force_full || d->clut_dirty;
    bool response = force_full || d->response_dirty;

    if (shape) {
        glUseProgram(program_for(d->format));
        const prog_uniforms_t *u = uniforms_for(d->format);
        if (u->u_fb_size >= 0)
            glUniform2f(u->u_fb_size, (float)d->width, (float)d->height);
        if (u->u_stride >= 0)
            glUniform1f(u->u_stride, (float)((d->format == PIXEL_32BPP_XRGB) ? d->stride / 4 : d->stride));
        if (u->u_texture >= 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_fb_tex);
            glUniform1i(u->u_texture, 0);
        }
        if (u->u_clut >= 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, s_clut_tex);
            glUniform1i(u->u_clut, 1);
        }
        if (u->u_response >= 0) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, s_response_tex);
            glUniform1i(u->u_response, 2);
        }
        allocate_fb_texture(d->format, d->stride, d->height);
        resize_canvas(d->width, d->height);
    }

    if (fb)
        upload_fb(d);

    if (clut && d->clut && d->clut_len > 0)
        upload_clut(d);

    if (response)
        upload_response(d);

    d->fb_dirty = false;
    d->shape_dirty = false;
    d->clut_dirty = false;
    d->response_dirty = false;
    return true;
}

static void draw(void) {
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        printf("GL error: %d\n", err);
}

// ============================================================================
// Public API
// ============================================================================

void em_video_init(void) {
    init_gl();
}

void em_video_update(void) {
    display_t *d = system_display();
    if (!d || !d->bits)
        return;
    // If the producer hasn't marked anything dirty, the GPU still holds
    // the right pixels and the canvas already shows them — nothing to do.
    if (!d->fb_dirty && !d->shape_dirty && !d->clut_dirty && !d->response_dirty)
        return;
    if (refresh_from_display(d, /*force_full*/ false))
        draw();
}

void em_video_force_redraw(void) {
    display_t *d = system_display();
    if (refresh_from_display(d, /*force_full*/ true))
        draw();
}

uint8_t *em_video_get_framebuffer(void) {
    return s_upload_scratch;
}

void frontend_force_redraw(void) {
    em_video_force_redraw();
}
