#[vertex]

#version 450

#VERSION_DEFINES

// Hardware path that writes the SAME visibility buffer as the compute software rasterizer (P3): the
// large clusters that P1's classifier kept on hardware are drawn with vertex-pulling, and a
// side-effect fragment shader atomicMax-es the packed (depth, slot, triangle) into the visbuffer -
// exactly what meshlet_software_rasterize.glsl does per pixel, but via the hardware rasterizer (which
// is efficient for large triangles). Depth test is off; the atomicMax IS the depth test.

layout(location = 0) out flat uint slot_out;
layout(location = 1) out flat uint tri_out;

struct VisibleMeshlet {
	uint instance_index;
	uint meshlet_index;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer VisibleMeshlets {
	uint count;
	VisibleMeshlet data[];
}
visible_meshlets;

layout(set = 0, binding = 1, std430) restrict readonly buffer Transforms {
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

layout(set = 0, binding = 2, std430) restrict readonly buffer MeshletDescriptors {
	MeshletDescriptor data[];
}
meshlet_descriptors;

layout(set = 0, binding = 3, std430) restrict readonly buffer MeshletVertexRemap {
	uint data[];
}
meshlet_vertex_remap;

layout(set = 0, binding = 4, std430) restrict readonly buffer MeshletTriangles {
	uint data[];
}
meshlet_triangles;

layout(set = 0, binding = 5, std430) restrict readonly buffer VertexPositions {
	vec4 data[];
}
vertex_positions;

layout(push_constant, std430) uniform Params {
	mat4 view_projection; // Absolute (projection * inverse-camera), matching the compute rasterizer.
	uint viewport_width;
	uint viewport_height;
	uint pad0;
	uint pad1;
}
params;

// fetch_triangle_local_vertex (reads meshlet_triangles above).
#include "meshlet_geometry_inc.glsl"

void main() {
	// instanceCount=1 per draw (MeshletCuller::emit_indirect_draws), so gl_InstanceIndex is this
	// meshlet's slot in the visible list.
	uint slot = uint(gl_InstanceIndex);
	VisibleMeshlet item = visible_meshlets.data[slot];
	mat4 model = transforms.data[item.instance_index];
	MeshletDescriptor d = meshlet_descriptors.data[item.meshlet_index];

	uint local_tri = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) % 3u;
	uint local_v = fetch_triangle_local_vertex(d.triangle_offset + local_tri * 3u + corner);
	uint global_v = meshlet_vertex_remap.data[d.vertex_remap_offset + local_v];

	gl_Position = params.view_projection * model * vec4(vertex_positions.data[global_v].xyz, 1.0);
	slot_out = slot;
	tri_out = local_tri;
}

#[fragment]

#version 450

#VERSION_DEFINES

#ifndef MESHLET_VISBUFFER_FALLBACK
#extension GL_EXT_shader_atomic_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif

layout(location = 0) in flat uint slot_in;
layout(location = 1) in flat uint tri_in;

#ifndef MESHLET_VISBUFFER_FALLBACK
layout(set = 0, binding = 6, std430) restrict buffer VisBuffer {
	uint64_t data[];
}
visbuffer;
#else
layout(set = 0, binding = 6, std430) restrict buffer VisDepth {
	uint data[];
}
vis_depth;
layout(set = 0, binding = 7, std430) restrict buffer VisPayload {
	uint data[];
}
vis_payload;
#endif

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	uint viewport_width;
	uint viewport_height;
	uint pad0;
	uint pad1;
}
params;

void main() {
	// gl_FragCoord.z is the same [0,1] reverse-Z NDC depth the compute rasterizer computes (both use
	// the same absolute view_projection), so the packed values are directly comparable in the shared
	// visbuffer. Payload matches meshlet_software_rasterize.glsl: 25-bit slot, 7-bit triangle.
	ivec2 p = ivec2(gl_FragCoord.xy);
	int pixel_index = p.y * int(params.viewport_width) + p.x;
	uint z_bits = floatBitsToUint(gl_FragCoord.z);
	// Payload: bit 31 = source list (0 = hardware), bits 30..7 = 24-bit slot, bits 6..0 = triangle.
	uint payload = ((slot_in & 0xFFFFFFu) << 7) | (tri_in & 0x7Fu);
#ifndef MESHLET_VISBUFFER_FALLBACK
	uint64_t packed = (uint64_t(z_bits) << 32) | uint64_t(payload);
	atomicMax(visbuffer.data[pixel_index], packed);
#else
	uint prev = atomicMax(vis_depth.data[pixel_index], z_bits);
	if (z_bits >= prev && vis_depth.data[pixel_index] == z_bits) {
		vis_payload.data[pixel_index] = payload;
	}
#endif
}
