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

layout(location=0) in vec2 vLocal;   // unit quad [-0.5,0.5]
layout(location=1) in float vBright; // HDR particle brightness
layout(location=2) in vec3 vColor;   // iridescent/metallic particle colour
layout(location=3) in float vSpark;  // diffraction-spike sparkle amount

layout(location=0) out vec4 FragColor;

layout(push_constant) uniform PC {
    vec4 tint;
    vec4 resolution_time_brightness;
    vec4 particle_params;
} pc;

void main(){
    vec2 coord = vLocal * 2.0;
    float d2 = dot(coord, coord);
    if(d2 > 1.0) {
        discard;
    }

    // xmb-web's sprite is round: a sharp specular core, a soft bokeh glow, and
    // a sparse four-point diffraction cross for larger/brighter glints.
    float core = exp(-d2 * 9.0);
    float glow = exp(-d2 * 1.8) * 0.40;
    float cross = (exp(-coord.y * coord.y * 70.0) +
                   exp(-coord.x * coord.x * 70.0)) * exp(-d2 * 1.2);
    float spark = cross * vSpark * 0.7;
    float alpha = (core + glow + spark) * vBright;

    float hot = clamp(vBright * 0.6 - 0.6, 0.0, 1.0);
    vec3 color = mix(vColor, vec3(1.0), hot);
    FragColor = vec4(color * alpha, alpha);
}
