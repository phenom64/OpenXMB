// Original background particles (vertex)
#version 450

layout(location=0) in vec2 inPos;   // unit quad vertices in [-0.5,0.5]
layout(location=1) in vec2 inSeed;  // per-instance seed in [0,1)

layout(push_constant) uniform PC {
    vec4 tint;        // RGB base
    vec2 resolution;  // width,height
    float time;       // seconds
    float brightness; // 0..1
} pc;

layout(location=0) out vec2 vLocal; // pass unit quad coord to frag
layout(location=1) out float vAlpha; // per-sprite alpha

// Dave Hoskins—style hash/value noise (2D)
float hash12(vec2 p){
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673U, 3812015801U);
    uint n = (q.x ^ q.y) * 1597334673U; return float(n) * 2.328306437080797e-10;
}
float value2d(vec2 p){
    vec2 pg=floor(p),pc=p-pg,k=vec2(0,1);
    pc*=pc*pc*(3.-2.*pc);
    return mix(mix(hash12(pg+k.xx),hash12(pg+k.yx),pc.x), mix(hash12(pg+k.xy),hash12(pg+k.yy),pc.x), pc.y);
}

// Approximate the ribbon SDF (same formulation as in original.frag),
// sampled at a z=0 slice. Used to bias particle density/size near the wave.
float sdf(vec3 q){
    q *= 2.0;
    float o = 4.2*sin(0.05*q.x + pc.time*0.25)
            + 0.04*q.z
            + sin(q.x*0.11 + pc.time)
            + 2.0*sin(q.z*0.20 + pc.time)
            + value2d(vec2(0.03,0.4)*q.xz + vec2(pc.time*0.5,0.0));
    return abs(dot(q, normalize(vec3(0.0,1.0,0.05))) + 2.5 + o*0.5);
}

void main(){
    // Smooth, non-teleport drift: sample value noise along time-varying lines
    float t = pc.time * 0.05;
    vec2 s = inSeed*64.0; // domain scale
    vec2 drift;
    drift.x = value2d(s + vec2(0.0, t)) - 0.5;
    drift.y = value2d(s + vec2(37.13, t*1.2)) - 0.5;
    drift *= 0.35; // magnitude in NDC-ish scale

    // Base position from seed (uniform in screen), then drift
    vec2 p = inSeed * pc.resolution;              // pixels
    p += drift * pc.resolution.y;                 // scale drift by height so AR-neutral
    vec2 center = p/pc.resolution * 2.0 - 1.0;    // NDC center

    // Approximate proximity to ribbon using SDF at z=0 slice (cheap bias)
    vec2 U = p; // pixels
    vec3 q0 = vec3((U - 0.5*pc.resolution)/pc.resolution.y, 0.0);
    float dfield = sdf(q0);
    float waveBias = smoothstep(0.9, 0.0, dfield); // high near ribbon

    // Sprite size: scale with brightness, a touch of noise, and wave bias
    float n = value2d(s + vec2(123.7, 913.1));
    float px = mix(1.5, 3.5, n) * (0.5 + 0.5*pc.brightness) * mix(0.6, 1.75, waveBias);
    vec2 halfSize = vec2(px/pc.resolution.y);     // keep aspect-independent

    // Expand the unit quad about the center
    vec2 pos = center + inPos * halfSize * 2.0;
    gl_Position = vec4(pos, 0.0, 1.0);

    vLocal = inPos;
    // Alpha prefers particles near the ribbon and scales with brightness
    vAlpha = mix(0.15, 0.95, n) * pc.brightness * mix(0.25, 1.0, waveBias);
}
