// shaders/WaveShader.frag
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.
// Adapted from Shadertoy PS3 XMB wave by int_45h.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform UniformBlock {
  float time;
  float speed;
  float amplitude;
  float frequency;
  vec4  baseColor;
  vec4  waveColor;
  float threshold;
  float dustIntensity;
  float minDist;
  float maxDist;
  int   maxDraws;
  vec2  resolution;
  float brightness;
  float pad;
} ub;

#define THRESHOLD ub.threshold
#define MIN_DIST  ub.minDist
#define MAX_DIST  ub.maxDist
#define MAX_DRAWS ub.maxDraws

// Float-only hash (iq style) to avoid uint literals across all backends.
float hash12(vec2 p) {
  float h = dot(p, vec2(127.1, 311.7));
  return fract(sin(h) * 43758.5453123);
}

float value2d(vec2 p) {
  vec2 pg = floor(p), pc = p - pg, k = vec2(0.0, 1.0);
  pc *= pc * pc * (3.0 - 2.0 * pc);
  return mix(
           mix(hash12(pg + k.xx), hash12(pg + k.yx), pc.x),
           mix(hash12(pg + k.xy), hash12(pg + k.yy), pc.x),
           pc.y);
}

float get_stars_rough(vec2 p) {
  float s = smoothstep(THRESHOLD, 1.0, hash12(p));
  if (s >= THRESHOLD) s = pow((s - THRESHOLD) / (1.0 - THRESHOLD), 10.0);
  return s;
}

float get_stars(vec2 p, float a, float t) {
  vec2 pg = floor(p), pc = p - pg, k = vec2(0.0, 1.0);
  pc *= pc * pc * (3.0 - 2.0 * pc);
  float s = mix(
      mix(get_stars_rough(pg + k.xx), get_stars_rough(pg + k.yx), pc.x),
      mix(get_stars_rough(pg + k.xy), get_stars_rough(pg + k.yy), pc.x),
      pc.y);
  return smoothstep(a, a + t, s) *
         pow(value2d(p * 0.1 + ub.time) * 0.5 + 0.5, 8.3);
}

float get_dust(vec2 p, vec2 size, float f) {
  vec2 ar = vec2(ub.resolution.x / ub.resolution.y, 1.0);
  vec2 pp = p * size * ar;
  return pow(0.64 + 0.46 * cos(p.x * 6.28), 1.7) *
         f *
         (get_stars(0.1 * pp + ub.time * vec2(20.0, -10.1), 0.11, 0.71) *
              4.0 +
          get_stars(0.2 * pp + ub.time * vec2(30.0, -10.1), 0.1, 0.31) *
              5.0 +
          get_stars(0.32 * pp + ub.time * vec2(40.0, -10.1), 0.1, 0.91) *
              2.0);
}

float sdf(vec3 p) {
  p *= 2.0;
  float o = 4.2 * sin(0.05 * p.x + ub.time * 0.25 * ub.speed) +
            (0.04 * p.z) * sin(p.x * 0.11 + ub.time * ub.speed) * 2.0 *
                sin(p.z * 0.2 + ub.time * ub.speed) *
                value2d(vec2(0.03, 0.4) * p.xz +
                        vec2(ub.time * 0.5 * ub.speed, 0));
  return abs(dot(p, normalize(vec3(0, 1, 0.05))) + 2.5 +
             o * 0.5 * ub.amplitude);
}

vec2 raymarch(vec3 o, vec3 d, float omega) {
  float t = 0.0, a = 0.0;
  float g = MAX_DIST, dt = 0.0, sl = 0.0, emin = 0.03, ed = emin;
  int dr = 0;
  bool hit = false;

  for (int i = 0; i < 100; i++) {
    vec3 p = o + d * t;
    float ndt = sdf(p);
    if (abs(dt) + abs(ndt) < sl) {
      sl -= omega * sl;
      omega = 1.0;
    } else {
      sl = ndt * omega;
    }
    dt = ndt;
    t += sl;
    g = (t > 10.0) ? min(g, abs(dt)) : MAX_DIST;
    if ((t += dt) >= MAX_DIST) break;
    if (dt < MIN_DIST) {
      if (dr > MAX_DRAWS) break;
      dr++;
      float f = smoothstep(0.09, 0.11, (p.z * 0.9) / 100.0);
      if (!hit) {
        a = 0.01;
        hit = true;
      }
      ed = 2.0 * max(emin, abs(ndt));
      a += 0.0135 * f * ub.frequency;
      t += ed;
    }
  }
  g /= 3.0;
  return vec2(a, max(1.0 - g, 0.0));
}

void main() {
  vec2 uv = vUV;
  vec3 o = vec3(0);
  vec3 d = vec3((uv - 0.5) * vec2(ub.resolution.x / ub.resolution.y, 1.0), 1.0);
  vec2 mg = raymarch(o, d, 1.2);
  float m = mg.x;

  vec3 c = mix(mix(ub.baseColor.rgb, ub.waveColor.rgb, uv.x),
               mix(ub.waveColor.rgb, ub.baseColor.rgb, uv.x), uv.y);

  c = mix(c, vec3(1.0), m);
  c += get_dust(uv, vec2(2000.0), mg.y) * 0.3 * ub.dustIntensity;

  c *= clamp(ub.brightness, 0.0, 1.5);

  fragColor = vec4(c, 1.0);
}