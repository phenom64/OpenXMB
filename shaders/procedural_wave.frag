#version 450
layout(std430,set=0,binding=0) readonly buffer WaveData {
    vec4 settings[16];
    float control[];
} wave;
layout(push_constant) uniform Push {
    vec4 tint;
    vec4 resolution_time_gain;
    vec4 particle_tint;
    vec4 mode_motion;
} pc;
layout(location=0) in vec3 position;
layout(location=1) in vec2 edge_sprite;
layout(location=2) in float particle_alpha;
layout(location=0) out vec4 color;
void main() {
    if (pc.mode_motion.x>0.5) {
        float distanceSquared=dot(edge_sprite,edge_sprite);
        if(distanceSquared>1.0) discard;
        float glow=(1.0-distanceSquared)*(1.0-distanceSquared);
        float a=clamp(particle_alpha*wave.settings[9].x*glow*pc.resolution_time_gain.w,0.0,1.0);
        color=vec4(pc.particle_tint.rgb*a,a);
        return;
    }
    // Undo the Vulkan Y flip when deriving the reference material's normals.
    vec3 n=normalize(cross(dFdx(position),-dFdy(position)));
    vec4 material=wave.settings[7];
    float fresnel=material.w*pow(max(0.0,1.0-n.z),material.z);
    float soft=wave.settings[8].y;
    float edge=soft>0.0 ? 1.0-smoothstep(1.0-soft,1.0,abs(edge_sprite.x)) : 1.0;
    float a=clamp((fresnel+wave.settings[8].x)*material.x*material.y*edge*pc.resolution_time_gain.w,0.0,1.0);
    color=vec4(pc.tint.rgb,a);
}
