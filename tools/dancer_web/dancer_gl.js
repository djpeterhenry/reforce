// The dancer preview's WebGL2 renderer, for a guest sign-up page (docs/guests.md). The parts come from reforce's own dancer code in
// WebAssembly (dancer_wasm.cpp -> dancer.js); the shaders are a port of render/DancerPipeline's body and outline
// passes with the studio light (reforce.Dancer.Lighting 0: there are no visuals behind the page to light them).
// Keep the two in step when the Metal shader changes.
//
//   const view = await createDancerView(canvas);
//   view.setKeys({HairStyle: 'bob', Top: '#ff4fa3', ...});   // dancer file keys
//   view.setBpm(122); view.destroy();

import createDancerModule from './dancer.js';

const FLOATS = 100;         // engine::kDancerInstFloats
const TEXELS = FLOATS / 4;  // instance rows: one RGBA32F texel per float4
const MAX_PARTS = 512;

const VS_COMMON = `#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D uInst;
uniform mat4 uViewProj;
uniform vec4 uEye;       // xyz, proj11
uniform vec4 uViewport;  // width, height, outline px
uniform int uBase;       // the first instance of this draw (the bent parts come last)
layout(location = 0) in vec3 aDir;
layout(location = 1) in float aEnd;
out vec3 vWorld;
out vec3 vN;
out vec3 vLocal;
flat out int vIid;
vec4 row(int k) { return texelFetch(uInst, ivec2(k, gl_InstanceID + uBase), 0); }
vec3 capsuleLocal(out vec3 nl) {
  vec4 geo = row(9);
  float r = mix(geo.y, geo.z, aEnd);
  vec3 d = aDir;
  if (abs(d.y) < 1e-4) nl = normalize(vec3(d.x, (geo.y - geo.z) / max(geo.x, 1e-4), d.z));
  else nl = d;
  return vec3(d.x * r, aEnd * geo.x + d.y * r, d.z * r);
}
// Long hair that bends: as DancerPipeline bendHair (keep in step).
void bendHair(vec3 local, vec3 nl, inout vec3 w, inout vec3 n) {
  vec4 geo = row(9);
  vec3 m0 = row(0).xyz, m1 = row(1).xyz, m2 = row(2).xyz;
  float sx = length(m0), sz = length(m2);
  vec3 fx = m0 / sx, fy = m1, fz = m2 / sz;
  vec3 q = vec3(local.x * sx, local.y, local.z * sz);
  float y0 = row(19).w, span = geo.x - y0;
  if (q.y <= y0 || span < 1e-6) return;
  float t = min((q.y - y0) / span, 1.0), extra = q.y - y0 - t * span, u = 1.0 - t;
  float s = clamp(local.x / max(mix(geo.y, geo.z, aEnd), 1e-6), -1.0, 1.0);
  vec3 p0 = vec3(0.0, y0, 0.0), p1 = vec3(0.0, y0 + span / 3.0, 0.0);
  vec3 n0 = vec3(nl.x / sx, nl.y, nl.z / sz);
  vec3 P = vec3(0.0), N = vec3(0.0);
  for (int c = 0; c < 3; ++c) {
    float wc = c == 0 ? 1.0 - abs(s) : c == 1 ? max(s, 0.0) : max(-s, 0.0);
    vec3 anchor = c == 0 ? vec3(0.0, 0.0, row(20).w) : vec3(c == 1 ? row(21).w : -row(21).w, 0.0, row(22).w);
    vec3 p2 = row(19 + c * 2).xyz, p3 = row(20 + c * 2).xyz;
    vec3 B = p0 * (u * u * u) + p1 * (3.0 * u * u * t) + p2 * (3.0 * u * t * t) + p3 * (t * t * t);
    vec3 dB = (p1 - p0) * (3.0 * u * u) + (p2 - p1) * (6.0 * u * t) + (p3 - p2) * (3.0 * t * t);
    vec3 T = normalize(dB + vec3(0.0, 1e-7, 0.0));
    vec3 k = vec3(T.z, 0.0, -T.x);
    float c1 = max(1.0 + T.y, 1e-4);
    vec3 off = vec3(q.x, 0.0, q.z) - anchor;
    P += wc * (B + anchor + off * T.y + cross(k, off) + k * (dot(k, off) / c1) + T * extra);
    N += wc * (n0 * T.y + cross(k, n0) + k * (dot(k, n0) / c1));
  }
  w = fx * P.x + fy * P.y + fz * P.z + row(3).xyz;
  n = fx * N.x + fy * N.y + fz * N.z;
}
`;

const VS_BODY = VS_COMMON + `
void main() {
  vec3 nl;
  vec3 local = capsuleLocal(nl);
  vec3 w = row(0).xyz * local.x + row(1).xyz * local.y + row(2).xyz * local.z + row(3).xyz;
  vec3 n = row(4).xyz * nl.x + row(5).xyz * nl.y + row(6).xyz * nl.z;
  if (row(19).w > 0.0) bendHair(local, nl, w, n);
  gl_Position = uViewProj * vec4(w, 1.0);
  vWorld = w;
  vN = n;
  vLocal = local;
  vIid = gl_InstanceID + uBase;
}`;

const VS_OUTLINE = VS_COMMON + `
void main() {
  vec3 nl;
  vec3 local = capsuleLocal(nl);
  vec3 w = row(0).xyz * local.x + row(1).xyz * local.y + row(2).xyz * local.z + row(3).xyz;
  vec3 n = row(4).xyz * nl.x + row(5).xyz * nl.y + row(6).xyz * nl.z;
  if (row(19).w > 0.0) bendHair(local, nl, w, n);
  n = normalize(n);
  vec4 c = uViewProj * vec4(w, 1.0);
  w += n * (uViewport.z * c.w * 2.0 / (uViewport.y * uEye.w));
  gl_Position = row(9).w > 0.5 ? uViewProj * vec4(w, 1.0) : vec4(0.0, 0.0, 2.0, 1.0);
  vWorld = w;
  vN = n;
  vLocal = local;
  vIid = gl_InstanceID + uBase;
}`;

const FS_COMMON = `#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D uInst;
uniform vec4 uEye;
uniform float uBeat;
in vec3 vWorld;
in vec3 vN;
in vec3 vLocal;
flat in int vIid;
out vec4 outColor;
vec4 row(int k) { return texelFetch(uInst, ivec2(k, vIid), 0); }
// smoothstep that also takes reversed edges (Metal allows them; GLSL leaves them undefined).
float sstep(float a, float b, float x) { float t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
float sat(float x) { return clamp(x, 0.0, 1.0); }
`;

const FS_BODY = FS_COMMON + `
float aaStep(float edge, float x, float w) { return sstep(edge - w, edge + w, x); }
float hash21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

vec3 drawFace(vec3 skin, vec3 d) {
  vec4 face = row(11), eyes = row(12), faceA = row(15), faceB = row(16), lips = row(17), beardC = row(18);
  float u = atan(d.x, d.z), v = asin(clamp(d.y, -1.0, 1.0));
  float aa = min(max(fwidth(u), fwidth(v)) * 0.9, 0.1) + 1e-4;
  if (d.z < 0.1) return skin;
  float faceFade = sstep(0.1, 0.3, d.z);
  vec3 col = skin;
  float mouth = face.x, blink = face.y, brow = face.z;
  vec3 eyeC = eyes.rgb;
  int eyeShape = int(faceA.x + 0.5), browStyle = int(faceA.y + 0.5);
  int mouthStyle = int(faceA.z + 0.5), beard = int(faceA.w + 0.5);
  bool lashes = faceB.x > 0.5, freckles = faceB.y > 0.5, lipstick = faceB.w > 0.5;
  vec3 hairC = beardC.rgb;
  for (int s = -1; s <= 1; s += 2) {
    float r = length(vec2(u - float(s) * 0.52, (v + 0.17) * 1.2));
    col = mix(col, vec3(1.0, 0.55, 0.55), faceB.z * (1.0 - sstep(0.03, 0.095, r)));
  }
  if (freckles) {
    for (int k = 0; k < 14; ++k) {
      float fk = float(k);
      vec2 c = vec2((fract(fk * 0.618) - 0.5) * 1.05, -0.1 - 0.11 * fract(fk * 0.377 + 0.2));
      if (abs(c.x) < 0.1) continue;
      float r = length(vec2(u, v) - c);
      col = mix(col, skin * 0.72, 0.8 * (1.0 - aaStep(0.013, r, aa)));
    }
  }
  // Stubble (the other beards are 3D): as DancerPipeline drawFace.
  if (beard == 1) {
    float top = -0.52 + 0.25 * abs(d.x);
    float jaw = sstep(0.03, -0.03, d.y - top) * sstep(0.1, 0.2, d.z);
    float lip = (1.0 - sstep(0.035, 0.06, abs(v + 0.245 + 0.4 * u * u))) * (1.0 - sstep(0.28, 0.34, abs(u)));
    float speck = step(0.45, hash21(floor(vec2(u, v) * 110.0)));
    col = mix(col, mix(col, hairC, 0.55), max(jaw, lip) * (0.3 + 0.4 * speck));
  }
  float mouthV = beard >= 2 && beard != 3 ? -0.35 : -0.30;  // every beard but stubble and goatee
  for (int s = -1; s <= 1; s += 2) {
    float sf = float(s);
    vec2 e = vec2(u - sf * 0.34, v - 0.05);
    float rx = 0.095, ry = 0.155;
    if (eyeShape == 1) { rx = 0.11; ry = 0.12; }
    if (eyeShape == 4) { rx = 0.125; ry = 0.18; }
    if (eyeShape == 5) { rx = 0.05; ry = 0.055; }
    if (eyeShape == 3) {
      float yc = 0.02 - 2.2 * e.x * e.x + 0.06 * (1.0 - blink);
      float dd = abs(e.y - yc) + max(0.0, abs(e.x) - 0.1);
      col = mix(col, eyeC, 1.0 - aaStep(0.02, dd, aa));
    } else {
      ry = ry * (1.0 - 0.93 * blink) + 0.008;
      float q = length(e / vec2(rx, ry));
      float inside = 1.0 - aaStep(1.0, q, aa / min(rx, ry));
      float lid = 0.0;
      if (eyeShape == 2) {
        lid = 1.0 - aaStep(0.015, -e.y + ry * 0.1, aa);
        inside *= 1.0 - lid;
      }
      col = mix(col, eyeC, inside);
      if (blink < 0.5 && eyeShape != 5) {
        float hl = eyeShape == 4 ? 0.045 : 0.034;
        float h = length(e - vec2(0.03, eyeShape == 2 ? 0.0 : 0.06));
        col = mix(col, vec3(1.0), (1.0 - aaStep(hl, h, aa)) * inside);
        if (eyeShape == 4) {
          float h2 = length(e - vec2(-0.035, -0.07));
          col = mix(col, vec3(1.0), (1.0 - aaStep(0.02, h2, aa)) * inside);
        }
      }
      if (eyeShape == 2) {
        float ll = abs(e.y - ry * 0.1) + max(0.0, abs(e.x) - rx);
        col = mix(col, eyeC * 0.8, 1.0 - aaStep(0.012, ll, aa));
      }
      if (lashes && blink < 0.5) {
        for (int k = 0; k < 3; ++k) {
          float th = 0.35 + 0.38 * float(k);
          vec2 base = vec2(sf * rx * cos(th), ry * sin(th));
          if (eyeShape == 2) base.y = min(base.y, ry * 0.1);
          vec2 dir = normalize(vec2(sf * cos(th) / rx, sin(th) / ry) + vec2(sf * 0.6, 0.0) * 9.0);
          vec2 pq = e - base;
          float t = clamp(dot(pq, dir), 0.0, 0.05);
          float dl = length(pq - dir * t);
          col = mix(col, eyeC * 0.7, 1.0 - aaStep(0.008, dl, aa));
        }
      }
    }
    float by = 0.29 + 0.05 * brow + (blink > 0.5 ? -0.02 : 0.0) + (eyeShape == 4 ? 0.04 : 0.0);
    vec2 b = vec2(u - sf * 0.34, v - by);
    b.y += (b.x * sf) * (-0.25 * brow);
    if (browStyle == 3) b.y += 3.0 * b.x * b.x - 0.01;
    float bw = browStyle == 1 ? 0.03 : browStyle == 2 ? 0.009 : 0.017;
    float bl = browStyle == 1 ? 0.085 : 0.075;
    vec2 bcen = vec2(clamp(b.x, -bl, bl), 0.0);
    float bd = length(b - bcen);
    col = mix(col, (browStyle == 1 ? hairC : eyeC) * 0.8, 1.0 - aaStep(bw, bd, aa));
  }
  vec3 lip = lipstick ? lips.rgb : vec3(0.3, 0.07, 0.09);
  float width = mouthStyle == 1 ? 0.26 : mouthStyle == 2 ? 0.08 : 0.18;
  if (mouth < 0.12) {
    float yc = mouthV + (mouthStyle == 1 ? 1.6 : 1.3) * u * u;
    if (mouthStyle == 3) yc += 0.35 * u * (u > 0.0 ? 1.0 : 0.15);
    float dd = abs(v - yc) + max(0.0, abs(u) - width);
    col = mix(col, lip, 1.0 - aaStep(lipstick ? 0.032 : 0.024, dd, aa));
    if (mouthStyle == 1) {
      float t = (1.0 - aaStep(0.03, abs(v - (yc - 0.035)), aa)) * (1.0 - aaStep(width * 0.8, abs(u), aa));
      col = mix(col, vec3(1.0), t * step(v, yc));
    }
  } else {
    float m = clamp((mouth - 0.12) / 0.88, 0.0, 1.0);
    float wk = width / 0.18;
    float rx = (0.13 + 0.05 * m) * wk, ry = 0.04 + 0.1 * m;
    vec2 e = vec2(u, v - mouthV);
    e.y = e.y > 0.0 ? e.y * 2.2 : e.y;
    float q = length(e / vec2(rx, ry));
    float inside = 1.0 - aaStep(1.0, q, aa / ry);
    vec3 inner = vec3(0.32, 0.06, 0.09);
    float tongue = 1.0 - sstep(0.5, 0.7, length((e + vec2(0.0, ry * 0.55)) / vec2(rx * 0.7, ry * 0.5)));
    inner = mix(inner, vec3(0.92, 0.45, 0.5), tongue);
    col = mix(col, inner, inside);
    if (lipstick) col = mix(col, lip, 1.0 - aaStep(0.02, abs(q - 1.0) * ry, aa));
  }
  return mix(skin, col, faceFade);
}

float printMask(int pattern, vec3 local, float len, float period) {
  if (pattern <= 0 || period <= 0.0) return 0.0;
  float r = max(length(local.xz), 1e-3);
  vec2 p = vec2(atan(local.x, local.z) * r, local.y) / period;
  vec2 w = max(fwidth(p), vec2(1e-4));
  if (pattern == 1) { float f = abs(fract(p.y) - 0.5); return sstep(0.25 + w.y, 0.25 - w.y, f); }
  if (pattern == 2) {
    vec2 q = p * 1.3;
    q.x += 0.5 * mod(floor(q.y), 2.0);
    float d = length(fract(q) - 0.5);
    return sstep(0.24 + w.x * 1.3, 0.24 - w.x * 1.3, d);
  }
  if (pattern == 3) {
    float a = sstep(0.3 + w.y, 0.3 - w.y, abs(fract(p.y) - 0.5));
    float b = sstep(0.3 + w.x, 0.3 - w.x, abs(fract(p.x) - 0.5));
    float ln = sstep(0.06 + w.y, 0.06 - w.y, abs(fract(p.y * 2.0 + 0.25) - 0.5));
    return clamp(0.4 * a + 0.4 * b + 0.5 * ln * (1.0 - a), 0.0, 1.0);
  }
  if (pattern == 4) return sstep(0.0, 1.0, clamp(local.y / max(len, 0.05), 0.0, 1.0)) * 0.85;
  if (pattern == 5) {
    float z = p.y + 0.6 * abs(fract(p.x * 0.8) - 0.5);
    float f = abs(fract(z) - 0.5);
    return sstep(0.2 + w.y * 1.5, 0.2 - w.y * 1.5, f);
  }
  return 0.0;
}

// Hair (material 4): as DancerPipeline hairLook.
float strandNoise(float x) {
  float i = floor(x), f = fract(x);
  float a = hash21(vec2(mod(i + 600.0, 60.0), 0.37));
  float b = hash21(vec2(mod(i + 601.0, 60.0), 0.37));
  return mix(a, b, f * f * (3.0 - 2.0 * f));
}
// Returns the albedo factor (< 0: the parting's scalp); v: the strand's value; T: the strand direction (or 0).
float hairLook(int look, vec3 q, float part, vec3 n, vec3 ax, vec3 ay, vec3 az, out float v, out vec3 T) {
  v = 0.5;
  T = vec3(0.0);
  int mode = look & 7;
  if (mode <= 2) {
    float s, fw;
    if (mode == 1) { s = q.z * 11.0; fw = fwidth(s); T = cross(n, az); }
    else if (mode == 2) { s = q.x * 11.0; fw = fwidth(s); T = cross(n, ax); }
    else {
      s = atan(q.x, q.z) * 9.5492966;
      fw = length(fwidth(q.xz)) / max(length(q.xz), 1e-3) * 9.5492966;
      T = ay - n * dot(n, ay);
    }
    float sv = 0.62 * strandNoise(s) + 0.38 * strandNoise(s * 2.7 + 13.0);
    float c = 1.0 - sstep(0.3, 0.8, fw);
    v = mix(0.5, sv, c);
    float factor = 1.0 + (sv - 0.5) * 0.6 * c;
    T = normalize(T + 1e-5);
    if (mode == 1 && part < 5.0) {
      float d = abs(q.x - part);
      float m = (1.0 - sstep(0.02, 0.02 + 1.5 * fwidth(q.x), d)) * sstep(0.25, 0.5, q.y) * sstep(-0.55, -0.05, q.z);
      factor = mix(factor, -1.0, m);
    }
    return factor;
  }
  if (mode == 3) {
    float ph = (q.y * 7.0 + 0.25 * sin(atan(q.x, q.z) * 3.0)) * 6.2831853;
    float c = 1.0 - sstep(0.25, 0.6, fwidth(q.y * 7.0));
    return 1.0 + 0.28 * sin(ph) * c;
  }
  if (mode != 4) return 1.0;  // smooth
  vec3 p = q * 9.0;
  float k = sin(p.x + 2.0 * sin(p.y * 0.8)) * sin(p.y + 2.0 * sin(p.z * 0.8)) * sin(p.z + 2.0 * sin(p.x * 0.8));
  float c = 1.0 - sstep(0.35, 0.9, length(fwidth(p)));
  return 1.0 + 0.35 * k * c;
}

float lensShape(int shape, int side, vec2 q) {
  if (shape == 2) return length(vec2(q.x * (1.0 + 0.2 * q.y), q.y * 1.05 + 0.12 * q.y * q.y)) - 1.0;
  if (shape == 3 || shape == 8) return pow(pow(abs(q.x), 4.0) + pow(abs(q.y), 4.0), 0.25) - 1.0;
  if (shape == 4) {
    vec2 h = vec2(abs(q.x), q.y * 1.1 + 0.55) * 0.62;
    if (h.y + h.x > 1.0) return (length(h - vec2(0.25, 0.75)) - 0.3536) / 0.62;
    float a = dot(h - vec2(0.0, 1.0), h - vec2(0.0, 1.0));
    vec2 m = h - 0.5 * max(h.x + h.y, 0.0);
    return sqrt(min(a, dot(m, m))) * sign(h.x - h.y) / 0.62;
  }
  if (shape == 5) {
    vec2 k1 = vec2(0.809016994, -0.587785252), k2 = vec2(-k1.x, k1.y);
    vec2 p = vec2(abs(q.x), q.y * 1.05 + 0.08);
    p -= 2.0 * max(dot(k1, p), 0.0) * k1;
    p -= 2.0 * max(dot(k2, p), 0.0) * k2;
    p.x = abs(p.x);
    p.y -= 1.0;
    vec2 ba = 0.5 * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
    float hh = clamp(dot(p, ba) / dot(ba, ba), 0.0, 1.0);
    return length(p - ba * hh) * sign(p.y * ba.x - p.x * ba.y);
  }
  if (shape == 6) {
    float xo = side == 0 ? q.x : -q.x;
    return length(q) - 1.0 - 0.4 * sstep(0.1, 1.0, xo) * sstep(-0.1, 0.9, q.y);
  }
  if (shape == 7) return pow(pow(abs(q.x), 6.0) + pow(abs(q.y), 6.0), 1.0 / 6.0) - 1.0;
  return length(q) - 1.0;
}

void main() {
  vec4 c0 = row(7), c1 = row(8), geo = row(9), clip = row(10), style = row(13), ink = row(14);
  if (dot(vLocal, clip.xyz) > clip.w) discard;
  int material = int(c0.w + 0.5);
  vec4 clip2 = row(17);
  if (material == 4 && dot(vLocal, clip2.xyz) > clip2.w) discard;
  vec3 n = normalize(vN);
  float split = c1.w;
  float sw = max(fwidth(vLocal.y), 1e-5);
  float region = material == 3 ? 0.0 : sstep(split - sw, split + sw, vLocal.y);
  vec3 albedo = mix(c0.rgb, c1.rgb, region);
  int ri = region > 0.5 ? 1 : 0;
  int fabric = int((ri == 0 ? style.z : style.w) + 0.5);
  float beat = uBeat;
  float pulse = exp(-fract(beat) * 4.0);
  float lensRim = 0.0;
  if (material == 3) {
    float squash = max(c1.w, 0.3);
    vec2 q = vec2(vLocal.x, vLocal.y / squash) / max(geo.y, 1e-4);
    float d = lensShape(int(style.x + 0.5), int(style.y + 0.5), q);
    float aa = max(fwidth(d), 1e-4);
    if (d > aa) discard;
    int kind = int(style.z + 0.5);
    float rimW = kind == 2 ? 0.26 : 0.2;
    lensRim = sstep(-rimW - aa, -rimW + aa, d);
    if (kind == 2 && lensRim < 0.5) discard;
    fabric = kind == 1 ? 1 : 0;
  } else if (material != 1) {
    int pat = int((ri == 0 ? style.x : style.y) + 0.5);
    float inkM = printMask(pat, vLocal, geo.x, ink.w);
    albedo = mix(albedo, ink.rgb, inkM);
  }
  if ((material == 2 || material == 4) && !gl_FrontFacing) {
    n = normalize(mix(-n, normalize(uEye.xyz - vWorld), 0.7));
    albedo *= 0.85;
  }
  if (material == 1) albedo = drawFace(albedo, normalize(vLocal));
  float hairV = 0.5;
  vec3 hairT = vec3(0.0);
  if (material == 4) {
    float f = hairLook(int(row(11).w + 0.5), vLocal / max(geo.y, 1e-4), row(12).w, n, normalize(row(0).xyz),
                       normalize(row(1).xyz), normalize(row(2).xyz), hairV, hairT);
    if (!gl_FrontFacing) hairT = vec3(0.0);
    albedo = f < 0.0 ? mix(albedo, vec3(0.42, 0.31, 0.26), 0.7) : albedo * f;
  }
  vec3 V = normalize(uEye.xyz - vWorld);
  if (fabric == 4) {
    float core = pow(sat(dot(n, V)), 2.0);
    vec3 hue = albedo / max(max(albedo.r, max(albedo.g, albedo.b)), 1e-3);
    vec3 col = hue * (0.95 + 0.35 * pulse) * (0.75 + 0.35 * core) + vec3(0.12) * core * core;
    outColor = vec4(col, 1.0);
    return;
  }
  // The studio light (reforce.Dancer.Lighting 0).
  vec3 Lk = normalize(vec3(-0.45, 0.75, 0.55));
  float wrapL = sat((dot(n, Lk) + 0.35) / 1.35);
  float toon = mix(wrapL, sstep(0.4, 0.52, wrapL), 0.55);
  vec3 hemi = mix(vec3(0.30, 0.28, 0.36), vec3(0.55, 0.56, 0.64), n.y * 0.5 + 0.5);
  vec3 lit = hemi * 0.62 + vec3(1.0, 0.96, 0.9) * toon * 0.72;
  float fres = pow(1.0 - sat(dot(n, V)), 3.0);
  vec3 h = normalize(Lk + V);
  vec3 col;
  if (fabric == 1 || fabric == 2) {
    vec3 Rr = reflect(-V, n);
    vec3 refl = mix(vec3(0.12, 0.11, 0.14), vec3(1.1, 1.05, 1.0), sstep(-0.3, 0.6, Rr.y));
    vec3 base = albedo * (0.25 * lit + 0.95 * refl);
    float sparkle = 0.0;
    if (fabric == 2) {
      float r = max(length(vLocal.xz), 1e-3);
      vec2 cp = vec2(atan(vLocal.x, vLocal.z) * r, vLocal.y) / max(ink.w * 0.22, 1e-4);
      vec2 cell = floor(cp);
      float hsh = hash21(cell);
      base *= 0.7 + 0.6 * hsh;
      float tw = fract(hsh * 13.7 + floor(beat * 2.0) * 0.618);
      float dot0 = 1.0 - sstep(0.15, 0.42, length(fract(cp) - 0.5));
      sparkle = step(0.86, tw) * dot0 * (0.6 + 0.8 * pulse);
    }
    col = base + pow(sat(dot(n, h)), 70.0) * 1.1 + sparkle * vec3(1.6) + fres * albedo * 0.6;
  } else if (fabric == 3) {
    float sheen = pow(1.0 - sat(dot(n, V)), 1.6);
    col = albedo * lit * 0.62 + albedo * sheen * 0.9;
  } else {
    col = albedo * lit;
    if (material == 4 && dot(hairT, hairT) > 0.5) {
      float th = dot(hairT, h) + (hairV - 0.5) * 0.35 - 0.1;
      float sn = sqrt(sat(1.0 - th * th));
      col += (pow(sn, 90.0) * 0.3 + pow(sn, 14.0) * 0.2 * (albedo + 0.05)) * (0.35 + 0.65 * toon);
    } else {
      float spec = pow(sat(dot(n, h)), 40.0) * (material == 2 || material == 4 ? 0.12 : 0.28);
      col += spec;
    }
  }
  if (material == 3) {
    if (fabric == 0) col = albedo * lit * 0.6 + pow(sat(dot(n, h)), 30.0) * 0.9 + fres * 0.3;
    float glint = sstep(0.75, 1.0, sat(dot(normalize(vec2(vLocal.x, vLocal.y) + 1e-5), vec2(-0.6, 0.8))));
    col += glint * 0.25 * (1.0 - lensRim);
    col = mix(col, c1.rgb * lit, lensRim);
    outColor = vec4(col, 1.0);
    return;
  }
  col += fres * vec3(0.3) * (fabric == 3 ? 0.25 : 0.7);
  outColor = vec4(col, 1.0);
}`;

const FS_OUTLINE = FS_COMMON + `
void main() {
  vec4 c0 = row(7), clip = row(10), clip2 = row(17);
  if (dot(vLocal, clip.xyz) > clip.w) discard;
  if (int(c0.w + 0.5) == 4 && dot(vLocal, clip2.xyz) > clip2.w) discard;
  outColor = vec4(c0.rgb * 0.18 + 0.01, 1.0);
}`;

// render/DancerPipeline capsuleMesh: a unit sphere split at the equator, the lower half at end 0 and the upper half
// at end 1 (`bandRings` rings between them: the tube mesh, long hair that bends). Counter-clockwise from outside.
function capsuleMesh(segments = 32, rings = 12, bandRings = 0) {
  const rs = [];
  for (let i = 0; i <= rings; ++i) { const a = -Math.PI / 2 + (Math.PI / 2) * i / rings; rs.push([Math.sin(a), Math.cos(a), 0]); }
  for (let i = 1; i <= bandRings; ++i) rs.push([0, 1, i / (bandRings + 1)]);
  for (let i = 0; i <= rings; ++i) { const a = (Math.PI / 2) * i / rings; rs.push([Math.sin(a), Math.cos(a), 1]); }
  const cols = segments + 1;
  const verts = new Float32Array(rs.length * cols * 4);
  let k = 0;
  for (const [y, r, end] of rs)
    for (let s = 0; s < cols; ++s) {
      const t = 2 * Math.PI * s / segments;
      verts[k++] = r * Math.sin(t); verts[k++] = y; verts[k++] = r * Math.cos(t); verts[k++] = end;
    }
  const idx = [];
  for (let i = 0; i + 1 < rs.length; ++i)
    for (let s = 0; s < segments; ++s) {
      const a = i * cols + s, b = i * cols + s + 1, c = (i + 1) * cols + s + 1, d = (i + 1) * cols + s;
      idx.push(a, b, c, a, c, d);
    }
  return { verts, indices: new Uint16Array(idx) };
}

function compile(gl, vs, fs) {
  const p = gl.createProgram();
  for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error('dancer shader: ' + gl.getShaderInfoLog(s));
    gl.attachShader(p, s);
  }
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error('dancer program: ' + gl.getProgramInfoLog(p));
  return p;
}

// Column-major 4x4 helpers.
function perspective(fovY, aspect, near, far) {
  const f = 1 / Math.tan(fovY / 2), nf = 1 / (near - far);
  return [f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0];
}
function lookAt(eye, target) {
  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const norm = (a) => { const l = Math.hypot(...a); return a.map((x) => x / l); };
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const z = norm(sub(eye, target)), x = norm(cross([0, 1, 0], z)), y = cross(z, x);
  return [x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1];
}
function mul(a, b) {
  const o = new Array(16).fill(0);
  for (let c = 0; c < 4; ++c) for (let r = 0; r < 4; ++r) for (let k = 0; k < 4; ++k) o[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
  return o;
}

export function dancerKeysText(keys) {
  return Object.entries(keys).map(([k, v]) => `${k}=${typeof v === 'boolean' ? (v ? 1 : 0) : v}`).join('\n');
}

export async function createDancerView(canvas, opts = {}) {
  const gl = canvas.getContext('webgl2', { antialias: true, alpha: true, premultipliedAlpha: true });
  if (!gl) throw new Error('WebGL2 is not available');
  const mod = await createDancerModule();
  const floats = mod._dancer_floats();
  if (floats !== FLOATS) throw new Error('dancer.js and dancer_gl.js are out of step');

  const body = compile(gl, VS_BODY, FS_BODY);
  const outline = compile(gl, VS_OUTLINE, FS_OUTLINE);
  // The capsule mesh, and the tube mesh for the bent parts (long hair that bends).
  const meshVao = (mesh) => {
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    const vb = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vb);
    gl.bufferData(gl.ARRAY_BUFFER, mesh.verts, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 1, gl.FLOAT, false, 16, 12);
    const ib = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, mesh.indices, gl.STATIC_DRAW);
    return { vao, count: mesh.indices.length };
  };
  const capsule = meshVao(capsuleMesh()), tube = meshVao(capsuleMesh(32, 12, 24));
  const tex = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, tex);
  gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32F, TEXELS, MAX_PARTS);
  for (const p of [gl.TEXTURE_MIN_FILTER, gl.TEXTURE_MAG_FILTER]) gl.texParameteri(gl.TEXTURE_2D, p, gl.NEAREST);
  const loc = (p, n) => gl.getUniformLocation(p, n);
  const U = {};
  for (const [name, p] of [['b', body], ['o', outline]])
    U[name] = { inst: loc(p, 'uInst'), vp: loc(p, 'uViewProj'), eye: loc(p, 'uEye'), viewport: loc(p, 'uViewport'), beat: loc(p, 'uBeat'),
                base: loc(p, 'uBase') };

  let bpm = opts.bpm || 120, raf = 0, keysText = '', stopped = false;
  const seed = opts.seed || 7;
  const t0 = performance.now() / 1000;
  // Framing (2026-11-21): a fixed height, room for the tallest guest (Tall 1.1) with an afro, so the Height setting
  // shows as standing taller or shorter rather than as the camera zooming; only a top hat on a tall dancer needs more.
  const FRAME_TOP = 1.24;
  let top = FRAME_TOP;

  function setKeys(keys) {
    const text = typeof keys === 'string' ? keys : dancerKeysText(keys);
    if (text === keysText) return;
    keysText = text;
    const bytes = new TextEncoder().encode(text + '\0');
    const ptr = mod._malloc(bytes.length);
    mod.HEAPU8 ? mod.HEAPU8.set(bytes, ptr) : new Uint8Array(mod.HEAPF32.buffer).set(bytes, ptr);
    mod._dancer_set(ptr, seed, performance.now() / 1000 - t0);
    mod._free(ptr);
  }

  function frame() {
    if (stopped) return;
    raf = requestAnimationFrame(frame);
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.round(canvas.clientWidth * dpr), h = Math.round(canvas.clientHeight * dpr);
    if (!w || !h) return;
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    const t = performance.now() / 1000 - t0;
    const all = mod._dancer_frame(t, bpm), n = Math.min(all, MAX_PARTS);
    const bent = all <= MAX_PARTS ? mod._dancer_bent() : 0;  // the last `bent` parts: drawn with the tube mesh
    gl.viewport(0, 0, w, h);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (!n) return;
    const data = new Float32Array(mod.HEAPF32.buffer, mod._dancer_parts(), n * FLOATS);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, TEXELS, n, gl.RGBA, gl.FLOAT, data);
    // Framing: from the floor to the fixed height, or to a steadied top of the head (hair, hat) where that is higher.
    top += (Math.max(FRAME_TOP, mod._dancer_head_top()) - top) * 0.05;
    const fov = 28 * Math.PI / 180, aspect = w / h;
    const halfH = (top + 0.12) / 2 * 1.08;
    const dist = Math.max(halfH, halfH * 0.75 / aspect) / Math.tan(fov / 2);
    const target = [0, (top + 0.12) / 2 - 0.03, 0];
    const eye = [0, target[1] + dist * 0.12, dist];
    const vp = mul(perspective(fov, aspect, 0.05, 30), lookAt(eye, target));
    const proj11 = 1 / Math.tan(fov / 2);
    gl.enable(gl.DEPTH_TEST);
    gl.activeTexture(gl.TEXTURE0);
    for (const [prog, u, cull] of [[body, U.b, false], [outline, U.o, true]]) {
      gl.useProgram(prog);
      gl.uniform1i(u.inst, 0);
      gl.uniformMatrix4fv(u.vp, false, vp);
      gl.uniform4f(u.eye, eye[0], eye[1], eye[2], proj11);
      gl.uniform4f(u.viewport, w, h, 2.2 * dpr, 0);  // outline width in device pixels
      if (u.beat) gl.uniform1f(u.beat, t * bpm / 60);
      if (cull) { gl.enable(gl.CULL_FACE); gl.cullFace(gl.FRONT); } else gl.disable(gl.CULL_FACE);
      for (const [m, base, count] of [[capsule, 0, n - bent], [tube, n - bent, bent]]) {
        if (count <= 0) continue;
        gl.bindVertexArray(m.vao);
        gl.uniform1i(u.base, base);
        gl.drawElementsInstanced(gl.TRIANGLES, m.count, gl.UNSIGNED_SHORT, 0, count);
      }
    }
  }

  // The web sample of mocap moves (moves.bin next to this file, or opts.moves; null: none): once it arrives the
  // dancer is made again and dances its style's recorded moves (until then, and without it, the hand-made ones).
  const movesUrl = opts.moves === undefined ? new URL('./moves.bin', import.meta.url) : opts.moves;
  if (movesUrl) {
    fetch(movesUrl).then((r) => (r.ok ? r.arrayBuffer() : null)).then((buf) => {
      if (!buf || stopped) return;
      const bytes = new Uint8Array(buf);
      const ptr = mod._malloc(bytes.length);
      mod.HEAPU8.set(bytes, ptr);
      const n = mod._dancer_moves(ptr, bytes.length);
      mod._free(ptr);
      if (opts.onMoves) opts.onMoves(n);  // how many moves loaded (-1: a bad file)
      if (n > 0 && keysText) {
        const keys = keysText;
        keysText = '';
        setKeys(keys);
      }
    }).catch(() => {});
  }

  if (opts.keys) setKeys(opts.keys);
  frame();
  return {
    setKeys,
    setBpm(v) { bpm = v; },
    destroy() { stopped = true; cancelAnimationFrame(raf); },
  };
}
