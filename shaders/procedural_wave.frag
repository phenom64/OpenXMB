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
    vec4 camera_motion;
} pc;
layout(location=0) in vec3 position;
layout(location=1) in vec2 edge_sprite;
layout(location=2) in float particle_alpha;
layout(location=3) in vec4 particle_style;
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
        if(pc.effect.z>0.5) {
            float box=max(abs(edge_sprite.x),abs(edge_sprite.y));
            float glow=1.0-smoothstep(0.25,1.0,box);
            float alpha=particle_alpha*glow*flowScene().wash*0.32;
            color=vec4(vec3(0.91,0.94,1.0)*alpha,alpha);return;
        }
        float glow;
        if(particle_style.a>1.5) {
            // The visualiser's left-end bloom.
            float d=length(edge_sprite);
            float a=clamp(particle_alpha*exp(-d*d*3.2)*(1.0-smoothstep(0.75,1.0,d))*0.4*pc.resolution_time_gain.w,0.0,1.0);
            color=vec4(particle_style.rgb*a,a);
            return;
        }
        if(particle_style.a>0.0) {
            // Out-of-focus disc: the further out of focus (particle_style.a),
            // the wider its blurred rim, until the nearest are soft blurs.
            float d=length(edge_sprite);
            if(d>1.0) discard;
            glow=1.0-smoothstep(mix(0.7,0.05,particle_style.a),1.0,d);
        } else {
            // Flat round flake in its own (tumbling) plane, so it projects
            // to an ellipse; its quad is 1.8x its size. A soft glow appears
            // only while it glints (position.x).
            float u=length(edge_sprite)*1.8;
            if(u>1.8) discard;
            glow=(1.0-smoothstep(0.5,1.1,u))+position.x*0.3*exp(-u*u*0.9);
        }
        float a=clamp(particle_alpha*wave.settings[9].x*glow*pc.resolution_time_gain.w,0.0,1.0);
        color=vec4(pc.particle_tint.rgb*particle_style.rgb*a,a);
        return;
    }
    // The vertex stage supplies the smooth analytic normal; only its facing
    // matters (the sheet is seen from both sides through its folds).
    vec3 n=normalize(particle_style.xyz);n.z=abs(n.z);
    vec4 material=wave.settings[7];
    // Every fold edge is where the surface turns exactly edge-on. Capping
    // the edge-on term turns each fold into a bright plateau rather than a
    // full-brightness one-pixel step (or, if faded out, a dark crease).
    float fresnel=material.w*pow(1.0-max(n.z,0.2),material.z);
    // The stream is nearest the viewer at the left, where it reads brighter.
    float near=1.0+0.35*smoothstep(0.1,-1.0,position.x);
    // edge_sprite.y is the position across the band (+-1 on the outline):
    // the outermost edge is dimmer than the inner strands and fades over a
    // few pixels into the background.
    float across=abs(edge_sprite.y);
    float outline=mix(1.0,0.6,smoothstep(0.7,1.0,across));
    // Faint mottling runs along the flow (edge_sprite.x goes once around the
    // loop, so whole periods keep the pattern seamless).
    float loop=edge_sprite.x*3.14159265,warp=0.9*sin(1.7*position.x+0.05*wave.settings[8].z);
    float fibres=0.86+0.09*sin(13.0*loop+warp)+0.05*sin(23.0*loop-1.3*warp+2.0);
    float a=clamp((fresnel*outline*fibres+wave.settings[8].x+0.05*pc.effect.x)*near*smoothstep(1.0,0.9,across)*
        material.x*material.y*pc.resolution_time_gain.w,0.0,1.0);
    // Music visualiser (pc.effect.x): the wave runs from a pale, hot left end
    // lit by the bloom (pc.flow.x) to the deep theme colour on the right.
    float music=pc.effect.x;
    float lit=exp(-pow((position.x+1.0)/0.42,2.0))*clamp(pc.flow.x,0.0,1.6);
    vec3 gradient=mix(vec3(1.0,0.7,0.95),vec3(0.74,0.17,0.8),smoothstep(-0.9,0.5,position.x));
    vec3 tint=mix(pc.tint.rgb,gradient,music);
    // Dense strands read as pale and misty rather than saturated (in the
    // visualiser only towards the lit end).
    tint=mix(tint,mix(vec3(0.86,0.9,1.0),vec3(1.0,0.76,0.96),music),
        mix(0.5,0.6*(1.0-smoothstep(-0.6,0.4,position.x)),music)*smoothstep(0.08,0.45,a));
    tint=mix(tint,vec3(1.0,0.9,0.98),clamp(music*lit*0.5,0.0,1.0));
    a=clamp(a*(1.0-0.4*music)*(1.0+music*(lit*0.8+pc.effect.y*0.15)),0.0,0.9);
    color=vec4(tint,a);
}
