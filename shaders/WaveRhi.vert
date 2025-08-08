#version 450

// Fullscreen quad: matches your QuadVertex { vec2 pos; vec2 uv; }
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 vUV;      // 0..1
layout(location = 1) out vec2 vPixel;   // pixel coords

// Must mirror src/WaveItem.cpp::UniformBlock exactly (std140)
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

void main()
{
    vUV    = inUV;
    vPixel = inUV * U.resolution;
    gl_Position = vec4(inPos, 0.0, 1.0);
}
