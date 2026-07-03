#[compute]

#version 450

#VERSION_DEFINES

// Visibility-buffer software rasterizer (P2b). One workgroup per software-visible meshlet
// (gl_WorkGroupID.x = its slot in the software worklist), one thread per triangle
// (gl_LocalInvocationID.x). Each thread scan-converts its triangle and, per covered pixel, packs
// reverse-Z depth + a (slot, triangle) payload and atomicMax-es it into the visbuffer so the nearest
// surface wins deterministically (no Z-fighting - unlike hardware equal-depth ties). The material
// resolve pass (P4) reads the visbuffer, unpacks (slot, tri), and shades.
//
// Two variants, selected by the caller from RD::SUPPORTS_BUFFER_ATOMIC_INT64:
//   - default: one uint64 visbuffer, single atomicMax of (depth<<32 | payload).
//   - MESHLET_VISBUFFER_FALLBACK: two uint32 buffers (depth + payload); atomicMax the depth, then
//     write payload iff we still hold the winning depth (benign race on exact ties).

#ifndef MESHLET_VISBUFFER_FALLBACK
#extension GL_EXT_shader_atomic_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

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

// Packed 4x uint8 triangle-local-vertex-indices per uint32 word. fetch_triangle_local_vertex()
// (meshlet_geometry_inc.glsl) reads this buffer by name, so declare it before the include.
layout(set = 0, binding = 4, std430) restrict readonly buffer MeshletTriangles {
	uint data[];
}
meshlet_triangles;

layout(set = 0, binding = 5, std430) restrict readonly buffer VertexPositions {
	vec4 data[];
}
vertex_positions;

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
	uint max_visible; // Cap for visible_meshlets.count (buffer capacity - the count is an atomic that can overrun it).
	uint pad;
}
params;

// oct_decode_normal (unused here) + fetch_triangle_local_vertex (reads meshlet_triangles above).
#include "meshlet_geometry_inc.glsl"

// Safety net against a large triangle slipping past the per-cluster classifier: skip scan-converting
// any triangle whose screen bounding box exceeds this many pixels (software raster is only a win for
// small triangles; a huge one here would be one thread looping over the whole box). The classifier is
// meant to keep clusters below this; a dropped large triangle is a rare, localized miss, not a stall.
const int MESHLET_VISBUFFER_MAX_BBOX_AREA = 4096; // e.g. 64x64.

void write_visbuffer(int pixel_index, float ndc_z, uint payload) {
	uint z_bits = floatBitsToUint(ndc_z); // Reverse-Z: near = 1.0 (large), far = 0.0. Monotonic in [0,1].
#ifndef MESHLET_VISBUFFER_FALLBACK
	uint64_t packed = (uint64_t(z_bits) << 32) | uint64_t(payload);
	atomicMax(visbuffer.data[pixel_index], packed);
#else
	uint prev = atomicMax(vis_depth.data[pixel_index], z_bits);
	// If our depth is now the winner (>= everything seen so far), claim the payload. A concurrent
	// thread at the exact same depth can still overwrite this - a benign per-pixel flicker on true
	// depth ties, acceptable for the fallback path.
	if (z_bits >= prev && vis_depth.data[pixel_index] == z_bits) {
		vis_payload.data[pixel_index] = payload;
	}
#endif
}

void main() {
	uint slot = gl_WorkGroupID.x;
	if (slot >= min(visible_meshlets.count, params.max_visible)) {
		return;
	}
	uint tri = gl_LocalInvocationID.x;

	uint instance_id = visible_meshlets.data[slot].instance_index;
	uint meshlet_id = visible_meshlets.data[slot].meshlet_index;
	MeshletDescriptor d = meshlet_descriptors.data[meshlet_id];
	if (tri >= d.triangle_count) {
		return;
	}

	mat4 model = transforms.data[instance_id];
	mat4 mvp = params.view_projection * model;

	// Fetch this triangle's three vertices (triangle-local index -> meshlet vertex remap -> global).
	uint base = d.triangle_offset + tri * 3u;
	vec4 clip[3];
	for (uint c = 0u; c < 3u; c++) {
		uint local_v = fetch_triangle_local_vertex(base + c);
		uint global_v = meshlet_vertex_remap.data[d.vertex_remap_offset + local_v];
		clip[c] = mvp * vec4(vertex_positions.data[global_v].xyz, 1.0);
	}

	// Reject if any vertex is behind the camera (w <= 0); a proper near-clip is future work - subpixel
	// clusters straddling the near plane are a rare edge for this path.
	if (clip[0].w <= 0.0 || clip[1].w <= 0.0 || clip[2].w <= 0.0) {
		return;
	}

	// Perspective divide -> NDC, then to screen (pixel) coordinates. Vulkan NDC xy in [-1,1].
	vec2 screen[3];
	float ndc_z[3];
	for (uint c = 0u; c < 3u; c++) {
		vec3 ndc = clip[c].xyz / clip[c].w;
		screen[c] = (ndc.xy * 0.5 + 0.5) * vec2(float(params.viewport_width), float(params.viewport_height));
		ndc_z[c] = ndc.z;
	}

	// No backface cull: atomicMax keeps the nearest surface deterministically, so rendering both
	// windings is Z-fight-free and correct for closed meshes (the front face is nearer and wins).
	// Matching the hardware path's CULL_BACK exactly is a P5 consistency concern, not needed here.
	vec2 lo = min(min(screen[0], screen[1]), screen[2]);
	vec2 hi = max(max(screen[0], screen[1]), screen[2]);
	int min_x = max(0, int(floor(lo.x)));
	int min_y = max(0, int(floor(lo.y)));
	int max_x = min(int(params.viewport_width) - 1, int(ceil(hi.x)));
	int max_y = min(int(params.viewport_height) - 1, int(ceil(hi.y)));
	if (min_x > max_x || min_y > max_y) {
		return; // Off-screen or degenerate.
	}
	if ((max_x - min_x + 1) * (max_y - min_y + 1) > MESHLET_VISBUFFER_MAX_BBOX_AREA) {
		return; // Safety net - see the cap's comment.
	}

	// Edge-function setup (screen space). area = signed area * 2; sign is winding-dependent, handled
	// by normalizing the barycentrics by `area` so both windings produce inside==true.
	vec2 e0 = screen[1] - screen[0];
	vec2 e1 = screen[2] - screen[1];
	vec2 e2 = screen[0] - screen[2];
	float area = e0.x * (screen[2].y - screen[0].y) - e0.y * (screen[2].x - screen[0].x);
	if (abs(area) < 1e-6) {
		return; // Degenerate/zero-area.
	}
	float inv_area = 1.0 / area;

	// Payload: bit 31 = source list (1 = software), bits 30..7 = 24-bit slot, bits 6..0 = triangle.
	// The resolve pass reads the software or hardware visible list per that bit to recover
	// (instance, meshlet) - both raster paths share one visbuffer, so the bit disambiguates.
	uint payload = (1u << 31) | ((slot & 0xFFFFFFu) << 7) | (tri & 0x7Fu);

	for (int y = min_y; y <= max_y; y++) {
		for (int x = min_x; x <= max_x; x++) {
			vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
			// Barycentric weights via edge functions, normalized by the signed area so the inside test
			// (all weights >= 0) works for either winding.
			float w0 = (e1.x * (p.y - screen[1].y) - e1.y * (p.x - screen[1].x)) * inv_area;
			float w1 = (e2.x * (p.y - screen[2].y) - e2.y * (p.x - screen[2].x)) * inv_area;
			float w2 = (e0.x * (p.y - screen[0].y) - e0.y * (p.x - screen[0].x)) * inv_area;
			if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) {
				continue;
			}
			float z = w0 * ndc_z[0] + w1 * ndc_z[1] + w2 * ndc_z[2];
			int pixel_index = y * int(params.viewport_width) + x;
			write_visbuffer(pixel_index, z, payload);
		}
	}
}
