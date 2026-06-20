#pragma once

// Phase 2 test shaders -- fullscreen triangle + audio-reactive feedback loop.

namespace vilk::test_shaders {

inline constexpr const char* kFullscreenQuadVert = R"glsl(
#version 450
layout(location = 0) out vec2 v_uv;
void main() {
    v_uv        = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

inline constexpr const char* kPassthroughFrag = R"glsl(
#version 450
layout(location = 0) in  vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D u_prev_frame;
layout(set = 0, binding = 1, std140) uniform AudioData {
    float bass;
    float mid;
    float treble;
    float vol;
    float time_s;
} u_audio;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 prev = texture(u_prev_frame, v_uv);

    // Feedback: decay toward black each frame
    vec3 color = prev.rgb * (0.94 + u_audio.vol * 0.04);

    // Radial ripple driven by bass
    vec2  center = v_uv - 0.5;
    float dist   = length(center);
    float ripple = sin(dist * 24.0 - u_audio.time_s * 4.0) * 0.5 + 0.5;
    float pulse  = u_audio.bass * ripple;

    // RGB bands: bass=red, mid=green, treble=blue
    color += vec3(u_audio.bass, u_audio.mid, u_audio.treble) * pulse * 0.15;

    // Brightness boost from overall volume
    color += vec3(u_audio.vol * 0.04);

    out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
)glsl";

} // namespace vilk::test_shaders
