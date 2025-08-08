#version 450

layout(location = 0) in vec2 inPos;   // NDC x in [-1,1], dummy y (ignored)
layout(location = 1) in vec2 inUV;    // uv in [0,1]^2

layout(location = 0) out vec2 vUV;
layout(location = 1) out float vHeight; // displaced y for crest detection

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

// Map the ribbon vertically into a comfortable band on screen.
// You can tweak these to taste (PS3 vibe sits a bit below centre).
const float bandMin = -0.25;
const float bandMax =  0.30;

void main() {
  vUV = inUV;

  float t = ub.time * ub.speed;

  // Layered sines (broad + mid + fine) – stable, PS3-ish motion.
  float w1 = sin(inPos.x * 6.0  + t * 0.80);
  float w2 = sin(inPos.x * 2.6  + t * 0.35 + inUV.y * 0.8);
  float w3 = sin(inPos.x * 12.0 + t * 1.40);

  // frequency scales the composite “wavelength”; amplitude the height.
  float disp = (0.55*w1 + 0.30*w2 + 0.15*w3);
  disp *= ub.amplitude;
  disp *= clamp(ub.frequency * 0.1, 0.3, 3.0); // tame over/under-frequency

  // Base vertical placement per-row, then add displacement
  float y = mix(bandMin, bandMax, inUV.y) + disp;

  vHeight = y;
  gl_Position = vec4(inPos.x, y, 0.0, 1.0);
}
