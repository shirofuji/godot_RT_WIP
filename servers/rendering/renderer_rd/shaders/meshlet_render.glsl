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
	material_id_interp = instance_material_ids.data[item.instance_index];
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
	const float DEPTH_BIAS_NDC_FRACTION = 0.005;
	gl_Position.z = min(gl_Position.z + DEPTH_BIAS_NDC_FRACTION * gl_Position.w, gl_Position.w);
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

// Mirrors MeshletStorage::MeshletMaterialGPU exactly (std430 layout) - a flattened snapshot of
// the subset of StandardMaterial3D/ORMMaterial3D parameters this pipeline reads; texture indices
// are uploaded but not sampled yet (deferred - bindless/array texture sampling is its own,
// separate scope from the lighting model added in this milestone).
struct MeshletMaterial {
	// Base PBR (16 bytes)
	vec4 albedo;
	
	// Emission + Normal Scale (16 bytes)
	vec3 emission;
	float normal_scale;
	
	// PBR factors + Clearcoat (16 bytes)
	float metallic;
	float roughness;
	float specular;
	float clearcoat;

	// Subsurface (16 bytes)
	float subsurface_weight;
	float subsurface_radius_x;
	float subsurface_radius_y;
	float subsurface_radius_z;
	
	// Subsurface Color + Clearcoat Roughness (16 bytes)
	vec3 subsurface_color;
	float clearcoat_roughness;

	// Anisotropy & Transmission & IOR (16 bytes)
	float anisotropy;
	float anisotropy_rotation;
	float transmission;
	float ior;

	// Sheen & Scissor & UV (16 bytes)
	float sheen;
	float sheen_tint;
	float alpha_scissor_threshold;
	float pad0;

	// UV Transform (16 bytes)
	vec2 uv1_scale;
	vec2 uv1_offset;

	// Flags & Base Textures (16 bytes)
	uint flags;
	uint albedo_texture_index;
	uint normal_texture_index;
	uint orm_texture_index;

	// Extended Textures (16 bytes)
	uint emission_texture_index;
	uint subsurface_texture_index;
	uint clearcoat_texture_index;
	uint anisotropy_texture_index;

	// Final Textures & padding (16 bytes)
	uint transmission_texture_index;
	uint pad1;
	uint pad2;
	uint pad3;
};

layout(set = 0, binding = 8, std430) restrict readonly buffer MeshletMaterials {
	MeshletMaterial data[];
}
meshlet_materials;

// Mirrors RenderForwardClustered::MeshletLightGPU exactly (std430 layout, 64 bytes) - real scene
// lights (directional/omni/spot), extracted CPU-side via LightStorage's public getters rather
// than binding Forward+'s real omni_lights/spot_lights/cluster_buffer (see
// RenderForwardClustered::_meshlet_collect_lights()'s comment for the full reasoning - binding
// those would need ~25 matching descriptor declarations across two uniform sets in this
// standalone shader, plus separate work for shadow/GI sampling, for marginal benefit at this
// stage). No spatial culling: every light up to MESHLET_MAX_LIGHTS is evaluated for every
// fragment unconditionally.
struct MeshletLight {
	vec3 position;
	float inv_radius;
	vec3 direction;
	float attenuation;
	vec3 color; // Energy-premultiplied.
	float size;
	float cone_angle; // cos(angle) - spot lights only.
	float cone_attenuation;
	uint is_directional;
	uint pad0;
};

layout(set = 0, binding = 9, std430) restrict readonly buffer Lights {
	MeshletLight data[];
}
lights;

// SVOGI sparse-voxel-octree GI (see servers/rendering/renderer_rd/environment/gi.cpp and
// svogi_voxelize.glsl). Mirrors svogi_voxelize.glsl's Node struct exactly (32 bytes). The octree
// is built in plain ABSOLUTE world space (voxelize transforms each vertex by its full model matrix
// and inserts at its absolute world position), so this fragment shader - which already has
// world_pos_interp/world_normal_interp in absolute world space - cone-traces it directly, with no
// camera-relative-space conversion (unlike Forward+'s scene_forward_gi_inc.glsl, which works in a
// camera-centered space). The root bounds (params.svogi_bounds) are the exact absolute-world AABB
// voxelize used for cascade 0, threaded in via push constant. When params.svogi_bounds.w (the root
// half-size) is 0, SVOGI is off / has no data this frame and the trace is skipped entirely (so the
// buffer below may legitimately be a harmless fallback that's never indexed).
struct SVOGINode {
	uint children_base_index;
	uint child_mask;
	uint albedo;
	uint normal;
	uint emission;
	uint pad0;
	uint pad1;
	uint pad2;
};

layout(set = 0, binding = 10, std430) restrict readonly buffer SVOGINodes {
	SVOGINode data[];
}
svogi_nodes;

// Sky radiance octmap array (Forward+'s sky reflection probe; the most-blurred layer ~= ambient
// irradiance). The standalone meshlet shader can't see Forward+'s scene UBO, so the sky ambient is
// sampled here directly and folded in below - without this, meshes on the meshlet path get only the
// flat ambient_light_color (near-black for a Sky ambient source after srgb->linear), reading as dark
// in shadow while the rest of the scene gets proper sky fill. params.svogi_params.z = the layer
// index to sample (MAX_ROUGHNESS_LOD), .y = exposure*energy scale, params.ambient_color.a = sky mix
// amount (0 => skip; a 1x1 black fallback is bound when there's no sky this frame).
layout(set = 0, binding = 11) uniform texture2DArray radiance_octmap;
layout(set = 0, binding = 12) uniform sampler radiance_sampler;

// Per-material PBR textures (albedo/normal/ORM), indexed per-fragment by the material's
// *_texture_index. Fixed-size descriptor array - MUST equal MeshletStorage::MAX_MATERIAL_TEXTURES
// (the renderer binds exactly that many, padding unused slots with a default white texture). The
// index is the per-instance-flat material_id's field, so it's dynamically uniform per draw the same
// way Forward+ indexes lightmap_textures[] - no nonuniformEXT needed.
layout(set = 0, binding = 13) uniform texture2D material_textures[256];
layout(set = 0, binding = 14) uniform sampler material_sampler;

const float M_PI = 3.14159265358979323846;
const uint MESHLET_TEXTURE_NONE = 0xFFFFFFFFu;

// The following are a deliberately trimmed extraction of scene_forward_lights_inc.glsl's
// light_compute()/D_GGX()/V_GGX()/SchlickFresnel()/F0()/get_omni_attenuation() - confirmed during
// research that light_compute() itself takes only explicit scalar/vector parameters (no buffer
// access) as long as none of its optional feature blocks (backlight/transmittance/rim/clearcoat/
// anisotropy/LIGHT_CODE_USED) are compiled in, which this file doesn't define. The wrapper
// functions that *do* need real buffer access (light_process_omni/light_process_spot - they read
// omni_lights.data[]/spot_lights.data[]/scene_data_block directly for shadow sampling even when
// shadows are nominally disabled) are NOT reusable here and are not used - this is why lights are
// looked up from this file's own flat array instead. Uses plain float/vec3 throughout rather than
// Forward+'s half/hvec3 (those only matter for explicit fp16 performance on supporting hardware,
// not correctness) - this is the non-fp16 path's exact behavior (half_inc.glsl's "#else" branch:
// half=float, hvec3=vec3, saturateHalf(x)=x, a no-op). Lambert diffuse (not Burley) + Schlick-GGX
// specular - Forward+'s own most common default combination for opaque PBR materials.

float D_GGX(float NoH, float roughness) {
	float a = NoH * roughness;
	float k = roughness / (1.0 - NoH * NoH + a * a);
	return k * k * (1.0 / M_PI);
}

float V_GGX(float NdotL, float NdotV, float alpha) {
	return 0.5 / mix(2.0 * NdotL * NdotV, NdotL + NdotV, alpha);
}

float SchlickFresnel(float u) {
	float m = 1.0 - u;
	float m2 = m * m;
	return m2 * m2 * m; // pow(m, 5).
}

vec3 F0(float metallic, float specular, vec3 albedo) {
	float dielectric = 0.16 * specular * specular;
	return mix(vec3(dielectric), albedo, metallic);
}

float get_omni_attenuation(float distance, float inv_range, float decay) {
	float nd = distance * inv_range;
	nd *= nd;
	nd *= nd; // nd^4.
	nd = max(1.0 - nd, 0.0);
	nd *= nd; // nd^2.
	return nd * pow(max(distance, 0.0001), -decay);
}

// Extended light_compute using MeshletMaterial to evaluate Principled BSDF features.
void light_compute(vec3 N, vec3 L, vec3 V, vec3 light_color, bool is_directional, float attenuation, MeshletMaterial mat, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	float NdotL = min(dot(N, L), 1.0);
	float cNdotV = max(dot(N, V), 1e-4);

	if (is_directional || attenuation > 1.175494351e-38) {
		float cNdotL = max(NdotL, 0.0);
		
		vec3 H = normalize(V + L);
		float cNdotH = clamp(dot(N, H), 0.0, 1.0);
		float cLdotH = clamp(dot(L, H), 0.0, 1.0);
		float cLdotH5 = SchlickFresnel(cLdotH);

		// F0 calculation from IOR and Metallic
		float f0_ior = pow((mat.ior - 1.0) / (mat.ior + 1.0), 2.0);
		vec3 f0 = mix(vec3(f0_ior), albedo, mat.metallic);
		
		float clearcoat_attenuation = 1.0;

		// Clearcoat (fixed IOR of 1.5 -> F0 = 0.04)
		if (mat.clearcoat > 0.0) {
			float cc_alpha = mix(0.001, 0.1, mat.clearcoat_roughness);
			float cc_D = D_GGX(cNdotH, cc_alpha);
			float cc_G = V_GGX(cNdotL, cNdotV, cc_alpha);
			float cc_F = mix(0.04, 1.0, cLdotH5) * mat.clearcoat;
			
			clearcoat_attenuation = 1.0 - cc_F;
			vec3 cc_specular = vec3(cNdotL * cc_D * cc_G * cc_F);
			specular_light += cc_specular * light_color * attenuation;
		}

		if (mat.metallic < 1.0) {
			// Basic diffuse (Lambert)
			float diffuse_brdf_NL = cNdotL * (1.0 / M_PI);
			
			// Simple Subsurface approximation (Wrap lighting)
			if (mat.subsurface_weight > 0.0) {
				float wrap = 0.5;
				float wrap_NdotL = max(0.0, (dot(N, L) + wrap) / (1.0 + wrap));
				vec3 sss_color = mat.subsurface_color * mat.subsurface_weight;
				diffuse_brdf_NL = mix(diffuse_brdf_NL, wrap_NdotL * (1.0 / M_PI), mat.subsurface_weight);
				diffuse_light += light_color * diffuse_brdf_NL * attenuation * clearcoat_attenuation * sss_color;
			} else {
				diffuse_light += light_color * diffuse_brdf_NL * attenuation * clearcoat_attenuation;
			}
		}

		if (mat.roughness > 0.0) {
			float alpha_ggx = mat.roughness * mat.roughness;
			float D = D_GGX(cNdotH, alpha_ggx);
			float G = V_GGX(cNdotL, cNdotV, alpha_ggx);
			
			float f90 = clamp(dot(f0, vec3(50.0 * 0.33)), mat.metallic, 1.0);
			vec3 F = f0 + (f90 - f0) * cLdotH5;
			vec3 specular_brdf_NL = vec3(cNdotL * D * G) * F;
			specular_light += specular_brdf_NL * light_color * attenuation * clearcoat_attenuation * mat.specular;
		}
	}
}

void light_process_directional(uint idx, vec3 N, vec3 V, MeshletMaterial mat, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	vec3 L = normalize(-lights.data[idx].direction);
	light_compute(N, L, V, lights.data[idx].color, true, 1.0, mat, albedo, diffuse_light, specular_light);
}

void light_process_omni(uint idx, vec3 vertex, vec3 N, vec3 V, MeshletMaterial mat, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	vec3 light_rel_vec = lights.data[idx].position - vertex;
	float light_length = length(light_rel_vec);
	float attenuation = get_omni_attenuation(light_length, lights.data[idx].inv_radius, lights.data[idx].attenuation);
	vec3 L = normalize(light_rel_vec);
	light_compute(N, L, V, lights.data[idx].color, false, attenuation, mat, albedo, diffuse_light, specular_light);
}

void light_process_spot(uint idx, vec3 vertex, vec3 N, vec3 V, MeshletMaterial mat, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	vec3 light_rel_vec = lights.data[idx].position - vertex;
	float light_length = length(light_rel_vec);
	vec3 light_rel_vec_norm = light_rel_vec / light_length;
	float attenuation = get_omni_attenuation(light_length, lights.data[idx].inv_radius, lights.data[idx].attenuation);
	float cone_angle = lights.data[idx].cone_angle;
	float scos = max(dot(-light_rel_vec_norm, lights.data[idx].direction), cone_angle);
	float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - cone_angle));
	attenuation *= 1.0 - pow(spot_rim, lights.data[idx].cone_attenuation);
	vec3 L = light_rel_vec_norm;
	light_compute(N, L, V, lights.data[idx].color, false, attenuation, mat, albedo, diffuse_light, specular_light);
}

// Diffuse cone-march of the SVOGI octree, in absolute world space. Adapted from
// scene_forward_gi_inc.glsl's svogi_cone_trace(), but simplified: bounds come straight from the
// push constant (the absolute-world AABB voxelize actually used for cascade 0) instead of being
// reconstructed from a camera-relative cascade UBO, and the octree root is treated as cubic with
// half-extent bounds_half (matching voxelize, which uses bounds.size.x*0.5 for a cubic root even
// when the source AABB isn't cubic). Returns rgb = accumulated incident radiance, a = coverage.
vec4 svogi_cone_trace(vec3 pos, vec3 dir, float tan_half_angle, float max_distance, float bias, vec3 bounds_center, float bounds_half, float energy) {
	vec4 color = vec4(0.0);
	float dist = bias;

	while (dist < max_distance && color.a < 0.95) {
		float diameter = max(1.0, 2.0 * tan_half_angle * dist);
		vec3 sample_pos = pos + dir * dist;

		vec3 d = abs(sample_pos - bounds_center);
		if (d.x > bounds_half || d.y > bounds_half || d.z > bounds_half) {
			break; // Left the octree's bounds.
		}

		uint node_idx = 0u;
		vec3 current_center = bounds_center;
		float current_half = bounds_half;
		vec4 voxel_color = vec4(0.0);
		float target_size = max(diameter, current_half / 64.0); // 6 levels: leaf = bounds/64.

		for (uint depth = 0u; depth < 6u; depth++) {
			bvec3 is_pos = greaterThan(sample_pos, current_center);
			uint child_idx = (is_pos.x ? 1u : 0u) | ((is_pos.y ? 1u : 0u) << 1) | ((is_pos.z ? 1u : 0u) << 2);

			if ((svogi_nodes.data[node_idx].child_mask & (1u << child_idx)) == 0u) {
				break; // Empty space.
			}
			uint base_idx = svogi_nodes.data[node_idx].children_base_index;
			if (base_idx == 0u) {
				break; // No children allocated.
			}
			node_idx = base_idx + child_idx;

			vec3 offset = vec3(is_pos.x ? 1.0 : -1.0, is_pos.y ? 1.0 : -1.0, is_pos.z ? 1.0 : -1.0);
			current_half *= 0.5;
			current_center += offset * current_half;

			// Internal nodes hold mipmap-aggregated data (see svogi_mipmap.glsl), so sampling a
			// coarser level for a wide cone is correct, not just empty.
			if (current_half * 2.0 <= target_size || depth == 5u) {
				// The node's "albedo" field actually holds LIT RADIANCE (outgoing diffuse light =
				// surface albedo * direct light + emission), written by svogi_voxelize.glsl's
				// direct-light-injection step. So this returns the bounced light arriving from that
				// voxel; the caller multiplies it by the *receiving* surface's albedo, giving proper
				// one-bounce indirect diffuse (red lit surface bounces red light onto its neighbors).
				uint radiance_packed = svogi_nodes.data[node_idx].albedo;
				if (radiance_packed != 0u) {
					vec3 vox_radiance = vec3(
							float((radiance_packed >> 24u) & 0xFFu),
							float((radiance_packed >> 16u) & 0xFFu),
							float((radiance_packed >> 8u) & 0xFFu)) /
							255.0;
					voxel_color = vec4(vox_radiance * energy, 1.0);
				}
				break;
			}
		}

		if (voxel_color.a > 0.0) {
			float a = (1.0 - color.a);
			color += a * voxel_color;
		}
		dist += max(0.5, diameter * 0.5);
	}

	return color;
}

// Branchless orthonormal basis from a unit normal (Duff et al. 2017), giving tangent/bitangent
// perpendicular to n - used to orient the diffuse cone set into the surface's hemisphere.
void svogi_basis(vec3 n, out vec3 t, out vec3 b) {
	float s = n.z >= 0.0 ? 1.0 : -1.0;
	float a = -1.0 / (s + n.z);
	float bb = n.x * n.y * a;
	t = vec3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
	b = vec3(bb, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted multi-cone hemisphere gather: 6 diffuse cones - one along the surface normal plus
// five tilted 60 deg off it and spread at 72 deg azimuthal increments - sample the octree across the
// whole upper hemisphere instead of a single direction. Directions are in tangent space (z = the
// surface normal); weights bias toward the normal (cosine-weighted, summing to 1) so the result
// approximates the Lambert diffuse irradiance integral. This is what turns the single-cone trace's
// weak, self-bleed-prone result into strong, smooth, directionally-correct indirect diffuse. ~6x
// the single-cone cost, but it's a per-pixel screen cost (bounded by resolution, independent of
// scene polygon count). Each cone is 60 deg aperture (tan_half_angle = tan(30) ~= 0.577).
vec3 svogi_hemisphere_gather(vec3 pos, vec3 normal, float surface_offset, float max_distance, vec3 bounds_center, float bounds_half, float energy) {
	vec3 t, b;
	svogi_basis(normal, t, b);

	const vec3 CONE_DIRS[6] = vec3[](
			vec3(0.0, 0.0, 1.0),
			vec3(0.0, 0.866025, 0.5),
			vec3(0.823639, 0.267617, 0.5),
			vec3(0.509037, -0.700629, 0.5),
			vec3(-0.509037, -0.700629, 0.5),
			vec3(-0.823639, 0.267617, 0.5));
	const float CONE_WEIGHTS[6] = float[](0.25, 0.15, 0.15, 0.15, 0.15, 0.15);

	// Push the shared cone origin off the surface ALONG THE NORMAL before tracing. Without this,
	// the 60-deg-tilted cones graze straight back into a convex surface's own geometry in a
	// direction-dependent way, producing the characteristic dark-petal / pinwheel artifact (one
	// dark streak per tilted cone). Offsetting along the normal clears the local curvature for all
	// cones uniformly; the cones then only need a tiny additional start bias for their own first
	// step. Trade-off: too large an offset starts skipping genuine near-field contact bleed, so
	// this is kept to a couple of leaf voxels.
	vec3 start = pos + normal * surface_offset;

	vec3 acc = vec3(0.0);
	for (int i = 0; i < 6; i++) {
		vec3 dir = normalize(t * CONE_DIRS[i].x + b * CONE_DIRS[i].y + normal * CONE_DIRS[i].z);
		acc += CONE_WEIGHTS[i] * svogi_cone_trace(start, dir, 0.577, max_distance, surface_offset * 0.5, bounds_center, bounds_half, energy).rgb;
	}
	return acc;
}

// Cotangent-frame normal mapping without precomputed vertex tangents (Christian Schueler, "Normal
// Mapping Without Precomputed Tangents"). Builds a TBN basis from the screen-space derivatives of
// world position and UV, so a tangent-space normal map can be applied to meshes that carry NO
// per-vertex tangent - which the meshlet pipeline deliberately never stores (saving the attribute
// memory + the runtime MikkTSpace generation, the way Nanite does for dense geometry). N is the
// geometric world normal, map_normal the unpacked tangent-space normal (z = up). dFdx/dFdy require
// this to run in the fragment stage.
vec3 perturb_normal(vec3 N, vec3 world_pos, vec2 uv, vec3 map_normal) {
	vec3 dp1 = dFdx(world_pos);
	vec3 dp2 = dFdy(world_pos);
	vec2 duv1 = dFdx(uv);
	vec2 duv2 = dFdy(uv);

	vec3 dp2perp = cross(dp2, N);
	vec3 dp1perp = cross(N, dp1);
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

	// Degenerate UV derivatives (e.g. a fragment with no UV gradient) -> keep the geometric normal.
	float det = max(dot(T, T), dot(B, B));
	if (det <= 0.0) {
		return N;
	}
	float invmax = inversesqrt(det);
	mat3 TBN = mat3(T * invmax, B * invmax, N);
	return normalize(TBN * map_normal);
}

#endif

void main() {
#ifndef MESHLET_DEPTH_ONLY
	MeshletMaterial mat = meshlet_materials.data[material_id_interp];
	vec3 N = normalize(world_normal_interp);
	vec3 V = normalize(params.camera_position - world_pos_interp);

	// StandardMaterial3D uv1 transform applied to the interpolated mesh UV.
	vec2 muv = uv_interp * mat.uv1_scale + mat.uv1_offset;

	// Albedo texture modulates the base color factor.
	vec3 albedo = mat.albedo.rgb;
	if (mat.albedo_texture_index != MESHLET_TEXTURE_NONE) {
		albedo *= texture(sampler2D(material_textures[mat.albedo_texture_index], material_sampler), muv).rgb;
	}

	// Normal map -> world normal via the derivative-built TBN (no stored vertex tangents). Unpack the
	// tangent-space normal (xyz: [0,1] -> [-1,1], blue = z), apply the material's normal_scale to xy.
	if (mat.normal_texture_index != MESHLET_TEXTURE_NONE) {
		vec3 nm = texture(sampler2D(material_textures[mat.normal_texture_index], material_sampler), muv).xyz * 2.0 - 1.0;
		nm.xy *= mat.normal_scale;
		N = perturb_normal(N, world_pos_interp, muv, normalize(nm));
	}

	// ORM map: r = occlusion, g = roughness, b = metallic (Godot's ORM channel packing). Modulates
	// the scalar roughness/metallic factors; occlusion is applied to indirect light below.
	float ao = 1.0;
	if (mat.orm_texture_index != MESHLET_TEXTURE_NONE) {
		vec3 orm = texture(sampler2D(material_textures[mat.orm_texture_index], material_sampler), muv).rgb;
		ao = orm.r;
		mat.roughness *= orm.g;
		mat.metallic *= orm.b;
	}

	vec3 diffuse_light = vec3(0.0);
	vec3 specular_light = vec3(0.0);
	for (uint i = 0; i < params.light_count; i++) {
		if (lights.data[i].is_directional != 0u) {
			light_process_directional(i, N, V, mat, albedo, diffuse_light, specular_light);
		} else if (lights.data[i].cone_angle < 1.0) {
			light_process_spot(i, world_pos_interp, N, V, mat, albedo, diffuse_light, specular_light);
		} else {
			light_process_omni(i, world_pos_interp, N, V, mat, albedo, diffuse_light, specular_light);
		}
	}

	// SVOGI indirect-diffuse bounce (params.svogi_bounds.w > 0 means the octree has data this
	// frame - see svogi_bounds' push-constant comment). A single wide diffuse cone along the
	// surface normal: a coarse, cheap first-cut "is there bounced light arriving roughly from the
	// SVOGI indirect-diffuse bounce via a cosine-weighted multi-cone hemisphere gather (see
	// svogi_hemisphere_gather()). Folded into diffuse_light so it picks up the same
	// albedo*(1-metallic) modulation as direct/ambient diffuse below. Start bias of a couple leaf
	// voxels steps the cones off the surface so they clear its own local curvature before sampling.
	vec3 gi_diffuse = vec3(0.0);
	if (params.svogi_bounds.w > 0.0) {
		float voxel_size = params.svogi_bounds.w / 32.0; // root half-size / 32 ~= leaf diameter.
		gi_diffuse = svogi_hemisphere_gather(world_pos_interp, N, voxel_size * 3.0, params.svogi_bounds.w * 2.0, params.svogi_bounds.xyz, params.svogi_bounds.w, params.svogi_params.x);
	}

	// Environment ambient. Base is the flat ambient_color (used directly for a Color ambient source);
	// for a Sky source the CPU sets params.ambient_color.a > 0 and we blend in the sky's average
	// radiance (center texel of the roughest octmap layer ~= directionless sky irradiance), matching
	// what the default Forward+ path samples per-normal. Directionless is fine here - the roughest
	// layer carries almost no directional variation - and it avoids threading the octahedral mapping
	// and per-fragment derivatives into this standalone shader.
	vec3 ambient = params.ambient_color.rgb;
	if (params.ambient_color.a > 0.0) {
		vec3 sky_ambient = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(0.5, 0.5, params.svogi_params.z), 0.0).rgb * params.svogi_params.y;
		ambient = mix(ambient, sky_ambient, params.ambient_color.a);
	}

	vec3 color = albedo * (1.0 - mat.metallic) * (diffuse_light + (ambient + gi_diffuse) * ao) + specular_light + mat.emission;
	frag_color = vec4(color, mat.albedo.a);
#endif
	// MESHLET_DEPTH_ONLY: no color output at all - this variant targets a depth-only
	// framebuffer (Forward+'s real depth pre-pass framebuffer, which has zero color
	// attachments) for the temporal early pass; depth write/test happens via fixed-function
	// state regardless of what (if anything) the fragment shader writes.
}
