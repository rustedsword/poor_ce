#version 450

layout(location = 0) in ivec2 position;
layout(location = 1) in uvec2 cell;
layout(location = 2) in vec4 color;

layout(push_constant) uniform constants {
	vec2 viewport;
	vec2 cell_size;
	vec2 atlas_size;
};

layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 tint;

void main()
{
	vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);

	gl_Position = vec4((vec2(position) + corner * cell_size) / viewport * 2.0 - 1.0, 0.0, 1.0);
	uv = (vec2(cell) + corner * cell_size) / atlas_size;
	tint = color;
}
