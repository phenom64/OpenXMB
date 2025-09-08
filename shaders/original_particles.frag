// Original background particles (fragment)
#version 450

layout(location=0) in vec2 vLocal;  // unit quad [-0.5,0.5]
layout(location=1) in float vAlpha;  // per-sprite alpha

layout(location=0) out vec4 FragColor;

layout(push_constant) uniform PC {
    vec4 tint;
    vec2 resolution;
    float time;
    float brightness;
} pc;

void main(){
    // Soft circular falloff
    float r = length(vLocal*2.0);             // 0 at center, ~1 at edges
    float a = smoothstep(1.0, 0.0, r);        // soft edge
    a = a*a;                                  // steeper core
    vec3 c = pc.tint.rgb * 0.6 + vec3(0.4);   // brighten slightly
    FragColor = vec4(c, a * vAlpha);
}

