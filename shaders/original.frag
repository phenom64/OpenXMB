#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(push_constant) uniform PC {
    vec4 tint;        // base tint colour
    vec2 resolution;  // screen resolution
    float time;       // seconds
    float brightness; // 0..1
} pc;

// Dave Hoskins style hash
float hash12(vec2 p)
{
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673U, 3812015801U);
    uint n = (q.x ^ q.y) * 1597334673U;
    return float(n) * 2.328306437080797e-10; // / 2^32
}

float value2d(vec2 p)
{
    vec2 pg=floor(p),pc=p-pg,k=vec2(0,1);
    pc*=pc*pc*(3.-2.*pc);
    return mix(
        mix(hash12(pg+k.xx),hash12(pg+k.yx),pc.x),
        mix(hash12(pg+k.xy),hash12(pg+k.yy),pc.x),
        pc.y
    );
}

float s5(float x){return .5+.5*sin(x);} 
float c5(float x){return .5+.5*cos(x);} 

float get_stars_rough(vec2 p)
{
    float THRESHOLD = .99;
    float s = smoothstep(THRESHOLD,1.,hash12(p));
    if (s >= THRESHOLD)
        s = pow((s-THRESHOLD)/(1.-THRESHOLD), 10.);
    return s;
}

float get_stars(vec2 p, float a, float t)
{
    vec2 pg=floor(p), pc=p-pg, k=vec2(0,1);
    pc *= pc*pc*(3.-2.*pc);
    float s = mix(
        mix(get_stars_rough(pg+k.xx), get_stars_rough(pg+k.yx), pc.x),
        mix(get_stars_rough(pg+k.xy), get_stars_rough(pg+k.yy), pc.x),
        pc.y);
    return smoothstep(a,a+t, s)*pow(value2d(p*.1 + t)*.5+.5,8.3);
}

// Based on the Shadertoy ray-march but simplified
float sdf(vec3 p, float t)
{
    p*=2.0;
    float o = 4.2*sin(.05*p.x+t*.25)
            + (.04*p.z)
            + sin(p.x*.11+t)
            + 2.0*sin(p.z*.2+t)
            + value2d(vec2(.03,.4)*p.xz+vec2(t*.5,0));
    return abs(dot(p,normalize(vec3(0,1,0.05)))+2.5+o*.5);
}

vec3 norm(vec3 p, float t)
{
    const vec2 k=vec2(1,-1);
    const float e=.001;
    return normalize(
        k.xyy*sdf(p+e*k.xyy,t) + 
        k.yyx*sdf(p+e*k.yyx,t) + 
        k.yxy*sdf(p+e*k.yxy,t) + 
        k.xxx*sdf(p+e*k.xxx,t)
    );
}

vec2 raymarch(vec3 o, vec3 d, float t)
{
    const float MIN_DIST=.13; const float MAX_DIST=40.0; const int MAX_STEPS=100; 
    float stepLen=0.0, dt=0.0, a=0.0, g=MAX_DIST; bool hit=false; float omega=1.2;
    for(int i=0;i<MAX_STEPS;i++){
        vec3 p=o+d*stepLen;
        float ndt = sdf(p, t);
        float sl = (abs(dt)+abs(ndt) < stepLen) ? (stepLen -= omega*stepLen, omega=1.0, stepLen) : (ndt*omega);
        dt=ndt; stepLen+=sl;
        g = (stepLen > 10.0) ? min(g,abs(dt)) : g;
        if ((stepLen+=dt)>=MAX_DIST) break;
        if (dt<MIN_DIST){
            if(!hit){ a=.01; hit=true; }
            float ed=2.0*max(.03,abs(ndt));
            a += .0135; stepLen += ed; 
        }
    }
    g /= 3.0; 
    return vec2(a, max(1.0-g,0.0));
}

void main() {
    vec2 ires = pc.resolution; 
    vec2 fragCoord = vUV * ires; 
    vec2 U = fragCoord; vec2 uv = U/ires; 
    float t = pc.time;

    vec3 o = vec3(0); 
    vec3 d = vec3( (U - 0.5*ires)/ires.y, 1.0 ); 
    vec2 mg = raymarch(o,d,t);
    float m = mg.x;

    // 4-point gradient, tinted by base colour
    vec3 base = pc.tint.rgb; 
    vec3 c = mix( mix(base*vec3(.7,.2,.2), base*vec3(.4,.1,.1), uv.x),
                  mix(base*vec3(.45,.1,.1), base*vec3(.8,.3,.5), uv.x), uv.y);
    // Blend with white based on wave alpha
    c = mix(c, vec3(1.0), m);
    // Dust layer (very light)
    vec2 ar = vec2(ires.x/ires.y,1.0);
    vec2 pp = uv*2000.0*ar;
    float dust = get_stars(pp*0.1 + t*vec2(20.0,-10.1), 0.11, 0.71)*0.2;
    c += dust*0.3;

    c *= pc.brightness;
    FragColor = vec4(c, 1.0);
}
