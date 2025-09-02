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

// Dave Hoskins hash and value noise
float hash12(vec2 p)
{
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673U, 3812015801U);
    uint n = (q.x ^ q.y) * 1597334673U; return float(n) * 2.328306437080797e-10;
}
float value2d(vec2 p)
{
    vec2 pg=floor(p),pc=p-pg,k=vec2(0,1);
    pc*=pc*pc*(3.-2.*pc);
    return mix(
        mix(hash12(pg+k.xx),hash12(pg+k.yx),pc.x),
        mix(hash12(pg+k.xy),hash12(pg+k.yy),pc.x),
        pc.y
    );
}

float get_stars_rough(vec2 p)
{
    const float THRESHOLD = .99;
    float s = smoothstep(THRESHOLD,1.,hash12(p));
    if (s >= THRESHOLD)
        s = pow((s-THRESHOLD)/(1.-THRESHOLD), 10.);
    return s;
}
float get_stars(vec2 p, float a, float t)
{
    vec2 pg=floor(p), pc=p-pg, k=vec2(0,1);
    pc *= pc*pc*(3.-2.*pc);
    float s = mix(
        mix(get_stars_rough(pg+k.xx), get_stars_rough(pg+k.yx), pc.x),
        mix(get_stars_rough(pg+k.xy), get_stars_rough(pg+k.yy), pc.x), pc.y);
    return smoothstep(a,a+t, s)*pow(value2d(p*.1 + t)*.5+.5,8.3);
}

// Signed distance to the wavy plane — ported from original
float sdf(vec3 p, float t)
{ 
    p*=2.0;
    float o = 4.2*sin(.05*p.x+t*.25)
            + (.04*p.z)
            + sin(p.x*.11+t)
            + 2.0*sin(p.z*.2+t)
            + value2d(vec2(.03,.4)*p.xz+vec2(t*.5,0));
    return abs(dot(p,normalize(vec3(0,1,0.05)))+2.5+o*.5); 
}

vec2 raymarch(vec3 o, vec3 d, float t)
{
    const float MIN_DIST=.13; const float MAX_DIST=40.0; const int MAX_STEPS=80; 
    float dist=0.0, prev=0.0, a=0.0, g=MAX_DIST; bool hit=false; float omega=1.2; float sl=0.0;
    for (int i=0;i<MAX_STEPS;i++)
    {
        vec3 p=o+d*dist;
        float ndt=sdf(p,t);
        if (abs(prev)+abs(ndt) < sl){ sl -= omega*sl; omega=1.0; }
        else sl = ndt*omega;
        prev=ndt; dist+=sl;
        g = (dist > 10.0) ? min(g,abs(prev)) : g;
        if ((dist+=prev)>=MAX_DIST) break;
        if (prev<MIN_DIST){ if(!hit){ a=.01; hit=true; }
            float ed=2.0*max(.03,abs(ndt)); a += .0135; dist += ed; }
    }
    g /= 3.0; return vec2(a, max(1.0-g,0.0));
}

void main(){
    vec2 ires = pc.resolution; vec2 U = vUV * ires; vec2 uv=U/ires; float t=pc.time;
    vec3 o=vec3(0), d=vec3((U-0.5*ires)/ires.y, 1);
    vec2 mg = raymarch(o,d,t); float m = mg.x;

    // Reddish 4‑point gradient, tinted by user colour
    vec3 tint = pc.tint.rgb; // Pure tint; no hue bias
    vec3 g0 = mix(vec3(.7,.2,.2), vec3(.4,.1,.1), uv.x);
    vec3 g1 = mix(vec3(.45,.1,.1),vec3(.8,.3,.5), uv.x);
    vec3 c = mix(g0, g1, uv.y) * tint;
    c = mix(c, vec3(1.0), m); // blend to white by ribbon alpha

    // Dust sampled at three depths
    vec2 ar = vec2(ires.x/ires.y,1.0);
    vec2 pp = uv*2000.0*ar;
    float dust = 0.0;
    dust += get_stars(.1*pp+t*vec2(20.0,-10.1), .11, .71)*4.0;
    dust += get_stars(.2*pp+t*vec2(30.0,-10.1), .10, .31)*5.0;
    dust += get_stars(.32*pp+t*vec2(40.0,-10.1), .10, .91)*2.0;
    c += dust*0.075;

    c *= pc.brightness;
    FragColor = vec4(c,1.0);
}
