#version 450

layout(set = 0, binding = 0) uniform sampler2D video_frame;
layout(push_constant) uniform Cover { vec2 crop; } cover;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    color = vec4(texture(video_frame, (uv - 0.5) * cover.crop + 0.5).rgb, 1.0);
}
