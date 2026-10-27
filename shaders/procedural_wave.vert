#version 450

layout(std430, set=0, binding=0) readonly buffer WaveData {
    vec4 settings[16];
    float control[];
} wave;
layout(push_constant) uniform Push {
    vec4 tint;
    vec4 resolution_time_gain;
    vec4 particle_tint;
    vec4 mode_motion;
} pc;

layout(location=0) out vec3 position;
layout(location=1) out vec2 edge_sprite;
layout(location=2) out float particle_alpha;

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
    p.z-=displacement(vec2(fract(uv.x-time*s0.x*0.04*s1.w),uv.y))*s5.w;
    p.y=p.y*s6.z+s6.x+p.x*s6.y;
    return p;
}
// Integer hashing keeps the cloud identical across runs and GPU vendors.
float seed(uint n) {
    n^=n>>16; n*=0x7feb352du; n^=n>>15; n*=0x846ca68bu; n^=n>>16;
    return float(n&0x00ffffffu)/16777216.0;
}
void main() {
    particle_alpha=0.0;
    if (pc.mode_motion.x<0.5) {
        vec2 a=vec2(float(gl_VertexIndex%100),float(gl_VertexIndex/100))/99.0*2.0-1.0;
        position=wavePosition(a);
        edge_sprite=vec2(a.y,0);
        // Vulkan's framebuffer Y axis is downward. Depth is unused, so avoid
        // clipping the reference's eye-space ribbon against Vulkan's Z plane.
        gl_Position=vec4(position.x,-position.y,0,1);
        return;
    }
    uint id=uint(gl_InstanceIndex);
    vec3 r=vec3(seed(id*3u+1337u),seed(id*3u+1338u),pow(seed(id*3u+1339u),8.0)+0.1);
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
    float pixels=max(0.0,r.z*wave.settings[9].z+wave.settings[9].y);
    // A native quad replaces gl_PointSize, so large-point feature support is
    // unnecessary and the shader has exactly the same circular falloff.
    gl_Position=vec4(vec2(x,-y)+c*pixels/pc.resolution_time_gain.xy,0,1);
    position=vec3(0);
    edge_sprite=c;
}
