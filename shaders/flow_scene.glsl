// Shared geometry and atmosphere for the continuously evolving Flow scene.
// Scene dwell is independent of the audio-modulated camera travel clock.
float flowWindow(float t,float start,float end,float fade) {
    return smoothstep(start,start+fade,t)*(1.0-smoothstep(end-fade,end,t));
}
float flowHash(vec2 p) {return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float flowNoise(vec2 p) {
    vec2 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);
    return mix(mix(flowHash(i),flowHash(i+vec2(1,0)),f.x),mix(flowHash(i+vec2(0,1)),flowHash(i+vec2(1,1)),f.x),f.y);
}
float flowFbm(vec2 p) {
    float result=0.0,gain=0.5;
    for(int i=0;i<3;++i){result+=gain*flowNoise(p);p=mat2(1.6,1.2,-1.2,1.6)*p;gain*=0.5;}
    return result;
}
struct FlowScene {float close,mist,wash,corridor,gold,green,motif;};
FlowScene flowScene() {
    // Long passages blend continuously; this is an authored schedule rather
    // than a claim that every original session uses one exact scene period.
    float t=mod(pc.flow.y,740.0);
    FlowScene s;
    s.close=flowWindow(t,148.0,383.0,14.0);
    s.mist=flowWindow(t,370.0,740.0,16.0);
    s.wash=max(max(flowWindow(t,48.0,74.0,10.0),flowWindow(t,143.0,164.0,9.0)),flowWindow(t,723.0,740.0,7.0));
    s.corridor=max(flowWindow(t,9.0,43.0,9.0),flowWindow(t,67.0,122.0,10.0));
    s.gold=flowWindow(t,112.0,144.0,10.0);
    s.green=1.0-smoothstep(7.0,18.0,t)+flowWindow(t,132.0,154.0,7.0)+smoothstep(730.0,740.0,t);
    s.motif=0.5+0.5*sin((pc.flow.y-380.0)*0.0785398163);
    return s;
}
float flowWarp(vec2 p,float seconds) {
    return p.x+sin(p.y*0.21+sin(p.x*0.13)*1.3)*1.7+
        (flowFbm(p*0.11+vec2(0,seconds*0.007))-0.4)*2.2;
}
float flowBaseHeight(vec2 p,float seconds) {
    FlowScene s=flowScene();
    float warp=flowWarp(p,seconds);
    float broad=sin(p.y*0.54+sin(warp*0.44)*1.25+flowNoise(p*0.15)*1.8);
    float folds=pow(0.5+0.5*broad,3.0)*0.34;
    float rolling=(flowFbm(p*0.23)-0.36)*1.7+folds;
    float trough=(sqrt(warp*warp+0.7)-0.84)*0.20;
    float corridor=trough+broad*(0.38+min(abs(warp),12.0)*0.07)+sin(p.y*1.13+warp*0.32)*0.14;
    float close=sin(warp*0.38+p.y*0.28)*0.8+sin(warp*0.17-p.y*0.49)*0.55+folds*0.5;
    float ribbons=sin(warp*0.37+p.y*0.19)*0.36+
        pow(0.5+0.5*sin(p.y*0.48+sin(warp*0.26)*1.2),3.0)*0.32;
    float h=mix(rolling,corridor,s.corridor);
    h=mix(h,close,s.close);h=mix(h,ribbons,s.mist);
    return h;
}
float flowHeight(vec2 p,float seconds) {
    // Audio changes folds and emission, never the sign of camera travel.
    float anchor=flowBaseHeight(pc.flow.zw,seconds);
    float height=flowBaseHeight(p,seconds);
    return anchor+(height-anchor)*(1.0+pc.audio.x*0.65+pc.audio.w*0.35)+
        sin(p.x*1.6+p.y*0.8)*pc.audio.y*0.045;
}
vec3 flowSky(vec2 uv) {
    FlowScene s=flowScene();
    vec3 sky=mix(vec3(0.43,0.49,0.51),vec3(0.055,0.29,0.27),s.green);
    sky=mix(sky,vec3(0.001,0.007,0.018),s.corridor);
    float closeChroma=0.5-0.5*cos((pc.flow.y-180.0)*0.0785398163);
    vec3 close=mix(vec3(0.005,0.012,0.15),vec3(0.46,0.27,0.73),closeChroma);
    sky=mix(sky,close,s.close);
    vec3 mist=mix(vec3(0.29,0.22,0.58),vec3(0.38,0.27,0.67),uv.y);
    vec2 light=vec2(0.5+0.36*sin(pc.flow.y*0.039),0.29+0.13*cos(pc.flow.y*0.021));
    float halo=exp(-dot((uv-light)*vec2(1.25,2.8),(uv-light)*vec2(1.25,2.8))*4.0);
    mist+=mix(vec3(0.12,0.16,0.21),vec3(0.37,0.29,0.04),s.motif)*halo;
    sky=mix(sky,mist,s.mist);
    return mix(sky,vec3(0.26,0.51,0.49),s.wash);
}
float flowCameraHeight(float seconds) {
    FlowScene s=flowScene();
    return flowBaseHeight(pc.flow.zw,seconds)+mix(0.24,0.16,s.close)+s.mist*0.04;
}
float flowHorizon() {
    FlowScene s=flowScene();
    return mix(mix(0.02,-0.95,s.close),-0.27,s.mist)+0.035*sin(pc.flow.y*0.019);
}

// Soft overlapping sheets let the camera move into an atmospheric fold rather
// than leaving every passage constrained to a distant terrain horizon.
vec3 flowVeil(vec2 uv) {
    FlowScene s=flowScene();
    float t=pc.flow.y;
    float motif=(t-380.0)*0.0785398163;
    float roll=0.65*sin(motif-0.8)+0.28*sin(motif*0.5);
    vec2 p=(uv-0.5)*vec2(pc.resolution_time_gain.x/pc.resolution_time_gain.y,1.0);
    p=mat2(cos(roll),-sin(roll),sin(roll),cos(roll))*p;
    float depth=1.0+0.22*sin(motif*0.43);
    p*=depth;
    p.x+=0.08*sin(pc.flow.x*0.055);
    float warp=flowFbm(p*2.2+vec2(0,pc.flow.x*0.017));
    float curve=(0.16*sin(p.x*3.1+motif*0.64)+0.13*sin(p.x*5.3-motif*0.39)+warp*0.23)*
        (1.0+pc.audio.x*0.65+pc.audio.w*0.35);
    vec3 mist=flowSky(uv);
    float closeChroma=0.5-0.5*cos((t-180.0)*0.0785398163);
    vec3 close=mix(vec3(0.001,0.003,0.035),vec3(0.30,0.12,0.55),closeChroma);
    vec3 rgb=mix(mist,close,s.close);
    for(int i=0;i<7;++i) {
        float id=float(i);
        float y=-0.51+id*0.16+0.05*sin(id*2.17+motif);
        float bend=curve*(0.58+id*0.085)+(0.025+pc.audio.y*0.045)*sin(p.x*(4.0+id*0.37)+id*1.9-motif*0.65);
        float distance=p.y-y-bend;
        float width=0.025+0.014*flowHash(vec2(id,7.0));
        float ridge=exp(-distance*distance/(width*width));
        float spill=exp(-distance*distance/(width*width*10.0));
        float shadow=exp(-(distance-width*2.4)*(distance-width*2.4)/(width*width*8.0));
        float selective=0.35+0.65*pow(0.5+0.5*sin(p.x*3.0+id*1.7+motif*0.5),2.0);
        vec3 light=mix(vec3(0.81,0.64,0.94),vec3(0.94,0.38,0.87),0.5+0.5*sin(motif+id*0.4));
        light=mix(light,mix(vec3(0.015,0.16,1.0),vec3(0.66,0.36,0.96),closeChroma),s.close);
        rgb*=1.0-shadow*(0.11+s.close*0.16);
        rgb+=light*(ridge*0.24+spill*0.14)*selective*(0.82+pc.audio.z*0.26+pc.audio.w*0.18);
    }
    float opening=flowNoise(p*2.8+vec2(pc.flow.x*0.018,0));
    rgb*=1.0-s.close*(1.0-smoothstep(0.25,0.52,opening))*0.95;
    float fan=pow(0.5+0.5*cos(motif+0.7),4.0)*s.mist;
    vec2 focus=vec2(0.40+0.12*sin(motif*0.5),0.04);
    float viewDepth=1.0/(0.16+uv.y*0.84);
    vec2 world=vec2((uv.x-focus.x)*viewDepth*2.6+pc.flow.z,viewDepth+pc.flow.w);
    vec3 fingers=flowSky(uv)*0.83;
    for(int i=0;i<6;++i) {
        float id=float(i);
        float bend=sin(world.y*0.52+id*0.67)*0.13+
            flowNoise(world*0.24+vec2(id,0))*0.26;
        float d=world.x-(-1.12+id*0.44)-bend;
        d/=1.0+pc.audio.x*0.35+pc.audio.w*0.18;
        float core=exp(-d*d/0.0035),halo=exp(-d*d/0.045);
        float shade=exp(-(d-0.16)*(d-0.16)/0.045);
        fingers*=1.0-shade*0.19;
        fingers+=mix(vec3(0.77,0.59,0.89),vec3(0.91,0.83,0.96),0.5+0.5*sin(id+motif))*
            (core*0.40+halo*0.22)*(0.7+pc.audio.z*0.3);
    }
    rgb=mix(rgb,fingers,fan);
    return rgb;
}
