// Fullscreen-ish quad. Position and texture coordinates come straight from
// the attribute buffer, already in clip space, so the letterbox rectangle is
// computed on the CPU once per frame rather than in the shader.
#version 420 core

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;

out vec2 v_uv;

void main()
{
    v_uv = a_uv;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
