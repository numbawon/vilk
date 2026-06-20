#version 450
layout(location = 0) in  vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D u_prev_frame;
layout(location = 0)     out vec4 out_color;
void main() {
    out_color = texture(u_prev_frame, v_uv);
}
