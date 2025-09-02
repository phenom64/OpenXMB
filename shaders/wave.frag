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
    FragColor = vec4(c, c, c, 1.0) * constants.color;
}
