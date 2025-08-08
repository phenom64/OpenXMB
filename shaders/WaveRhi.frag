// shaders/WaveShader.frag
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.
// Adapted from Shadertoy PS3 XMB wave by int_45h.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec2 vPixel;
layout(location = 0) out vec4 outColor;

// Must mirror your WaveItem.cpp::UniformBlock (std140)
layout(std140, binding = 0) uniform UniformBlock {
    float    time;
    float    speed;
    float    amplitude;
    float    frequency;
    vec4     baseColor;
    vec4     waveColor;
    float    threshold;
    float    dustIntensity;
    float    minDist;
    float    maxDist;
    int      maxDraws;
    vec2     resolution; // pixels
    float    brightness;
    float    pad;
} U;

// dissolve.png (256x256), bound at binding=1
layout(binding = 1) uniform sampler2D iChannel0;

// ---- constants to avoid textureSize()/texelFetch() ----
const vec2 NOISE_TEX_SIZE = vec2(256.0, 256.0);
const float TAU = 6.28318530718;

// ---- helpers ----
float s5(float x) { return 0.5 + 0.5 * sin(x); }
float c5(float x) { return 0.5 + 0.5 * cos(x); }

// Hash using nearest-neighbor sample at an integer texel index.
// Sample at texel centers: (i + 0.5)/size  (ESSL-100 friendly)
float hash12(vec2 p)
{
    vec2 ip = floor(p);
    vec2 pi = mod(ip, NOISE_TEX_SIZE);
    vec2 uv = (pi + 0.5) / NOISE_TEX_SIZE;
    return texture(iChannel0, uv).r;
}

// Value noise using normalized coords
float value2d(vec2 p)
{
    return texture(iChannel0, p / NOISE_TEX_SIZE).r;
}

// ---- starfield ----
float get_stars_rough(vec2 p)
{
    float s = smoothstep(U.threshold, 1.0, hash12(p));
    if (s >= U.threshold)
        s = pow((s - U.threshold) / (1.0 - U.threshold), 10.0);
    return s;
}

float get_stars(vec2 p, float a, float t)
{
    vec2 pg = floor(p), pc = p - pg, k = vec2(0.0, 1.0);
    pc *= pc * pc * (3.0 - 2.0 * pc);

    float s = mix(
        mix(get_stars_rough(pg + k.xx), get_stars_rough(pg + k.yx), pc.x),
        mix(get_stars_rough(pg + k.xy), get_stars_rough(pg + k.yy), pc.x),
        pc.y
    );

    float T = U.time * U.speed;
    return smoothstep(a, a + t, s) * pow(value2d(p * 0.1 + T) * 0.5 + 0.5, 8.3);
}

float get_dust(vec2 p, vec2 size, float f)
{
    vec2 ar = vec2(U.resolution.x / U.resolution.y, 1.0);
    vec2 pp = p * size * ar;
    float T = U.time * U.speed;

    float dust =
        pow(0.64 + 0.46 * cos(p.x * TAU), 1.7) *
        f * (
            get_stars(0.1 * pp + T * vec2(20.0, -10.1), 0.11, 0.71) * 4.0 +
            get_stars(0.2 * pp + T * vec2(30.0, -10.1), 0.10, 0.31) * 5.0 +
            get_stars(0.32 * pp + T * vec2(40.0, -10.1), 0.10, 0.91) * 2.0
        );
    return dust * U.dustIntensity;
}

// ---- SDF & normal ----
float sdf(vec3 p)
{
    float A = U.amplitude;
    float F = U.frequency;
    float T = U.time * U.speed;

    p *= 2.0;
    float o =
        4.2 * A * sin(0.05 * F * p.x + T * 0.25) +
        (0.04 * p.z) *
        sin(F * p.x * 0.11 + T) *
        2.0 * sin(F * p.z * 0.2 + T) *
        value2d(vec2(0.03, 0.4) * p.xz + vec2(T * 0.5, 0.0));

    return abs(dot(p, normalize(vec3(0.0, 1.0, 0.05))) + 2.5 + o * 0.5);
}

vec3 norm(vec3 p)
{
    const vec2 k = vec2(1.0, -1.0);
    const float t = 0.001;
    return normalize(
        k.xyy * sdf(p + t * k.xyy) +
        k.yyx * sdf(p + t * k.yyx) +
        k.yxy * sdf(p + t * k.yxy) +
        k.xxx * sdf(p + t * k.xxx)
    );
}

// ---- raymarch (returns alpha, glow) ----
vec2 raymarch(vec3 o, vec3 d, float omega)
{
    float t  = 0.0;
    float a  = 0.0;
    float g  = U.maxDist;
    float dt = 0.0;
    float sl = 0.0;
    float emin = 0.03, ed = emin;
    int   dr = 0;
    bool  hit = false;

    for (int i = 0; i < 100; ++i)
    {
        vec3 p = o + d * t;
        float ndt = sdf(p);

        if (abs(dt) + abs(ndt) < sl) { sl -= omega * sl; omega = 1.0; }
        else                          { sl = ndt * omega; }

        dt = ndt;
        t += sl;

        g = (t > 10.0) ? min(g, abs(dt)) : U.maxDist;

        if ((t += dt) >= U.maxDist) break;

        if (dt < U.minDist)
        {
            if (dr > U.maxDraws) break;
            dr++;

            float f = smoothstep(0.09, 0.11, (p.z * 0.9) / 100.0);
            if (!hit) { a = 0.01; hit = true; }

            ed = 2.0 * max(emin, abs(ndt));
            a += 0.0135 * f;
            t += ed;
        }
    }

    g /= 3.0;
    return vec2(a, max(1.0 - g, 0.0));
}

void main()
{
    vec2 ires = U.resolution;
    vec2 Upx  = vPixel;
    vec2 uv   = Upx / ires;

    vec3 o = vec3(0.0);
    vec3 d = vec3((Upx - 0.5 * ires) / ires.y, 1.0);

    vec2 mg = raymarch(o, d, 1.2);
    float m = mg.x;

    // base tint from baseColor, mild vertical grading
    vec3 base = mix(U.baseColor.rgb * 0.6, U.baseColor.rgb, uv.y);

    // highlight towards waveColor
    vec3 c = mix(base, U.waveColor.rgb, m);

    // star dust
    c += get_dust(uv, vec2(2000.0), mg.y) * 0.3;

    // global brightness
    c *= U.brightness;

    outColor = vec4(c, 1.0);
}
