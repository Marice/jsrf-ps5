#version 450
layout(location = 0) in vec4 colour;
layout(location = 0) out vec4 outColour;
layout(constant_id = 1) const float TINT = 1.0;
void main() { outColour = vec4(colour.rgb * TINT, 1.0); }
