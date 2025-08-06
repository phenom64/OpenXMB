// shaders/WaveShader.frag
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.
// Adapted from Shadertoy PS3 XMB wave by int_45h.

// Configurable defines.
#define THRESHOLD uniform_threshold
#define DUST
#define MIN_DIST uniform_minDist
#define MAX_DIST uniform_maxDist
#define MAX_DRAWS uniform_maxDraws

#version 330 core

precision mediump float;

varying highp vec2 coord;
uniform float uniform_time;
uniform float uniform_speed;
uniform float uniform_amplitude;
uniform float uniform_frequency;
uniform vec4 uniform_baseColor;
uniform vec4 uniform_waveColor;
uniform float uniform_threshold;
uniform float uniform_dustIntensity;
uniform float uniform_minDist;
uniform float uniform_maxDist;
uniform int uniform_maxDraws;
uniform vec2 uniform_resolution;

// Dave Hoskins hash for noise.
float hash12(vec2 p) {
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673U, 3812015801U);
    uint n = (q.x ^ q.y) * 1597334673U;
    return float(n) * 2.328306437080797e-10;
}

float value2d(vec2 p) {
    vec2 pg = floor(p), pc = p - pg, k = vec2(0,1);
    pc *= pc * pc * (3. - 2. * pc);
    return mix(
        mix(hash12(pg + k.xx), hash12(pg + k.yx), pc.x),
        mix(hash12(pg + k.xy), hash12(pg + k.yy), pc.x),
        pc.y
    );
}

float s5(float x) { return 0.5 + 0.5 * sin(x); }
float c5(float x) { return 0.5 + 0.5 * cos(x); }

float get_stars_rough(vec2 p) {
    float s = smoothstep(THRESHOLD, 1., hash12(p));
    if (s >= THRESHOLD) s = pow((s - THRESHOLD) / (1. - THRESHOLD), 10.);
    return s;
}

float get_stars(vec2 p, float a, float t) {
    vec2 pg = floor(p), pc = p - pg, k = vec2(0,1);
    pc *= pc * pc * (3. - 2. * pc);
    float s = mix(
        mix(get_stars_rough(pg + k.xx), get_stars_rough(pg + k.yx), pc.x),
        mix(get_stars_rough(pg + k.xy), get_stars_rough(pg + k.yy), pc.x),
        pc.y
    );
    return smoothstep(a, a + t, s) * pow(value2d(p * 0.1 + uniform_time) * 0.5 + 0.5, 8.3);
}

float get_dust(vec2 p, vec2 size, float f) {
    vec2 ar = vec2(uniform_resolution.x / uniform_resolution.y, 1);
    vec2 pp = p * size * ar;
    return pow(0.64 + 0.46 * cos(p.x * 6.28), 1.7) * f * (
        get_stars(0.1 * pp + uniform_time * vec2(20., -10.1), 0.11, 0.71) * 4. +
        get_stars(0.2 * pp + uniform_time * vec2(30., -10.1), 0.1, 0.31) * 5. +
        get_stars(0.32 * pp + uniform_time * vec2(40., -10.1), 0.1, 0.91) * 2.
    );
}

float sdf(vec3 p) {
    p *= 2.;
    float o = 4.2 * sin(0.05 * p.x + uniform_time * 0.25 * uniform_speed) +
              (0.04 * p.z) * sin(p.x * 0.11 + uniform_time * uniform_speed) *
              2. * sin(p.z * 0.2 + uniform_time * uniform_speed) *
              value2d(vec2(0.03, 0.4) * p.xz + vec2(uniform_time * 0.5 * uniform_speed, 0));
    return abs(dot(p, normalize(vec3(0,1,0.05))) + 2.5 + o * 0.5 * uniform_amplitude);
}

vec3 norm(vec3 p) {
    const vec2 k = vec2(1, -1);
    const float t = 0.001;
    return normalize(
        k.xyy * sdf(p + t * k.xyy) +
        k.yyx * sdf(p + t * k.yyx) +
        k.yxy * sdf(p + t * k.yxy) +
        k.xxx * sdf(p + t * k.xxx)
    );
}

vec2 raymarch(vec3 o, vec3 d, float omega) {
    float t = 0., a = 0.;
    float g = MAX_DIST, dt = 0., sl = 0., emin = 0.03, ed = emin;
    int dr = 0;
    bool hit = false;
    for (int i = 0; i < 100; i++) {
        vec3 p = o + d * t;
        float ndt = sdf(p);
        if (abs(dt) + abs(ndt) < sl) {
            sl -= omega * sl;
            omega = 1.;
        } else {
            sl = ndt * omega;
        }
        dt = ndt;
        t += sl;
        g = (t > 10.) ? min(g, abs(dt)) : MAX_DIST;
        if ((t += dt) >= MAX_DIST) break;
        if (dt < MIN_DIST) {
            if (dr > MAX_DRAWS) break;
            dr++;
            float f = smoothstep(0.09, 0.11, (p.z * 0.9) / 100.);
            if (!hit) {
                a = 0.01;
                hit = true;
            }
            ed = 2. * max(emin, abs(ndt));
            a += 0.0135 * f * uniform_frequency;
            t += ed;
        }
    }
    g /= 3.;
    return vec2(a, max(1. - g, 0.));
}

void main() {
    vec2 uv = coord;
    vec3 o = vec3(0);
    vec3 d = vec3((coord - 0.5) * vec2(uniform_resolution.x / uniform_resolution.y, 1), 1);
    vec2 mg = raymarch(o, d, 1.2);
    float m = mg.x;
    vec3 c = mix(
        mix(uniform_baseColor.rgb, uniform_waveColor.rgb, uv.x),
        mix(uniform_waveColor.rgb, uniform_baseColor.rgb, uv.x),
        uv.y
    );
    c = mix(c, vec3(1.), m);
#ifdef DUST
    c += get_dust(uv, vec2(2000.), mg.y) * 0.3 * uniform_dustIntensity;
#endif
    gl_FragColor = vec4(c, 1.0);
}