#[compute]

#version 450

#VERSION_DEFINES

// Material-resolve pass (P4). One thread per screen pixel: read the visibility buffer, unpack the
// winning (source-list, slot, triangle), refetch that triangle's three vertices, recompute
// perspective-correct barycentrics + analytic screen-space gradients (compute has no dFdx), interpolate
// world position / normal / UV, and shade via the shared meshlet_shade() (the exact same lighting the
// hardware color fragment uses). Writes the lit color to an output image; pixels with no fragment
// (visbuffer == 0) are left untouched so the rest of the scene shows through.
//
// Two variants (selected by the caller from RD::SUPPORTS_BUFFER_ATOMIC_INT64), matching the raster:
//   - default: one uint64 visbuffer.
//   - MESHLET_VISBUFFER_FALLBACK: separate depth + payload buffers.

#ifndef MESHLET_VISBUFFER_FALLBACK
#extension GL_EXT_shader_atomic_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Structs (MeshletMaterial/MeshletLight/SVOGINode) + M_PI, MESHLET_TEXTURE_NONE. Include before the
// buffers that use these types.
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
	vec4 data[]; // xy = octahedral normal, zw = uv.
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

layout(set = 0, binding = 18, rgba32f) uniform restrict writeonly image2D out_color;

layout(push_constant, std430) uniform Params {
	mat4 view_projection; // Absolute (projection * inverse-camera) - matches the rasterizers.
	vec3 camera_position;
	uint light_count;
	vec4 ambient_color;
	vec4 svogi_bounds;
	vec4 svogi_params;
}
params;

// oct_decode_normal + fetch_triangle_local_vertex (reads meshlet_triangles above).
#include "meshlet_geometry_inc.glsl"
// BRDF/light/SVOGI/ambient + meshlet_shade() (references meshlet_materials/lights/svogi_nodes/radiance*
// /material* declared above).
#include "meshlet_shade_inc.glsl"

// Shared per-pixel resolve (edge/pc_bary/resolve_visbuffer_pixel).
#include "meshlet_visbuffer_resolve_inc.glsl"

void main() {
	ivec2 dims = imageSize(out_color);
	ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
	if (pix.x >= dims.x || pix.y >= dims.y) {
		return;
	}
	vec3 color;
	float depth;
	if (resolve_visbuffer_pixel(pix, dims, color, depth)) {
		imageStore(out_color, pix, vec4(color, 1.0));
	}
	// Uncovered / discarded pixels are left untouched (background shows through).
}
