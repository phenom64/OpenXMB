#version 450
#extension GL_GOOGLE_include_directive : require

layout(std430, set=0, binding=0) readonly buffer WaveData {
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

layout(location=0) out vec3 position;
layout(location=1) out vec2 edge_sprite;
layout(location=2) out float particle_alpha;
// Particle colour multiplier (rgb) and out-of-focus disc weight (a).
layout(location=3) out vec4 particle_style;

const int ROWS=64;
const int POINTS=28;
float spline(int row,float u) {
    float s=clamp(u,0.0,1.0)*25.0;
    int i=min(int(s),24), base=row*POINTS+i;
    float t=min(s-float(i),1.0),t2=t*t,t3=t2*t;
    return ((1.0-3.0*t+3.0*t2-t3)*wave.control[base]+(4.0-6.0*t2+3.0*t3)*wave.control[base+1]+
        (1.0+3.0*t+3.0*t2-3.0*t3)*wave.control[base+2]+t3*wave.control[base+3])/6.0;
}
float displacement(vec2 uv) {
    float row=clamp(uv.y,0.0,1.0)*float(ROWS-1);
    int i=min(int(row),ROWS-1);
    return mix(spline(i,uv.x),spline(min(i+1,ROWS-1),uv.x),fract(row));
}
vec3 wavePosition(vec2 a) {
    vec4 s0=wave.settings[0],s1=wave.settings[1],s2=wave.settings[2];
    vec4 s3=wave.settings[3],s4=wave.settings[4],s5=wave.settings[5],s6=wave.settings[6];
    float time=wave.settings[8].z;
    vec2 uv=(a+1.0)*0.5;
    vec3 p=vec3(a.x,displacement(uv),a.y);
    p.y+=sin(p.x*s3.x+s5.x+time*s0.x)*s3.w;
    p.z+=cos(p.z*s4.z+s5.z+time*s0.x)*s4.w;
    float base=cos(p.x*s6.w-time*0.5*s1.w)*s2.x+s2.y;
    base*=1.0-s0.z;
    base+=s0.y*sin(p.x*s0.w+time*s0.x*s1.w*0.25);
    float perturbation=s1.y*s1.z*(
        sin((p.x*s0.w*6.0+p.z*0.5)*s1.x*0.01+time*s0.x*s1.w*0.7)*0.5+
        sin((p.x*s0.w*10.0-p.z*0.8)*s1.x*0.005-time*s0.x*s1.w*0.35)*0.25);
    p.y-=s2.w*tanh((base+perturbation)*s2.z/s2.w);
    // The scrolling detail wraps around; cross-fade two half-period-offset
    // samples so neither is used at its wrap point (a raw fract() wrap left a
    // vertical seam drifting across the sheet).
    float scroll=uv.x-time*s0.x*0.04*s1.w;
    float seam=abs(2.0*fract(scroll)-1.0);
    p.z-=mix(displacement(vec2(fract(scroll),uv.y)),displacement(vec2(fract(scroll+0.5),uv.y)),seam)*s5.w;
    p.y=p.y*s6.z+s6.x+p.x*s6.y;
    p.y+=(pc.audio.x*0.012+pc.audio.w*0.028)*sin(p.x*3.0+time*0.8);
    return p;
}
// Integer hashing keeps the cloud identical across runs and GPU vendors.
float seed(uint n) {
    n^=n>>16; n*=0x7feb352du; n^=n>>15; n*=0x846ca68bu; n^=n>>16;
    return float(n&0x00ffffffu)/16777216.0;
}

#include "flow_scene.glsl"

void main() {
    particle_alpha=0.0;
    particle_style=vec4(1,1,1,0);
    if(pc.mode_motion.x>2.5) {
        vec2 uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
        gl_Position=vec4(uv*2.0-1.0,0,1);position=vec3(uv,0);edge_sprite=uv;return;
    }
    if(pc.mode_motion.x>1.5) {
        float seconds=pc.flow.x;
        vec2 uv=vec2(float(gl_VertexIndex%512),float(gl_VertexIndex/512))/511.0;
        float z=exp(mix(log(0.045),log(48.0),uv.y));
        float x=(uv.x*2.0-1.0)*z*1.65;
        vec2 world=vec2(x+pc.flow.z,z+pc.flow.w);
        float h=flowHeight(world,seconds);
        position=vec3(world.x,h,z);edge_sprite=vec2(flowWarp(world,seconds),world.y);
        float aspect=pc.resolution_time_gain.x/pc.resolution_time_gain.y;
        const float nearPlane=0.04,farPlane=60.0;
        vec3 view=vec3(x,flowCameraHeight(seconds)-h,z);
        float yaw=pc.camera_motion.x,pitch=pc.camera_motion.y,roll=pc.camera_motion.z;
        view.xz=mat2(cos(yaw),-sin(yaw),sin(yaw),cos(yaw))*view.xz;
        view.yz=mat2(cos(pitch),-sin(pitch),sin(pitch),cos(pitch))*view.yz;
        view.xy=mat2(cos(roll),-sin(roll),sin(roll),cos(roll))*view.xy;
        position.z=view.z;
        gl_Position=vec4(view.x/(0.8*aspect),flowHorizon()*view.z+view.y/0.65,
            farPlane/(farPlane-nearPlane)*view.z-farPlane*nearPlane/(farPlane-nearPlane),view.z);return;
    }

    if (pc.mode_motion.x<0.5) {
        // kProceduralWaveMeshColumns x kProceduralWaveMeshRows.
        const int COLUMNS=128,MESH_ROWS=448;
        vec2 a=vec2(float(gl_VertexIndex%COLUMNS),float(gl_VertexIndex/COLUMNS))/vec2(COLUMNS-1,MESH_ROWS-1)*2.0-1.0;
        position=wavePosition(a);
        // Smooth surface normal from the analytic surface, interpolated per
        // fragment; per-triangle derivative normals faceted the sheet into
        // hard streaks the reference's soft translucent sheets do not have.
        const float e=1.0/99.0;
        vec3 du=wavePosition(a+vec2(e,0))-wavePosition(a-vec2(e,0));
        vec3 dv=wavePosition(a+vec2(0,e))-wavePosition(a-vec2(0,e));
        if(pc.effect.w>0.5) {
            float center=wavePosition(vec2(a.x,0)).y;position.y=center+(position.y-center)*1.65;
            du.y*=1.65;dv.y*=1.65;
        }
        particle_style=vec4(normalize(cross(du,dv)),0);
        edge_sprite=vec2(a.y,0);
        // Vulkan's framebuffer Y axis is downward. Depth is unused, so avoid
        // clipping the reference's eye-space ribbon against Vulkan's Z plane.
        gl_Position=vec4(position.x,-position.y,0,1);
        return;
    }
    uint id=uint(gl_InstanceIndex);
    vec3 r=vec3(seed(id*3u+1337u),seed(id*3u+1338u),pow(seed(id*3u+1339u),8.0)+0.1);
    if(pc.effect.z>0.5) {
        float age=fract(seed(id+219u)+pc.flow.x*0.045);
        float depth=1.0+age*3.5;
        vec2 center=vec2((r.x-0.5)*4.0,(r.y-0.5)*3.0)/depth;
        center+=vec2(0.07*sin(pc.flow.x*0.032),0.08);
        center=flowScreen(center*0.5+0.5)*2.0-1.0;
        const vec2 corners[6]=vec2[6](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(-1,1),vec2(1,-1),vec2(1,1));
        vec2 c=corners[gl_VertexIndex];
        float angle=pc.camera_motion.z+0.12*sin(pc.flow.x*0.06+r.x*6.0);
        vec2 corner=mat2(cos(angle),-sin(angle),sin(angle),cos(angle))*(c*vec2(1.0,0.64));
        float pixels=(4.0+42.0*pow(r.y,5.0))/depth*pc.resolution_time_gain.x/1280.0;
        gl_Position=vec4(center+corner*pixels/pc.resolution_time_gain.xy,0,1);
        edge_sprite=c;position=vec3(0);
        particle_alpha=sin(age*3.14159265)*sin(age*3.14159265)*(0.25+r.x*0.65);
        return;
    }
    float time=pc.resolution_time_gain.z*wave.settings[9].w;
    float ratio=clamp(pc.resolution_time_gain.x/pc.resolution_time_gain.y,1.0,2.0)*0.375;
    float x=fract(time*(r.x-0.5)/15.0+r.y*50.0)*2.0-1.0;
    float y=sin(time*(r.y+1.5)/4.0+r.x*100.0)/((6.0-r.x*4.0*r.y)/ratio);
    float op=mix(sin(time*(r.x+0.5)*12.0+r.y*10.0),sin(time*(r.y+1.5)*6.0+r.x*4.0),y*0.5+0.5)*r.x+r.y;
    particle_alpha=op*op*(1.0-fract(r.x+time*0.00285));
    vec2 motion=pc.mode_motion.yz*(0.6+r.z*0.8);
    x=fract((x+motion.x+1.0)*0.5)*2.0-1.0;
    float center=pc.mode_motion.w>0.5 ? wavePosition(vec2(x,0)).y : 0.0;
    y=center+y*wave.settings[10].y+wave.settings[10].x+motion.y;
    const vec2 corners[6]=vec2[6](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(-1,1),vec2(1,-1),vec2(1,1));
    vec2 c=corners[gl_VertexIndex];
    // Particle population measured from the RPCS3 wave captures: most are
    // tiny sharp motes with a faint glow; each is a flake that turns over
    // (2.5-8 s per turn) and glints twice a turn for ~0.3 s at 2.5-4x its
    // resting brightness, briefly taking an iridescent tint, and narrows when
    // edge-on; a few near the camera are large, flat, soft bokeh discs that
    // gather on the left and drift faster (parallax).
    float seconds=pc.resolution_time_gain.z;
    float h1=seed(id*7u+4001u),h2=seed(id*7u+4002u),h3=seed(id*7u+4003u),h4=seed(id*7u+4004u);
    float h5=seed(id*7u+4005u),h6=seed(id*7u+4006u);
    float turn=6.2831853*seconds/(2.5+5.5*h5)+h6*6.2831853;
    float facing=abs(cos(turn));
    float glint=pow(facing,40.0)*(1.5+2.0*h3);
    particle_alpha*=mix(0.5,1.0,pow(h1,1.6))*mix(0.55,1.0,facing)*(1.0+glint);
    vec3 iridescent=h4<0.3 ? vec3(1.0) : h4<0.5 ? vec3(1.0,0.74,0.86) : h4<0.7 ? vec3(1.0,0.86,0.58) :
        h4<0.85 ? vec3(0.86,0.80,1.0) : vec3(0.80,0.95,1.0);
    particle_style.rgb=mix(vec3(1.0,0.97,0.92),iridescent,clamp(glint*0.45,0.0,1.0));
    // Quads are four core widths across so the glow halo fits inside.
    float core=(1.1+1.9*pow(r.z-0.1,0.5)+1.2*h3*h3)*pc.resolution_time_gain.y/1080.0;
    float pixels=core*4.0;
    vec2 shape=vec2(mix(0.45,1.0,facing),1.0);
    if(h2>0.92) {
        float life=fract(h1+seconds*0.2/(1.0+h3));
        x=-0.62-life*0.5;
        y=(pc.mode_motion.w>0.5 ? wavePosition(vec2(x,0)).y : 0.0)+wave.settings[10].x+(h6-0.5)*0.22-life*0.15+motion.y;
        particle_style=vec4(mix(vec3(1.0,0.95,0.88),iridescent,0.35*glint),1.0);
        particle_alpha=0.22*(0.5+0.5*h4)*sin(life*3.14159265)*(1.0+0.6*glint);
        pixels=(6.0+18.0*h3*h3)*pc.resolution_time_gain.y/1080.0;
        shape=vec2(1.0);
    }
    float angle=h4*6.2831853+seconds*0.15*(h1-0.5);
    vec2 corner=mat2(cos(angle),sin(angle),-sin(angle),cos(angle))*(c*shape);
    // A native quad replaces gl_PointSize, so large-point feature support is
    // unnecessary; the fragment falloff stays in the quad's own coordinates.
    gl_Position=vec4(vec2(x,-y)+corner*pixels/pc.resolution_time_gain.xy,0,1);
    position=vec3(0);
    edge_sprite=c;
}
