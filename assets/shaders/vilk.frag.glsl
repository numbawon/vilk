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

    // Decay toward black -- keep < 1.0 so feedback doesn't saturate
    vec3 color = prev.rgb * 0.96;

    // Radial ripple pulsing with bass
    vec2  center = v_uv - 0.5;
    float dist   = length(center);
    float ripple = sin(dist * 24.0 - u_audio.time_s * 4.0) * 0.5 + 0.5;
    float pulse  = u_audio.bass * ripple;

    // RGB bands: bass = red, mid = green, treble = blue
    color += vec3(u_audio.bass, u_audio.mid, u_audio.treble) * pulse * 0.15;

    // Subtle overall brightness
    color += vec3(u_audio.vol * 0.02);

    out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
