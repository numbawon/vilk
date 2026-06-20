#version 450

// Inputs from kWarpMeshVertGlsl (per-pixel-equation rotation already applied to .xy)
layout(location = 0) in vec4 frag_COLOR;
layout(location = 1) in vec4 frag_TEXCOORD0; // .xy = pre-rotated PP source UV
layout(location = 2) in vec2 frag_TEXCOORD1; // unused

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D pp;
layout(set = 0, binding = 1, std140) uniform PresetBlock {
    vec4 rand_frame, rand_preset;
    vec4 _c0, _c1, _c2, _c3, _c4, _c5, _c6, _c7;
} ubo;

void main() {
    vec2  uv    = frag_TEXCOORD0.xy;
    float t     = ubo._c2.x;
    float bass  = ubo._c3.x;
    float inv_w = ubo._c7.z;   // 1 / viewport_width

    // Spatially-varied neighbor direction: angle depends on position + time.
    // The original Kali Mix uses a fixed (+1px, +1px) diagonal neighbor which
    // creates a uniform drift direction → diagonal stripe attractor.
    // This varied direction breaks that symmetry → turbulent flow.
    float dir   = uv.x * 9.42 + uv.y * 7.54 + t * 0.71;
    vec2  delta = inv_w * vec2(cos(dir), sin(dir));

    // Painterly warp: displace sample UV based on neighbour vs. neutral point.
    vec3 blurry = texture(pp, uv + delta).rgb;
    vec2 disp   = (blurry.rg - 0.37) * 0.02;

    // Read PP at displaced UV, apply Kali Mix constant fade.
    vec3 ret = texture(pp, uv + disp).rgb;
    ret -= 0.004;

    // Subtle bass pulse without adding net directional bias.
    ret += vec3(0.003, 0.002, 0.001) * bass;

    out_color = vec4(max(ret, vec3(0.0)), 1.0);
}
