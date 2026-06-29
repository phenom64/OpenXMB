/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Original background particles (vertex)
#version 450

layout(location=0) in vec2 inPos;   // unit quad vertices in [-0.5,0.5]
layout(location=1) in vec2 inSeed;  // per-instance seed in [0,1)

layout(push_constant) uniform PC {
    vec4 tint;        // RGB base
    vec2 resolution;  // width,height
    float time;       // seconds
    float brightness; // 0..1
} pc;

layout(location=0) out vec2 vLocal;  // pass unit quad coord to frag
layout(location=1) out float vAlpha; // per-sprite alpha
layout(location=2) out float vGlint; // rare soft specular lift

// Dave Hoskins—style hash/value noise (2D)
float hash12(vec2 p){
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673U, 3812015801U);
    uint n = (q.x ^ q.y) * 1597334673U; return float(n) * 2.328306437080797e-10;
}
float value2d(vec2 p){
    vec2 pg=floor(p),pc=p-pg,k=vec2(0,1);
    pc*=pc*pc*(3.-2.*pc);
    return mix(mix(hash12(pg+k.xx),hash12(pg+k.yx),pc.x), mix(hash12(pg+k.xy),hash12(pg+k.yy),pc.x), pc.y);
}

// Screen-space approximation of xmb-web's particle cloud: a wave-coupled bokeh/glitter band,
// not a full-screen star field. The analytic crest is
// intentionally broad and low-frequency so it follows the captured ribbon's
// perceived sweep without adding a second obvious waveform.
float crestY(float x, float t) {
    float sweepPhase = t * 0.18;
    float primary = sin(x * 4.15 - 1.30 + sweepPhase);
    float secondary = sin(x * 2.0 + sweepPhase * 0.6);
    return 0.505
         + 0.115 * primary
         - 0.035 * secondary
         + 0.012 * sin(x * 5.6549 + t * 0.35);
}

void main(){
    // Smooth, non-teleport drift: sample value noise along time-varying lines.
    // The cloud stays bound to the wave lane; drift only breathes it.
    float t = pc.time * 0.038;
    vec2 s = inSeed*64.0; // domain scale
    vec2 drift;
    drift.x = value2d(s + vec2(0.0, t)) - 0.5;
    drift.y = value2d(s + vec2(37.13, t*1.2)) - 0.5;
    drift *= 0.055;

    float x = mix(-0.08, 1.08, inSeed.x) + drift.x;
    float lane = inSeed.y - 0.62;
    float laneNoise = value2d(s * 0.31 + vec2(11.0, -pc.time * 0.015));
    float spread = mix(0.020, 0.074, clamp(x, 0.0, 1.0)) *
                   mix(0.62, 1.18, laneNoise);
    float y = crestY(clamp(x, 0.0, 1.0), pc.time) + lane * spread + drift.y;
    vec2 center = vec2(x, y) * 2.0 - 1.0;         // normalized screen -> NDC

    float edgeFade = smoothstep(-0.08, 0.06, x) * smoothstep(1.08, 0.90, x);
    float laneFade = 1.0 - smoothstep(0.18, 0.58, abs(lane));
    float bandEnergy = edgeFade * mix(0.40, 1.0, laneFade);

    // Sprite size: soft bokeh specks, larger than star pixels and widest near
    // the frayed right side of the wave.
    float n = value2d(s + vec2(123.7, 913.1));
    float shimmer = mix(0.82, 1.10, value2d(s*0.45 + vec2(pc.time*0.09, -pc.time*0.04)));
    float edgeGlint = step(0.92, value2d(s + vec2(5.1, 91.7)));
    float px = mix(3.20, 9.50, n) *
               mix(0.92, 1.42, clamp(x, 0.0, 1.0)) *
               mix(1.0, 1.35, edgeGlint) *
               (0.58 + 0.42*pc.brightness) * shimmer;
    vec2 halfSize = vec2(px/pc.resolution.y);     // keep aspect-independent

    // Expand the unit quad about the center
    vec2 pos = center + inPos * halfSize * 2.0;
    gl_Position = vec4(pos, 0.0, 1.0);

    vLocal = inPos;
    vGlint = edgeGlint * smoothstep(0.45, 1.0, bandEnergy) *
             (0.35 + 0.35 * sin(pc.time * 2.74 + n * 31.416));
    vAlpha = clamp(mix(0.008, 0.060, n) * pc.brightness * bandEnergy,
                   0.0, 0.075);
}
