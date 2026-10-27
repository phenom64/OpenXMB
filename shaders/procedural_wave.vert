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
    vec4 effect;
    vec4 audio;
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
    p.y+=(pc.audio.x*0.012+pc.audio.w*0.028)*sin(p.x*3.0+time*0.8);
    return p;
}
// Integer hashing keeps the cloud identical across runs and GPU vendors.
float seed(uint n) {
    n^=n>>16; n*=0x7feb352du; n^=n>>15; n*=0x846ca68bu; n^=n>>16;
    return float(n&0x00ffffffu)/16777216.0;
}

float flowWindow(float t,float start,float end,float fade) {return smoothstep(start,start+fade,t)*(1.0-smoothstep(end-fade,end,t));}
float flowHash(vec2 p) {return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float flowNoise(vec2 p) {vec2 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);return mix(mix(flowHash(i),flowHash(i+vec2(1,0)),f.x),mix(flowHash(i+vec2(0,1)),flowHash(i+vec2(1,1)),f.x),f.y);}
float flowFbm(vec2 p) {float value=0.0,gain=0.5;for(int i=0;i<4;++i){value+=gain*flowNoise(p);p=mat2(1.6,1.2,-1.2,1.6)*p;gain*=0.5;}return value;}
vec4 flowWeights(float seconds) {float t=mod(seconds,128.0);return vec4(flowWindow(t,15.0,65.0,12.0),flowWindow(t,59.0,91.0,10.0),flowWindow(t,79.0,109.0,10.0),flowWindow(t,101.0,117.0,6.0));}
float flowBaseHeight(vec2 world,float seconds) {
    vec4 w=flowWeights(seconds);
    float bend=world.x*0.85+sin(world.y*0.28-seconds*0.11)*0.9+sin(world.x*0.21+world.y*0.19)*0.45;
    float ridge=sin(bend*11.0-world.y*0.41)*0.072+sin(bend*22.0-world.y*0.72)*0.013;
    ridge+=sin(world.x*0.35+world.y*0.12-seconds*0.05)*0.18;
    float hills=(flowFbm(world*0.22)-0.32)*1.65+ridge*0.22;
    float ocean=sin(world.y*0.6+seconds*0.22)*0.09+sin(world.x*0.65+world.y*0.35)*0.055;
    float mountains=pow(flowFbm(world*0.17),1.6)*2.2-0.3;
    float h=mix(ridge,hills,w.x);h=mix(h,ocean,w.y);h=mix(h,mountains,w.z);h=mix(h,ocean*0.25,w.w);
    const float depth=max(0.0,world.y-seconds*0.22-3.0);
    h+=(1.0-max(w.x,w.z))*sin(world.x*0.28+world.y*0.12-seconds*0.11)*depth*0.045;
    return h;
}
float flowHeight(vec2 world,float seconds) {
    return flowBaseHeight(world,seconds)*(1.0+pc.audio.x*0.28+pc.audio.w*0.18)+sin(world.x*2.8+world.y*1.1-seconds*0.8)*pc.audio.y*0.025;
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
    particle_alpha=0.0;
    if(pc.mode_motion.x>2.5) {
        vec2 uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
        gl_Position=vec4(uv*2.0-1.0,0,1);position=vec3(uv,0);edge_sprite=uv;return;
    }
    if(pc.mode_motion.x>1.5) {
        float seconds=pc.resolution_time_gain.z-100.0;
        vec4 w=flowWeights(seconds);float phase=mod(seconds,128.0);
        vec2 uv=vec2(float(gl_VertexIndex%512),float(gl_VertexIndex/512))/511.0;
        float z=exp(mix(log(0.045),log(42.0),uv.y));
        float x=(uv.x*2.0-1.0)*z*1.5;
        vec2 world=vec2(x+0.4*sin(seconds*0.035),z+seconds*0.22);
        float bend=world.x*0.85+sin(world.y*0.28-seconds*0.11)*0.9+sin(world.x*0.21+world.y*0.19)*0.45;
        float h=flowHeight(world,seconds);
        float camera=0.5+flowBaseHeight(vec2(0.4*sin(seconds*0.035),seconds*0.22),seconds);
        float horizon=0.02+0.035*sin(seconds*0.025);
        position=vec3(world.x,h,z);edge_sprite=vec2(bend,world.y);
        float aspect=pc.resolution_time_gain.x/pc.resolution_time_gain.y;
        // Keep homogeneous W for perspective-correct surface interpolation,
        // and real depth so distant ridges cannot paint over the foreground.
        const float nearPlane=0.04,farPlane=60.0;
        gl_Position=vec4(x/(0.8*aspect),horizon*z+(camera-h)/0.65+x*0.025*sin(seconds*0.045),
            farPlane/(farPlane-nearPlane)*z-farPlane*nearPlane/(farPlane-nearPlane),z);return;
    }
    if (pc.mode_motion.x<0.5) {
        vec2 a=vec2(float(gl_VertexIndex%100),float(gl_VertexIndex/100))/99.0*2.0-1.0;
        position=wavePosition(a);
        if(pc.effect.w>0.5) {float center=wavePosition(vec2(a.x,0)).y;position.y=center+(position.y-center)*1.65;}
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
