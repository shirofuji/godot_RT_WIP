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

// Signed area * 2 of triangle (a, b, c) - the edge function.
float edge(vec2 a, vec2 b, vec2 c) {
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Perspective-correct, normalized barycentric weights of screen point p in triangle (s0,s1,s2) with
// per-vertex 1/w (iw0..2). area = edge(s0,s1,s2) (nonzero, caller-checked).
vec3 pc_bary(vec2 s0, vec2 s1, vec2 s2, float iw0, float iw1, float iw2, vec2 p, float area) {
	float b0 = edge(s1, s2, p) / area;
	float b1 = edge(s2, s0, p) / area;
	float b2 = edge(s0, s1, p) / area;
	b0 *= iw0;
	b1 *= iw1;
	b2 *= iw2;
	float s = b0 + b1 + b2;
	if (abs(s) < 1e-20) {
		return vec3(b0, b1, b2);
	}
	return vec3(b0, b1, b2) / s;
}

void main() {
	ivec2 dims = imageSize(out_color);
	ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
	if (pix.x >= dims.x || pix.y >= dims.y) {
		return;
	}
	int idx = pix.y * dims.x + pix.x;

#ifndef MESHLET_VISBUFFER_FALLBACK
	uint64_t packed = visbuffer.data[idx];
	if (packed == 0ul) {
		return; // No fragment here - leave the output untouched.
	}
	uint payload = uint(packed & 0xFFFFFFFFul);
#else
	uint depth_bits = vis_depth.data[idx];
	if (depth_bits == 0u) {
		return;
	}
	uint payload = vis_payload.data[idx];
#endif

	uint is_sw = (payload >> 31) & 0x1u;
	uint slot = (payload >> 7) & 0xFFFFFFu;
	uint tri = payload & 0x7Fu;

	VisibleMeshlet vm = (is_sw == 1u) ? sw_visible.data[slot] : hw_visible.data[slot];
	MeshletDescriptor d = meshlet_descriptors.data[vm.meshlet_index];
	mat4 model = transforms.data[vm.instance_index];

	// Inverse-transpose (cofactor) of the model matrix for normals (see meshlet_render.glsl).
	mat3 m3 = mat3(model);
	vec3 cof0 = cross(m3[1], m3[2]);
	mat3 normal_matrix = mat3(cof0, cross(m3[2], m3[0]), cross(m3[0], m3[1]));
	float det_sign = sign(dot(m3[0], cof0));

	// Fetch the triangle's three vertices: world position, world normal, UV, and screen position.
	vec3 wpos[3];
	vec3 wnrm[3];
	vec2 uv[3];
	vec2 screen[3];
	float invw[3];
	uint base = d.triangle_offset + tri * 3u;
	for (uint c = 0u; c < 3u; c++) {
		uint local_v = fetch_triangle_local_vertex(base + c);
		uint global_v = meshlet_vertex_remap.data[d.vertex_remap_offset + local_v];
		vec4 lpos = vertex_positions.data[global_v];
		vec4 attrib = vertex_attributes.data[global_v];
		wpos[c] = (model * vec4(lpos.xyz, 1.0)).xyz;
		wnrm[c] = normalize(normal_matrix * oct_decode_normal(attrib.xy)) * det_sign;
		uv[c] = attrib.zw;
		vec4 clip = params.view_projection * vec4(wpos[c], 1.0);
		invw[c] = 1.0 / clip.w;
		vec3 ndc = clip.xyz * invw[c];
		screen[c] = (ndc.xy * 0.5 + 0.5) * vec2(dims);
	}

	float area = edge(screen[0], screen[1], screen[2]);
	if (abs(area) < 1e-9) {
		return; // Degenerate on screen.
	}

	vec2 p = vec2(pix) + 0.5;
	vec3 bc = pc_bary(screen[0], screen[1], screen[2], invw[0], invw[1], invw[2], p, area);
	vec3 world_pos = bc.x * wpos[0] + bc.y * wpos[1] + bc.z * wpos[2];
	vec3 world_normal = normalize(bc.x * wnrm[0] + bc.y * wnrm[1] + bc.z * wnrm[2]);
	vec2 tex_uv = bc.x * uv[0] + bc.y * uv[1] + bc.z * uv[2];

	// Analytic screen-space gradients: perspective-correct interpolate one pixel over in x and y.
	vec3 bcx = pc_bary(screen[0], screen[1], screen[2], invw[0], invw[1], invw[2], p + vec2(1.0, 0.0), area);
	vec3 bcy = pc_bary(screen[0], screen[1], screen[2], invw[0], invw[1], invw[2], p + vec2(0.0, 1.0), area);
	vec3 dpdx = (bcx.x * wpos[0] + bcx.y * wpos[1] + bcx.z * wpos[2]) - world_pos;
	vec3 dpdy = (bcy.x * wpos[0] + bcy.y * wpos[1] + bcy.z * wpos[2]) - world_pos;
	vec2 duvdx = (bcx.x * uv[0] + bcx.y * uv[1] + bcx.z * uv[2]) - tex_uv;
	vec2 duvdy = (bcy.x * uv[0] + bcy.y * uv[1] + bcy.z * uv[2]) - tex_uv;

	uint material_id = instance_material_ids.data[vm.instance_index] & 0x7FFFFFFFu; // Strip the owns-own-depth flag.

	bool do_discard = false;
	vec4 shaded = meshlet_shade(material_id, world_normal, world_pos, tex_uv, params.camera_position, params.ambient_color, params.svogi_bounds, params.svogi_params, params.light_count, dpdx, dpdy, duvdx, duvdy, do_discard);
	if (do_discard) {
		return; // Alpha-scissor cutout - leave the background.
	}
	imageStore(out_color, pix, vec4(shaded.rgb, 1.0));
}
