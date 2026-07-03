#[vertex]

#version 450

#VERSION_DEFINES

// Fullscreen triangle (no vertex buffer): gl_VertexIndex 0,1,2 -> a triangle covering the viewport.
void main() {
	vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

// Fragment variant of the visibility-buffer resolve (P5): writes the shaded color to the framebuffer's
// color attachment AND the fragment's reverse-Z depth to gl_FragDepth, so the meshlet geometry ends up
// in the real depth buffer and sky/transparents composite correctly - which the compute+imageStore
// resolve (meshlet_visbuffer_resolve.glsl) can't do. Same per-pixel math (shared resolve include).

#ifndef MESHLET_VISBUFFER_FALLBACK
#extension GL_EXT_shader_atomic_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif

layout(location = 0) out vec4 frag_color;

#include "meshlet_shade_types_inc.glsl"

#ifndef MESHLET_VISBUFFER_FALLBACK
layout(set = 0, binding = 0, std430) restrict readonly buffer VisBuffer {
	uint64_t data[];
}
visbuffer;
#else
layout(set = 0, binding = 0, std430) restrict readonly buffer VisDepth {
	uint data[];
}
vis_depth;
layout(set = 0, binding = 1, std430) restrict readonly buffer VisPayload {
	uint data[];
}
vis_payload;
#endif

struct VisibleMeshlet {
	uint instance_index;
	uint meshlet_index;
};

layout(set = 0, binding = 2, std430) restrict readonly buffer SwVisible {
	uint count;
	VisibleMeshlet data[];
}
sw_visible;

layout(set = 0, binding = 3, std430) restrict readonly buffer HwVisible {
	uint count;
	VisibleMeshlet data[];
}
hw_visible;

layout(set = 0, binding = 4, std430) restrict readonly buffer Transforms {
	mat4 data[];
}
transforms;

struct MeshletDescriptor {
	vec3 bounds_center;
	float bounds_radius;
	vec3 cone_axis;
	float cone_cutoff;
	uint vertex_remap_offset;
	uint triangle_offset;
	uint vertex_count;
	uint triangle_count;
};

layout(set = 0, binding = 5, std430) restrict readonly buffer MeshletDescriptors {
	MeshletDescriptor data[];
}
meshlet_descriptors;

layout(set = 0, binding = 6, std430) restrict readonly buffer MeshletVertexRemap {
	uint data[];
}
meshlet_vertex_remap;

layout(set = 0, binding = 7, std430) restrict readonly buffer MeshletTriangles {
	uint data[];
}
meshlet_triangles;

layout(set = 0, binding = 8, std430) restrict readonly buffer VertexPositions {
	vec4 data[];
}
vertex_positions;

layout(set = 0, binding = 9, std430) restrict readonly buffer VertexAttributes {
	vec4 data[];
}
vertex_attributes;

layout(set = 0, binding = 10, std430) restrict readonly buffer InstanceMaterialIds {
	uint data[];
}
instance_material_ids;

layout(set = 0, binding = 11, std430) restrict readonly buffer MeshletMaterials {
	MeshletMaterial data[];
}
meshlet_materials;

layout(set = 0, binding = 12, std430) restrict readonly buffer Lights {
	MeshletLight data[];
}
lights;

layout(set = 0, binding = 13, std430) restrict readonly buffer SVOGINodes {
	SVOGINode data[];
}
svogi_nodes;

layout(set = 0, binding = 14) uniform texture2DArray radiance_octmap;
layout(set = 0, binding = 15) uniform sampler radiance_sampler;
layout(set = 0, binding = 16) uniform texture2D material_textures[256];
layout(set = 0, binding = 17) uniform sampler material_sampler;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec3 camera_position;
	uint light_count;
	vec4 ambient_color;
	vec4 svogi_bounds;
	vec4 svogi_params; // .w packs the viewport dims: (width << 16) | height, reinterpreted as float.
}
params;

#include "meshlet_geometry_inc.glsl"
#include "meshlet_shade_inc.glsl"
#include "meshlet_visbuffer_resolve_inc.glsl"

void main() {
	uint packed_dims = floatBitsToUint(params.svogi_params.w);
	ivec2 dims = ivec2(int(packed_dims >> 16), int(packed_dims & 0xFFFFu));
	ivec2 pix = ivec2(gl_FragCoord.xy);
	vec3 color;
	float depth;
	if (!resolve_visbuffer_pixel(pix, dims, color, depth)) {
		discard; // No fragment / alpha-scissor: keep whatever is already in the framebuffer here.
	}
	frag_color = vec4(color, 1.0);
	gl_FragDepth = depth; // Reverse-Z NDC depth of the winning surface.
}
