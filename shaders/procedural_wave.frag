#version 450
#extension GL_GOOGLE_include_directive : require
layout(std430,set=0,binding=0) readonly buffer WaveData {
    vec4 settings[16];
    float control[];
} wave;
layout(push_constant) uniform Push {
    vec4 tint;
    vec4 resolution_time_gain;
    vec4 particle_tint;
    vec4 mode_motion;
    vec4 effect;
    vec4 audio;
    vec4 flow;
} pc;
layout(location=0) in vec3 position;
layout(location=1) in vec2 edge_sprite;
layout(location=2) in float particle_alpha;
layout(location=0) out vec4 color;

#include "flow_scene.glsl"

void main() {
    if(pc.mode_motion.x>2.5) {
        FlowScene s=flowScene();
        color=vec4(mix(flowSky(edge_sprite),flowVeil(edge_sprite),max(s.close,s.mist))*
            (1.0+pc.effect.y*0.15+pc.audio.w*0.22),1);return;
    }
    if(pc.mode_motion.x>1.5) {
        FlowScene s=flowScene();
        vec3 world=vec3(position.x,position.y,edge_sprite.y);
        vec3 normal=normalize(cross(dFdx(world),dFdy(world)));
        if(normal.y<0.0)normal=-normal;
        vec3 eye=vec3(pc.flow.z,flowCameraHeight(pc.flow.x),pc.flow.w);
        vec3 view=normalize(eye-world);
        vec3 light=normalize(vec3(-0.4,0.64,-0.55));
        float diffuse=max(dot(normal,light),0.0);
        float rim=pow(1.0-clamp(abs(dot(normal,view)),0.0,1.0),3.5);
        float spec=pow(max(dot(reflect(-light,normal),view),0.0),18.0);
        float uneven=0.42+0.58*flowNoise(vec2(world.x,world.z)*0.18);
        vec3 tint=mix(vec3(0.24,0.32,0.25),vec3(0.10,0.27,0.025),s.green);
        tint=mix(tint,vec3(0.30,0.29,0.19),s.gold);
        tint=mix(tint,vec3(0.005,0.042,0.055),s.corridor);
        vec3 crest=mix(vec3(0.69,0.73,0.62),vec3(0.10,0.68,0.83),s.corridor);
        vec3 rgb=tint*(0.15+diffuse*0.68)+crest*(rim*0.55+spec*0.75+pow(spec,0.32)*0.13)*uneven;
        float fibrePhase=edge_sprite.x*31.0+flowNoise(vec2(world.x,world.z)*0.26)*5.0;
        float fibres=pow(0.5+0.5*sin(fibrePhase),10.0)/(1.0+fwidth(fibrePhase)*2.0);
        rgb+=crest*fibres*uneven*(0.005+rim*0.04);
        float broadFold=sin(world.z*0.54+sin(edge_sprite.x*0.44)*1.25+flowNoise(vec2(world.x,world.z)*0.15)*1.8);
        float leading=pow(clamp(0.5+0.5*broadFold,0.0,1.0),7.0)*uneven;
        rgb+=crest*leading*(0.12+0.7*s.corridor);
        float greenLight=pow(0.5+0.5*sin(world.z*0.54+edge_sprite.x*0.12+1.4),13.0)*
            smoothstep(0.8,2.5,abs(world.x-pc.flow.z))*exp(-position.z*0.24);
        rgb+=vec3(0.08,0.66,0.16)*greenLight*s.corridor*0.7;
        vec3 closeBase=mix(vec3(0.002,0.006,0.10),vec3(0.25,0.08,0.50),s.motif);
        vec3 closeLight=mix(vec3(0.005,0.15,0.95),vec3(0.59,0.34,0.91),s.motif);
        vec3 close=closeBase*(0.5+diffuse*0.5)+closeLight*(rim*1.1+pow(spec,0.4)*0.55);
        rgb=mix(rgb,close,s.close);
        vec3 mist=vec3(0.25,0.16,0.43)*(0.65+diffuse*0.30)+
            mix(vec3(0.79,0.61,0.88),vec3(0.80,0.43,0.86),s.motif)*(rim*0.54+pow(spec,0.36)*0.48);
        rgb=mix(rgb,mist,s.mist);
        vec2 screen=gl_FragCoord.xy/pc.resolution_time_gain.xy;
        float fog=1.0-exp(-position.z*mix(mix(0.075,0.025,s.corridor),0.15,s.mist));
        fog=max(fog,s.wash);
        rgb=mix(rgb,flowSky(screen),clamp(fog,0.0,1.0));
        rgb=mix(rgb,flowVeil(screen),max(s.close,s.mist));
        color=vec4(rgb*(1.0+pc.effect.y*0.15+pc.audio.w*0.22),1);return;
    }
    if (pc.mode_motion.x>0.5) {
        float distanceSquared=dot(edge_sprite,edge_sprite);
        if(distanceSquared>1.0) discard;
        float glow=(1.0-distanceSquared)*(1.0-distanceSquared);
        float a=clamp(particle_alpha*wave.settings[9].x*glow*pc.resolution_time_gain.w,0.0,1.0);
        if(pc.effect.z>0.5) a*=flowScene().wash;
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
    // A travelling highlight wraps only while fully beyond the visible ribbon.
    float center=1.8-mod(pc.resolution_time_gain.z*0.12,3.6);
    float distance=position.x-center;
    float halo=exp(-distance*distance/0.22)*pc.effect.x;
    float core=exp(-distance*distance/0.016)*pc.effect.x;
    vec3 tint=mix(pc.tint.rgb,vec3(1.0,0.88,1.0),clamp(halo*0.7+core*0.6+pc.audio.w*0.2,0.0,1.0));
    a=clamp(a*(1.0+halo*0.6+core*0.7)*(1.0+pc.effect.y*0.25+pc.audio.w*0.45),0.0,0.9);
    if(pc.effect.w>0.5) a*=halo*0.22;
    color=vec4(tint*(1.0+core*0.5),a);
}
