#version 450

layout(location = 0) in vec4 in_clip;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec3 out_ndc;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec2 out_uv;

layout(push_constant) uniform CapturedWaveParameters {
    vec4 tint_and_gain;       // rgb tint, wave gain/fade
    vec4 transform_alpha;     // y flip, scale y, scale x, fragment alpha
    vec4 material;            // silk, specular weight, exponent, y-fade low
    vec4 fade_offset;         // y-fade high, clip offset x/y, reserved
} wave;

void main()
{
    vec4 position = in_clip;
    position.y *= wave.transform_alpha.x * wave.transform_alpha.y;
    position.x *= wave.transform_alpha.z;
    position.xy += wave.fade_offset.yz * position.w;

    gl_Position = position;
    out_ndc = position.xyz / position.w;
    out_normal = in_normal;
    out_uv = in_uv;
}
