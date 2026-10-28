#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform MonthlyBackgroundParameters {
    vec4 top_and_night;
    vec4 bottom_and_reserved;
    vec4 boot;
    vec4 modes;
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

void write_background(vec3 rgb) {
    const vec2 music_direction=vec2(-0.06975647,0.99756405);
    float u=clamp((dot(in_uv,music_direction)-music_direction.x)/
                  ((music_direction.y-music_direction.x)*0.62),0.0,1.0);
    float gradient=1.0-pow(1.0-u,2.0);
    vec3 music=mix(vec3(106.0,45.0,118.0)/255.0,vec3(2.0,0.0,4.0)/255.0,gradient);
    out_color=vec4(mix(rgb,music,background.modes.x),1.0);
}
void main()
{
    const float screen_y = clamp(in_uv.y, 0.0, 1.0);
    if(background.bottom_and_reserved.a>0.0) {
        const vec2 manual_direction=vec2(-0.20791169,0.9781476);
        float u=(dot(in_uv,manual_direction)-manual_direction.x)/
                (manual_direction.y-manual_direction.x);
        float t=smoothstep(0.0,background.bottom_and_reserved.a,u);
        vec3 rgb=mix(background.top_and_night.rgb,background.bottom_and_reserved.rgb,t);
        write_background(rgb);return;
    }
    const float bottom_mix = smoothstep(0.70, 1.0, screen_y);
    vec3 color = mix(background.top_and_night.rgb,
                     background.bottom_and_reserved.rgb,
                     bottom_mix);

    // Match xmb-web's active month-base Original branch: the palette carries the
    // hue and this shader supplies the firmware-like value/chroma structure.
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = max(vec3(luma) + (color - vec3(luma)) * 1.25, vec3(0.0));

    // Value and chroma field fitted to the firmware's Original background
    // (RPCS3 captures, October, recorded 05:14-05:35): brightest top-left,
    // darkest low on the left, a warm lift towards the bottom right, and
    // slightly more chroma lower down. Only that time of day is measured.
    const float u = clamp(in_uv.x, 0.0, 1.0), v = screen_y;
    const float field =
        0.7780 + 0.6431 * v - 2.5036 * v * v + 1.5062 * v * v * v
        - 1.0887 * u + 0.5040 * u * v + 1.2681 * u * v * v
        + 1.0982 * u * u - 1.0241 * u * u * v - 0.2568 * u * u * u;
    luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = vec3(luma) + (color - vec3(luma)) * (1.007 + 0.139 * v);
    color = max(color, vec3(0.0)) * max(field, 0.0);

    // Night is not in the reference; keep the earlier darkening model.
    const float night = clamp(background.top_and_night.a, 0.0, 1.0);
    color *= mix(1.0, mix(0.02, 1.0, pow(screen_y, 3.4)), night);

    if (background.boot.w > 0.5) {
        const float fade =
            clamp(mix(background.boot.x, background.boot.y, screen_y), 0.0, 1.0);
        const float sweep_band = 0.55;
        const float front = background.boot.z * (1.0 + sweep_band) - sweep_band;
        float sweep_mask = clamp((front - in_uv.x) / sweep_band + 1.0, 0.0, 1.0);
        sweep_mask = mix(0.35, 1.0, sweep_mask);
        color *= fade * sweep_mask;
    }

    // The composite's result is written as display (8-bit canvas) values.
    write_background(clamp(xmb_web_composite(color),0.0,1.0));
}
