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
    return cos(x.z * 4.0) * cos((x.z + (constants.time / 10.0)) + x.x);
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
    vec3 v = vec3(VertexCoord.x, 0.0, VertexCoord.y);
//	vec3 v = vec3(0, 0, 0);
    vec3 v2 = v;
    vec3 v3 = v;
    vec3 param = v2;
    v.y = xmb_noise2(param) / 8.0;
    v3.x -= (constants.time / 5.0);
    v3.x /= 4.0;
    v3.z -= (constants.time / 10.0);
    v3.y -= (constants.time / 100.0);
    vec3 param_1 = v3 * 7.0;
    v.z -= (_noise(param_1) / 15.0);
    vec3 param_2 = v3 * 7.0;
    v.y -= (((_noise(param_2) / 15.0) + (cos((v.x * 2.0) - (constants.time / 2.0)) / 5.0)) - 0.300000011920928955078125);
    vEC = v;
    gl_Position = vec4(v, 1.0);
}
