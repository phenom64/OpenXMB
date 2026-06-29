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
layout(location=2) in float vGlint;  // rare soft specular lift

layout(location=0) out vec4 FragColor;

layout(push_constant) uniform PC {
    vec4 tint;
    vec2 resolution;
    float time;
    float brightness;
} pc;

void main(){
    // Soft circular falloff: no pinprick star core. xmb-web's particles read
    // as small bokeh/glitter carried by the wave, so the centre lift stays broad
    // and low rather than becoming a hard white pixel.
    float r = length(vLocal*2.0);             // 0 at center, ~1 at edges
    float halo = exp(-r * r * 2.35);
    float centre = exp(-r * r * 8.0);
    float a = smoothstep(1.0, 0.0, r) * (halo * 0.82 + centre * 0.18);
    vec3 c = mix(pc.tint.rgb, vec3(1.0), 0.48 + 0.20 * clamp(vGlint, 0.0, 1.0));
    c += vec3(0.10, 0.08, 0.035) * clamp(vGlint, 0.0, 1.0);
    // xmb-web's particle pass uses additive ONE/ONE blending with
    // premultiplied colour. Keep the alpha term in RGB or every particle adds
    // a full-bright sprite regardless of its soft falloff.
    float alpha = a * vAlpha;
    FragColor = vec4(c * alpha, alpha);
}
