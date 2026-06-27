#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform MonthlyBackgroundParameters {
    vec4 top_and_night;
    vec4 bottom_and_reserved;
} background;

vec3 srgb_to_linear(vec3 color)
{
    const bvec3 low = lessThanEqual(color, vec3(0.04045));
    const vec3 low_segment = color / 12.92;
    const vec3 high_segment = pow((color + 0.055) / 1.055, vec3(2.4));
    return mix(high_segment, low_segment, low);
}

void main()
{
    const float screen_y = clamp(in_uv.y, 0.0, 1.0);
    const float bottom_mix = smoothstep(0.70, 1.0, screen_y);
    vec3 color = mix(background.top_and_night.rgb,
                     background.bottom_and_reserved.rgb,
                     bottom_mix);

    // Saturation pre-compensation from xmb-web's active LDR month-base path.
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = max(vec3(luma) + (color - vec3(luma)) * 1.25, vec3(0.0));

    // Measured bright-top / dark menu-band / bright-bottom structure.
    const float dip = exp(-pow((screen_y - 0.68) / 0.14, 2.0));
    float value_envelope = 1.05 - 0.26 * dip;
    value_envelope *= 1.0 + 0.05 * smoothstep(0.30, 0.0, screen_y);
    value_envelope *= 1.0 + 0.06 * smoothstep(0.85, 1.0, screen_y);
    color *= value_envelope;

    luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = vec3(luma) + (color - vec3(luma)) * (1.0 + 0.22 * dip);

    // The captured browser pipeline applies its LDR shoulder after the month
    // field has already rolled off toward the bottom edge.  Without that
    // roll-off the native June field became much too bright and green below
    // the crossbar even though the upper half already matched.  Keep the top
    // untouched, then reproduce the measured bottom value/saturation fit.
    const float bottom_rolloff = smoothstep(0.50, 1.0, screen_y);
    luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = vec3(luma) +
        (color - vec3(luma)) * mix(1.0, 0.894, bottom_rolloff);
    color *= mix(1.0, 0.72, bottom_rolloff);
    color.b *= mix(1.0, 1.05, bottom_rolloff);

    // Asymmetric horizontal and corner vignette measured by xmb-web.
    float dx = in_uv.x - 0.52;
    color *= 1.0 - 0.30 * dx * dx * (dx < 0.0 ? 1.6 : 1.0);

    dx = in_uv.x - 0.5;
    const float bottom_edge = clamp(screen_y * (abs(dx) * 2.0), 0.0, 1.0);
    const float bottom_side = mix(0.847, 0.925, smoothstep(-0.5, 0.5, dx));
    color *= mix(1.0, bottom_side, bottom_edge);

    const float top_corner = clamp(
        pow(1.0 - screen_y, 1.5) * (abs(dx) * 2.0), 0.0, 1.0);
    const float top_side = mix(0.88, 1.0, smoothstep(0.0, 0.5, dx));
    color *= 1.0 - 0.40 * top_corner * top_side;

    // Day/night changes how far the dark top extends; the bottom remains the
    // same measured month colour at every time of day.
    const float night = clamp(background.top_and_night.a, 0.0, 1.0);
    const float exponent = mix(1.15, 3.4, night);
    const float vertical_reveal = pow(screen_y, exponent);
    const float top_multiplier = mix(0.30, 0.02, night);
    color *= mix(top_multiplier, 1.0, vertical_reveal);

    // Global luminance-only shoulder: retain hue/chroma while containing value.
    color *= 1.05;
    const float input_luma = dot(color, vec3(0.299, 0.587, 0.114));
    const float output_luma = input_luma / (1.0 + input_luma * 0.30);
    if (input_luma > 0.0001) {
        color *= output_luma / input_luma;
    }

    // xmb-web's fitted constants target numeric 8-bit canvas values. The
    // Vulkan target is sRGB, so feed it linear light to avoid encoding twice.
    out_color = vec4(srgb_to_linear(clamp(color, 0.0, 1.0)), 1.0);
}
