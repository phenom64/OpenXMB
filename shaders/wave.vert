/*
 * RetroArch - A frontend for libretro.
 * Copyright (C) 2010-2014 Hans-Kristian Arntzen
 * Copyright (C) 2011-2021 Daniel De Matteis
 * (and other contributors, per upstream headers)
 *
 * Adapted for OpenXMB from:
 *   gfx/drivers/vulkan_shaders/pipeline_ribbon.vert/.frag
 *
 * Licensed under GPLv3; see LICENSE and LICENSE.RetroArch.
 */

#version 450

layout(push_constant) uniform UBO
{
    vec4 color;
    float time;
} constants;
layout(location = 0) in vec3 VertexCoord;
layout(location = 0) out vec3 vEC;

// Taken from https://github.com/libretro/RetroArch/blob/master/gfx/drivers/vulkan_shaders/pipeline_ribbon.vert
float xmb_noise2(vec3 x)
{
    float t = constants.time * 0.82;
    return cos(x.z * 3.4) * cos((x.z + (t / 12.0)) + x.x * 0.92);
}

float iqhash(float n)
{
    return fract(sin(n) * 43758.546875);
}

float _noise(vec3 x)
{
    vec3 p = floor(x);
    vec3 f = fract(x);
    f = (f * f) * (vec3(3.0) - (f * 2.0));
    float n = (p.x + (p.y * 57.0)) + (113.0 * p.z);
    float param = n;
    float param_1 = n + 1.0;
    float param_2 = n + 57.0;
    float param_3 = n + 58.0;
    float param_4 = n + 113.0;
    float param_5 = n + 114.0;
    float param_6 = n + 170.0;
    float param_7 = n + 171.0;
    return mix(mix(mix(iqhash(param), iqhash(param_1), f.x), mix(iqhash(param_2), iqhash(param_3), f.x), f.y), mix(mix(iqhash(param_4), iqhash(param_5), f.x), mix(iqhash(param_6), iqhash(param_7), f.x), f.y), f.z);
}

void main()
{
    float t = constants.time * 0.82;
    vec3 v = vec3(VertexCoord.x * 1.08, 0.0, VertexCoord.y * 0.92);
//	vec3 v = vec3(0, 0, 0);
    vec3 v2 = v;
    vec3 v3 = v;
    vec3 param = v2;
    v.y = xmb_noise2(param) / 10.5;
    v3.x -= (t / 6.4);
    v3.x /= 5.2;
    v3.z -= (t / 12.0);
    v3.y -= (t / 110.0);
    vec3 param_1 = v3 * 6.2;
    v.z -= (_noise(param_1) / 18.0);
    vec3 param_2 = v3 * 6.2;
    v.y -= (((_noise(param_2) / 20.0) + (cos((v.x * 1.65) - (t / 2.8)) / 8.5)) - 0.245);
    vEC = v;
    gl_Position = vec4(v, 1.0);
}
