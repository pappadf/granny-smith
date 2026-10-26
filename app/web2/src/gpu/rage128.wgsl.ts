// The Rage 128 WebGPU takeover's shaders.  The fragment shader is the 3D
// engine's per-pixel pipe — texel fetch and conversion, the two combine
// stages, texture lighting, the colour key, specular, fog, the alpha
// test, the Z code, dither and pack — written in i32/u32 arithmetic beside
// the C (shade() and its helpers in rage128_raster.c), stage for stage and
// name for name, so the two can be read against each other.  The GPU
// contributes coverage, interpolation, the depth compare and the blend.
//
// Triangle set-up is not here: the walker's own code snaps, culls and
// flat-shades each triangle and evaluates its attributes at the vertices
// (rage128_gpu.c), and every attribute is interpolated linearly in screen
// space, as the walker's planes are.  Pixel centres are at (x + 0.5,
// y + 0.5) on both sides, so no offset is needed.
//
// Textures arrive as rg32uint atlases — level L below level L-1, each at
// its own width — holding every texel twice: converted to ARGB8888 by the
// walker's own texel conversion (r128_3d_texel_argb, alpha kept) and raw,
// for the colour key.  TEX_MAP_AEN, the draw's, is applied here.

export const RAGE128_WGSL = /* wgsl */ `
struct Tex {
  on: u32, cntl: u32, comb: u32, fmt: u32,
  lpitch: u32, lheight: u32, top: u32, border: u32,
  flags: u32, p0: u32, p1: u32, p2: u32,
  p3: u32, p4: u32, p5: u32, p6: u32,
};

struct Uniforms {
  tex_cntl: u32, misc: u32, scale: u32, zs: u32,
  constant: u32, fog_color: u32, key: u32, key_mask: u32,
  dst_type: u32, flags: u32, z_bits: u32, aux_cntl: u32,
  aux: array<vec4<i32>, 3>,
  tex: array<Tex, 2>,
  fog: array<vec4<u32>, 16>,
  target_w: f32, target_h: f32, p0: u32, p1: u32,
  p2: vec4<u32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var atlas0: texture_2d<u32>;
@group(0) @binding(2) var atlas1: texture_2d<u32>;

// TEX_CNTL_C.
const TC_TEX_EN: u32 = 0x10u;
const TC_FOG_EN: u32 = 0x80u;
const TC_DITHER_EN: u32 = 0x100u;
const TC_ALPHA_TST: u32 = 0x400u;
const TC_SPECULAR: u32 = 0x800u;
const TC_CHROMA_KEY: u32 = 0x1000u;
const TC_AMASK: u32 = 0x2000u;
// SCALE_3D_CNTL.
const S3_DITHER_TABLE: u32 = 0x2u;
const S3_DITHER_INIT: u32 = 0x8u;
const S3_ROUND_EN: u32 = 0x10u;
const S3_TEX_MAP_AEN: u32 = 0x40000000u;
// MISC_3D_STATE_CNTL_REG.
const MISC_FOG_TABLE: u32 = 0x4000u;
// The texture unit's control.
const TX_MIP_DIS: u32 = 0x80u;
const MIN_NEAREST_MIP: u32 = 2u;
const MIN_LINEAR: u32 = 1u;
const MIN_LINEAR_MIP: u32 = 3u;
const MIN_TRILINEAR: u32 = 5u;
// Uniform flags.
const F_BLENDING: u32 = 1u;

struct VIn {
  @location(0) pos: vec2<f32>,
  @location(1) z: f32,
  @location(2) rgba: vec4<f32>,
  @location(3) spec: vec3<f32>,
  @location(4) fog: f32,
  @location(5) t0: vec3<f32>,
  @location(6) t1: vec3<f32>,
};

struct VOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) @interpolate(linear) rgba: vec4<f32>,
  @location(1) @interpolate(linear) spec: vec3<f32>,
  @location(2) @interpolate(linear) zf: vec2<f32>,
  @location(3) @interpolate(linear) t0: vec3<f32>,
  @location(4) @interpolate(linear) t1: vec3<f32>,
};

@vertex fn vs_main(in: VIn) -> VOut {
  var o: VOut;
  o.pos = vec4<f32>(in.pos.x / u.target_w * 2.0 - 1.0, 1.0 - in.pos.y / u.target_h * 2.0, 0.5, 1.0);
  o.rgba = in.rgba;
  o.spec = in.spec;
  o.zf = vec2<f32>(in.z, in.fog);
  o.t0 = in.t0;
  o.t1 = in.t1;
  return o;
}

// A software adapter (SwiftShader) inlines every call, so each costly
// function below has ONE call site, inside a loop where the C walks the
// same thing several times (taps, levels, units, channels).

fn clamp255(v: i32) -> i32 { return clamp(v, 0, 255); }
fn mul255(a: i32, b: i32) -> i32 { return (a * b + 127) / 255; }

fn unpack_argb(v: u32) -> vec4<i32> {
  return vec4<i32>(i32((v >> 16u) & 0xFFu), i32((v >> 8u) & 0xFFu), i32(v & 0xFFu), i32(v >> 24u));
}

fn fog_entry(i: u32) -> i32 {
  let w = u.fog[i >> 4u][(i >> 2u) & 3u];
  return i32((w >> (8u * (i & 3u))) & 0xFFu);
}

// The clamp modes: a texel index, or -1 for the border.
fn wrap(i: i32, n: i32, mode: u32) -> i32 {
  let m = ((i % (2 * n)) + 2 * n) % (2 * n);
  if (mode == 0u) { return ((i % n) + n) % n; }
  if (mode == 1u) { return select(2 * n - 1 - m, m, m < n); }
  if (mode == 2u) { return clamp(i, 0, n - 1); }
  return select(i, -1, i < 0 || i >= n);
}

fn level_w(t: Tex, lev: u32) -> u32 { return 1u << select(0u, t.lpitch - lev, t.lpitch > lev); }
fn level_h(t: Tex, lev: u32) -> u32 { return 1u << select(0u, t.lheight - lev, t.lheight > lev); }
// The atlas row of a level: the heights of the levels above it, in closed
// form (2^(L+1) − 2^(L+1−m) for the halving ones, then 1 each).
fn level_row(t: Tex, lev: u32) -> u32 {
  let l1 = t.lheight + 1u;
  let m = min(lev, l1);
  var row = (1u << l1) - (1u << (l1 - m));
  if (lev > l1) { row += lev - l1; }
  return row;
}

fn lerp_argb(a: u32, b: u32, f: u32) -> u32 {
  var out = 0u;
  for (var k = 0u; k < 32u; k += 8u) {
    let x = (a >> k) & 0xFFu;
    let y = (b >> k) & 0xFFu;
    out |= ((x * (256u - f) + y * f + 128u) >> 8u) << k;
  }
  return out;
}

// sample() of rage128_raster.c: the level(s), the filter, then up to two
// levels of up to four taps, walked in loops around one texel fetch.
// Returns (ARGB, the first tap's raw texel).
fn sample(unit: u32, t: Tex, sc: f32, tc: f32, lod: f32) -> vec2<u32> {
  let minf = (t.cntl >> 1u) & 7u;
  let magf = (t.cntl >> 4u) & 7u;
  let mip = (t.cntl & TX_MIP_DIS) == 0u && minf >= MIN_NEAREST_MIP;
  var lev0 = 0u;
  var nlev = 1u;
  var lf = 0u;
  var linear: bool;
  if (lod <= 0.0 || t.top == 0u || !mip) {
    if (lod <= 0.0) { linear = (magf & 1u) != 0u; }
    else { linear = minf == MIN_LINEAR || minf == MIN_LINEAR_MIP || minf == MIN_TRILINEAR; }
  } else {
    linear = minf == MIN_LINEAR_MIP || minf == MIN_TRILINEAR;
    if (minf == MIN_NEAREST_MIP || minf == MIN_LINEAR_MIP) {
      lev0 = min(u32(floor(lod + 0.5)), t.top);
    } else {
      lev0 = u32(floor(lod));
      if (lev0 >= t.top) { lev0 = t.top; } else { nlev = 2u; lf = u32((lod - floor(lod)) * 256.0); }
    }
  }
  let mode_s = (t.cntl >> 8u) & 3u;
  let mode_t = (t.cntl >> 11u) & 3u;
  let aen = (u.scale & S3_TEX_MAP_AEN) != 0u;
  var lv: array<u32, 2>;
  var raw0 = 0u;
  let ntaps = select(1u, 4u, linear);
  for (var li = 0u; li < nlev; li++) {
    let lev = lev0 + li;
    let w = level_w(t, lev);
    let h = level_h(t, lev);
    let row = i32(level_row(t, lev));
    var uu = sc * f32(w);
    var vv = tc * f32(h);
    if (linear) { uu -= 0.5; vv -= 0.5; }
    let fu = floor(uu);
    let fv = floor(vv);
    let wx = u32((uu - fu) * 256.0);
    let wy = u32((vv - fv) * 256.0);
    var taps: array<u32, 4>;
    for (var k = 0u; k < ntaps; k++) {
      let tx = wrap(i32(fu) + i32(k & 1u), i32(w), mode_s);
      let ty = wrap(i32(fv) + i32(k >> 1u), i32(h), mode_t);
      var texel = vec2<u32>(t.border, 0u);
      if (tx >= 0 && ty >= 0) {
        let at = vec2<i32>(tx, ty + row);
        if (unit == 0u) { texel = textureLoad(atlas0, at, 0).rg; } else { texel = textureLoad(atlas1, at, 0).rg; }
        if (!aen) { texel.x |= 0xFF000000u; }
      }
      taps[k] = texel.x;
      if (li == 0u && k == 0u) { raw0 = texel.y; }
    }
    if (linear) {
      lv[li] = lerp_argb(lerp_argb(taps[0], taps[1], wx), lerp_argb(taps[2], taps[3], wx), wy);
    } else {
      lv[li] = taps[0];
    }
  }
  if (nlev == 2u) { return vec2<u32>(lerp_argb(lv[0], lv[1], lf), raw0); }
  return vec2<u32>(lv[0], raw0);
}

// The combine function (comb() in rage128_raster.c).
fn comb(fnc: u32, a: i32, b: i32, pss: i32, a_interp: i32, a_tex: i32, a_const: i32, a_prev: i32, c_const: i32) -> i32 {
  switch fnc {
    case 0u: { return pss; }
    case 1u: { return a; }
    case 2u: { return b; }
    case 3u: { return mul255(a, b); }
    case 4u: { return clamp255(2 * mul255(a, b)); }
    case 5u: { return clamp255(4 * mul255(a, b)); }
    case 6u: { return clamp255(a + b); }
    case 7u: { return clamp255(a + b - 128); }
    case 8u: { return clamp255(mul255(a, a_interp) + mul255(b, 255 - a_interp)); }
    case 9u: { return clamp255(mul255(a, a_tex) + mul255(b, 255 - a_tex)); }
    case 10u: { return clamp255(mul255(a, a_const) + mul255(b, 255 - a_const)); }
    case 11u: { return clamp255(a + mul255(b, 255 - a_tex)); }
    case 12u: { return clamp255(mul255(a, a_prev) + mul255(b, 255 - a_prev)); }
    case 13u: { return clamp255(a + mul255(b, a_tex)); }
    case 14u: { return clamp255(2 * (a + b - 128)); }
    default: { return clamp255(mul255(a, c_const) + mul255(b, 255 - c_const)); }
  }
}

// One unit's stage (stage() in rage128_raster.c), its four channels in
// one loop: k < 3 the colour arguments, k == 3 the alpha ones.
fn stage(t: Tex, secondary: bool, tex: vec4<i32>, prev: vec4<i32>, diff: vec4<i32>) -> vec4<i32> {
  let cc = unpack_argb(u.constant);
  let w = t.comb;
  let cf = (w >> 4u) & 0xFu; let inf = (w >> 10u) & 0xFu;
  let af = (w >> 18u) & 0xFu; let ina = (w >> 25u) & 7u;
  let a_prev = select(tex.a, prev.a, secondary);
  var out: vec4<i32>;
  for (var k = 0; k < 4; k++) {
    var a: i32;
    var b: i32;
    var fnc: u32;
    if (k < 3) {
      fnc = w & 0xFu;
      switch cf {
        case 0u: { a = cc[k]; }
        case 1u: { a = 255 - cc[k]; }
        case 5u: { a = 255 - tex[k]; }
        case 6u: { a = tex.a; }
        case 7u: { a = 255 - tex.a; }
        case 8u: { a = prev[k]; }
        default: { a = tex[k]; }
      }
      switch inf {
        case 2u: { b = cc[k]; }
        case 3u: { b = cc.a; }
        case 5u: { b = diff.a; }
        case 8u: { b = prev[k]; }
        case 9u: { b = prev.a; }
        default: { b = select(diff[k], prev[k], secondary); }
      }
    } else {
      fnc = (w >> 14u) & 0xFu;
      if (af == 7u) { a = 255 - tex.a; } else if (af == 0u) { a = cc.a; } else if (af == 1u) { a = 255 - cc.a; } else { a = tex.a; }
      if (ina == 1u) { b = cc.a; } else if (ina == 4u) { b = prev.a; } else if (secondary && ina != 2u) { b = prev.a; } else { b = diff.a; }
    }
    out[k] = comb(fnc, a, b, tex[k], diff.a, tex.a, cc.a, a_prev, cc[k]);
  }
  return out;
}

fn cmp_op(op: u32, a: u32, b: u32) -> bool {
  switch op {
    case 0u: { return false; }
    case 1u: { return a < b; }
    case 2u: { return a <= b; }
    case 3u: { return a == b; }
    case 4u: { return a >= b; }
    case 5u: { return a > b; }
    case 6u: { return a != b; }
    default: { return true; }
  }
}

fn scissored(x: i32, y: i32) -> bool {
  let ac = u.aux_cntl;
  var any_add = false;
  var in_add = false;
  for (var k = 0u; k < 3u; k++) {
    if ((ac & (1u << (2u * k))) == 0u) { continue; }
    let r = u.aux[k];
    let inside = x >= r.x && x <= r.y && y >= r.z && y <= r.w;
    if ((ac & (2u << (2u * k))) != 0u) {
      if (inside) { return true; }
    } else {
      any_add = true;
      in_add = in_add || inside;
    }
  }
  return any_add && !in_add;
}

// The 4 x 4 ordered dither (k_bayer).
fn bayer(x: i32, y: i32) -> i32 {
  let m = array<i32, 16>(0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
  return m[(y & 3) * 4 + (x & 3)];
}

// dst_pack and dst_unpack composed: the colour as the surface stores it,
// each channel reduced (dither, round or truncate) to the datatype's
// width and widened back as the walker's dst_unpack widens it.
fn store(c: vec4<i32>, x: i32, y: i32) -> vec4<f32> {
  let blending = (u.flags & F_BLENDING) != 0u;
  var d = -1;
  let dither = (u.tex_cntl & TC_DITHER_EN) != 0u &&
               !(blending && (u.scale & S3_DITHER_TABLE) != 0u && (u.scale & S3_DITHER_INIT) != 0u);
  if (dither) { d = bayer(x, y); } else if ((u.scale & S3_ROUND_EN) != 0u) { d = 8; }
  // Bits per channel, R G B A; 0 = not stored (reads 255), 1 = ARGB1555's
  // alpha bit (set from 128 up, never dithered).
  var bits = vec4<u32>(8u, 8u, 8u, 8u);
  switch u.dst_type {
    case 3u: { bits = vec4<u32>(5u, 5u, 5u, 1u); }
    case 4u: { bits = vec4<u32>(5u, 6u, 5u, 0u); }
    case 7u: { bits = vec4<u32>(3u, 3u, 2u, 0u); }
    case 15u: { bits = vec4<u32>(4u, 4u, 4u, 4u); }
    default: {}
  }
  var out: vec4<f32>;
  for (var k = 0; k < 4; k++) {
    let n = bits[k];
    if (n == 0u) { out[k] = 1.0; continue; }
    if (n == 1u) { out[k] = select(0.0, 1.0, c[k] >= 128); continue; }
    if (n == 8u) { out[k] = f32(c[k]) / 255.0; continue; }
    let drop = 8u - n;
    var v = c[k];
    if (d >= 0) { v += (d * (1 << drop)) / 16; }
    let code = u32(clamp255(v)) >> drop;
    out[k] = f32(code * 255u / ((1u << n) - 1u)) / 255.0;
  }
  return out;
}

struct FOut {
  @location(0) color: vec4<f32>,
  @builtin(frag_depth) depth: f32,
};

struct Shaded { color: vec4<f32>, z: f32, };

// Everything shade() does before the Z buffer and the blend.  Returns the
// colour to write and the Z (0..1); discards what the walker kills.
fn shade(in: VOut) -> Shaded {
  // The texel steps first: derivatives need uniform control flow.  A
  // linearly interpolated value's fine derivative is its plane gradient,
  // which is what make_frag steps by.
  var dx: array<vec3<f32>, 2>;
  var dy: array<vec3<f32>, 2>;
  dx[0] = dpdxFine(in.t0); dy[0] = dpdyFine(in.t0);
  dx[1] = dpdxFine(in.t1); dy[1] = dpdyFine(in.t1);
  let x = i32(floor(in.pos.x));
  let y = i32(floor(in.pos.y));
  if (scissored(x, y)) { discard; }
  var diff: vec4<i32>;
  for (var k = 0; k < 4; k++) { diff[k] = clamp255(i32(in.rgba[k])); }
  var spec: vec3<i32>;
  for (var k = 0; k < 3; k++) { spec[k] = clamp255(i32(in.spec[k])); }
  var c = diff;
  if ((u.tex_cntl & TC_TEX_EN) != 0u && u.tex[0].on != 0u) {
    var tex: array<vec4<i32>, 2>;
    var raw0 = 0u;
    for (var unit = 0u; unit < 2u; unit++) {
      let t = u.tex[unit];
      if (t.on == 0u) { continue; }
      var a = in.t0;
      if (unit == 1u) { a = in.t1; }
      var w = a.z;
      if (w == 0.0) { w = 1e-30; }
      let sc = a.x / w;
      let tc = a.y / w;
      var w1 = a.z + dx[unit].z; if (w1 == 0.0) { w1 = 1e-30; }
      var w2 = a.z + dy[unit].z; if (w2 == 0.0) { w2 = 1e-30; }
      let tw = f32(1u << t.lpitch);
      let th = f32(1u << t.lheight);
      let dsx = ((a.x + dx[unit].x) / w1 - sc) * tw;
      let dtx = ((a.y + dx[unit].y) / w1 - tc) * th;
      let dsy = ((a.x + dy[unit].x) / w2 - sc) * tw;
      let dty = ((a.y + dy[unit].y) / w2 - tc) * th;
      let rho = max(max(abs(dsx), abs(dtx)), max(abs(dsy), abs(dty)));
      var lod = -64.0;
      if (rho > 0.0) { lod = log2(rho); }
      lod -= f32(i32(u.tex_cntl) >> 24) / 128.0;
      let s = sample(unit, t, sc, tc, lod);
      tex[unit] = unpack_argb(s.x);
      if (unit == 0u) { raw0 = s.y; }
    }
    for (var unit = 0u; unit < 2u; unit++) {
      if (unit == 1u && u.tex[1].on == 0u) { break; }
      c = stage(u.tex[unit], unit == 1u, tex[unit], c, diff);
    }
    // Texture lighting: the combined colour against the interpolated one.
    let lf = (u.tex_cntl >> 14u) & 0xFu;
    let la = (u.tex_cntl >> 18u) & 7u;
    let cc = unpack_argb(u.constant);
    var lit: vec4<i32>;
    for (var k = 0; k < 4; k++) {
      let fnc = select(lf, la, k == 3);
      lit[k] = comb(fnc, c[k], diff[k], c[k], diff.a, tex[0].a, cc.a, c.a, cc[k]);
    }
    c = lit;
    if ((u.tex_cntl & TC_CHROMA_KEY) != 0u) {
      let fnc = (u.misc >> 30u) & 3u;
      let eq = ((raw0 ^ u.key) & u.key_mask) == 0u;
      let draw = fnc == 1u || (fnc == 2u && !eq) || (fnc == 3u && eq);
      if (!draw) { discard; }
    }
    if ((u.tex_cntl & TC_AMASK) != 0u && (tex[0].a & 1) == 0) { discard; }
  }
  if ((u.tex_cntl & TC_SPECULAR) != 0u) {
    for (var k = 0; k < 3; k++) { c[k] = clamp255(c[k] + spec[k]); }
  }
  let z = in.zf.x;
  if ((u.tex_cntl & TC_FOG_EN) != 0u) {
    var ff: i32;
    if ((u.misc & MISC_FOG_TABLE) != 0u) { ff = fog_entry(u32(max(z, 0.0) * 255.0) & 0xFFu); }
    else { ff = clamp255(i32(in.zf.y)); }
    let fc = unpack_argb(u.fog_color);
    for (var k = 0; k < 3; k++) { c[k] = clamp255(mul255(c[k], ff) + mul255(fc[k], 255 - ff)); }
  }
  if ((u.tex_cntl & TC_ALPHA_TST) != 0u &&
      !cmp_op((u.misc >> 24u) & 7u, u32(c.a), u.misc & 0xFFu)) { discard; }
  var out: vec4<f32>;
  if ((u.flags & F_BLENDING) != 0u) {
    // The blend is the GPU's; the pack happens on the way back to VRAM.
    out = vec4<f32>(c) / 255.0;
  } else {
    out = store(c, x, y);
  }
  // The Z code: z·(2^16 − 1), truncated, clamped (shade()).
  var code = 0.0;
  if (z >= 1.0) { code = 65535.0; } else if (z > 0.0) { code = floor(z * 65535.0); }
  return Shaded(out, code / 65535.0);
}

@fragment fn fs_main(in: VOut) -> FOut {
  let s = shade(in);
  return FOut(s.color, s.z);
}

@fragment fn fs_nodepth(in: VOut) -> @location(0) vec4<f32> {
  return shade(in).color;
}
`;

// The present pass: the colour surface through the display table.  Each
// channel is the code the surface stores at the scanout's width, which
// indexes the table at code << (8 − bits), as the card's DAC does.
export const PRESENT_WGSL = /* wgsl */ `
struct P { bits: vec4<u32>, };
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var lut: texture_2d<f32>;
@group(0) @binding(2) var<uniform> p: P;

@vertex fn vs_present(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
  var q = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(q[i], 0.0, 1.0);
}

fn index(v: f32, bits: u32) -> i32 {
  let m = (1u << bits) - 1u;
  let code = (u32(v * 255.0 + 0.5) * m + 127u) / 255u;
  return i32(code << (8u - bits));
}

@fragment fn fs_present(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let c = textureLoad(src, vec2<i32>(pos.xy), 0);
  let r = textureLoad(lut, vec2<i32>(index(c.r, p.bits.x), 0), 0).r;
  let g = textureLoad(lut, vec2<i32>(index(c.g, p.bits.y), 1), 0).r;
  let b = textureLoad(lut, vec2<i32>(index(c.b, p.bits.z), 2), 0).r;
  return vec4<f32>(r, g, b, 1.0);
}
`;

// The depth restore pass (the Voodoo2's): a depth format takes no partial
// buffer copy, so rows of 16-bit Z codes go into an r16uint staging
// texture and this pass writes them through frag_depth, scissored.  Fills
// of the Z buffer go the same way.
export const DEPTH_RESTORE_WGSL = /* wgsl */ `
@group(0) @binding(0) var codes: texture_2d<u32>;

@vertex fn vs_restore(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
  var p = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(p[i], 0.5, 1.0);
}

@fragment fn fs_restore(@builtin(position) pos: vec4<f32>) -> @builtin(frag_depth) f32 {
  let code = textureLoad(codes, vec2<i32>(pos.xy), 0).r;
  return f32(code & 0xFFFFu) / 65535.0;
}
`;
