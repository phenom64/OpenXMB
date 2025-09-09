/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025 Syndromatic Ltd. All rights reserved
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

#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(push_constant) uniform PC {
    vec4 tint;        // base tint colour
    vec2 resolution;  // screen resolution
    float time;       // seconds
    float brightness; // 0..1
} pc;

// Dave Hoskins hash and value noise (needed by SDF)
float hash12(vec2 p){
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673U, 3812015801U);
    uint n = (q.x ^ q.y) * 1597334673U;
    return float(n) * 2.328306437080797e-10;
}
float value2d(vec2 p){
    vec2 pg=floor(p),pc=p-pg,k=vec2(0,1);
    pc*=pc*pc*(3.-2.*pc);
    return mix(
        mix(hash12(pg+k.xx),hash12(pg+k.yx),pc.x),
        mix(hash12(pg+k.xy),hash12(pg+k.yy),pc.x),
        pc.y);
}

// Signed distance to the translucent wave (from Shadertoy reference)
float sdf(vec3 p, float t){
    p *= 2.0;
    float o = 4.2*sin(0.05*p.x + t*0.25)
            + 0.04*p.z
            + sin(p.x*0.11 + t)
            + 2.0*sin(p.z*0.20 + t)
            + value2d(vec2(0.03,0.4)*p.xz + vec2(t*0.5,0.0));
    return abs(dot(p, normalize(vec3(0.0,1.0,0.05))) + 2.5 + o*0.5);
}

// Enhanced sphere tracing; returns (alpha, glow)
vec2 raymarch(vec3 o, vec3 d, float t){
    const float MIN_DIST = 0.13;
    const float MAX_DIST = 40.0;
    const int   MAX_STEPS= 100;
    const int   MAX_DRAWS= 40;

    float dist=0.0, prev=0.0, a=0.0;
    float g = MAX_DIST; // glow accumulator
    float sl=0.0;       // step length
    float omega = 1.2;  // step relaxation
    float emin = 0.03;  // epsilon min
    float ed = emin;    // dynamic epsilon
    int   draws = 0;
    bool  hit = false;

    for(int i=0;i<MAX_STEPS;i++){
        vec3 p = o + d*dist;
        float ndt = sdf(p, t);
        if(abs(prev)+abs(ndt) < sl){ sl -= omega*sl; omega = 1.0; }
        else                        sl = ndt*omega;
        prev = ndt; dist += sl;

        if(dist > 10.0) g = min(g, abs(prev));
        if((dist += prev) >= MAX_DIST) break;
        if(prev < MIN_DIST){
            if(draws++ > MAX_DRAWS) break;
            float f = smoothstep(0.09, 0.11, (p.z*0.9)/100.0);
            if(!hit){ a = 0.01; hit = true; }
            ed = 2.0*max(emin, abs(ndt));
            a += 0.0135 * f;
            dist += ed; // prevent re-sampling same spot
        }
    }
    g /= 3.0;
    return vec2(a, max(1.0 - g, 0.0));
}

void main(){
    vec2 ires = pc.resolution; vec2 U = vUV * ires; vec2 uv = U/ires; float t = pc.time;
    vec3 o = vec3(0.0);
    vec3 d = vec3((U - 0.5*ires)/ires.y, 1.0);
    // Layer the ribbon with slight spatial/temporal offsets to emulate multiple bands
    vec2 rm0 = raymarch(o, d, t);
    vec2 rm1 = raymarch(o + vec3(0.0, 0.08, 0.0), d, t*0.97 + 0.15);
    vec2 rm2 = raymarch(o + vec3(0.0, -0.06, 0.0), d, t*1.05 - 0.12);
    float m = clamp(rm0.x*1.0 + rm1.x*0.65 + rm2.x*0.50, 0.0, 1.0);
    float glow = clamp(max(max(rm0.y, rm1.y*0.8), rm2.y*0.7), 0.0, 1.0);

    // Theme-driven gradient: derive two tones from tint and blend by position
    vec3 tint = pc.tint.rgb;
    vec3 baseDark  = clamp(tint * vec3(0.35, 0.35, 0.35), 0.0, 1.0);
    vec3 baseMid   = clamp(tint * vec3(0.55, 0.55, 0.55), 0.0, 1.0);
    vec3 baseLight = clamp(tint * vec3(0.85, 0.85, 0.85), 0.0, 1.0);
    vec3 g0 = mix(baseDark, baseMid,   smoothstep(0.0, 1.0, uv.x));
    vec3 g1 = mix(baseMid,  baseLight, smoothstep(0.0, 1.0, uv.x));
    vec3 c = mix(g0, g1, smoothstep(0.0, 1.0, uv.y));
    // Blend to white by ribbon alpha, and boost a bit with glow
    float w = clamp(m + glow * 0.85, 0.0, 1.0);
    c = mix(c, vec3(1.0), w);

    // No dust here — the particle renderer draws drifting dust in a separate pipeline.
    c *= pc.brightness;
    FragColor = vec4(c, 1.0);
}
