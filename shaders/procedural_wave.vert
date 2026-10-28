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

const float SHEET_RADIUS=0.11,SHEET_B2=0.75,SHEET_B3=0.45,SHEET_DEPTH=0.15,PLEAT_DEPTH=0.7;
// The firmware's centre line is one long sine (about 3.8 half-widths per
// wavelength) whose phase drifts rightward, so the screen always shows a
// single smooth S rather than several bumps; its amplitude breathes slowly.
// posY and tilt keep their meaning as offsets (music mode animates them).
// The music visualiser swings ~1.3x wider, and the swell of the music
// (pc.effect.w, a slow loudness envelope) widens it further.
float waveCentre(float x) {
    float t=wave.settings[8].z;
    float amplitude=(0.2+0.06*sin(0.37*t+1.0)+0.035*sin(0.83*t))*(1.0+pc.effect.x*(0.3+0.4*pc.effect.w));
    float phase=-0.26*t-0.09*sin(0.19*t+2.0);
    float y=wave.settings[6].x+0.148+(wave.settings[6].y*0.22+0.045*sin(0.23*t+2.0))*x+amplitude*sin(1.66*x+phase);
    return y+(pc.audio.x*0.012+pc.audio.w*0.028)*sin(x*3.0+t*0.8);
}
// The firmware's wave is one translucent sheet rolled into a closed tube
// and seen side-on, shaded by its angle to the viewer: the top and bottom
// outlines are where the surface turns edge-on (brightest), the face-on
// middle is a dark "eye", and harmonics in the cross-section add inner folds
// that are born and die along the wave, drawing tapered bright lenses.
// Mesh rows run once around the cross-section (v in 0..2pi).
// Cross-section at screen x: (height, depth) for loop angle v, in units of
// the radius, plus the radius itself.
vec3 sheetSection(float x,float v) {
    float t=wave.settings[8].z;
    float radius=SHEET_RADIUS*mix(1.06,0.86,x*0.5+0.5)*(1.0+0.18*sin(1.3*x-0.11*t+0.6))*(1.0+0.18*pc.effect.x*pc.effect.w);
    float b2=SHEET_B2*(0.6+0.4*sin(1.2*x+0.16*t+1.1));
    float b3=SHEET_B3*sin(1.6*x-0.23*t+2.3);
    float p2=0.9*x+0.07*t,p3=-1.4*x+0.12*t;
    float y=sin(v)+b2*sin(2.0*v+p2)+b3*sin(3.0*v+p3);
    // The pleats run much deeper than the main loop, so they are seen
    // nearly edge-on over long stretches and stack into bright strands,
    // while the loop's outline stays a thinner edge.
    float z=SHEET_DEPTH*cos(v)+PLEAT_DEPTH*(b2*cos(2.0*v+p2)+b3*cos(3.0*v+p3));
    return vec3(y,z,radius);
}
vec3 sheetPosition(vec2 a) {
    vec3 section=sheetSection(a.x,(a.y+1.0)*3.14159265);
    return vec3(a.x,waveCentre(a.x)+section.z*section.x,section.y);
}
// The column's outline (lowest and highest height around the loop).
vec2 sheetExtent(float x) {
    vec2 extent=vec2(1e9,-1e9);
    for(int i=0;i<48;++i) {
        float y=sheetSection(x,float(i)*(6.2831853/48.0)).x;
        extent=vec2(min(extent.x,y),max(extent.y,y));
    }
    return extent;
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
        position=sheetPosition(a);
        // Smooth analytic normal (per-triangle derivative normals faceted the
        // sheet into hard streaks); the cross-section is closed, so central
        // differences wrap.
        const float e=1.0/127.0,f=1.0/float(MESH_ROWS);
        vec3 du=sheetPosition(a+vec2(e,0))-sheetPosition(a-vec2(e,0));
        vec3 dv=sheetPosition(a+vec2(0,f))-sheetPosition(a-vec2(0,f));
        particle_style=vec4(normalize(cross(du,dv)),0);
        // Position across the band: -1 and 1 on the outline.
        vec2 extent=sheetExtent(a.x);
        float height=sheetSection(a.x,(a.y+1.0)*3.14159265).x;
        edge_sprite=vec2(a.y,(2.0*height-extent.x-extent.y)/max(extent.y-extent.x,1e-3));
        // Vulkan's framebuffer Y axis is downward. Depth is unused, so avoid
        // clipping the reference's eye-space ribbon against Vulkan's Z plane.
        gl_Position=vec4(position.x,-position.y,0,1);
        return;
    }
    uint id=uint(gl_InstanceIndex);
    const vec2 corners[6]=vec2[6](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(-1,1),vec2(1,-1),vec2(1,1));
    if(pc.effect.z<0.5&&id>=uint(wave.settings[10].z)) {
        // The music visualiser's light at the left end of the wave: a broad
        // soft bloom (pc.flow.x is its flare level).
        vec2 c=corners[gl_VertexIndex];
        float scale=pc.resolution_time_gain.y/1080.0;
        vec2 size=vec2(330.0,190.0)*scale*(0.85+0.25*pc.flow.x);
        vec2 center=vec2(-1.0,waveCentre(-0.96));
        gl_Position=vec4(center.x,-center.y,0,1)+vec4(c*size*2.0/pc.resolution_time_gain.xy,0,0);
        edge_sprite=c;position=vec3(0);
        particle_style=vec4(1.0,0.9,0.97,2.0);
        particle_alpha=pc.effect.x*clamp(pc.flow.x,0.0,1.6);
        return;
    }
    vec3 r=vec3(seed(id*3u+1337u),seed(id*3u+1338u),pow(seed(id*3u+1339u),8.0)+0.1);
    if(pc.effect.z>0.5) {
        float age=fract(seed(id+219u)+pc.flow.x*0.045);
        float depth=1.0+age*3.5;
        vec2 center=vec2((r.x-0.5)*4.0,(r.y-0.5)*3.0)/depth;
        center+=vec2(0.07*sin(pc.flow.x*0.032),0.08);
        center=flowScreen(center*0.5+0.5)*2.0-1.0;
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
    vec2 motion=pc.mode_motion.yz*(0.6+r.z*0.8);
    x=fract((x+motion.x+1.0)*0.5)*2.0-1.0;
    float seconds=pc.resolution_time_gain.z;
    float h1=seed(id*7u+4001u),h2=seed(id*7u+4002u),h3=seed(id*7u+4003u),h4=seed(id*7u+4004u);
    float h5=seed(id*7u+4005u),h6=seed(id*7u+4006u),h7=seed(id*7u+4007u),h8=seed(id*7u+4008u);
    // Flakes fill much of the screen's height, densest around the wave.
    float center=pc.mode_motion.w>0.5 ? waveCentre(x) : 0.0;
    y=center+y*wave.settings[10].y*(h8>0.55 ? 2.4 : 1.0)+wave.settings[10].x+motion.y;
    vec2 c=corners[gl_VertexIndex];
    // Particle population measured from PS3 captures (lossless RPCS3 frames
    // and a 1080p recording): small flat discs tumbling in 3D, so they show
    // as circles narrowing to ellipses and slivers; most are dim, a few flash
    // tinted when they mirror the light. Near the camera on the left the
    // stream defocuses into flat, feather-edged discs.
    // Each flake turns about its own random axis (2.5-8 s a turn).
    vec3 axis=normalize(vec3(h5-0.5,h6-0.5,h7-0.5)+vec3(0.0,0.0,0.001));
    vec3 e0=normalize(cross(axis,vec3(0.36,0.48,0.8))),f0=cross(axis,e0);
    float turn=6.2831853*seconds/(2.5+5.5*h5)+h6*6.2831853;
    float ct=cos(turn),st=sin(turn);
    // Rodrigues rotation of the flake's two in-plane edges.
    vec3 e1=e0*ct+cross(axis,e0)*st+axis*dot(axis,e0)*(1.0-ct);
    vec3 e2=f0*ct+cross(axis,f0)*st+axis*dot(axis,f0)*(1.0-ct);
    vec3 normal=cross(e1,e2);
    float facing=abs(normal.z);
    // A glint lasts ~0.3 s in the captures, so only ~1% flash at once.
    float glint=pow(abs(dot(normal,normalize(vec3(-0.3,0.45,1.0)))),80.0)*(1.6+2.2*h3);
    // Measured: the median flake is only ~15 levels above the background,
    // while rare glints reach ~115.
    particle_alpha=mix(0.045,0.15,h1)*(0.45+0.55*sqrt(facing))*(1.0+6.0*glint);
    // The visualiser keeps only a sparse dust; the rest fade out with the
    // glide rather than popping.
    particle_alpha*=clamp((1.0-0.78*pc.effect.x-seed(id*7u+4009u))*12.0,0.0,1.0);
    vec3 iridescent=h4<0.3 ? vec3(1.0) : h4<0.5 ? vec3(1.0,0.74,0.86) : h4<0.7 ? vec3(1.0,0.86,0.58) :
        h4<0.85 ? vec3(0.86,0.80,1.0) : vec3(0.80,0.95,1.0);
    particle_style.rgb=mix(vec3(1.0,0.97,0.92),iridescent,clamp(glint*0.5,0.0,1.0));
    float scale=pc.resolution_time_gain.y/1080.0;
    // Disc radius, in pixels.
    float half_edge=(1.9+1.5*h3+1.5*pow(r.z-0.1,0.6))*scale;
    float glow=clamp(glint,0.0,2.0);
    if(h2>0.9) {
        // The near-camera stream on the left: depth falls from the screen
        // edge inwards, so size and defocus fall with it. Fast at first,
        // slowing as it nears the edge, so the cloud is densest there.
        float life=fract(h1+seconds*0.18/(1.0+h3));
        x=-0.62-0.52*sqrt(life);
        float nearness=clamp((-0.62-x)/0.52,0.0,1.0);
        y=(pc.mode_motion.w>0.5 ? waveCentre(x) : 0.0)+wave.settings[10].x+(h6-0.5)*mix(0.1,0.32,nearness)-life*0.06+motion.y;
        particle_style.a=h7>0.4 ? smoothstep(0.15,0.7,nearness) : 0.0;
        particle_style.rgb=mix(vec3(1.0,0.95,0.88),iridescent,0.6);
        particle_alpha=mix(particle_alpha*1.6,0.06+0.2*h4*h4,particle_style.a)*sin(life*3.14159265)*
            clamp((1.0-0.78*pc.effect.x-seed(id*7u+4009u))*12.0,0.0,1.0);
        half_edge*=mix(1.0,1.5,nearness);
        if(particle_style.a>0.0) {
            // Flat disc, 12-30 px across.
            float pixels=mix(half_edge*4.0,(12.0+18.0*h3*h3)*scale,particle_style.a);
            gl_Position=vec4(vec2(x,-y)+c*pixels/pc.resolution_time_gain.xy,0,1);
            position=vec3(0);edge_sprite=c;return;
        }
    }
    // The flake's disc, rotated in 3D and projected (an ellipse), inside a
    // quad 1.8x its size so a glinting flake's glow fits; the extra pixel
    // keeps edge-on slivers from vanishing between pixel centres.
    vec2 corner=(c.x*e1.xy+c.y*e2.xy)*half_edge*1.8+c*(0.7*scale);
    gl_Position=vec4(vec2(x,-y)+corner*2.0/pc.resolution_time_gain.xy,0,1);
    position=vec3(glow,0,0);
    edge_sprite=c;
}
