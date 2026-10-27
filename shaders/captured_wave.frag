#version 450

layout(location = 0) in vec3 in_ndc;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform CapturedWaveParameters {
    vec4 tint_and_gain;       // rgb tint, wave gain/fade
    vec4 transform_alpha;     // y flip, scale y, scale x, fragment alpha
    vec4 material;            // silk, specular weight, exponent, y-fade low
    vec4 fade_offset;         // y-fade high, clip offset x/y, reserved
} wave;

void main()
{
    vec3 normal = normalize(in_normal);

    float fresnel = 0.5 * pow(
        max(1.0 + dot(vec3(0.0, 0.0, -1.0), normal), 0.0),
        3.0);
    vec3 view_direction = normalize(vec3(3.12367, -0.0166247, -0.0852542));
    vec3 key_light = normalize(vec3(0.484729, 0.382283, -0.97268));
    vec3 half_vector = normalize(key_light + view_direction);
    float specular = pow(
        max(dot(normal, half_vector), 0.0),
        wave.material.z);

    float silk = clamp(
        fresnel * 0.667 * 1.7 + specular * wave.material.y,
        0.0,
        0.95);
    float lit = mix(1.0, 0.107345 + silk, wave.material.x);
    float edge = smoothstep(0.0, 0.06, in_uv.x)
        * smoothstep(0.0, 0.06, 1.0 - in_uv.x);
    float vertical_fade = 1.0 - smoothstep(
        wave.material.w,
        wave.fade_offset.x,
        in_ndc.y);
    float alpha = clamp(
        wave.transform_alpha.w * edge * lit * vertical_fade,
        0.0,
        1.0);

    out_color = vec4(wave.tint_and_gain.rgb * wave.tint_and_gain.a, alpha);
}
