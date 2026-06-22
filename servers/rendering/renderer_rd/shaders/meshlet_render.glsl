#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec3 normal_interp;
layout(location = 1) out flat uint meshlet_index_interp;
layout(location = 2) out flat uint material_id_interp;
layout(location = 3) out vec3 vertex_light_interp;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec3 light_direction;
	float pad0;
	vec3 light_color; // Already includes energy (premultiplied on the C++ side).
	float pad1;
	vec3 camera_position;
	float pad2;
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

#ifndef MESHLET_DEPTH_ONLY
// Mirrors MeshletStorage::MeshletMaterialGPU exactly (std430 layout) - see the fragment stage's
// copy of this struct for the full field-by-field rationale. Declared in the vertex stage too
// (B2: vertex-lit milestone) since the per-vertex lighting calculation below needs
// roughness/metallic/specular; the depth-only shader variant skips this entirely (no color
// output at all, so no need for material data in its vertex stage either).
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

	// Negated: this pipeline's render pipeline uses POLYGON_CULL_FRONT (see
	// MeshletRenderer::_ensure_pipeline()'s comment - meshoptimizer/SurfaceTool::build_meshlets()
	// produces triangles wound opposite to Godot's normal front-facing convention, and CULL_FRONT
	// is the established fix for that, empirically verified to eliminate the original "holes"
	// bug). One consequence never previously surfaced: for a closed surface, culling what Godot
	// considers "front-facing" and keeping only "back-facing" triangles means the *actual visible*
	// triangles are the geometrically-far side of the surface as seen from the camera (there's
	// nothing left to occlude them, so they pass the depth test and get drawn at their own,
	// correct position) - and that far side's own correctly-computed outward normal naturally
	// points away from the camera, not toward it. Confirmed directly: a synthetic sphere's
	// vertex-shader-computed N.z was negative at the camera-facing point and positive only near
	// the silhouette - exactly inverted from the expected gradient - until this negation was
	// added. B1's flat per-meshlet debug coloring and the original N.L-based debug shading never
	// surfaced this, since "looks like a smoothly shaded blob" doesn't distinguish a correct
	// gradient from a geometrically-backward one - this is the first shading in this pipeline that
	// actually depends on the *absolute* direction of N, not just its smoothness.
	vec3 world_normal = -normalize(mat3(transform) * oct_decode_normal(attrib.xy));

	normal_interp = world_normal;
	meshlet_index_interp = item.meshlet_index;
	uint material_id = instance_material_ids.data[item.instance_index];
	material_id_interp = material_id;

#ifndef MESHLET_DEPTH_ONLY
	// B2 (vertex-lit milestone): a simplified, per-vertex Lambertian-diffuse + Blinn-Phong-
	// specular response to the scene's real directional light (color/energy/direction resolved
	// CPU-side via LightStorage, not Forward+'s real DirectionalLights UBO - see
	// _meshlet_get_directional_light()'s comment for why that's deferred to B3). Deliberately not
	// a literal reuse of scene_forward_lights_inc.glsl's light_compute() (that function has
	// transitive dependencies - shadow sampling, the real light/shadow-atlas bindings - which
	// would need the full SCENE_UNIFORM_SET integration B3 is scoped to do anyway); this is "close
	// enough to be visually comparable" by design, not bit-for-bit identical to Forward+'s own
	// lighting, matching this milestone's explicitly scoped intent.
	MeshletMaterial mat = meshlet_materials.data[material_id];
	vec3 N = world_normal;
	vec3 L = normalize(-params.light_direction);
	vec3 V = normalize(params.camera_position - world_pos);
	vec3 H = normalize(L + V);
	float ndotl = max(dot(N, L), 0.0);
	float ndoth = max(dot(N, H), 0.0);

	// Real metals have ~no diffuse response; non-metals keep their full albedo as diffuse color.
	vec3 diffuse_color = mat.albedo.rgb * (1.0 - mat.metallic);
	vec3 diffuse = diffuse_color * params.light_color * ndotl;

	// Rough Blinn-Phong stand-in for a real GGX specular lobe - roughness controls the shininess
	// exponent (rougher = lower exponent = wider, dimmer highlight), metallic blends the highlight
	// color from white (dielectric) toward the albedo (metals tint their specular reflections).
	float shininess = mix(128.0, 2.0, mat.roughness);
	float spec_power = ndotl > 0.0 ? pow(ndoth, shininess) : 0.0;
	vec3 specular_color = mix(vec3(mat.specular), mat.albedo.rgb, mat.metallic);
	vec3 specular = specular_color * params.light_color * spec_power;

	vertex_light_interp = diffuse + specular;
#else
	vertex_light_interp = vec3(0.0);
#endif

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
layout(location = 3) in vec3 vertex_light_interp;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec3 light_direction;
	float pad0;
	vec3 light_color;
	float pad1;
	vec3 camera_position;
	float pad2;
}
params;

#ifndef MESHLET_DEPTH_ONLY
layout(location = 0) out vec4 frag_color;

// Mirrors MeshletStorage::MeshletMaterialGPU exactly (std430 layout) - a flattened snapshot of
// the subset of StandardMaterial3D/ORMMaterial3D parameters this pipeline reads; texture indices
// are uploaded but not sampled yet (B3 - per-fragment, where texture detail actually matters).
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
	// B2 (vertex-lit milestone): the lighting contribution (diffuse+specular against the scene's
	// real directional light) was already computed per-vertex and interpolated in - just add
	// emission here. No per-fragment light_compute() yet (that's B3).
	vec3 color = vertex_light_interp + mat.emission;
	frag_color = vec4(color, mat.albedo.a);
#endif
	// MESHLET_DEPTH_ONLY: no color output at all - this variant targets a depth-only
	// framebuffer (Forward+'s real depth pre-pass framebuffer, which has zero color
	// attachments) for the temporal early pass; depth write/test happens via fixed-function
	// state regardless of what (if anything) the fragment shader writes.
}
