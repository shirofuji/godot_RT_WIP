#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec3 normal_interp;
layout(location = 1) out flat uint meshlet_index_interp;
layout(location = 2) out flat uint material_id_interp;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec3 light_direction;
	float pad0;
}
params;

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

// Packed 4 uint8 triangle-local-vertex-indices per uint32 word (matches MeshletStorage's
// meshlet_triangle_buffer, which is a plain byte buffer on the C++ side).
layout(set = 0, binding = 4, std430) restrict readonly buffer MeshletTriangles {
	uint data[];
}
meshlet_triangles;

layout(set = 0, binding = 5, std430) restrict readonly buffer VertexPositions {
	vec4 data[];
}
vertex_positions;

layout(set = 0, binding = 6, std430) restrict readonly buffer VertexAttributes {
	vec4 data[]; // xy = octahedral-encoded normal, zw = uv.
}
vertex_attributes;

// Per-instance material slot (see MeshletStorage::upload_material()) - resolved once per frame
// from each instance's real material at scan time (render_forward_clustered.cpp), not baked into
// MeshletDescriptor: materials are mutable after a mesh is uploaded (mesh_surface_set_material),
// and meshlets are shared across many instances that may each have a different material override.
layout(set = 0, binding = 7, std430) restrict readonly buffer InstanceMaterialIds {
	uint data[];
}
instance_material_ids;

vec3 oct_decode_normal(vec2 e) {
	vec3 v = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
	float t = clamp(-v.z, 0.0, 1.0);
	v.x += v.x >= 0.0 ? -t : t;
	v.y += v.y >= 0.0 ? -t : t;
	return normalize(v);
}

uint fetch_triangle_local_vertex(uint p_byte_index) {
	uint word = meshlet_triangles.data[p_byte_index / 4];
	return (word >> ((p_byte_index % 4) * 8)) & 0xFFu;
}

void main() {
	// instanceCount=1 per draw (see MeshletCuller::emit_indirect_draws), so gl_InstanceIndex
	// equals the draw's firstInstance exactly: its slot in the visible-meshlet list.
	VisibleMeshlet item = visible_meshlets.data[gl_InstanceIndex];
	mat4 transform = transforms.data[item.instance_index];
	MeshletDescriptor d = meshlet_descriptors.data[item.meshlet_index];

	// gl_VertexIndex comes from the shared synthetic index buffer (0, 1, 2, ...) - the indirect
	// command's index_count was set to exactly this meshlet's triangle_count * 3, so this is
	// always in range for this meshlet's own triangle list.
	uint local_triangle = gl_VertexIndex / 3;
	uint local_corner = gl_VertexIndex % 3;
	uint local_vertex_id = fetch_triangle_local_vertex(d.triangle_offset + local_triangle * 3 + local_corner);
	uint global_vertex_id = meshlet_vertex_remap.data[d.vertex_remap_offset + local_vertex_id];

	vec4 local_pos = vertex_positions.data[global_vertex_id];
	vec4 attrib = vertex_attributes.data[global_vertex_id];

	vec3 world_pos = (transform * vec4(local_pos.xyz, 1.0)).xyz;
	gl_Position = params.view_projection * vec4(world_pos, 1.0);

	vec3 world_normal = normalize(mat3(transform) * oct_decode_normal(attrib.xy));

	normal_interp = world_normal;
	meshlet_index_interp = item.meshlet_index;
	material_id_interp = instance_material_ids.data[item.instance_index];

	// Manual depth bias toward "nearer" (reversed-Z: larger device-Z = nearer - see
	// meshlet_occlusion_test.glsl's comment). This pipeline frequently draws on top of real depth
	// already written for the exact same surface by Forward+'s own depth pre-pass (intentional);
	// this vertex-pulling path and Forward+'s own real vertex shader compute device-Z for the
	// same logical point via two different paths, which produces a small but consistent (not
	// floating-point-noise-level) gap. Without this nudge, GREATER_OR_EQUAL loses that near-tie
	// far more often than it wins, leaving only edge/silhouette meshlets visible (confirmed live:
	// a single GPU-readback sample measured a ~0.0013 gap for one meshlet, but that undersold the
	// real range - 0.002 had no visible effect at all, while 0.005-0.05 reliably fixed full
	// coverage; 0.005 is the smallest value tested that worked, kept deliberately small since
	// this is a real tradeoff - too large a bias risks this object incorrectly rendering in front
	// of *other*, genuinely-nearer-by-a-smaller-margin geometry elsewhere in a real scene). RD's
	// own depth_bias_* pipeline state was tried first and found useless here: it scales by
	// Vulkan's per-fragment minimum-resolvable-difference (sized for sub-ULP anti-z-fighting
	// noise), nowhere near large enough for this systematic gap even at high constant-factor
	// values. Scaling by gl_Position.w keeps the nudge a fixed fraction of NDC-Z regardless of
	// distance, rather than a fixed absolute amount that would be too large up close and too
	// small far away.
	const float DEPTH_BIAS_NDC_FRACTION = 0.005;
	gl_Position.z = min(gl_Position.z + DEPTH_BIAS_NDC_FRACTION * gl_Position.w, gl_Position.w);
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(location = 0) in vec3 normal_interp;
layout(location = 1) in flat uint meshlet_index_interp;
layout(location = 2) in flat uint material_id_interp;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec3 light_direction;
	float pad0;
}
params;

#ifndef MESHLET_DEPTH_ONLY
layout(location = 0) out vec4 frag_color;

// Mirrors MeshletStorage::MeshletMaterialGPU exactly (std430 layout) - a flattened snapshot of
// the subset of StandardMaterial3D/ORMMaterial3D parameters this milestone (B1: unlit/emissive)
// reads; metallic/roughness/specular/texture indices are uploaded but not consumed yet (B2/B3).
struct MeshletMaterial {
	vec4 albedo;
	vec3 emission;
	float metallic;
	float roughness;
	float specular;
	uint albedo_texture_index;
	uint normal_texture_index;
	uint orm_texture_index;
	uint emission_texture_index;
	uint flags;
	float alpha_scissor_threshold;
	vec2 uv1_scale;
	vec2 uv1_offset;
};

layout(set = 0, binding = 8, std430) restrict readonly buffer MeshletMaterials {
	MeshletMaterial data[];
}
meshlet_materials;
#endif

void main() {
#ifndef MESHLET_DEPTH_ONLY
	MeshletMaterial mat = meshlet_materials.data[material_id_interp];
	// B1 (unlit/emissive milestone): flat albedo + emission, no lighting at all - this proves the
	// material-id threading end-to-end (per-instance resolution -> upload_material() dedup ->
	// this lookup, across however many distinct real materials are present in one indirect
	// multi-draw) before taking on per-vertex/per-fragment lighting (B2/B3).
	vec3 color = mat.albedo.rgb + mat.emission;
	frag_color = vec4(color, mat.albedo.a);
#endif
	// MESHLET_DEPTH_ONLY: no color output at all - this variant targets a depth-only
	// framebuffer (Forward+'s real depth pre-pass framebuffer, which has zero color
	// attachments) for the temporal early pass; depth write/test happens via fixed-function
	// state regardless of what (if anything) the fragment shader writes.
}
