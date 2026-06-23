#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec3 world_normal_interp;
layout(location = 1) out flat uint meshlet_index_interp;
layout(location = 2) out flat uint material_id_interp;
layout(location = 3) out vec3 world_pos_interp;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec3 camera_position;
	uint light_count;
	vec4 ambient_color; // .rgb = pre-multiplied color*energy (linear), .a = reserved
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
	// Normals need the inverse-transpose of the model matrix, not the model matrix itself, to
	// transform correctly under non-uniform scale (a uniformly-scaled or unscaled transform would
	// be fine with mat3(transform) directly, but this pipeline has no per-instance "is this
	// uniform scale" flag to take that cheaper path conditionally like Forward+'s own
	// model_normal_matrix does - scene_forward_clustered.glsl checks
	// INSTANCE_FLAGS_NON_UNIFORM_SCALE per-instance and only pays for the inverse when needed).
	// Always paying for it here is correct for every transform and cheap enough at this pipeline's
	// vertex-shader scale not to matter.
	mat3 normal_matrix = transpose(inverse(mat3(transform)));
	vec3 world_normal = normalize(normal_matrix * local_normal);

	world_normal_interp = world_normal;
	world_pos_interp = world_pos;
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

layout(location = 0) in vec3 world_normal_interp;
layout(location = 1) in flat uint meshlet_index_interp;
layout(location = 2) in flat uint material_id_interp;
layout(location = 3) in vec3 world_pos_interp;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec3 camera_position;
	uint light_count;
	vec4 ambient_color; // .rgb = pre-multiplied color*energy (linear), .a = reserved
}
params;

#ifndef MESHLET_DEPTH_ONLY
layout(location = 0) out vec4 frag_color;

// Mirrors MeshletStorage::MeshletMaterialGPU exactly (std430 layout) - a flattened snapshot of
// the subset of StandardMaterial3D/ORMMaterial3D parameters this pipeline reads; texture indices
// are uploaded but not sampled yet (deferred - bindless/array texture sampling is its own,
// separate scope from the lighting model added in this milestone).
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

const float M_PI = 3.14159265358979323846;

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

// Trimmed light_compute(): no backlight/transmittance/rim/clearcoat/anisotropy, no shadows (the
// real shadow atlas isn't bound here - see this file's top-level comment), no LIGHT_CODE_USED
// (custom ShaderMaterial light() functions aren't representable in MeshletMaterial's flattened
// schema and fall back to normal Forward+ rendering well before reaching this shader).
void light_compute(vec3 N, vec3 L, vec3 V, vec3 light_color, bool is_directional, float attenuation, vec3 f0, float roughness, float metallic, float specular_amount, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	float NdotL = min(dot(N, L), 1.0);
	float cNdotV = max(dot(N, V), 1e-4);

	if (is_directional || attenuation > 1.175494351e-38) {
		float cNdotL = max(NdotL, 0.0);

		if (metallic < 1.0) {
			float diffuse_brdf_NL = cNdotL * (1.0 / M_PI); // Lambert.
			diffuse_light += light_color * diffuse_brdf_NL * attenuation;
		}

		if (roughness > 0.0) {
			vec3 H = normalize(V + L);
			float cNdotH = clamp(dot(N, H), 0.0, 1.0);
			float cLdotH = clamp(dot(L, H), 0.0, 1.0);

			float alpha_ggx = roughness * roughness;
			float D = D_GGX(cNdotH, alpha_ggx);
			float G = V_GGX(cNdotL, cNdotV, alpha_ggx);
			float cLdotH5 = SchlickFresnel(cLdotH);
			float f90 = clamp(dot(f0, vec3(50.0 * 0.33)), metallic, 1.0);
			vec3 F = f0 + (f90 - f0) * cLdotH5;
			vec3 specular_brdf_NL = vec3(cNdotL * D * G) * F;
			specular_light += specular_brdf_NL * light_color * attenuation * specular_amount;
		}
	}
}

void light_process_directional(uint idx, vec3 N, vec3 V, vec3 f0, float roughness, float metallic, float specular_amount, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	vec3 L = normalize(-lights.data[idx].direction);
	light_compute(N, L, V, lights.data[idx].color, true, 1.0, f0, roughness, metallic, specular_amount, albedo, diffuse_light, specular_light);
}

void light_process_omni(uint idx, vec3 vertex, vec3 N, vec3 V, vec3 f0, float roughness, float metallic, float specular_amount, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	vec3 light_rel_vec = lights.data[idx].position - vertex;
	float light_length = length(light_rel_vec);
	float attenuation = get_omni_attenuation(light_length, lights.data[idx].inv_radius, lights.data[idx].attenuation);
	vec3 L = normalize(light_rel_vec);
	light_compute(N, L, V, lights.data[idx].color, false, attenuation, f0, roughness, metallic, specular_amount, albedo, diffuse_light, specular_light);
}

void light_process_spot(uint idx, vec3 vertex, vec3 N, vec3 V, vec3 f0, float roughness, float metallic, float specular_amount, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	vec3 light_rel_vec = lights.data[idx].position - vertex;
	float light_length = length(light_rel_vec);
	vec3 light_rel_vec_norm = light_rel_vec / light_length;
	float attenuation = get_omni_attenuation(light_length, lights.data[idx].inv_radius, lights.data[idx].attenuation);
	float cone_angle = lights.data[idx].cone_angle;
	float scos = max(dot(-light_rel_vec_norm, lights.data[idx].direction), cone_angle);
	float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - cone_angle));
	attenuation *= 1.0 - pow(spot_rim, lights.data[idx].cone_attenuation);
	vec3 L = light_rel_vec_norm;
	light_compute(N, L, V, lights.data[idx].color, false, attenuation, f0, roughness, metallic, specular_amount, albedo, diffuse_light, specular_light);
}

#endif

void main() {
#ifndef MESHLET_DEPTH_ONLY
	MeshletMaterial mat = meshlet_materials.data[material_id_interp];
	vec3 N = normalize(world_normal_interp);
	vec3 V = normalize(params.camera_position - world_pos_interp);
	vec3 albedo = mat.albedo.rgb;
	vec3 f0 = F0(mat.metallic, mat.specular, albedo);

	vec3 diffuse_light = vec3(0.0);
	vec3 specular_light = vec3(0.0);
	for (uint i = 0; i < params.light_count; i++) {
		if (lights.data[i].is_directional != 0u) {
			light_process_directional(i, N, V, f0, mat.roughness, mat.metallic, mat.specular, albedo, diffuse_light, specular_light);
		} else if (lights.data[i].cone_angle < 1.0) {
			// cone_angle defaults to 1.0 (cos(0)) for omni lights, where it's meaningless - real
			// spot lights always have a smaller cone_angle than that default.
			light_process_spot(i, world_pos_interp, N, V, f0, mat.roughness, mat.metallic, mat.specular, albedo, diffuse_light, specular_light);
		} else {
			light_process_omni(i, world_pos_interp, N, V, f0, mat.roughness, mat.metallic, mat.specular, albedo, diffuse_light, specular_light);
		}
	}

	vec3 color = albedo * (1.0 - mat.metallic) * (diffuse_light + params.ambient_color.rgb) + specular_light + mat.emission;
	frag_color = vec4(color, mat.albedo.a);
#endif
	// MESHLET_DEPTH_ONLY: no color output at all - this variant targets a depth-only
	// framebuffer (Forward+'s real depth pre-pass framebuffer, which has zero color
	// attachments) for the temporal early pass; depth write/test happens via fixed-function
	// state regardless of what (if anything) the fragment shader writes.
}
