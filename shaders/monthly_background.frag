#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform MonthlyBackgroundParameters {
    vec4 top_and_night;
    vec4 bottom_and_reserved;
    vec4 boot;
} background;

vec3 srgb_to_linear(vec3 color)
{
    const bvec3 low = lessThanEqual(color, vec3(0.04045));
    const vec3 low_segment = color / 12.92;
    const vec3 high_segment = pow((color + 0.055) / 1.055, vec3(2.4));
    return mix(high_segment, low_segment, low);
}

vec3 xmb_web_composite(vec3 scene)
{
    // xmb-web FS_COMPOSITE steady day path: 1 - exp(-scene * EXPOSURE / WHITE).
    const float exposure = 1.05;
    const float white_level = 0.899181;
    return vec3(1.0) - exp(-max(scene, vec3(0.0)) * (exposure / white_level));
}

void main()
{
    const float screen_y = clamp(in_uv.y, 0.0, 1.0);
    const float bottom_mix = smoothstep(0.70, 1.0, screen_y);
    vec3 color = mix(background.top_and_night.rgb,
                     background.bottom_and_reserved.rgb,
                     bottom_mix);

    // Match xmb-web's active month-base Original branch: the palette carries the
    // hue and this shader supplies the firmware-like value/chroma structure.
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = max(vec3(luma) + (color - vec3(luma)) * 1.25, vec3(0.0));

    float dip = exp(-pow((screen_y - 0.68) / 0.14, 2.0));
    float value_envelope = 1.05 - 0.26 * dip;
    value_envelope *= 1.0 + 0.05 * smoothstep(0.30, 0.0, screen_y);
    value_envelope *= 1.0 + 0.06 * smoothstep(0.85, 1.0, screen_y);
    color *= value_envelope;

    luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = vec3(luma) + (color - vec3(luma)) * (1.0 + 0.22 * dip);

    float dx = in_uv.x - 0.52;
    color *= 1.0 - 0.30 * dx * dx * (dx < 0.0 ? 1.6 : 1.0);

    dx = in_uv.x - 0.5;
    const float bottom_edge = clamp(screen_y * abs(dx) * 2.0, 0.0, 1.0);
    const float bottom_side = mix(0.847, 0.925, smoothstep(-0.5, 0.5, dx));
    color *= mix(1.0, bottom_side, bottom_edge);

    const float top_corner = clamp(
        pow(1.0 - screen_y, 1.5) * (abs(dx) * 2.0), 0.0, 1.0);
    const float top_side = mix(0.88, 1.0, smoothstep(0.0, 0.5, dx));
    color *= 1.0 - 0.40 * top_corner * top_side;

    const float night = clamp(background.top_and_night.a, 0.0, 1.0);
    const float exponent = mix(1.15, 3.4, night);
    const float vertical = pow(screen_y, exponent);
    const float top_multiplier = mix(0.30, 0.02, night);
    color *= mix(top_multiplier, 1.0, vertical);

    color *= 1.05;
    const float input_luma = dot(color, vec3(0.299, 0.587, 0.114));
    const float output_luma = input_luma / (1.0 + input_luma * 0.30);
    if (input_luma > 0.0001) {
        color *= output_luma / input_luma;
    }

    if (background.boot.w > 0.5) {
        const float fade =
            clamp(mix(background.boot.x, background.boot.y, screen_y), 0.0, 1.0);
        const float sweep_band = 0.55;
        const float front = background.boot.z * (1.0 + sweep_band) - sweep_band;
        float sweep_mask = clamp((front - in_uv.x) / sweep_band + 1.0, 0.0, 1.0);
        sweep_mask = mix(0.35, 1.0, sweep_mask);
        color *= fade * sweep_mask;
    }

    // xmb-web's fitted constants target numeric 8-bit canvas values. The
    // Vulkan target is sRGB, so feed it linear light to avoid encoding twice.
    out_color = vec4(srgb_to_linear(clamp(xmb_web_composite(color), 0.0, 1.0)), 1.0);
}
