// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_video.c
// WebGL2 renderer for the emulator's display: format-aware shader pipeline
// + variable canvas, on a thread of its own.
//
// Two sides.  The core thread (em_video_update, once per tick) reads the
// active `display_t` from system_display(), decides whether anything
// changed and PUBLISHES the frame into a shared slot: geometry, the pixel
// bytes, the CLUT and the response table.  The render thread owns the
// `#screen` OffscreenCanvas and the WebGL context; on every animation frame
// it takes the newest published frame, uploads it and draws.  So the GL
// work, and the browser presenting the canvas (which happens only when the
// owning thread returns to its event loop), never hold up the emulator:
// the core's thread can run back to back in turbo.
//
// The publish consumes the display's dirty flags:
//
//   * shape_dirty: rebind the fragment-shader program for the current
//     format, reallocate the framebuffer texture, resize the canvas,
//     and re-upload pixels
//   * fb_dirty (and not shape_dirty): re-upload pixels into the
//     existing texture
//   * clut_dirty: re-upload the CLUT texture (indexed formats only)
//   * response_dirty: re-upload the per-channel CRT response LUT
//
// Full pixel paths exist for every pixel_format_t.  2 and 4 bpp were grey
// stubs until 2026-09-16 — see FS_2BPP for why "no shipping card boots into
// them" was the wrong test.

#include "em.h"

#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "log.h"
#include "system.h"
#include "event/event.h"

// The renderer is part of the display path, so it shares its category:
// `log.set("video", N)` turns on the producers AND the consumer.
// These five messages used to be bare printfs, so they
// could not be levelled, filed or redirected at all.
LOG_USE_CATEGORY_NAME("video");

// ============================================================================
// Internal state
// ============================================================================

// Maximum supported framebuffer area: stride * height, not width * height.
// The largest monitor any card drives is 1152x870, but a card pads its rows:
// the PM 7500's 1152x870 at 32 bpp has a 4640-byte stride, 28 KB more than
// 1152 * 870 * 4, and the screen froze at that size (S1018-16).  1280x1024 at
// 32 bpp leaves room for any row padding up to 6 KB at 870 rows.  We still
// allocate per-display, but the scratch upload buffer is sized once (on the
// heap, when WebGL comes up).
#define MAX_FB_BYTES (1280u * 1024u * 4u)

// Does this descriptor's raster fit the scratch buffer?  A larger one is
// refused, not clamped: the clamp bounded the memcpy INTO the scratch but
// glTexSubImage2D still read stride*height back OUT of it, so a mode past the
// ceiling (DAFB reaches stride 16380 x 2048, the Mach64 32736) read past the
// static array either way -- and half a frame is not a useful degradation.
// Past that ceiling it is the producers' job to keep stride*height inside the
// buffer they point at: most through display_set_scanout (the 8*24 GC's
// VidComm too), the Voodoo2 by clamping its screen to its scanout allocation.
// This is the consumer's own ceiling, not the guarantee that a descriptor is backed.
static bool fb_fits_scratch(const display_t *d) {
    return (uint64_t)d->stride * d->height <= MAX_FB_BYTES;
}

// WebGL resources
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE s_ctx = 0;
static GLuint s_vbo = 0;
static GLuint s_fb_tex = 0; // framebuffer texture (format depends on pixel_format)
static GLuint s_clut_tex = 0; // 256x1 RGBA texture for indexed-format CLUTs
static GLuint s_response_tex = 0; // 256x3 R8 texture — per-channel CRT response LUT
// The identity CRT response (every row 0..255), filled once in init_gl and
// uploaded whenever the display's monitor has no response table of its own.
static uint8_t s_identity_response[3 * 256];

// Per-format shader programs.  Indexed by pixel_format_t.  A NULL slot means
// "no program for this format yet" — fall back to the 1bpp program with
// black output to keep the renderer alive.
#define NUM_FORMATS 7
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

// The render thread's copy of the frame it is uploading: MAX_FB_BYTES,
// allocated by init_gl once the context exists.  Filled from the slot under
// its lock, uploaded outside it.
static uint8_t *s_upload_scratch;

// The canvas's intrinsic size as last set (resize_canvas), 0 before the first
static uint32_t s_canvas_w, s_canvas_h;

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

// 32 bpp direct (PIXEL_32BPP_XRGB): the framebuffer is 00RRGGBB stored
// big-endian, so the bytes in memory are [00, RR, GG, BB].  Uploaded as
// RGBA8 that lands in the texel as (r=00, g=RR, b=GG, a=BB) — so the real
// colour is (texel.g, texel.b, texel.a).  u_stride is the texel width
// (stride/4, set by refresh_from_display), one texel per pixel.  Route
// each channel through the CRT response LUT exactly like the 8bpp path.
static const char *FS_32BPP = "#version 300 es\n"
                              "precision mediump float;\n"
                              "uniform sampler2D u_texture;\n"
                              "uniform sampler2D u_response;\n"
                              "uniform vec2 u_fb_size;\n"
                              "uniform float u_stride;\n"
                              "in  vec2 v_uv;\n"
                              "out vec4 fragColor;\n"
                              "void main() {\n"
                              "    vec2 px      = v_uv * u_fb_size;\n"
                              "    float x      = floor(px.x);\n"
                              "    float y      = floor(px.y);\n"
                              "    vec2 tc      = vec2((x + 0.5) / u_stride,\n"
                              "                        (y + 0.5) / u_fb_size.y);\n"
                              "    vec4 texel   = texture(u_texture, tc);\n"
                              "    float r_out  = texture(u_response, vec2(texel.g, 0.5 / 3.0)).r;\n"
                              "    float g_out  = texture(u_response, vec2(texel.b, 1.5 / 3.0)).r;\n"
                              "    float b_out  = texture(u_response, vec2(texel.a, 2.5 / 3.0)).r;\n"
                              "    fragColor    = vec4(r_out, g_out, b_out, 1.0);\n"
                              "}\n";

// 16 bpp direct (PIXEL_16BPP_555): 0RRRRRGGGGGBBBBB stored big-endian, so
// the two bytes per pixel are [hi, lo] with hi = 0RRRRRGG, lo = GGGBBBBB.
// The texture is R8 (u_stride = byte stride), so pixel x reads bytes 2x and
// 2x+1.  highp is needed for the 15-bit reconstruction.  5-bit channels
// scale to [0,1] via /31, then through the CRT response LUT.
static const char *FS_16BPP =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_texture;\n"
    "uniform sampler2D u_response;\n"
    "uniform vec2 u_fb_size;\n"
    "uniform float u_stride;\n"
    "in  vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec2 px      = v_uv * u_fb_size;\n"
    "    float x      = floor(px.x);\n"
    "    float y      = floor(px.y);\n"
    "    float row    = (y + 0.5) / u_fb_size.y;\n"
    "    float hi     = floor(texture(u_texture, vec2((2.0 * x + 0.5) / u_stride, row)).r * 255.0 + 0.5);\n"
    "    float lo     = floor(texture(u_texture, vec2((2.0 * x + 1.5) / u_stride, row)).r * 255.0 + 0.5);\n"
    "    float v      = hi * 256.0 + lo;\n"
    "    float r5     = mod(floor(v / 1024.0), 32.0);\n"
    "    float g5     = mod(floor(v / 32.0), 32.0);\n"
    "    float b5     = mod(v, 32.0);\n"
    "    float r_out  = texture(u_response, vec2(r5 / 31.0, 0.5 / 3.0)).r;\n"
    "    float g_out  = texture(u_response, vec2(g5 / 31.0, 1.5 / 3.0)).r;\n"
    "    float b_out  = texture(u_response, vec2(b5 / 31.0, 2.5 / 3.0)).r;\n"
    "    fragColor    = vec4(r_out, g_out, b_out, 1.0);\n"
    "}\n";

// 16 bpp direct (PIXEL_16BPP_565): RRRRRGGGGGGBBBBB stored big-endian, so
// the two bytes per pixel are [hi, lo] with hi = RRRRRGGG, lo = GGGBBBBB.
// Same R8 texture walk as the 5-5-5 shader; the green channel is 6 bits
// wide so it scales via /63 where red and blue scale via /31.
static const char *FS_16BPP_565 =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_texture;\n"
    "uniform sampler2D u_response;\n"
    "uniform vec2 u_fb_size;\n"
    "uniform float u_stride;\n"
    "in  vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec2 px      = v_uv * u_fb_size;\n"
    "    float x      = floor(px.x);\n"
    "    float y      = floor(px.y);\n"
    "    float row    = (y + 0.5) / u_fb_size.y;\n"
    "    float hi     = floor(texture(u_texture, vec2((2.0 * x + 0.5) / u_stride, row)).r * 255.0 + 0.5);\n"
    "    float lo     = floor(texture(u_texture, vec2((2.0 * x + 1.5) / u_stride, row)).r * 255.0 + 0.5);\n"
    "    float v      = hi * 256.0 + lo;\n"
    "    float r5     = floor(v / 2048.0);\n"
    "    float g6     = mod(floor(v / 32.0), 64.0);\n"
    "    float b5     = mod(v, 32.0);\n"
    "    float r_out  = texture(u_response, vec2(r5 / 31.0, 0.5 / 3.0)).r;\n"
    "    float g_out  = texture(u_response, vec2(g6 / 63.0, 1.5 / 3.0)).r;\n"
    "    float b_out  = texture(u_response, vec2(b5 / 31.0, 2.5 / 3.0)).r;\n"
    "    fragColor    = vec4(r_out, g_out, b_out, 1.0);\n"
    "}\n";

// 2 bpp and 4 bpp indexed.  The 8 bpp shader with the byte unpacked into four
// or two pixels, MSB first -- the Mac convention display.h's decoder follows.
//
// These were grey stubs, justified as "no shipping card boots into them".
// Booting is not the only way in: six producers switch into
// 2 and 4 bpp at runtime from the Monitors control panel, and PNG capture has
// always decoded them correctly -- so the browser showed a flat grey field for
// a desktop the goldens rendered properly.  The divergence was in the renderer
// and only there; no captured pixel changes with this.
//
// The unpack stays in float arithmetic: the byte arrives normalised from an R8
// texture, so recover it, divide down to the wanted field, take the remainder.
static const char *FS_2BPP = "#version 300 es\n"
                             "precision mediump float;\n"
                             "uniform sampler2D u_texture;\n"
                             "uniform sampler2D u_clut;\n"
                             "uniform sampler2D u_response;\n"
                             "uniform vec2 u_fb_size;\n"
                             "uniform float u_stride;\n"
                             "in  vec2 v_uv;\n"
                             "out vec4 fragColor;\n"
                             "void main() {\n"
                             "    vec2 px      = v_uv * u_fb_size;\n"
                             "    float x      = floor(px.x);\n"
                             "    float y      = floor(px.y);\n"
                             "    float byte_x = floor(x / 4.0);\n"
                             "    vec2 tc      = vec2((byte_x + 0.5) / u_stride,\n"
                             "                        (y + 0.5) / u_fb_size.y);\n"
                             "    float b      = floor(texture(u_texture, tc).r * 255.0 + 0.5);\n"
                             "    float shift  = (4.0 - 1.0 - mod(x, 4.0)) * 2.0;\n"
                             "    float idx    = mod(floor(b / pow(2.0, shift)), 4.0);\n"
                             "    vec4 entry   = texture(u_clut, vec2((idx + 0.5) / 256.0, 0.5));\n"
                             "    float r_out  = texture(u_response, vec2(entry.r, 0.5 / 3.0)).r;\n"
                             "    float g_out  = texture(u_response, vec2(entry.g, 1.5 / 3.0)).r;\n"
                             "    float b_out  = texture(u_response, vec2(entry.b, 2.5 / 3.0)).r;\n"
                             "    fragColor    = vec4(r_out, g_out, b_out, 1.0);\n"
                             "}\n";

static const char *FS_4BPP = "#version 300 es\n"
                             "precision mediump float;\n"
                             "uniform sampler2D u_texture;\n"
                             "uniform sampler2D u_clut;\n"
                             "uniform sampler2D u_response;\n"
                             "uniform vec2 u_fb_size;\n"
                             "uniform float u_stride;\n"
                             "in  vec2 v_uv;\n"
                             "out vec4 fragColor;\n"
                             "void main() {\n"
                             "    vec2 px      = v_uv * u_fb_size;\n"
                             "    float x      = floor(px.x);\n"
                             "    float y      = floor(px.y);\n"
                             "    float byte_x = floor(x / 2.0);\n"
                             "    vec2 tc      = vec2((byte_x + 0.5) / u_stride,\n"
                             "                        (y + 0.5) / u_fb_size.y);\n"
                             "    float b      = floor(texture(u_texture, tc).r * 255.0 + 0.5);\n"
                             "    float shift  = (2.0 - 1.0 - mod(x, 2.0)) * 4.0;\n"
                             "    float idx    = mod(floor(b / pow(2.0, shift)), 16.0);\n"
                             "    vec4 entry   = texture(u_clut, vec2((idx + 0.5) / 256.0, 0.5));\n"
                             "    float r_out  = texture(u_response, vec2(entry.r, 0.5 / 3.0)).r;\n"
                             "    float g_out  = texture(u_response, vec2(entry.g, 1.5 / 3.0)).r;\n"
                             "    float b_out  = texture(u_response, vec2(entry.b, 2.5 / 3.0)).r;\n"
                             "    fragColor    = vec4(r_out, g_out, b_out, 1.0);\n"
                             "}\n";

// ============================================================================
// Static helpers
// ============================================================================

// Log a shader's or program's info log in full (`what` names the step).
// Driver logs run past any fixed buffer, and the truncated part is the
// useful one: the final error line.
static void log_info_log(GLuint obj, bool is_program, const char *what) {
    GLint len = 0;
    if (is_program)
        glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len);
    else
        glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &len);
    char *log = len > 0 ? malloc((size_t)len) : NULL;
    if (!log) {
        LOG(0, "%s failed (no info log)", what);
        return;
    }
    if (is_program)
        glGetProgramInfoLog(obj, len, NULL, log);
    else
        glGetShaderInfoLog(obj, len, NULL, log);
    LOG(0, "%s failed: %s", what, log);
    free(log);
}

// Compile a shader.  Returns 0 (logged, nothing leaked) if compilation failed.
static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        log_info_log(s, false, "shader compile");
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// Link a vertex+fragment pair into a program; stash uniform locations.
static GLuint link_program(const char *vs_src, const char *fs_src, prog_uniforms_t *u_out) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        glDeleteShader(vs); // deleting 0 is a no-op
        glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_position");
    glLinkProgram(prog);
    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); // flagged for deletion; freed with the program
    glDeleteShader(fs);
    if (!ok) {
        log_info_log(prog, true, "shader link");
        glDeleteProgram(prog);
        return 0;
    }
    if (u_out) {
        u_out->u_fb_size = glGetUniformLocation(prog, "u_fb_size");
        u_out->u_stride = glGetUniformLocation(prog, "u_stride");
        u_out->u_texture = glGetUniformLocation(prog, "u_texture");
        u_out->u_clut = glGetUniformLocation(prog, "u_clut");
        u_out->u_response = glGetUniformLocation(prog, "u_response");
    }
    return prog;
}

// Build the per-format program table: one program per pixel_format_t.
static void init_programs(void) {
    s_progs[PIXEL_1BPP_MSB] = link_program(VS_SHARED, FS_1BPP, &s_uniforms[PIXEL_1BPP_MSB]);
    s_progs[PIXEL_2BPP_MSB] = link_program(VS_SHARED, FS_2BPP, &s_uniforms[PIXEL_2BPP_MSB]);
    s_progs[PIXEL_4BPP_MSB] = link_program(VS_SHARED, FS_4BPP, &s_uniforms[PIXEL_4BPP_MSB]);
    s_progs[PIXEL_8BPP] = link_program(VS_SHARED, FS_8BPP, &s_uniforms[PIXEL_8BPP]);
    s_progs[PIXEL_16BPP_555] = link_program(VS_SHARED, FS_16BPP, &s_uniforms[PIXEL_16BPP_555]);
    s_progs[PIXEL_32BPP_XRGB] = link_program(VS_SHARED, FS_32BPP, &s_uniforms[PIXEL_32BPP_XRGB]);
    s_progs[PIXEL_16BPP_565] = link_program(VS_SHARED, FS_16BPP_565, &s_uniforms[PIXEL_16BPP_565]);
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
    case PIXEL_16BPP_565:
        internal = GL_R8; // raw big-endian bytes; FS_16BPP* decode the pixel
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

// ============================================================================
// The frame slot (core thread -> render thread)
// ============================================================================

// The newest published frame.  The core thread is the only writer of
// everything but the consumed flags; it writes under `lock`, and it may read
// `pixels` without the lock (nobody else writes them).  The render thread
// reads under the lock.  The change flags accumulate across publishes the
// renderer has not taken yet (it draws the newest frame, not every one) and
// are cleared by the renderer when it takes them.
typedef struct frame_slot {
    pthread_mutex_t lock;
    uint32_t seq; // bumped by every publish
    // The frame
    bool blank; // show black (no display or no framebuffer)
    pixel_format_t format;
    uint32_t width, height, stride;
    uint8_t *pixels; // MAX_FB_BYTES; stride * height valid
    uint8_t clut[256 * 4]; // RGBA, unused entries black
    uint8_t response[3 * 256]; // the composed per-channel response LUT
    // Accumulated changes since the renderer last took the slot
    bool shape, clut_changed, response_changed;
    bool hide_overlay; // hide the Voodoo2 overlay once the canvas shows this frame
} frame_slot_t;

static frame_slot_t s_slot = {.lock = PTHREAD_MUTEX_INITIALIZER};

// Core-thread state of the publisher.
static bool s_published_any; // the slot's pixels hold a real frame
static bool s_overlay_up; // the Voodoo2 overlay is (or may be) shown
static bool s_blanked; // the last publish was a blank: the next frame goes in full

// Formats the renderer has no program for (bit per pixel_format_t), set by
// the render thread and logged once by the core thread (the log is the
// core's).
static _Atomic uint32_t s_refused_formats;
static uint32_t s_refused_logged;

// The render thread is up and owns the canvas (0 until init_gl ran there).
static _Atomic int s_renderer_up;

// Why the renderer gave up for good, set once by the render thread and
// reported once by the core thread (report_render_failure): events and the
// log belong to the core.  A WebGL context the browser has lost answers
// every query with null, and Emscripten's canvas resize dereferences that
// null and kills the thread -- so the renderer stops before any GL work
// instead.  Safari can hand out contexts that are lost from the start, a
// state only restarting the browser clears.
typedef enum render_failure {
    RENDER_OK = 0,
    RENDER_NO_CONTEXT, // emscripten_webgl_create_context failed
    RENDER_CONTEXT_DEAD, // lost while the renderer came up
    RENDER_CONTEXT_LOST, // lost later, between frames
    RENDER_SHADER_FAILED, // the driver would not build the display shaders
} render_failure_t;
static _Atomic int s_render_failure;
static bool s_render_failure_reported;

// (render thread) Record why the renderer gave up; the first reason stays.
static void render_failed(render_failure_t why) {
    int none = RENDER_OK;
    atomic_compare_exchange_strong(&s_render_failure, &none, (int)why);
}

// ---- render thread: GL uploads -------------------------------------------

static void upload_fb(pixel_format_t format, uint32_t stride, uint32_t height) {
    GLenum src_fmt = (format == PIXEL_32BPP_XRGB) ? GL_RGBA : GL_RED;
    uint32_t tex_width = (format == PIXEL_32BPP_XRGB) ? stride / 4 : stride;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_fb_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_width, height, src_fmt, GL_UNSIGNED_BYTE, s_upload_scratch);
}

// Unit 1 / unit 2 explicitly, so these never clobber the framebuffer
// texture's binding on unit 0.
static void upload_clut(const uint8_t *rgba) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_clut_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

static void upload_response(const uint8_t *table) {
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s_response_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 3, GL_RED, GL_UNSIGNED_BYTE, table);
}

// Resize the canvas element + viewport to match the live display.  The
// CSS scale (zoom) is applied JS-side via the screen-wrapper element; here
// we set the *intrinsic* canvas resolution so 1 canvas pixel == 1 emulator
// pixel.  The page learns the geometry from the screen event, not from here.
// Only on an actual change: a shape change that keeps the size (a depth
// switch) or a forced full redraw does not touch the canvas element.
static void resize_canvas(uint32_t width, uint32_t height) {
    if (width == s_canvas_w && height == s_canvas_h)
        return;
    s_canvas_w = width;
    s_canvas_h = height;
    emscripten_set_canvas_element_size("#screen", (int)width, (int)height);
    glViewport(0, 0, (int)width, (int)height);
}

// (core thread) Announce the display's geometry to the page (a `screen` state event):
// the framebuffer size and the monitor's pixel aspect ratio, so the page can
// reflow its screen wrapper and show non-square pixels (the Lisa's 720x364
// raster is 2:3).  Sent where a shape change is consumed and once when a
// machine is attached; the page holds the only copy.
static void announce_geometry(const display_t *d) {
    event_emitf(GS_EVENT_STATE, "{\"event\":\"screen\",\"width\":%u,\"height\":%u,\"par_w\":%u,\"par_h\":%u}", d->width,
                d->height, d->par_w ? d->par_w : 1u, d->par_h ? d->par_h : 1u);
}

// ============================================================================
// Lifecycle (render thread)
// ============================================================================

// Create the WebGL2 context and everything the renderer keeps for its
// lifetime.  Once only: em_video_init guards against a second call.
static void init_gl(void) {
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.majorVersion = 2;
    attr.minorVersion = 0;
    s_ctx = emscripten_webgl_create_context("#screen", &attr);
    if (s_ctx == 0) { // the handle is unsigned; 0 means no context
        // No newline on the old printf here, so this one ran into whatever came
        // next in the output.  LOG ends its own lines.
        LOG(0, "WebGL 2 not supported - cannot run");
        render_failed(RENDER_NO_CONTEXT);
        return;
    }
    emscripten_webgl_make_context_current(s_ctx);
    if (emscripten_is_webgl_context_lost(s_ctx)) { // created, but dead on arrival
        render_failed(RENDER_CONTEXT_DEAD);
        return;
    }

    s_upload_scratch = calloc(1, MAX_FB_BYTES);
    if (!s_upload_scratch) {
        LOG(0, "renderer: cannot allocate the %u-byte upload buffer - cannot run", (unsigned)MAX_FB_BYTES);
        return;
    }

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
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++)
            s_identity_response[c * 256 + v] = (uint8_t)v;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 3, 0, GL_RED, GL_UNSIGNED_BYTE, s_identity_response);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Every shader is plain GLSL ES 3.00: one the driver refuses means its
    // WebGL is broken (a lost context fails every compile the same way).
    if (emscripten_is_webgl_context_lost(s_ctx)) {
        render_failed(RENDER_CONTEXT_DEAD);
        return;
    }
    for (int f = 0; f < NUM_FORMATS; f++)
        if (!s_progs[f]) {
            render_failed(RENDER_SHADER_FAILED);
            return;
        }
}

// Bind the program for `format` and point its uniforms at the geometry and
// the three texture units, (re)allocate the framebuffer texture and size the
// canvas.  False when this build has no program for the format: refused
// rather than rendered through another format's program (program_for's 1 bpp
// fallback would sample an RGBA8 texture with byte-index maths).
static bool apply_shape(pixel_format_t format, uint32_t width, uint32_t height, uint32_t stride) {
    if (format < 0 || (int)format >= NUM_FORMATS || !s_progs[format]) {
        int slot = (format >= 0 && (int)format < NUM_FORMATS) ? (int)format : NUM_FORMATS;
        atomic_fetch_or(&s_refused_formats, 1u << slot);
        return false;
    }
    glUseProgram(program_for(format));
    const prog_uniforms_t *u = uniforms_for(format);
    if (u->u_fb_size >= 0)
        glUniform2f(u->u_fb_size, (float)width, (float)height);
    if (u->u_stride >= 0)
        glUniform1f(u->u_stride, (float)((format == PIXEL_32BPP_XRGB) ? stride / 4 : stride));
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
    allocate_fb_texture(format, stride, height);
    resize_canvas(width, height);
    return true;
}

// Draw the quad.  (glGetError, a pipeline sync point, is not asked here:
// the log belongs to the core thread.)
static void draw(void) {
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void hide_overlay(void) {
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM({ if (typeof Module.onVoodooGpuOverlay === 'function') Module.onVoodooGpuOverlay(0); });
    // clang-format on
}

// One animation frame of the render thread: take the newest published
// frame, if there is one it has not drawn, and draw it.
static uint32_t s_drawn_seq;
static bool s_shape_ok; // the current shape has a program (apply_shape succeeded)

static void render_frame(void) {
    static uint8_t clut[256 * 4], response[3 * 256];
    // A lost context: stop drawing for good, before any GL call.
    if (emscripten_is_webgl_context_lost(s_ctx)) {
        render_failed(RENDER_CONTEXT_LOST);
        emscripten_cancel_main_loop();
        return;
    }
    pthread_mutex_lock(&s_slot.lock);
    bool fresh = s_slot.seq != s_drawn_seq;
    bool hide = s_slot.hide_overlay;
    s_slot.hide_overlay = false;
    bool blank = s_slot.blank, shape = false, clut_new = false, response_new = false;
    pixel_format_t format = s_slot.format;
    uint32_t width = s_slot.width, height = s_slot.height, stride = s_slot.stride;
    if (fresh) {
        s_drawn_seq = s_slot.seq;
        shape = s_slot.shape;
        clut_new = s_slot.clut_changed;
        response_new = s_slot.response_changed;
        s_slot.shape = s_slot.clut_changed = s_slot.response_changed = false;
        if (!blank) {
            memcpy(s_upload_scratch, s_slot.pixels, (size_t)stride * height);
            if (clut_new)
                memcpy(clut, s_slot.clut, sizeof clut);
            if (response_new)
                memcpy(response, s_slot.response, sizeof response);
        }
    }
    pthread_mutex_unlock(&s_slot.lock);

    if (fresh) {
        if (blank) {
            // No signal: black, as a monitor would show, not the last frame.
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
        } else {
            if (shape)
                s_shape_ok = apply_shape(format, width, height, stride);
            if (s_shape_ok) {
                upload_fb(format, stride, height);
                if (clut_new)
                    upload_clut(clut);
                if (response_new)
                    upload_response(response);
                draw();
            }
        }
    }
    // The canvas shows the newest frame now (or already did): the overlay
    // may go.
    if (hide)
        hide_overlay();
}

// The render thread: bring WebGL up on the transferred canvas, then draw on
// every animation frame (a worker's requestAnimationFrame) for good.
static void *render_thread_main(void *arg) {
    (void)arg;
    init_gl();
    if (!s_upload_scratch || atomic_load(&s_render_failure) != RENDER_OK)
        return NULL; // no working WebGL: nothing renders (init_gl said why)
    atomic_store(&s_renderer_up, 1);
    emscripten_set_main_loop(render_frame, 0, 1); // does not return
    return NULL;
}

// ============================================================================
// The core thread: publishing
// ============================================================================

// Publish a blank (no display, or one without a framebuffer -- the ANS
// between a restart and Open Firmware reprogramming the Cirrus).  Once per
// transition.
static void publish_blank(void) {
    if (s_blanked)
        return;
    s_blanked = true;
    pthread_mutex_lock(&s_slot.lock);
    s_slot.blank = true;
    s_slot.hide_overlay = s_slot.hide_overlay || s_overlay_up;
    s_slot.seq++;
    pthread_mutex_unlock(&s_slot.lock);
    s_overlay_up = false;
}

// Publish `d` in full or in the parts its dirty flags (and `full`) name,
// then clear the flags.  The pixels are always copied whole: the renderer
// draws whatever the slot holds.  `hide` asks the renderer to hide the
// overlay once this frame is on the canvas.
static void publish(display_t *d, bool full, bool hide) {
    bool shape = full || d->shape_dirty || !s_published_any;
    bool clut = (full || d->clut_dirty || shape) && d->clut && d->clut_len > 0;
    bool response = full || d->response_dirty || shape;
    pthread_mutex_lock(&s_slot.lock);
    s_slot.blank = false;
    if (shape) {
        s_slot.format = d->format;
        s_slot.width = d->width;
        s_slot.height = d->height;
        s_slot.stride = d->stride;
        s_slot.shape = true;
    }
    memcpy(s_slot.pixels, d->bits, (size_t)d->stride * d->height);
    if (clut) {
        // Unused entries black, so an out-of-range index samples no stale colour.
        memset(s_slot.clut, 0, sizeof s_slot.clut);
        uint32_t n = d->clut_len > 256 ? 256 : d->clut_len;
        for (uint32_t i = 0; i < n; i++) {
            s_slot.clut[i * 4 + 0] = d->clut[i].r;
            s_slot.clut[i * 4 + 1] = d->clut[i].g;
            s_slot.clut[i * 4 + 2] = d->clut[i].b;
            s_slot.clut[i * 4 + 3] = 255;
        }
        s_slot.clut_changed = true;
    }
    if (response) {
        // The card's direct-colour DAC table (dac_lut) followed by the
        // monitor's CRT response (crt_response), composed into one 3x256
        // table; with neither, the identity.
        for (int c = 0; c < 3; c++)
            for (int v = 0; v < 256; v++) {
                int in = d->dac_lut ? d->dac_lut[c][v] : v;
                s_slot.response[c * 256 + v] = d->crt_response ? d->crt_response[c][in] : (uint8_t)in;
            }
        s_slot.response_changed = true;
    }
    if (hide)
        s_slot.hide_overlay = true;
    s_slot.seq++;
    pthread_mutex_unlock(&s_slot.lock);
    if (d->shape_dirty)
        announce_geometry(d); // a producer changed the shape: tell the page
    s_published_any = true;
    s_blanked = false;
    // Consumer-cleared, plain bools: the producers (the machine's video
    // hardware) and the publisher both run on the emulator thread.
    d->fb_dirty = false;
    d->shape_dirty = false;
    d->clut_dirty = false;
    d->response_dirty = false;
}

// Ask the renderer to hide the overlay without a new frame (the canvas
// already shows the newest one, or will once the renderer takes it).
static void request_overlay_hide(void) {
    if (!s_overlay_up)
        return;
    s_overlay_up = false;
    pthread_mutex_lock(&s_slot.lock);
    s_slot.hide_overlay = true;
    pthread_mutex_unlock(&s_slot.lock);
}

// (core thread) Report the renderer's failure, once: log it and tell the
// page (a `state:renderer` event), which replaces the UI with an
// explanation and what to try -- usually restarting the browser.
static void report_render_failure(void) {
    if (s_render_failure_reported)
        return;
    const char *status, *detail;
    switch ((render_failure_t)atomic_load(&s_render_failure)) {
    case RENDER_NO_CONTEXT:
        status = "no_context";
        detail = "no WebGL 2 context for the screen canvas in the render thread";
        break;
    case RENDER_CONTEXT_DEAD:
        status = "context_dead";
        detail = "the WebGL 2 context was lost as soon as it was created";
        break;
    case RENDER_CONTEXT_LOST:
        status = "context_lost";
        detail = "the browser lost the WebGL 2 context";
        break;
    case RENDER_SHADER_FAILED:
        status = "shader_failed";
        detail = "the WebGL driver would not build the display shaders";
        break;
    default:
        return; // rendering fine
    }
    s_render_failure_reported = true;
    LOG(0, "renderer: %s -- %s", status, detail);
    // Both strings are literals without quotes or backslashes: valid JSON as is.
    event_emitf(GS_EVENT_STATE, "{\"event\":\"renderer\",\"status\":\"%s\",\"detail\":\"%s\"}", status, detail);
}

static void log_refused_formats(void) {
    uint32_t refused = atomic_load(&s_refused_formats) & ~s_refused_logged;
    for (int f = 0; refused; f++, refused >>= 1)
        if (refused & 1u) {
            s_refused_logged |= 1u << f;
            LOG(0, "renderer: no shader program for %s -- frames in this format are refused",
                f < NUM_FORMATS ? display_format_name((pixel_format_t)f) : "an unknown format");
        }
}

// ============================================================================
// Public API (core thread)
// ============================================================================

// Start the render thread and hand it the canvas; a second call is a no-op.
// The canvas goes by its element id ("screen", no '#'): that is the key
// under which this thread holds it (-sOFFSCREENCANVASES_TO_PTHREAD gave it
// to the core's thread), and a pthread can pass on only a canvas it holds.
void em_video_init(void) {
    static bool started;
    if (started)
        return;
    started = true;
    s_slot.pixels = calloc(1, MAX_FB_BYTES);
    if (!s_slot.pixels) {
        LOG(0, "renderer: cannot allocate the %u-byte frame slot - cannot run", (unsigned)MAX_FB_BYTES);
        return;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    emscripten_pthread_attr_settransferredcanvases(&attr, "screen");
    pthread_t thread;
    int rc = pthread_create(&thread, &attr, render_thread_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        LOG(0, "renderer: the render thread could not be started (%d) - nothing will be drawn", rc);
    else
        pthread_detach(thread);
}

void em_video_update(void) {
    if (!s_slot.pixels)
        return; // no frame slot (allocation failed)
    report_render_failure();
    log_refused_formats();
    display_t *d = system_display();
    if (!d || !d->bits) {
        publish_blank(); // ...and hide the overlay: nothing for it to cover
        return;
    }
    if (!fb_fits_scratch(d)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG(0, "renderer: mode %ux%u stride %u exceeds the %u-byte frame buffer; screen frozen", d->width,
                d->height, d->stride, (unsigned)MAX_FB_BYTES);
        }
        return;
    }

    // A frame presented by someone else (the Voodoo2's WebGPU overlay):
    // keep the canvas at the card's geometry so the page lays out the
    // overlay over it, but never send the pixels' changes -- the stale
    // scanout underneath is hidden.
    if (d->presented_externally) {
        s_overlay_up = true;
        if (d->shape_dirty)
            publish(d, false, false);
        return;
    }

    // A card that builds its frame on demand (the Mach64's hardware-cursor
    // composite) builds it now
    display_sync_pixels(d);

    // The producer-side dirty flags are undercomplete: pixel writes from
    // emulated CPU code (the ROM's RAM test, the OS painting between
    // vsyncs, any direct framebuffer poke) never trip fb_dirty, and
    // plus_use_video_buffer sets it on every VIA1 port-A write whether
    // pixels moved or not.  So fb_dirty is a hint: content change is
    // re-derived by comparing the framebuffer with the slot's pixels, the
    // last frame published (only this thread writes them, so no lock).
    bool content_changed = !s_published_any || s_slot.stride != d->stride || s_slot.height != d->height ||
                           memcmp(s_slot.pixels, d->bits, (size_t)d->stride * d->height) != 0;
    if (!s_blanked && !content_changed && !d->shape_dirty && !d->clut_dirty && !d->response_dirty) {
        request_overlay_hide(); // the canvas already shows this frame
        return;
    }
    bool hide = s_overlay_up;
    s_overlay_up = false;
    publish(d, /*full*/ s_blanked, hide);
}

// Everything again, whatever the dirty flags say (a forced redraw).
void em_video_force_redraw(void) {
    if (!s_slot.pixels)
        return;
    display_t *d = system_display();
    if (!d || !d->bits || !fb_fits_scratch(d))
        return;
    if (!d->presented_externally)
        display_sync_pixels(d);
    publish(d, /*full*/ true, false);
}

void platform_force_redraw(void) {
    em_video_force_redraw();
}

// A machine was attached: announce its display's geometry before any frame,
// so a restored machine whose mode never changes in this page still shows
// at the right size.
void em_video_machine_attached(void) {
    display_t *d = system_display();
    if (d && d->width && d->height)
        announce_geometry(d);
}
