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
    vec4 effect;
} pc;
layout(location=0) in vec3 position;
layout(location=1) in vec2 edge_sprite;
layout(location=2) in float particle_alpha;
layout(location=0) out vec4 color;

float flowWindow(float t,float start,float end,float fade) {return smoothstep(start,start+fade,t)*(1.0-smoothstep(end-fade,end,t));}
float flowHash(vec2 p) {return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float flowNoise(vec2 p) {vec2 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);return mix(mix(flowHash(i),flowHash(i+vec2(1,0)),f.x),mix(flowHash(i+vec2(0,1)),flowHash(i+vec2(1,1)),f.x),f.y);}
float flowFbm(vec2 p) {float value=0.0,gain=0.5;for(int i=0;i<4;++i){value+=gain*flowNoise(p);p=mat2(1.6,1.2,-1.2,1.6)*p;gain*=0.5;}return value;}
vec4 flowWeights(float seconds) {float t=mod(seconds,128.0);return vec4(flowWindow(t,15.0,65.0,12.0),flowWindow(t,59.0,91.0,10.0),flowWindow(t,79.0,109.0,10.0),flowWindow(t,101.0,117.0,6.0));}
float flowHeight(vec2 world,float seconds) {
    vec4 w=flowWeights(seconds);
    float bend=world.x*0.85+sin(world.y*0.28-seconds*0.11)*0.9+sin(world.x*0.21+world.y*0.19)*0.45;
    float ridge=sin(bend*11.0-world.y*0.41)*0.072+sin(bend*22.0-world.y*0.72)*0.013;
    ridge+=sin(world.x*0.35+world.y*0.12-seconds*0.05)*0.18;
    float hills=(flowFbm(world*0.22)-0.32)*1.65+ridge*0.22;
    float ocean=sin(world.y*0.6+seconds*0.22)*0.09+sin(world.x*0.65+world.y*0.35)*0.055;
    float mountains=pow(flowFbm(world*0.17),1.6)*2.2-0.3;
    float h=mix(ridge,hills,w.x);h=mix(h,ocean,w.y);h=mix(h,mountains,w.z);h=mix(h,ocean*0.25,w.w);
    return h*(1.0+pc.effect.y*0.16);
}
vec3 flowSky(float seconds,float screenY) {
    float t=mod(seconds,128.0);vec4 w=flowWeights(seconds);
    float day=flowWindow(t,25.0,63.0,13.0),green=flowWindow(t,39.0,64.0,10.0);
    vec3 sky=mix(vec3(0),mix(vec3(0.55,0.67,0.68),vec3(0.05,0.35,0.40),green),day);
    sky=mix(sky,vec3(0.0,0.07,0.22),w.y*0.92);
    sky=mix(sky,vec3(0.52,0.56,0.61),w.z*0.9);
    sky=mix(sky,vec3(0.055,0.32,0.34),w.w);
    return sky*mix(0.58,1.0,clamp(screenY,0.0,1.0));
}

void main() {
    if(pc.mode_motion.x>2.5) {
        float seconds=pc.resolution_time_gain.z-100.0;
        color=vec4(flowSky(seconds,edge_sprite.y),1);return;
    }
    if(pc.mode_motion.x>1.5) {
        float seconds=pc.resolution_time_gain.z-100.0,phase=mod(seconds,128.0);vec4 w=flowWeights(seconds);
        const float e=0.025;vec2 world=vec2(position.x,edge_sprite.y);
        float dx=(flowHeight(world+vec2(e,0),seconds)-flowHeight(world-vec2(e,0),seconds))/(2.0*e);
        float dz=(flowHeight(world+vec2(0,e),seconds)-flowHeight(world-vec2(0,e),seconds))/(2.0*e);
        vec3 n=normalize(vec3(-dx,1.0,-dz));
        float stripePhase=edge_sprite.x*11.0-edge_sprite.y*0.41;
        float crest=pow(0.5+0.5*sin(stripePhase),12.0)/(1.0+fwidth(stripePhase)*1.5);
        float broad=pow(0.5+0.5*sin(stripePhase-0.16),4.0);
        float gold=flowWindow(phase,13.0,48.0,12.0),green=flowWindow(phase,38.0,65.0,10.0),neon=flowWindow(phase,115.0,128.0,5.0);
        vec3 tint=mix(vec3(0.06,0.50,0.63),vec3(0.72,0.66,0.38),gold);
        tint=mix(tint,vec3(0.24,0.38,0.035),green);tint=mix(tint,vec3(0.02,0.23,0.73),w.y);tint=mix(tint,vec3(0.67,0.71,0.82),w.z);tint=mix(tint,vec3(0.13,0.70,0.36),neon);
        float detail=flowFbm(vec2(position.x,edge_sprite.y)*6.0);
        float lit=max(dot(n,normalize(vec3(-0.3,0.9,-0.25))),0.0);
        float terrain=max(w.x,w.z);
        vec3 rgb=tint*(0.04+broad*0.22+crest*1.25);
        vec3 land=tint*(0.2+lit*0.7)*(0.7+detail*0.5)+vec3(0.33)*pow(lit,12.0);
        rgb=mix(rgb,land,terrain);rgb=mix(rgb,tint*(0.05+lit*0.4)+vec3(0.12,0.35,0.9)*crest,w.y);
        float fog=1.0-exp(-position.z*mix(0.055,0.11,w.w));
        rgb=mix(rgb,flowSky(seconds,0.54),clamp(fog+w.w*0.65,0.0,1.0));
        color=vec4(rgb*(1.0+pc.effect.y*0.12),1);return;
    }
    if (pc.mode_motion.x>0.5) {
        float distanceSquared=dot(edge_sprite,edge_sprite);
        if(distanceSquared>1.0) discard;
        float glow=(1.0-distanceSquared)*(1.0-distanceSquared);
        float a=clamp(particle_alpha*wave.settings[9].x*glow*pc.resolution_time_gain.w,0.0,1.0);
        if(pc.effect.z>0.5) {float t=mod(pc.resolution_time_gain.z-100.0,128.0);a*=max(flowWindow(t,101.0,117.0,6.0),flowWindow(t,115.0,128.0,5.0)*0.25);}
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
    vec3 tint=mix(pc.tint.rgb,vec3(1.0,0.88,1.0),clamp(halo*0.7+core*0.6,0.0,1.0));
    a=clamp(a*(1.0+halo*0.6+core*0.7)*(1.0+pc.effect.y*0.12),0.0,0.9);
    if(pc.effect.w>0.5) a*=halo*0.22;
    color=vec4(tint*(1.0+core*0.5),a);
}
