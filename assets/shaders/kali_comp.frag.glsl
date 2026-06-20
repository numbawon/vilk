#version 450

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D pp;
layout(set = 0, binding = 1, std140) uniform PresetBlock {
    vec4 rand_frame, rand_preset;
    vec4 _c0, _c1, _c2, _c3, _c4, _c5, _c6, _c7;
} ubo;

void main() {
    const float PI  = 3.14159265;
    const float SEG = 2.0 * PI / 7.0;  // one sector of the 7-fold symmetry

    float t    = ubo._c2.x;
    float bass = ubo._c3.x;
    float mid  = ubo._c3.y;

    // Aspect-ratio correction: compute polar coords in pixel space, not UV space.
    // Without this, stretching the window squishes the kaleidoscope circle into an ellipse.
    float aspect = ubo._c7.x / ubo._c7.y;   // viewport_w / viewport_h
    vec2  c = v_uv - 0.5;
    c.x *= aspect;

    float rad = length(c);
    float ang = atan(c.y, c.x);   // [-π, π]

    // 7-fold mirror kaleidoscope: fold screen angle into a single sector.
    float a2 = mod(ang + 6.28318 + t * 0.045, SEG);   // → [0, SEG)
    if (a2 > SEG * 0.5) a2 = SEG - a2;                 // mirror → [0, SEG/2]

    // Remap the folded half-sector to [0, π/2] so the composite samples
    // a full quadrant of the PP (not just a narrow 25° arc).
    float a_pp = a2 / (SEG * 0.5) * (PI * 0.5);

    // Radial: sqrt-compress like Kali Mix; audio pulse at centre.
    float rad_new = sqrt(rad) * (1.0 + bass * 0.08);

    // Sample PP through the kaleidoscope UV.
    vec2 uv2 = vec2(0.5) + rad_new * 0.5 * vec2(cos(a_pp), sin(a_pp));
    vec3 ret = texture(pp, uv2).rgb;

    // Kali Mix contrast boost.
    ret = -0.3 + 1.7 * ret;

    // Audio: mid-reactive centre glow.
    float glow = max(0.0, 1.0 - rad * 5.0) * mid * 0.4;
    ret += vec3(glow * 0.4, glow, glow * 0.8);

    out_color = vec4(clamp(ret, 0.0, 1.0), 1.0);
}
