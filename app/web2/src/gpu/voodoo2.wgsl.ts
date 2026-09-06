// The Voodoo2 WebGPU takeover's shaders (proposal-voodoo2-webgpu-takeover
// §5.3-§5.4): the vertex shader places the walker's 12.4 vertices at the
// half-pixel offset that makes the GPU's centre test equal the walker's
// integer-sample test, and the fragment shader is the p.15 pixel pipe —
// texture chain, chroma key, colour/alpha combine, fog, alpha test,
// depth, dither — written in u32 arithmetic beside the C
// (voodoo2_raster.c), stage for stage and name for name, so the two
// implementations can be read against each other.  The GPU contributes
// coverage, interpolation, the depth compare and the alpha blend.
//
// Everything the walker decodes per draw arrives in the uniform block
// (voodoo2_gpu_protocol.h: the raw registers plus the per-TMU decode);
// the pipeline permutation is selected by uniform branches rather than
// shader variants, which keeps the pipeline cache to the handful of
// blend/depth/write-mask combinations WebGPU actually bakes in.

export const VOODOO2_WGSL = /* wgsl */ `
struct Tmu {
  mode: u32, tlod: u32, trex1: u32, lodmin: i32,
  lodmax: i32, lodbias: i32, flags: u32, base_level: u32,
  w0: u32, h0: u32, pad0: u32, pad1: u32,
  pad2: vec4<u32>,
};

struct Uniforms {
  fbz: u32, fcp: u32, amode: u32, fogmode: u32,
  fogcolor: u32, color0: u32, color1: u32, zacolor: u32,
  chromakey: u32, chromarange: u32, stipple: u32, flags: u32,
  screen_h: f32, target_w: f32, target_h: f32, fill: u32,
  fogtable: array<vec4<u32>, 8>,
  tmu: array<Tmu, 2>,
  pad: array<vec4<u32>, 12>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var tex0: texture_2d<f32>;
@group(0) @binding(2) var tex1: texture_2d<f32>;

// Uniform flag bits (V2GPU_F_*).
const F_TEX_ON: u32 = 1u;
const F_USES_TEX: u32 = 2u;
const F_SKIP_TMU1: u32 = 4u;
const F_Y_FLIP: u32 = 8u;
const F_FILL: u32 = 16u;
const F_FILL_RAW: u32 = 32u;
const F_LFB_PIXEL: u32 = 64u;
// Per-TMU flag bits (V2GPU_TF_*).
const TF_BILIN_MIN: u32 = 1u;
const TF_BILIN_MAG: u32 = 2u;
const TF_CLAMP_S: u32 = 4u;
const TF_CLAMP_T: u32 = 8u;
const TF_PERSP: u32 = 16u;
const TF_TCLAMPW: u32 = 32u;
const TF_SEND_CFG: u32 = 64u;
const TF_TSPLIT: u32 = 128u;
const TF_LOD_ODD: u32 = 256u;
const TF_LOD_PINNED: u32 = 512u;
const TF_BOUND: u32 = 1024u;

struct VIn {
  @location(0) pos: vec2<f32>,
  @location(1) zw: vec2<f32>,
  @location(2) rgba: vec4<f32>,
  @location(3) t0: vec3<f32>,
  @location(4) t1: vec3<f32>,
};

struct VOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) @interpolate(linear) rgba: vec4<f32>,
  @location(1) @interpolate(linear) zw: vec2<f32>,
  @location(2) @interpolate(linear) t0: vec3<f32>,
  @location(3) @interpolate(linear) t1: vec3<f32>,
};

// The walker tests pixel (x, y) at the integer point (x, y); the GPU
// tests it at (x + 0.5, y + 0.5).  Shifting every vertex by (0.5, 0.5)
// makes the two tests identical, ties included (both top-left).  The
// Y flip of fbzMode[17] maps row y to screen_h - 1 - y, which for the
// shifted point is screen_h - y'.
@vertex fn vs_main(in: VIn) -> VOut {
  var o: VOut;
  let x = in.pos.x + 0.5;
  var y = in.pos.y + 0.5;
  if ((u.flags & F_Y_FLIP) != 0u) { y = u.screen_h - y; }
  o.pos = vec4<f32>(x / u.target_w * 2.0 - 1.0, 1.0 - y / u.target_h * 2.0, 0.5, 1.0);
  o.rgba = in.rgba;
  o.zw = in.zw;
  o.t0 = in.t0;
  o.t1 = in.t1;
  return o;
}

// --- the shared combine-unit shape (v2_combine) -------------------------
fn combine(other: u32, local: u32, m: u32, ctl: u32, add: u32) -> u32 {
  let zero_other = (ctl & 1u) != 0u;
  let sub_local = (ctl & 2u) != 0u;
  let reverse = (ctl & 4u) != 0u;
  let invert = (ctl & 8u) != 0u;
  let acc: i32 = select(i32(other), 0, zero_other) - select(0, i32(local), sub_local);
  let f: u32 = select(m ^ 0xFFu, m, reverse) + 1u;
  var o: i32 = ((acc * i32(f)) >> 8u) + i32(add);
  o = clamp(o, 0, 255);
  return select(u32(o), u32(o) ^ 0xFFu, invert);
}

// --- dither (v2_pack565): the CHOSEN Bayer orders and remainder rule --
var<private> dither4: array<u32, 16> = array<u32, 16>(0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u);
var<private> dither2: array<u32, 4> = array<u32, 4>(0u, 2u, 3u, 1u);

fn dith5(d: u32, v: u32) -> u32 { return (v * 31u + 15u * d) / 255u; }
fn dith6(d: u32, v: u32) -> u32 { return (v * 63u + 13u * d) / 255u; }

// The 5-6-5 the walker would store, expanded back to 8 bits the way the
// scanout (and the walker's blend) expand it.
fn pack_expand(x: u32, y: u32, r: u32, g: u32, b: u32) -> vec3<u32> {
  var r5: u32; var g6: u32; var b5: u32;
  if ((u.fbz & 0x100u) != 0u) {
    var d: u32;
    if ((u.fbz & 0x800u) != 0u) { d = dither2[(y & 1u) * 2u + (x & 1u)] * 4u; }
    else { d = dither4[(y & 3u) * 4u + (x & 3u)]; }
    r5 = dith5(d, r); g6 = dith6(d, g); b5 = dith5(d, b);
  } else {
    r5 = r >> 3u; g6 = g >> 2u; b5 = b >> 3u;
  }
  return vec3<u32>((r5 << 3u) | (r5 >> 2u), (g6 << 2u) | (g6 >> 4u), (b5 << 3u) | (b5 >> 2u));
}

// --- iterator clamp/wrap (v2_iter_rgba, v2_iter_z, v2_iter_w8) ----------
fn iter_rgba(v: f32, clampm: bool) -> u32 {
  let i: i32 = i32(floor(v));
  if (clampm) { return u32(clamp(i, 0, 255)); }
  let ipart: u32 = u32(i) & 0xFFFu;
  if (ipart == 0xFFFu) { return 0u; }
  if (ipart == 0x100u) { return 0xFFu; }
  return u32(i) & 0xFFu;
}

fn iter_z(v: f32, clampm: bool) -> u32 {
  let i: i32 = i32(floor(v));
  if (clampm) { return u32(clamp(i, 0, 65535)); }
  let ipart: u32 = u32(i) & 0xFFFFFu;
  if (ipart == 0xFFFFFu) { return 0u; }
  if (ipart == 0x10000u) { return 0xFFFFu; }
  return u32(i) & 0xFFFFu;
}

// w is the 2.30 iterator as a real number (raw / 2^30).
fn iter_w8(w: f32, clampm: bool) -> u32 {
  if (clampm) {
    if (w < 0.0) { return 0u; }
    if (w >= 1.0) { return 0xFFu; }
    return u32(floor(w * 256.0)) & 0xFFu;
  }
  return u32(i32(floor(w * 256.0))) & 0xFFu;
}

// The CHOSEN 1/W -> 4.12 inverted-mantissa float (v2_depth_float), on a
// 2.30 raw value given as a real.
fn depth_float(val: f32) -> u32 {
  if (val <= 0.0 || val >= 1.0) { return 0u; }
  var m: u32 = u32(val * 1073741824.0);
  if (m == 0u) { return 0u; }
  var e: u32 = 0u;
  loop {
    if (m >= 536870912u || e >= 15u) { break; }
    m = m << 1u;
    e = e + 1u;
  }
  let mant = (m >> 17u) & 0xFFFu;
  return (e << 12u) | (~mant & 0xFFFu);
}

fn blend_mul(c: u32, f: u32) -> u32 { return (c * (f + (f >> 7u))) >> 8u; }

fn expand8(c: vec4<f32>) -> vec4<u32> {
  return vec4<u32>(c * 255.0 + 0.5);
}

fn pack_argb(c: vec4<u32>) -> u32 {
  return (c.a << 24u) | (c.r << 16u) | (c.g << 8u) | c.b;
}

fn tex_coord(c: i32, dim: u32, clampc: bool) -> u32 {
  if (clampc) { return u32(clamp(c, 0, i32(dim) - 1)); }
  return u32(c) & (dim - 1u);
}

fn tex_load(tmu: u32, level: u32, s: u32, t: u32) -> vec4<u32> {
  if (tmu == 0u) { return expand8(textureLoad(tex0, vec2<i32>(i32(s), i32(t)), i32(level))); }
  return expand8(textureLoad(tex1, vec2<i32>(i32(s), i32(t)), i32(level)));
}

// One TMU's sample at texel-space (s, t) — point or bilinear per the
// filter bits (v2_tmu_sample), the 2x2 blend in the walker's 8-bit
// fractions.
fn tmu_sample(tmu: u32, tm: Tmu, s_in: f32, t_in: f32, lod: u32, magnify: bool) -> u32 {
  let bilinear = select((tm.flags & TF_BILIN_MIN) != 0u, (tm.flags & TF_BILIN_MAG) != 0u, magnify);
  let s = ldexp(s_in, -i32(lod));
  let t = ldexp(t_in, -i32(lod));
  let w = max(1u, tm.w0 >> lod);
  let h = max(1u, tm.h0 >> lod);
  let glevel = lod - tm.base_level;
  let cs = (tm.flags & TF_CLAMP_S) != 0u;
  let ct = (tm.flags & TF_CLAMP_T) != 0u;
  if (!bilinear) {
    let sa = tex_coord(i32(floor(s)), w, cs);
    let ta = tex_coord(i32(floor(t)), h, ct);
    return pack_argb(tex_load(tmu, glevel, sa, ta));
  }
  let fs = s - 0.5;
  let ft = t - 0.5;
  let s0 = floor(fs);
  let t0 = floor(ft);
  let frac_s = u32((fs - s0) * 256.0) & 0xFFu;
  let frac_t = u32((ft - t0) * 256.0) & 0xFFu;
  let sa = tex_coord(i32(s0), w, cs);
  let sb = tex_coord(i32(s0) + 1, w, cs);
  let ta = tex_coord(i32(t0), h, ct);
  let tb = tex_coord(i32(t0) + 1, h, ct);
  let c00 = tex_load(tmu, glevel, sa, ta);
  let c10 = tex_load(tmu, glevel, sb, ta);
  let c01 = tex_load(tmu, glevel, sa, tb);
  let c11 = tex_load(tmu, glevel, sb, tb);
  let top = (c00 * (256u - frac_s) + c10 * frac_s) >> vec4<u32>(8u);
  let bot = (c01 * (256u - frac_s) + c11 * frac_s) >> vec4<u32>(8u);
  let o = ((top * (256u - frac_t) + bot * frac_t) >> vec4<u32>(8u)) & vec4<u32>(0xFFu);
  return pack_argb(o);
}

// The texture chain for one pixel (v2_texture_chain_full): TMU1 samples
// and combines first, its output feeding TMU0's c_other.
fn texture_chain(t0: vec3<f32>, t1: vec3<f32>, d0x: vec3<f32>, d0y: vec3<f32>, d1x: vec3<f32>, d1y: vec3<f32>) -> u32 {
  var chain: u32 = 0u;
  for (var i: i32 = 1; i >= 0; i = i - 1) {
    let tmu = u32(i);
    if (tmu == 1u && (u.flags & F_SKIP_TMU1) != 0u) { continue; }
    let tm = u.tmu[tmu];
    let mode = tm.mode;
    if ((tm.flags & TF_SEND_CFG) != 0u) {
      // Send config: the TMU outputs its configuration word as colour.
      let sel = (tm.trex1 >> 23u) & 7u;
      var contrib: u32 = 0u;
      if (sel == 0u) {
        contrib = 2u << (7u * tmu);
        if (tmu >= 1u) { contrib = contrib | (1u << (7u * tmu - 1u)); }
      } else if (sel == 5u) {
        contrib = ((1u << 4u) | 1u) << (8u * tmu);
      }
      chain = 0xFF000000u | ((chain & 0xFFFFFFu) | contrib);
      continue;
    }
    var texel: u32 = 0xFF000000u;
    var lod4: i32 = tm.lodmin;
    if ((tm.flags & TF_BOUND) != 0u) {
      let it = select(t0, t1, tmu == 1u);
      let dx = select(d0x, d1x, tmu == 1u);
      let dy = select(d0y, d1y, tmu == 1u);
      // The sample point (s, t): the perspective divide, or the plain
      // 14.18 unscale.  The iterators are reals here: s/w = raw/2^18,
      // 1/w = raw/2^30, so s = (s/w) / (1/w) directly.
      let persp = (mode & 1u) != 0u;
      let w_clamped = (mode & 8u) != 0u && it.z < 0.0;
      var wdiv = it.z;
      if (wdiv == 0.0) { wdiv = 9.313225746154785e-10; } // a zero 1/W reads as raw 1
      var s: f32; var t: f32;
      if (persp) {
        if (w_clamped) { s = 0.0; t = 0.0; } else { s = it.x / wdiv; t = it.y / wdiv; }
      } else {
        s = it.x; t = it.y;
      }
      // Per-pixel LOD from the analytic texel-space steps (chosen), in
      // 4.2 fixed, biased and clamped per tLOD — unless the level is
      // pinned (lodmin == lodmax with equal filters).
      var magnify = false;
      if ((tm.flags & TF_LOD_PINNED) != 0u) {
        lod4 = tm.lodmin;
      } else {
        var s1: f32; var t1v: f32; var s2: f32; var t2: f32;
        if (persp) {
          if (w_clamped) { s1 = 0.0; t1v = 0.0; s2 = 0.0; t2 = 0.0; }
          else {
            var wx = it.z + dx.z; if (wx == 0.0) { wx = 9.313225746154785e-10; }
            var wy = it.z + dy.z; if (wy == 0.0) { wy = 9.313225746154785e-10; }
            s1 = (it.x + dx.x) / wx; t1v = (it.y + dx.y) / wx;
            s2 = (it.x + dy.x) / wy; t2 = (it.y + dy.y) / wy;
          }
        } else {
          s1 = s + dx.x; t1v = t + dx.y; s2 = s + dy.x; t2 = t + dy.y;
        }
        let stepx = (s1 - s) * (s1 - s) + (t1v - t) * (t1v - t);
        let stepy = (s2 - s) * (s2 - s) + (t2 - t) * (t2 - t);
        let step2 = max(stepx, stepy);
        lod4 = 0;
        if (step2 > 1.0) { lod4 = i32(2.0 * log2(step2)); }
        lod4 = lod4 + tm.lodbias;
        magnify = lod4 <= tm.lodmin;
        lod4 = clamp(lod4, tm.lodmin, tm.lodmax);
      }
      var level: i32 = lod4 >> 2u;
      if ((tm.flags & TF_TSPLIT) != 0u) {
        let odd = select(0, 1, (tm.flags & TF_LOD_ODD) != 0u);
        if ((level & 1) != odd) { level = level + 1; }
      }
      level = clamp(level, i32(tm.base_level), 8);
      texel = tmu_sample(tmu, tm, s, t, u32(level), magnify);
    }
    // Texture Combine Unit (textureMode[29:12]): c_local is this TMU's
    // texel, c_other the downstream chain.
    let lr = (texel >> 16u) & 0xFFu; let lg = (texel >> 8u) & 0xFFu; let lb = texel & 0xFFu; let lA = texel >> 24u;
    let or_ = (chain >> 16u) & 0xFFu; let og = (chain >> 8u) & 0xFFu; let ob = chain & 0xFFu; let oA = chain >> 24u;
    let lodfrac = (u32(lod4) & 3u) << 6u;
    var m_r: u32; var m_g: u32; var m_b: u32; var m_a: u32;
    switch ((mode >> 14u) & 7u) {
      case 1u: { m_r = lr; m_g = lg; m_b = lb; }
      case 2u: { m_r = oA; m_g = oA; m_b = oA; }
      case 3u: { m_r = lA; m_g = lA; m_b = lA; }
      case 4u: { m_r = 0xFFu; m_g = 0xFFu; m_b = 0xFFu; }
      case 5u: { m_r = lodfrac; m_g = lodfrac; m_b = lodfrac; }
      default: { m_r = 0u; m_g = 0u; m_b = 0u; }
    }
    switch ((mode >> 23u) & 7u) {
      case 1u, 3u: { m_a = lA; }
      case 2u: { m_a = oA; }
      case 4u: { m_a = 0xFFu; }
      case 5u: { m_a = lodfrac; }
      default: { m_a = 0u; }
    }
    let tc_ctl = ((mode >> 12u) & 3u) | (((mode >> 17u) & 1u) << 2u) | (((mode >> 20u) & 1u) << 3u);
    let tca_ctl = ((mode >> 21u) & 3u) | (((mode >> 26u) & 1u) << 2u) | (((mode >> 29u) & 1u) << 3u);
    var add_r: u32; var add_g: u32; var add_b: u32;
    if (((mode >> 19u) & 1u) != 0u) { add_r = lA; add_g = lA; add_b = lA; }
    else if (((mode >> 18u) & 1u) != 0u) { add_r = lr; add_g = lg; add_b = lb; }
    else { add_r = 0u; add_g = 0u; add_b = 0u; }
    let tca_add = select(0u, lA, (((mode >> 28u) & 1u) != 0u) || (((mode >> 27u) & 1u) != 0u));
    let rr = combine(or_, lr, m_r, tc_ctl, add_r);
    let rg = combine(og, lg, m_g, tc_ctl, add_g);
    let rb = combine(ob, lb, m_b, tc_ctl, add_b);
    let ra = combine(oA, lA, m_a, tca_ctl, tca_add);
    chain = (ra << 24u) | (rr << 16u) | (rg << 8u) | rb;
  }
  return chain;
}

struct FOut {
  @location(0) color: vec4<f32>,
  @builtin(frag_depth) depth: f32,
};

@fragment fn fs_main(in: VOut) -> FOut {
  var o: FOut;
  let px = u32(in.pos.x);
  let py = u32(in.pos.y);
  let fbz = u.fbz;
  let fcp = u.fcp;
  let amode = u.amode;

  // Fill draws (fastfillCMD, the SGRAM fill): colour and/or depth
  // constants, bypassing the pipeline (V2 §5.24).
  if ((u.flags & F_FILL) != 0u) {
    var rgb: vec3<u32>;
    if ((u.flags & F_FILL_RAW) != 0u) {
      let r5 = (u.fill >> 11u) & 0x1Fu; let g6 = (u.fill >> 5u) & 0x3Fu; let b5 = u.fill & 0x1Fu;
      rgb = vec3<u32>((r5 << 3u) | (r5 >> 2u), (g6 << 2u) | (g6 >> 4u), (b5 << 3u) | (b5 >> 2u));
    } else {
      rgb = pack_expand(px, py, (u.fill >> 16u) & 0xFFu, (u.fill >> 8u) & 0xFFu, u.fill & 0xFFu);
    }
    o.color = vec4<f32>(vec3<f32>(rgb) / 255.0, 1.0);
    o.depth = f32(u.zacolor & 0xFFFFu) / 65535.0;
    return o;
  }

  // The iterated values, clamped/wrapped per fbzColorPath[28].
  let clampm = ((fcp >> 28u) & 1u) != 0u;
  let r = iter_rgba(in.rgba.x, clampm);
  let g = iter_rgba(in.rgba.y, clampm);
  let b = iter_rgba(in.rgba.z, clampm);
  let a = iter_rgba(in.rgba.w, clampm);
  let z16 = iter_z(in.zw.x, clampm);
  let w8 = iter_w8(in.zw.y, clampm);

  // Stipple (fbzMode[2]) in pattern mode (rotate-mode masking falls
  // back to the walker: a per-pixel register the GPU cannot honour).
  if ((fbz & 4u) != 0u && (fbz & 0x1000u) != 0u) {
    let row = (u.stipple >> (8u * (py & 3u))) & 0xFFu;
    if (((row >> (7u - (px & 7u))) & 1u) == 0u) { discard; }
  }

  // The texture chain, only when the pipeline reads it.
  var tex_argb: u32 = 0u;
  if ((u.flags & F_TEX_ON) != 0u && (u.flags & F_USES_TEX) != 0u && (u.flags & F_LFB_PIXEL) == 0u) {
    tex_argb = texture_chain(in.t0, in.t1, dpdxFine(in.t0), dpdyFine(in.t0), dpdxFine(in.t1), dpdyFine(in.t1));
  }

  // c_other / a_other selection (fbzColorPath[1:0], [3:2]).
  var oc_r: u32; var oc_g: u32; var oc_b: u32; var oa: u32;
  switch (fcp & 3u) {
    case 1u: { oc_r = (tex_argb >> 16u) & 0xFFu; oc_g = (tex_argb >> 8u) & 0xFFu; oc_b = tex_argb & 0xFFu; }
    case 2u: { oc_r = (u.color1 >> 16u) & 0xFFu; oc_g = (u.color1 >> 8u) & 0xFFu; oc_b = u.color1 & 0xFFu; }
    default: { oc_r = r; oc_g = g; oc_b = b; }
  }
  switch ((fcp >> 2u) & 3u) {
    case 1u: { oa = tex_argb >> 24u; }
    case 2u: { oa = u.color1 >> 24u; }
    default: { oa = a; }
  }

  // Chroma-key / chroma-range on c_other (V2 p.46).
  if ((fbz & 2u) != 0u) {
    let key = u.chromakey & 0xFFFFFFu;
    var match_: bool;
    if ((u.chromarange & (1u << 28u)) != 0u) {
      let hi = u.chromarange & 0xFFFFFFu;
      match_ = oc_b >= (key & 0xFFu) && oc_b <= (hi & 0xFFu) && oc_g >= ((key >> 8u) & 0xFFu) &&
               oc_g <= ((hi >> 8u) & 0xFFu) && oc_r >= ((key >> 16u) & 0xFFu) && oc_r <= ((hi >> 16u) & 0xFFu);
    } else {
      match_ = ((oc_r << 16u) | (oc_g << 8u) | oc_b) == key;
    }
    if (match_) { discard; }
  }

  // c_local / a_local (fbzColorPath[4], [6:5], override [7]).
  var lc_r: u32; var lc_g: u32; var lc_b: u32; var la: u32;
  var local_is_c0 = ((fcp >> 4u) & 1u) != 0u;
  if (((fcp >> 7u) & 1u) != 0u) { local_is_c0 = ((tex_argb >> 31u) & 1u) != 0u; }
  if (local_is_c0) {
    lc_r = (u.color0 >> 16u) & 0xFFu; lc_g = (u.color0 >> 8u) & 0xFFu; lc_b = u.color0 & 0xFFu;
  } else {
    lc_r = r; lc_g = g; lc_b = b;
  }
  switch ((fcp >> 5u) & 3u) {
    case 1u: { la = u.color0 >> 24u; }
    case 2u: { la = z16 >> 8u; }
    case 3u: { la = w8; }
    default: { la = a; }
  }

  // Colour Combine Unit (fbzColorPath[16:8]).
  var cc_m_r: u32; var cc_m_g: u32; var cc_m_b: u32;
  switch ((fcp >> 10u) & 7u) {
    case 1u: { cc_m_r = lc_r; cc_m_g = lc_g; cc_m_b = lc_b; }
    case 2u: { cc_m_r = oa; cc_m_g = oa; cc_m_b = oa; }
    case 3u: { cc_m_r = la; cc_m_g = la; cc_m_b = la; }
    case 4u: { let ta = tex_argb >> 24u; cc_m_r = ta; cc_m_g = ta; cc_m_b = ta; }
    case 5u: { cc_m_r = (tex_argb >> 16u) & 0xFFu; cc_m_g = (tex_argb >> 8u) & 0xFFu; cc_m_b = tex_argb & 0xFFu; }
    default: { cc_m_r = 0u; cc_m_g = 0u; cc_m_b = 0u; }
  }
  let cc_ctl = ((fcp >> 8u) & 3u) | (((fcp >> 13u) & 1u) << 2u) | (((fcp >> 16u) & 1u) << 3u);
  var add_r: u32; var add_g: u32; var add_b: u32;
  if (((fcp >> 15u) & 1u) != 0u) { add_r = la; add_g = la; add_b = la; }
  else if (((fcp >> 14u) & 1u) != 0u) { add_r = lc_r; add_g = lc_g; add_b = lc_b; }
  else { add_r = 0u; add_g = 0u; add_b = 0u; }
  var out_r = combine(oc_r, lc_r, cc_m_r, cc_ctl, add_r);
  var out_g = combine(oc_g, lc_g, cc_m_g, cc_ctl, add_g);
  var out_b = combine(oc_b, lc_b, cc_m_b, cc_ctl, add_b);

  // Alpha Combine Unit (fbzColorPath[25:17]).
  var ca_m: u32;
  switch ((fcp >> 19u) & 7u) {
    case 1u, 3u: { ca_m = la; }
    case 2u: { ca_m = oa; }
    case 4u: { ca_m = tex_argb >> 24u; }
    default: { ca_m = 0u; }
  }
  let ca_ctl = ((fcp >> 17u) & 3u) | (((fcp >> 22u) & 1u) << 2u) | (((fcp >> 25u) & 1u) << 3u);
  let ca_add = select(select(0u, la, ((fcp >> 23u) & 1u) != 0u), la, ((fcp >> 24u) & 1u) != 0u);
  let out_a = combine(oa, la, ca_m, ca_ctl, ca_add);

  // Fog (fogMode; V2 §5.18), the chosen table indexing.
  let fog = u.fogmode;
  if ((fog & 1u) != 0u) {
    var fa: u32;
    switch ((fog >> 3u) & 3u) {
      case 1u: { fa = a; }
      case 2u: { fa = z16 >> 8u; }
      case 3u: { fa = w8; }
      default: {
        var idx: u32 = 0u;
        let w = in.zw.y;
        if (w > 0.0 && w < 1.0) {
          var m: u32 = u32(w * 1073741824.0);
          var e: u32 = 0u;
          loop {
            if (m >= 536870912u || e >= 15u) { break; }
            m = m << 1u;
            e = e + 1u;
          }
          idx = (e << 2u) | ((m >> 27u) & 3u);
          if (idx > 63u) { idx = 63u; }
        }
        let word = u.fogtable[idx >> 3u][(idx >> 1u) & 3u];
        fa = select(word >> 8u, word >> 24u, (idx & 1u) != 0u) & 0xFFu;
      }
    }
    let fr = (u.fogcolor >> 16u) & 0xFFu; let fg = (u.fogcolor >> 8u) & 0xFFu; let fb = u.fogcolor & 0xFFu;
    let fogadd_zero = ((fog >> 1u) & 1u) != 0u;
    let fogmult_zero = ((fog >> 2u) & 1u) != 0u;
    let add_r2 = select(blend_mul(fr, fa), 0u, fogadd_zero);
    let add_g2 = select(blend_mul(fg, fa), 0u, fogadd_zero);
    let add_b2 = select(blend_mul(fb, fa), 0u, fogadd_zero);
    let mul_r = select(blend_mul(out_r, 255u - fa), 0u, fogmult_zero);
    let mul_g = select(blend_mul(out_g, 255u - fa), 0u, fogmult_zero);
    let mul_b = select(blend_mul(out_b, 255u - fa), 0u, fogmult_zero);
    out_r = min(mul_r + add_r2, 255u);
    out_g = min(mul_g + add_g2, 255u);
    out_b = min(mul_b + add_b2, 255u);
  }

  // Alpha test (alphaMode[3:0]) and the alpha-channel mask (fbzMode[13]).
  if ((amode & 1u) != 0u) {
    let aref = amode >> 24u;
    var passed: bool;
    switch ((amode >> 1u) & 7u) {
      case 0u: { passed = false; }
      case 1u: { passed = out_a < aref; }
      case 2u: { passed = out_a == aref; }
      case 3u: { passed = out_a <= aref; }
      case 4u: { passed = out_a > aref; }
      case 5u: { passed = out_a != aref; }
      case 6u: { passed = out_a >= aref; }
      default: { passed = true; }
    }
    if (!passed) { discard; }
  }
  if ((fbz & 0x2000u) != 0u && (out_a & 1u) == 0u) { discard; }

  // Depth (fbzMode[7:3], [16], [21]): the code the walker would compare
  // and write; the compare itself is the pipeline's depth state.  (The
  // zaColor compare mode, fbzMode[20], falls back to the walker.)
  var depth_write: u32 = z16;
  if ((fbz & 0x10u) != 0u) {
    var src: u32;
    if ((fbz & 8u) != 0u) {
      src = depth_float(select(in.zw.y, in.zw.x * 4096.0 / 1073741824.0, (fbz & 0x200000u) != 0u));
    } else {
      src = z16;
    }
    if ((fbz & 0x10000u) != 0u) {
      let bias = i32(u.zacolor << 16u) >> 16u;
      src = u32(clamp(i32(src) + bias, 0, 65535));
    }
    depth_write = src;
  }
  o.depth = f32(depth_write) / 65535.0;

  // Dither and pack: opaque draws store the 5-6-5 the walker stores,
  // expanded; blended draws hand the blender the 8-bit value (the
  // documented <= 1 LSB ordering difference, voodoo2.md).
  if ((amode & 0x10u) != 0u) {
    o.color = vec4<f32>(f32(out_r) / 255.0, f32(out_g) / 255.0, f32(out_b) / 255.0, f32(out_a) / 255.0);
  } else {
    let rgb = pack_expand(px, py, out_r, out_g, out_b);
    o.color = vec4<f32>(vec3<f32>(rgb) / 255.0, 1.0);
  }
  return o;
}
`;

// The present pass: the displayed colour target through the gamma ramp
// onto the canvas (§5.8).  A full-screen triangle; the LUT is a 256x3
// r8unorm texture, rows R, G, B.
export const PRESENT_WGSL = /* wgsl */ `
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var lut: texture_2d<f32>;

@vertex fn vs_present(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
  var p = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(p[i], 0.0, 1.0);
}

@fragment fn fs_present(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let c = textureLoad(src, vec2<i32>(pos.xy), 0);
  let r = textureLoad(lut, vec2<i32>(i32(c.r * 255.0 + 0.5), 0), 0).r;
  let g = textureLoad(lut, vec2<i32>(i32(c.g * 255.0 + 0.5), 1), 0).r;
  let b = textureLoad(lut, vec2<i32>(i32(c.b * 255.0 + 0.5), 2), 0).r;
  return vec4<f32>(r, g, b, 1.0);
}
`;

// The depth restore pass: a depth-format texture cannot take a partial
// buffer copy (WebGPU requires whole-subresource copies for depth
// formats), so rows of depth codes go into an r16uint staging texture
// and this pass writes them into the attachment through frag_depth,
// scissored to the rectangle.
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
