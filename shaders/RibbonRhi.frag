#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in float vHeight;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform UniformBlock {
  float time;
  float speed;
  float amplitude;
  float frequency;
  vec4  baseColor;
  vec4  waveColor;
  float threshold;
  float dustIntensity;
  float minDist;
  float maxDist;
  int   maxDraws;
  vec2  resolution;
  float brightness;
  float pad;
} ub;

// tiny float-only hash / value noise
float hash12(vec2 p){ float h=dot(p,vec2(127.1,311.7)); return fract(sin(h)*43758.5453123); }
float value2d(vec2 p){
  vec2 pg=floor(p), pc=p-pg, k=vec2(0.0,1.0);
  pc*=pc*pc*(3.0-2.0*pc);
  return mix(mix(hash12(pg+k.xx),hash12(pg+k.yx),pc.x),
             mix(hash12(pg+k.xy),hash12(pg+k.yy),pc.x),pc.y);
}

void main() {
  // Vertical gradient between your theme colours
  vec3 c = mix(ub.baseColor.rgb, ub.waveColor.rgb, vUV.y);

  // Crest highlight from screen-space curvature (based on displaced height)
  float d = abs(dFdx(vHeight)) + abs(dFdy(vHeight));
  float crest = clamp(d * 24.0, 0.0, 1.0); // tune 16..32 to taste
  c = mix(c, vec3(1.0), crest * 0.35);

  // A touch of dust twinkle – optional and cheap
  if (ub.dustIntensity > 0.0) {
    float tw = step(0.995, value2d(vUV * vec2(600.0, 300.0) + ub.time * 0.30));
    c += tw * 0.08 * ub.dustIntensity;
  }

  // Keep brightness floor so nights don’t go pitch black
  float b = pow(clamp(ub.brightness, 0.12, 1.5), 0.85);
  fragColor = vec4(c * b, 1.0);
}
