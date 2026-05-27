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
layout(location = 0) in vec3 vEC;
layout(location = 0) out vec4 FragColor;

// Taken from https://github.com/libretro/RetroArch/blob/master/gfx/drivers/vulkan_shaders/pipeline_ribbon.frag
void main()
{
    vec3 x = dFdx(vEC);
    vec3 y = dFdy(vEC);
    vec3 normal = normalize(cross(x, y));
    float c = 1.0 - dot(normal, vec3(0.0, 0.0, 1.0));
    c = (1.0 - cos(c * c)) / 3.0;
    c = smoothstep(0.015, 0.36, c);

    float depthFade = smoothstep(-0.92, -0.52, vEC.z) * (1.0 - smoothstep(0.56, 0.94, vEC.z));
    float horizonFade = smoothstep(-0.08, 0.34, vEC.y) * (1.0 - smoothstep(0.58, 0.84, vEC.y));
    float intensity = c * mix(0.52, 1.0, depthFade) * mix(0.72, 1.0, horizonFade);

    vec3 tint = mix(constants.color.rgb, vec3(1.0), 0.58);
    vec3 hot = mix(tint, vec3(1.0), smoothstep(0.55, 1.0, intensity));
    FragColor = vec4(hot * intensity * 0.74, intensity * 0.55);
}
