#version 450

layout(location = 0) in vec2 inPosition;  // from WaveItem grid, x in [-1,1], y in [0..1] strip
layout(location = 1) in vec2 inUV;        // same as above

layout(location = 0) out VS_OUT {
    vec2  uv;        // 0..1 across strip
    float vDist;     // distance from centreline (0 at the mid)
    float vSlope;    // curve slope for specular hint
} v;

layout(std140, binding = 0) uniform RibbonUBO {
    float time;
    float speed;
    float amplitude;   // NDC amplitude, e.g. 0.06..0.12
    float frequency;   // base angular frequency in clip space
    vec4  baseColor;   // background tint used for subtle mix
    vec4  waveColor;   // main ribbon colour
    float brightness;  // overall brightness multiplier
    vec2  resolution;  // px; not used here but kept to match UBO
    float _pad0;       // std140 pad
    float _pad1;
} ub;

// Multi‑octave sine curve for a PS3‑ish shape (matches XMBShell flavour)
float waveY(float x) {
    float t = ub.time * ub.speed;
    float f = ub.frequency;
    return
        0.55 * sin(x * f * 0.90 + t * 1.20) +
        0.30 * sin(x * f * 0.50 + t * 0.65) +
        0.15 * sin(x * f * 1.70 + t * 0.95);
}

float waveYd(float x) {
    float t = ub.time * ub.speed;
    float f = ub.frequency;
    return
        0.55 * (f * 0.90) * cos(x * f * 0.90 + t * 1.20) +
        0.30 * (f * 0.50) * cos(x * f * 0.50 + t * 0.65) +
        0.15 * (f * 1.70) * cos(x * f * 1.70 + t * 0.95);
}

void main(){
    float x = inPosition.x;             // already in clip space

    // Base vertical placement: slightly below centre like XMB
    float baseY = -0.08;

    // NDC amplitude (WaveItem drives this)
    float amp = ub.amplitude;           // try 0.08 initially

    // Curve centreline
    float centre = baseY + amp * waveY(x);

    // Physical thickness of the ribbon in NDC
    float thickness = clamp(amp * 1.6, 0.02, 0.20);

    // Expand the 0..1 strip around the curve
    float y = centre + (inUV.y - 0.5) * thickness;

    // Varyings for the fragment shader
    v.uv    = inUV;
    v.vDist = abs(inUV.y - 0.5) * (2.0);   // 0 at centre, ~1 at edge
    v.vSlope= atan(waveYd(x));

    gl_Position = vec4(x, y, 0.0, 1.0);
}