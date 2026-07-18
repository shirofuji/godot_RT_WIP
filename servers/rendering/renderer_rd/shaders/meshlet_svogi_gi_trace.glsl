#[compute]

#version 450

#VERSION_DEFINES

// Standalone HALF-RESOLUTION SVOGI indirect-diffuse trace (Phase 1 of the GI denoiser). One thread per
// half-res pixel: pick the covering full-res visbuffer pixel, reconstruct its world position + normal
// via the shared resolve_visbuffer_geometry(), cone-trace the octree, and write the raw (un-filtered)
// GI radiance to gi_color, plus this pixel's normal + reverse-Z depth to gi_normal / gi_depth so the
// fragment resolve can bilaterally upsample the (later: temporally + spatially filtered) GI back to
// full res. No shading here - MESHLET_GEOMETRY_ONLY keeps meshlet_shade() (and its material/light
// buffers) out of this pass entirely.

#ifndef MESHLET_VISBUFFER_FALLBACK
#extension GL_EXT_shader_atomic_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// SVOGINode struct + M_PI + MESHLET_TEXTURE_NONE.
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

layout(set = 0, binding = 11, std430) restrict readonly buffer SVOGINodes {
	SVOGINode data[];
}
svogi_nodes;

// Half-res outputs. gi_color = raw traced radiance (a = coverage: 1 covered, 0 miss). gi_normal =
// world normal (a unused). gi_depth = winning reverse-Z NDC depth - same representation the fragment
// resolve computes, so the bilateral-upsample depth compare is apples-to-apples.
layout(set = 0, binding = 12, rgba16f) uniform restrict writeonly image2D gi_color;
layout(set = 0, binding = 13, rgba16f) uniform restrict writeonly image2D gi_normal;
layout(set = 0, binding = 14, r32f) uniform restrict writeonly image2D gi_depth;

layout(push_constant, std430) uniform Params {
	mat4 view_projection; // Camera-relative (projection * rotation-only inverse camera) - matches the rasterizers.
	vec3 camera_position;
	uint full_width;
	vec4 svogi_bounds; // xyz = octree center, w = half-size (0 = SVOGI off).
	vec4 svogi_params; // x = energy.
	uint full_height;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

// oct_decode_normal + fetch_triangle_local_vertex (reads meshlet_triangles above).
#include "meshlet_geometry_inc.glsl"
// edge / pc_bary / resolve_visbuffer_geometry only (MESHLET_GEOMETRY_ONLY skips the shading half).
#define MESHLET_GEOMETRY_ONLY
#include "meshlet_visbuffer_resolve_inc.glsl"
// svogi_cone_trace / svogi_hemisphere_gather (references svogi_nodes declared above).
#include "meshlet_svogi_inc.glsl"

void main() {
	ivec2 half_dims = imageSize(gi_color);
	ivec2 hp = ivec2(gl_GlobalInvocationID.xy);
	if (hp.x >= half_dims.x || hp.y >= half_dims.y) {
		return;
	}

	ivec2 full_dims = ivec2(int(params.full_width), int(params.full_height));
	// Sample the top-left full-res pixel of this half-res block. (Phase 2/3 can add a rotated-grid
	// pick or per-half-pixel jitter; centroid-of-block is fine for a first cut.)
	ivec2 fp = min(hp * 2, full_dims - ivec2(1));

	vec3 world_pos;
	vec3 world_normal;
	vec2 tex_uv;
	uint material_id;
	float depth;
	vec3 dpdx, dpdy;
	vec2 duvdx, duvdy;
	uint slot;
	if (!resolve_visbuffer_geometry(fp, full_dims, world_pos, world_normal, tex_uv, material_id, depth, dpdx, dpdy, duvdx, duvdy, slot)) {
		// Uncovered / degenerate: write an invalid sample the upsample can reject (depth 0 = far in
		// reverse-Z, coverage 0).
		imageStore(gi_color, hp, vec4(0.0));
		imageStore(gi_normal, hp, vec4(0.0));
		imageStore(gi_depth, hp, vec4(0.0));
		return;
	}

	vec3 gi = vec3(0.0);
	if (params.svogi_bounds.w > 0.0) {
		float voxel_size = params.svogi_bounds.w / 32.0; // root half-size / 32 ~= leaf diameter.
		gi = svogi_hemisphere_gather(world_pos, world_normal, voxel_size * 3.0, params.svogi_bounds.w * 2.0, params.svogi_bounds.xyz, params.svogi_bounds.w, params.svogi_params.x);
	}

	imageStore(gi_color, hp, vec4(gi, 1.0));
	imageStore(gi_normal, hp, vec4(world_normal, 1.0));
	imageStore(gi_depth, hp, vec4(depth, 0.0, 0.0, 0.0));
}
