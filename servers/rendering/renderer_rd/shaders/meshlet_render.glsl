#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec3 world_normal_interp;
layout(location = 1) out flat uint meshlet_index_interp;
layout(location = 2) out flat uint material_id_interp;
layout(location = 3) out vec3 world_pos_interp;
layout(location = 4) out flat vec3 instance_pos_interp;
layout(location = 5) out vec2 uv_interp;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec3 camera_position;
	uint light_count;
	vec4 ambient_color; // .rgb = flat ambient (color*energy, linear), .a = sky-radiance mix amount (0 = none)
	vec4 svogi_bounds; // .xyz = octree root center (absolute world), .w = root half-size (0 = SVOGI off)
	vec4 svogi_params; // .x = SVOGI energy, .y = sky-radiance exposure*energy scale, .z = MAX_ROUGHNESS_LOD layer, .w reserved
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

// oct_decode_normal() + fetch_triangle_local_vertex() (the latter reads meshlet_triangles, declared
// above). Shared with the visibility-buffer software raster / resolve passes.
#include "meshlet_geometry_inc.glsl"

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
	// Camera-relative projection. params.view_projection is the *camera-relative* view-projection
	// (projection * rotation-only inverse-camera, with no translation - see
	// MeshletRenderer::render()), so applying it to (world_pos - camera_position) produces the exact
	// same clip position as an absolute `view_projection * world_pos` would, but computed entirely
	// from small-magnitude operands. The absolute form multiplied a large world coordinate (~1e3+
	// for geometry far from the world origin) by a matrix whose z/w rows carry an equal-and-opposite
	// ~-1e3 camera translation; in float32 those terms cancel catastrophically down to the tiny
	// clip-space value, losing ~7-8 mantissa bits. That error is not fixed per object - it shifts as
	// the object (or camera) moves sub-unit each frame - so the resulting device-Z jitter would
	// intermittently lose the GREATER_OR_EQUAL depth tie against Forward+'s own depth pre-pass (which
	// is already precise: it transforms to view space *before* projecting), making moving/distant
	// meshlet geometry flicker and swap which face/meshlet wins per pixel. Subtracting the camera
	// position first keeps the projected coordinate near the origin, matching Forward+'s precision.
	// world_pos itself stays absolute below (world_pos_interp / SVOGI need true world space).
	gl_Position = params.view_projection * vec4(world_pos - params.camera_position, 1.0);

	// No local-Z flip here. History: this pipeline used to flip the local-Z axis of the decoded
	// normal to compensate for POLYGON_CULL_FRONT, on the theory that culling what Godot considers
	// "front-facing" leaves only the geometrically-far side of a closed surface visible. That
	// theory predicted (and a direct screenshot comparison against meshlet_selftest.cpp's synthetic
	// test sphere - whose normals are built directly from vertex position, unrelated to mesh
	// winding - seemed to confirm) that the flip was required. A real-scene regression proved that
	// reasoning wrong: removing the flip (matching CULL_FRONT actually rendering the front, not far,
	// face) fixed a real, user-reported inconsistent-shadow-direction bug across many differently-
	// transformed instances, and restoring the flip to satisfy the synthetic test brought that real
	// bug straight back. The synthetic test's own disagreement is not yet root-caused - it's most
	// likely the test's hand-built triangle winding not matching what
	// SurfaceTool::build_meshlets()/meshoptimizer actually produces for real meshes, making the
	// test require a compensation real geometry doesn't need - but until that's confirmed, trust
	// the real-scene result over the synthetic one. Do not reintroduce this flip without first
	// fixing whatever's actually wrong in the synthetic test's winding.
	vec3 local_normal = oct_decode_normal(attrib.xy);
	// Normals need the inverse-transpose of the model matrix (not the model matrix itself) to
	// transform correctly under non-uniform scale. Computed here via the cofactor-matrix identity
	// rather than a literal transpose(inverse(mat3(transform))): the cofactor matrix (cross
	// products of the model matrix's columns) equals det(M)*transpose(inverse(M)), so
	// normalize(cofactor * n) * sign(det) gives the exact same normalized direction as
	// normalize(transpose(inverse(M)) * n) for every transform - but it avoids the full 3x3 matrix
	// inverse(), which a literal inverse() would recompute redundantly for every vertex invocation
	// (3x per triangle, across hundreds of millions of polygons - this is the hottest shader in the
	// pipeline, so a per-vertex inverse is a real, measurable cost). Pure optimization: identical
	// output, strictly cheaper. (A per-instance precomputed normal matrix would be cheaper still,
	// but needs a new buffer threaded through every transforms-buffer consumer - this in-shader
	// form captures the bulk of the win with a one-line, zero-new-state change.)
	mat3 m = mat3(transform);
	vec3 cofactor0 = cross(m[1], m[2]);
	mat3 normal_matrix = mat3(cofactor0, cross(m[2], m[0]), cross(m[0], m[1]));
	float det_sign = sign(dot(m[0], cofactor0));
	vec3 world_normal = normalize(normal_matrix * local_normal) * det_sign;

	world_normal_interp = world_normal;
	world_pos_interp = world_pos;
	meshlet_index_interp = item.meshlet_index;
	// Per-instance material slot, with bit 31 repurposed as the "owns its own depth" flag (set
	// CPU-side for plain INSTANCE_MESH meshlet objects, which are skipped from Forward+'s depth
	// pre-pass so the late pass writes their depth alone - no self-tie, so no depth bias needed).
	// Material slots are small indices, so bit 31 is always free. MultiMesh flora leaves it clear:
	// it stays in the depth pre-pass and still needs the bias to win the tie (see below).
	uint raw_material_id = instance_material_ids.data[item.instance_index];
	bool owns_own_depth = (raw_material_id & 0x80000000u) != 0u;
	material_id_interp = raw_material_id & 0x7FFFFFFFu;
	instance_pos_interp = transform[3].xyz;
	uv_interp = attrib.zw; // VertexAttributes packs uv in .zw (see the buffer's layout comment).

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
	// Bias is 0 for instances that own their own depth (skipped from Forward+'s depth pre-pass - the
	// late pass writes their depth alone, so the color pass ties exactly against itself and a real
	// depth-test against the world correctly occludes them: no holes, no see-through). MultiMesh flora
	// stays in the depth pre-pass, so its color pass still has to out-bias Forward+'s slightly-different
	// depth for the same surface - keep the ~0.005 band-aid only for those.
	float depth_bias_ndc_fraction = owns_own_depth ? 0.0 : 0.005;
	gl_Position.z = min(gl_Position.z + depth_bias_ndc_fraction * gl_Position.w, gl_Position.w);
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(location = 0) in vec3 world_normal_interp;
layout(location = 1) in flat uint meshlet_index_interp;
layout(location = 2) in flat uint material_id_interp;
layout(location = 3) in vec3 world_pos_interp;
layout(location = 4) in flat vec3 instance_pos_interp;
layout(location = 5) in vec2 uv_interp;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec3 camera_position;
	uint light_count;
	vec4 ambient_color; // .rgb = flat ambient (color*energy, linear), .a = sky-radiance mix amount (0 = none)
	vec4 svogi_bounds; // .xyz = octree root center (absolute world), .w = root half-size (0 = SVOGI off)
	vec4 svogi_params; // .x = SVOGI energy, .y = sky-radiance exposure*energy scale, .z = MAX_ROUGHNESS_LOD layer, .w reserved
}
params;

#ifndef MESHLET_DEPTH_ONLY
layout(location = 0) out vec4 frag_color;

// Shading data layouts (MeshletMaterial/MeshletLight/SVOGINode) + constants. Included before the
// storage-buffer declarations that use these struct types.
#include "meshlet_shade_types_inc.glsl"

layout(set = 0, binding = 8, std430) restrict readonly buffer MeshletMaterials {
	MeshletMaterial data[];
}
meshlet_materials;

layout(set = 0, binding = 9, std430) restrict readonly buffer Lights {
	MeshletLight data[];
}
lights;

layout(set = 0, binding = 10, std430) restrict readonly buffer SVOGINodes {
	SVOGINode data[];
}
svogi_nodes;

// Sky radiance octmap array (Forward+ sky reflection probe; roughest layer ~= ambient irradiance).
layout(set = 0, binding = 11) uniform texture2DArray radiance_octmap;
layout(set = 0, binding = 12) uniform sampler radiance_sampler;

// Per-material PBR textures (albedo/normal/ORM), indexed per-fragment by the material's
// *_texture_index. Array size MUST equal MeshletStorage::MAX_MATERIAL_TEXTURES (the renderer binds
// exactly that many, padding unused slots with a default white texture).
layout(set = 0, binding = 13) uniform texture2D material_textures[256];
layout(set = 0, binding = 14) uniform sampler material_sampler;

// All meshlet PBR shading (BRDF, light loop, SVOGI cone-trace, ambient) + meshlet_shade(). Included
// AFTER the buffer declarations above, which its functions reference by name.
#include "meshlet_shade_inc.glsl"

#endif

void main() {
#ifndef MESHLET_DEPTH_ONLY
	// All shading now lives in meshlet_shade() (meshlet_shade_inc.glsl), so the visibility-buffer
	// material-resolve compute pass can call the exact same code. Inputs that were push-constant
	// fields / interpolated varyings are passed explicitly. r_discard reports the alpha-scissor
	// cutout result (compute can't `discard`), which this fragment stage turns into a real discard.
	bool do_discard = false;
	// Fragment stage: screen-space gradients come from dFdx/dFdy (identical to what perturb_normal used
	// to compute internally). The compute resolve pass passes analytic triangle gradients instead.
	vec3 dpdx = dFdx(world_pos_interp);
	vec3 dpdy = dFdy(world_pos_interp);
	vec2 duvdx = dFdx(uv_interp);
	vec2 duvdy = dFdy(uv_interp);
	vec4 shaded = meshlet_shade(
			material_id_interp,
			world_normal_interp,
			world_pos_interp,
			uv_interp,
			params.camera_position,
			params.ambient_color,
			params.svogi_bounds,
			params.svogi_params,
			params.light_count,
			dpdx,
			dpdy,
			duvdx,
			duvdy,
			do_discard);
	if (do_discard) {
		discard;
	}
	frag_color = shaded;
#endif
	// MESHLET_DEPTH_ONLY: no color output at all - this variant targets a depth-only
	// framebuffer (Forward+'s real depth pre-pass framebuffer, which has zero color
	// attachments) for the temporal early pass; depth write/test happens via fixed-function
	// state regardless of what (if anything) the fragment shader writes.
}
