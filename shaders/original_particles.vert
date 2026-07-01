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

layout(location=0) in vec2 inPos;       // unit quad vertices in [-0.5,0.5]
layout(location=1) in vec4 inHomeAge;   // xyz = xmb-web eye-space home, w = lifetime phase
layout(location=2) in vec4 inParams;    // xy = seeds, z = edge flag, w = size scale
layout(location=3) in vec4 inTintSpin;  // rgb = edge tint, a = normal spin phase 0
layout(location=4) in vec4 inSpinMisc;  // x = phase 1, y/z = spin rates

layout(push_constant) uniform PC {
    vec4 tint;                         // retained for ABI/tint evolution
    vec4 resolution_time_brightness;   // xy=width/height, z=time, w=brightness
    vec4 particle_params;              // x=night/day blend
} pc;

layout(location=0) out vec2 vLocal;    // pass unit quad coord to frag
layout(location=1) out float vBright;  // HDR particle brightness
layout(location=2) out vec3 vColor;    // iridescent/metallic particle colour
layout(location=3) out float vSpark;   // diffraction-spike strength

float hash12(vec2 p){
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673U, 3812015801U);
    uint n = (q.x ^ q.y) * 1597334673U;
    return float(n) * 2.328306437080797e-10;
}

float value2d(vec2 p){
    vec2 pg=floor(p), f=p-pg, k=vec2(0,1);
    f*=f*f*(3.-2.*f);
    return mix(mix(hash12(pg+k.xx),hash12(pg+k.yx),f.x),
               mix(hash12(pg+k.xy),hash12(pg+k.yy),f.x), f.y);
}

float waveMotionDeltaY(float ndcX, float t) {
    // Same time-varying terms as xmb-web's particle pass. The static/DC crest is
    // already carried by the firmware eye-space cloud, so only the moving wave
    // delta is added here. This keeps the particles riding the captured ribbon
    // instead of forming an independent star layer.
    float moving = 0.09 * cos(ndcX * 2.0 - t * 0.5)
                 + 0.25 * sin(ndcX * 0.306 + t * 0.075);
    return -(moving * 0.5) * 0.6;
}

vec3 safeNormalize(vec3 value, vec3 fallback) {
    float len2 = dot(value, value);
    return len2 > 0.000001 ? value * inversesqrt(len2) : fallback;
}

float ageFade(float age) {
    if(age < 0.12) {
        return age / 0.12;
    }
    if(age > 0.75) {
        return max((1.0 - age) / 0.25, 0.0);
    }
    return 1.0;
}

void main(){
    vec2 resolution = pc.resolution_time_brightness.xy;
    float time = pc.resolution_time_brightness.z;
    float brightness = pc.resolution_time_brightness.w;
    float nightBlend = clamp(pc.particle_params.x, 0.0, 1.0);
    vec2 seed = inParams.xy;

    // Firmware cloud homes are eye-space positions. Add only a tiny smooth
    // bounded drift around each home; xmb-web's fixed-step sim is deliberately
    // almost static, with the visible life coming from specular spin and aging.
    vec3 eye = inHomeAge.xyz;
    float w0 = max(-eye.z + 2.0, 0.001);
    vec2 domain = seed * 71.0;
    eye.x += (value2d(domain + vec2(time * 0.020, 13.0)) - 0.5) * 0.026 * w0;
    eye.y += (value2d(domain + vec2(47.0, time * 0.024)) - 0.5) * 0.018 * w0;
    eye.z += (value2d(domain + vec2(time * 0.017, 83.0)) - 0.5) * 0.022 * w0;

    const float projFx = 1.12820041;
    const float projFy = 2.00568986;
    float w = -eye.z + 2.0;
    vec2 center = vec2(4.0, 4.0);
    float visible = 0.0;
    if(w > 0.001) {
        center.x = (projFx * eye.x) / w;
        center.y = (projFy * eye.y) / w;
        center.y += waveMotionDeltaY(center.x, time);
        visible = (abs(center.x) <= 1.15 && abs(center.y) <= 1.15) ? 1.0 : 0.0;
    }

    const vec3 light = normalize(vec3(4.16, 2.63, -7.6));
    vec3 viewDir = safeNormalize(-eye, vec3(0.0, 0.0, 1.0));
    vec3 halfDir = safeNormalize(light + viewDir, vec3(0.0, 0.0, 1.0));

    float spin0 = inTintSpin.a + time * inSpinMisc.y * 0.0088883 * 0.7 * 60.0;
    float spin1 = inSpinMisc.x + time * inSpinMisc.z * 0.0088883 * 0.7 * 60.0;
    float c0 = sin(spin0);
    vec3 normal = vec3(c0 * cos(spin1), c0 * sin(spin1), cos(spin0));
    float ndh = max(dot(normal, halfDir), 0.0);
    float ndv = max(dot(normal, viewDir), 0.0);
    float spec = pow(ndh, 35.2904) * 74.74;
    float fres = pow(max(1.0 - ndv, 0.0), 1.33319);

    float ageRate = 0.00285223 * 0.9 * 60.0 *
                    (1.0 + (seed.y - 0.5) * 0.493003);
    float age = fract(inHomeAge.w + time * ageRate);
    float fade = ageFade(age);

    float depthDist = -eye.z;
    float dd = depthDist - 5.8;
    float defocus = dd >= 0.0 ? dd / 1.2 : -dd / 1.6;
    defocus = clamp(defocus, 0.0, 1.0);
    float dofDark = 1.0 / (1.0 + defocus * defocus * 2.4);

    bool edge = inParams.z > 0.5;
    float glintGain = edge ? 2.4 : 1.0;
    float bodyGain = edge ? 2.2 : 1.0;
    float bodyLevel = 0.05 + 0.11 * nightBlend;
    float glintExpo = 0.0390458 * (0.7 + 0.9 * nightBlend);
    float bright = (bodyLevel * bodyGain + fres * 0.04 + spec * glintExpo * glintGain)
                 * fade * dofDark * brightness * visible;

    float hs = resolution.y / 720.0;
    float sizePx = (0.0772033 * 90.0 / max(w, 0.001)) * hs * inParams.w;
    sizePx *= 1.0 + defocus * 1.5;
    sizePx = max(sizePx, 1.0);
    sizePx += min(bright, 2.0) * 0.159705 * 2.0;
    sizePx = min(sizePx, edge ? 22.0 : 12.0);
    vec2 halfSize = vec2(sizePx / resolution.x, sizePx / resolution.y);

    vec2 pos = center + inPos * halfSize * 2.0;
    gl_Position = vec4(pos, 0.0, 1.0);

    vLocal = inPos;
    vBright = max(bright, 0.0);

    if(edge) {
        float refl = 0.5 + min(1.0, spec * glintExpo * 0.6);
        vColor = inTintSpin.rgb * refl;
    } else {
        float iridescent = ndh * 6.2832;
        vColor = vec3(
            0.86 + 0.14 * (0.5 + 0.5 * cos(iridescent)),
            0.88 + 0.12 * (0.5 + 0.5 * cos(iridescent + 2.094)),
            0.92 + 0.08 * (0.5 + 0.5 * cos(iridescent + 4.188)));
    }
    float spark = min(1.0, (sizePx - 3.0) / 5.0) * 0.6 +
                  min(1.0, vBright * 0.5) * 0.7;
    if(edge) {
        spark += 0.4;
    }
    vSpark = clamp(spark, 0.0, 1.4);
}
