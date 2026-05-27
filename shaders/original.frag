/*
 * Copyright (c) 2026 OpenXMB
 *
 * This file is part of OpenXMB.
 *
 * OpenXMB is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * OpenXMB is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * OpenXMB. If not, see <https://www.gnu.org/licenses/>.
 */

#version 450

layout(push_constant) uniform Push {
  vec4 tint;
  vec2 resolution;
  float time;
  float brightness;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

float hash12(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}

float noise2(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);

  float a = hash12(i);
  float b = hash12(i + vec2(1.0, 0.0));
  float c = hash12(i + vec2(0.0, 1.0));
  float d = hash12(i + vec2(1.0, 1.0));

  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
  float v = 0.0;
  float a = 0.5;
  mat2 r = mat2(0.82, -0.57, 0.57, 0.82);
  for (int i = 0; i < 4; ++i) {
    v += a * noise2(p);
    p = r * p * 2.03 + vec2(11.7, 4.2);
    a *= 0.5;
  }
  return v;
}

float ribbon_curve(float x, float seed, float t) {
  float slow = t * (0.045 + seed * 0.011);
  float y = sin(x * (1.15 + seed * 0.07) + slow + seed * 3.1) * 0.115;
  y += sin(x * (2.05 + seed * 0.11) - slow * 1.45 + seed * 6.4) * 0.045;
  y += sin(x * (3.10 + seed * 0.19) + slow * 0.72 + seed * 2.4) * 0.018;
  return y;
}

float ribbon_mask(vec2 p, float seed, float t, float width, out float glow) {
  float y = ribbon_curve(p.x, seed, t) + seed * 0.045 - 0.055;
  float d = abs(p.y - y);
  float core = 1.0 - smoothstep(width * 0.12, width, d);
  glow = 1.0 - smoothstep(width, width * 7.0, d);
  return core;
}

float dust_layer(vec2 uv, float scale, float drift, float threshold) {
  vec2 p = uv * scale + vec2(drift * 0.07, -drift * 0.035);
  vec2 cell = floor(p);
  vec2 f = fract(p) - 0.5;
  float id = hash12(cell);
  float star = smoothstep(threshold, 1.0, id);
  float size = mix(0.012, 0.055, hash12(cell + 7.31));
  float sparkle = 1.0 - smoothstep(size * 0.25, size, length(f));
  float twinkle = 0.55 + 0.45 * sin(pc.time * (0.6 + id * 1.7) + id * 12.0);
  return star * sparkle * twinkle;
}

void main() {
  vec2 safeResolution = max(pc.resolution, vec2(1.0));
  vec2 uv = vUV;
  vec2 p = uv * 2.0 - 1.0;
  p.x *= safeResolution.x / safeResolution.y;

  vec3 tint = clamp(pc.tint.rgb, vec3(0.0), vec3(1.0));
  vec3 deep = tint * vec3(0.18, 0.22, 0.32);
  vec3 mid = tint * vec3(0.48, 0.55, 0.72);
  vec3 top = tint * vec3(0.72, 0.78, 0.96);

  float vertical = smoothstep(-0.85, 0.95, p.y);
  float leftGlow = exp(-length((p - vec2(-1.28, 0.22)) * vec2(0.64, 1.05)) * 1.35);
  float horizonGlow = exp(-abs(p.y + 0.03) * 1.9) * (1.0 - smoothstep(-0.25, 1.65, abs(p.x)));
  float vignette = 1.0 - smoothstep(0.18, 1.42, length(p * vec2(0.74, 1.03)));

  vec3 color = mix(deep, mid, vertical);
  color += top * leftGlow * 0.22;
  color += tint * horizonGlow * 0.15;

  float t = pc.time;
  float totalGlow = 0.0;
  vec3 waveColor = mix(vec3(0.83, 0.88, 1.0), tint + vec3(0.28), 0.34);

  float glow0;
  float core0 = ribbon_mask(p + vec2(0.00, 0.012), 0.2, t, 0.022, glow0);
  float glow1;
  float core1 = ribbon_mask(p + vec2(0.16, -0.018), 1.1, t, 0.017, glow1);
  float glow2;
  float core2 = ribbon_mask(p + vec2(-0.12, 0.030), 2.0, t, 0.013, glow2);
  float glow3;
  float core3 = ribbon_mask(p + vec2(0.08, -0.048), 3.0, t, 0.010, glow3);
  float glow4;
  float core4 = ribbon_mask(p + vec2(-0.22, 0.056), 3.8, t, 0.018, glow4);

  float core = core0 * 0.42 + core1 * 0.34 + core2 * 0.26 + core3 * 0.18 + core4 * 0.12;
  totalGlow = glow0 * 0.20 + glow1 * 0.18 + glow2 * 0.14 + glow3 * 0.10 + glow4 * 0.08;

  float crossing = smoothstep(0.05, 0.75, fbm(vec2(p.x * 0.74, p.y * 1.8 + t * 0.018)));
  core *= mix(0.78, 1.26, crossing);

  color += waveColor * totalGlow * 0.30;
  color = mix(color, vec3(0.92, 0.96, 1.0), clamp(core * 0.62, 0.0, 0.72));
  color += vec3(0.80, 0.88, 1.0) * pow(clamp(core, 0.0, 1.0), 2.2) * 0.34;

  vec2 dustUv = vec2(uv.x * (safeResolution.x / safeResolution.y), uv.y);
  float dust = dust_layer(dustUv, 42.0, t, 0.986);
  dust += dust_layer(dustUv + vec2(13.1, 4.7), 76.0, t * 0.7, 0.992) * 0.65;
  float dustGate = 0.38 + totalGlow * 1.65 + leftGlow * 0.22;
  color += vec3(0.90, 0.96, 1.0) * dust * dustGate;

  float fineNoise = fbm(uv * vec2(5.0, 3.0) + vec2(t * 0.006, -t * 0.004));
  color += (fineNoise - 0.5) * 0.018;
  color *= mix(0.68, 1.0, vignette);
  color *= clamp(pc.brightness, 0.0, 1.0);

  FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
