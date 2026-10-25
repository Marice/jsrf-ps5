#version 450
// JSRF-like vertex work: a position, per-draw constants (8 vec4), from push constants or a uniform buffer.
layout(location = 0) in vec2 pos;
layout(push_constant) uniform Push { vec4 c[8]; } pc;
layout(set = 0, binding = 0) uniform Block { vec4 c[8]; } ub;
layout(constant_id = 0) const int FROM_UBO = 0;
layout(location = 0) out vec4 colour;
void main() {
    vec4 k0 = FROM_UBO != 0 ? ub.c[0] : pc.c[0];
    vec4 k1 = FROM_UBO != 0 ? ub.c[1] : pc.c[1];
    vec4 k7 = FROM_UBO != 0 ? ub.c[7] : pc.c[7];
    gl_Position = vec4(pos * k0.xy + k0.zw, 0.0, 1.0);
    colour = k1 * 0.8 + k7 * 0.2;
}
