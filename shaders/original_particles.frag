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

// Original background particles (fragment)
#version 450

layout(location=0) in vec2 vLocal;  // unit quad [-0.5,0.5]
layout(location=1) in float vAlpha;  // per-sprite alpha

layout(location=0) out vec4 FragColor;

layout(push_constant) uniform PC {
    vec4 tint;
    vec2 resolution;
    float time;
    float brightness;
} pc;

void main(){
    // Soft circular falloff
    float r = length(vLocal*2.0);             // 0 at center, ~1 at edges
    float halo = smoothstep(1.0, 0.0, r);     // soft edge
    float core = smoothstep(0.34, 0.0, r);    // tiny bright center
    float a = halo*halo*0.70 + core*0.30;
    // Subtle tint; avoid stark white specks
    vec3 c = mix(pc.tint.rgb, vec3(1.0), 0.38);
    // xmb-web's particle pass uses additive ONE/ONE blending with
    // premultiplied colour. Keep the alpha term in RGB or every particle adds
    // a full-bright sprite regardless of its soft falloff.
    float alpha = a * vAlpha;
    FragColor = vec4(c * alpha, alpha);
}
