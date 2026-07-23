#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec3 world_normal_interp;
layout(location = 1) out flat uint meshlet_index_interp;
layout(location = 2) out flat uint material_id_interp;
layout(location = 3) out vec3 world_pos_interp;
layout(location = 4) out flat vec3 instance_pos_interp;
layout(location = 5) out vec2 uv_interp;
#ifdef MESHLET_TERRAIN
// Debug-only visualisation channel, fed by terrain_params.tp_extra.y (0 = off). Computed in the
// vertex stage because that's where the cluster descriptor and the heightmap sample live; the
// fragment just blits it. Terrain-only, so the varying layout of the other variants is untouched.
// Interpolated, not flat: the per-cluster modes write the same value at all 3 corners (so they stay
// constant across the triangle anyway), while the per-vertex modes (height, UV) want the gradient.
layout(location = 6) out vec3 debug_color_interp;
#endif

// When tessellation stages follow, gl_Position must be redeclared here so the vertex output block
// matches what the control stage declares as its input block. ShaderRD injects USE_TESSELLATION only
// for versions it flags as tessellated, so non-tess variants are byte-identical.
#ifdef USE_TESSELLATION
out gl_PerVertex {
	vec4 gl_Position;
};
#endif

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

#ifdef MESHLET_TERRAIN
// T2 terrain variant (spine step): the terrain chunks are meshletized from their FLAT base plane, so
// the meshlet path would draw them flat. Restore the real shape by sampling the terrain heightmap at
// each vertex's world XZ and offsetting Y here - mirroring terrain_viewer.gdshader's vertex()
// displacement (h = disp.r*height_range + height_min; VERTEX.y += h*height_scale). tp = terrain params:
// .x = terrain_size (UV scale), .y = height_min, .z = height_range, .w = height_scale.
layout(set = 0, binding = 19) uniform texture2D terrain_heightmap;
layout(set = 0, binding = 20) uniform sampler terrain_hm_sampler;
layout(set = 0, binding = 21, std140) uniform TerrainParams {
	vec4 tp0; // x = terrain_size, y = height_min, z = height_range, w = height_scale
	vec4 tp_tiles; // per-material tile scale (world metres per tile) for materials 0..3
	vec4 tp_extra; // x = tile scale for material 4, y = debug mode, z = splat weight cutoff, w = roughness_min
	vec4 tp_mat; // x = normal_strength, y = macro_variation_strength, z = water_level, w = shore_band
	// T2.3d de-tiling. The two schemes are per-material alternatives: hex when hex_strength > 0
	// (translation-only stochastic tiling, 3 taps/plane - used on rock, where the lattice shows),
	// otherwise the cheaper two-stamp variant blend.
	vec4 tp_var_str; // variant_strength for materials 0..3
	vec4 tp_var_scale; // variant blotch scale for materials 0..3
	vec4 tp_hex; // hex_strength for materials 0..3
	vec4 tp_var4; // x = variant_strength_4, y = variant_scale_4, z = hex_strength_4, w = variant_blend_width
	vec4 tp_var_rot; // x = variant_rotation, yzw = variant_offset
	vec4 tp_det; // per-material detail displacement depth, materials 0..3
	vec4 tp_det2; // x = detail_depth_4, y = tessellation_distance, z = tessellation_fade, w = detail_mip_bias
}
terrain_params;
#endif

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
#ifdef MESHLET_TERRAIN
	// Displace the flat terrain vertex by the heightmap before it's projected / handed downstream, so
	// the meshlet-rendered terrain has its real relief (see the TerrainParams comment above).
	vec3 terr_base_pos = world_pos; // pre-displacement, for debug mode 6.
	float terr_size = terrain_params.tp0.x;
	vec2 terr_uv = world_pos.xz / terr_size + 0.5;
	float terr_h = textureLod(sampler2D(terrain_heightmap, terrain_hm_sampler), terr_uv, 0.0).r * terrain_params.tp0.z + terrain_params.tp0.y;
	// Debug mode 10 replaces the heightmap fetch with a procedural egg-carton of known amplitude,
	// leaving the rest of the path (transform, gl_Position, shading) untouched. If the surface becomes
	// wavy, displacement reaches the rasteriser fine and the texture fetch is at fault; if it stays
	// flat, the displacement never makes it into gl_Position for these clusters.
	int terr_dbg = int(terrain_params.tp_extra.y + 0.5);
	if (terr_dbg == 10) {
		terr_h = 30.0 * sin(world_pos.x * 0.1) + 30.0 * sin(world_pos.z * 0.1);
	}
	world_pos.y += terr_h * terrain_params.tp0.w;
#endif
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

#ifdef MESHLET_TERRAIN
	// The flat plane's own normal is a useless +Y; derive the real surface normal from the heightmap
	// gradient (finite differences), matching terrain_viewer.gdshader's fragment normal, so slopes light.
	float terr_tx = 1.0 / terr_size;
	float terr_hL = textureLod(sampler2D(terrain_heightmap, terrain_hm_sampler), terr_uv - vec2(terr_tx, 0.0), 0.0).r * terrain_params.tp0.z;
	float terr_hR = textureLod(sampler2D(terrain_heightmap, terrain_hm_sampler), terr_uv + vec2(terr_tx, 0.0), 0.0).r * terrain_params.tp0.z;
	float terr_hD = textureLod(sampler2D(terrain_heightmap, terrain_hm_sampler), terr_uv - vec2(0.0, terr_tx), 0.0).r * terrain_params.tp0.z;
	float terr_hU = textureLod(sampler2D(terrain_heightmap, terrain_hm_sampler), terr_uv + vec2(0.0, terr_tx), 0.0).r * terrain_params.tp0.z;
	world_normal = normalize(vec3(terr_hL - terr_hR, 2.0, terr_hD - terr_hU));

	// --- Terrain debug probes (terrain_params.tp_extra.y; 0 = off). Set via
	// --meshlet-terrain-debug=N or ProjectSettings "rendering/meshlet/terrain_debug".
	//
	// Each probe isolates ONE link of the chain that turns a flat baked plane into displaced, shaded
	// terrain, so a capture localizes a failure instead of a theory. Sample the resulting PNG
	// numerically - never judge one by eye.
	//
	// DESIGN RULE, learned the hard way: a probe must be QUANTISED INTO SATURATED PRIMARIES. The debug
	// colour is written pre-tonemap, and the tonemapper compresses a smooth 0..1 ramp into a narrow
	// bright band that reads as "constant". Every ramp-valued probe this path once had (cluster radius,
	// height, UV, UBO readback) produced a confident wrong answer at least once and has been removed;
	// the numbering below is deliberately non-contiguous because of it. Likewise a probe must be able
	// to DISTINGUISH the thing it claims to measure - visualising gl_TessCoord to read tessellation
	// level was removed for that reason (it is continuous across a patch, so it looks the same at any
	// level; mode 22 reads gl_TessLevelInner directly instead).
	//
	//   GEOMETRY / CLUSTERING
	//    1 cluster id        per-meshlet hash - the cluster mosaic
	//    5 instance id       per-chunk hash, to line probes up with chunk boundaries
	//    6 base position     UNDISPLACED world xz in 128 m bands - separates "vertices are wrong" from
	//                        "the heightmap lookup is wrong"
	//   HEIGHTMAP PLUMBING
	//   13 bound texture     is the real displacement map bound? red 1x1 stand-in / green 1024 / blue other
	//   14 terrain_size      quantised, as the GPU sees it (a huge value collapses UV to a constant)
	//   16 UV response       does the fetch respond to UV at all? two fixed far-apart UVs compared
	//   12 raw fetch         quantised heightmap value; all-white means the white stand-in is bound
	//   17 UV bands          terr_uv in cycling bands - does UV vary at the scale that matters?
	//   18 height bands      terr_h in 5 m bands - does the displacement vary as the CPU dump says?
	//   GEOMETRY OVERRIDES (these change the surface, and shade normally)
	//   10 procedural        replaces the heightmap with a sine egg-carton of known amplitude: proves
	//                        whether displacement reaches the rasteriser at all
	//   SHADING (fragment stage)
	//   19 no normal map     geometric normal only
	//   20 unlit albedo      the albedo blend on its own
	//   22 tess level        gl_TessLevelInner, quantised (red 1 / green 2 / blue 3 / white >=4)
	//
	// -1 marks "no probe wrote a colour", so the fragment stage knows to shade normally. Interpolation
	// preserves it, and every real probe writes >= 0.
	int dbg_mode = int(terrain_params.tp_extra.y + 0.5);
	debug_color_interp = vec3(-1.0);
	if (dbg_mode == 1) {
		uint h = item.meshlet_index * 2654435761u;
		debug_color_interp = vec3(float((h >> 16) & 255u), float((h >> 8) & 255u), float(h & 255u)) / 255.0;
	} else if (dbg_mode == 5) {
		uint h = item.instance_index * 2246822519u;
		debug_color_interp = vec3(float((h >> 16) & 255u), float((h >> 8) & 255u), float(h & 255u)) / 255.0;
	} else if (dbg_mode == 6) {
		debug_color_interp = vec3(fract(terr_base_pos.xz / 128.0), 0.0);
	} else if (dbg_mode == 12) {
		float raw = textureLod(sampler2D(terrain_heightmap, terrain_hm_sampler), terr_uv, 0.0).r;
		debug_color_interp = (raw < 0.25) ? vec3(0.0, 0.0, 1.0) : ((raw < 0.5) ? vec3(0.0, 1.0, 0.0) : ((raw < 0.75) ? vec3(1.0, 0.0, 0.0) : vec3(1.0)));
	} else if (dbg_mode == 13) {
		ivec2 hsz = textureSize(sampler2D(terrain_heightmap, terrain_hm_sampler), 0);
		debug_color_interp = (hsz.x <= 1) ? vec3(1.0, 0.0, 0.0) : ((hsz.x == 1024) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
	} else if (dbg_mode == 14) {
		float ts = terrain_params.tp0.x;
		debug_color_interp = (ts != ts) ? vec3(1.0, 0.0, 1.0) : ((ts < 2048.0) ? vec3(1.0, 0.0, 0.0) : ((ts <= 8192.0) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0)));
	} else if (dbg_mode == 16) {
		float a = textureLod(sampler2D(terrain_heightmap, terrain_hm_sampler), vec2(0.2, 0.2), 0.0).r;
		float b = textureLod(sampler2D(terrain_heightmap, terrain_hm_sampler), vec2(0.8, 0.8), 0.0).r;
		debug_color_interp = (abs(a - b) > 0.02) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	} else if (dbg_mode == 17) {
		int band = int(fract(terr_uv.x * 100.0) * 3.0);
		debug_color_interp = (band == 0) ? vec3(1.0, 0.0, 0.0) : ((band == 1) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
	} else if (dbg_mode == 18) {
		int band = int(mod(floor(terr_h / 5.0), 3.0));
		debug_color_interp = (band == 0) ? vec3(1.0, 0.0, 0.0) : ((band == 1) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
	}
#endif

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
#ifdef MESHLET_TERRAIN
layout(location = 6) in vec3 debug_color_interp; // see the vertex stage's debug-mode comment.
#endif

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

// Per-material PBR textures. Two binding layouts, chosen at compile time so the default build is
// untouched by virtual texturing:
//  * Default: the direct material_textures[] descriptor array (bindings 13/14), exactly as before.
//  * MESHLET_USE_VIRTUAL_TEXTURES: the *_texture_index fields are vt_ids, and albedo/normal/ORM are
//    fetched through VirtualTextureStorage's page pool + indirection page table via sampleVirtual().
#ifdef MESHLET_USE_VIRTUAL_TEXTURES

// MUST match VirtualTextureStorage's constants (virtual_texture_storage.h).
#define VT_PAGE_SIZE 128.0
#define VT_PAGE_BORDER 4.0
#define VT_STORED_PAGE_SIZE 136.0
// VT_POOL_TILES_X is injected into this variant's version string by MeshletRenderer from
// VirtualTextureStorage::get_pool_tiles_dim() (the pool_size_mb setting), so it matches the runtime
// pool allocation. This #ifndef default only applies if that injection is ever absent.
#ifndef VT_POOL_TILES_X
#define VT_POOL_TILES_X 64
#endif
#define VT_INDIRECTION_MIPS 7

layout(set = 0, binding = 13) uniform texture2D vt_page_pool;
layout(set = 0, binding = 14) uniform sampler vt_pool_sampler; // linear, clamp
layout(set = 0, binding = 15) uniform utexture2DArray vt_indirection;
layout(set = 0, binding = 16) uniform sampler vt_point_sampler; // nearest (texelFetch ignores it)

struct VTMeta {
	uint width;
	uint height;
	uint mip_count;
	uint resident_mip_floor;
};
layout(set = 0, binding = 17, std430) restrict readonly buffer VTMetadata {
	VTMeta data[];
}
vt_meta;

// S0b GPU feedback: one r32ui texel per VT_FEEDBACK_TILE-pixel screen tile records which
// (vt_id, mip, page) that tile sampled, so the CPU can stream exactly the pages on screen (S0c).
// Packing (mip in the high bits so atomicMin keeps the FINEST mip a tile needs):
//   bits 0-5 page_x | 6-11 page_y | 12-19 vt_id | 20-23 mip.  0xFFFFFFFF (cleared) = no request.
// MUST match VirtualTextureStorage's decode + VT_FEEDBACK_TILE.
#define VT_FEEDBACK_TILE 16
layout(r32ui, set = 0, binding = 18) uniform restrict uimage2D vt_feedback;

void vt_write_feedback(uint vt_id, vec2 uv) {
	VTMeta m = vt_meta.data[vt_id];
	vec2 tex_size = vec2(max(m.width, 1u), max(m.height, 1u));
	vec2 dx = dFdx(uv) * tex_size;
	vec2 dy = dFdy(uv) * tex_size;
	float lod = max(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8)), 0.0);
	int max_mip = int(min(m.mip_count, uint(VT_INDIRECTION_MIPS))) - 1;
	int mip = clamp(int(floor(lod)), 0, max(max_mip, 0));
	ivec2 mip_size = max(ivec2(int(m.width) >> mip, int(m.height) >> mip), ivec2(1));
	ivec2 page = ivec2(fract(uv) * vec2(mip_size)) / int(VT_PAGE_SIZE);
	uint packed = (uint(mip) << 20) | (vt_id << 12) | (uint(page.y) << 6) | uint(page.x);
	imageAtomicMin(vt_feedback, ivec2(gl_FragCoord.xy) / VT_FEEDBACK_TILE, packed);
}

#define VT_FEEDBACK(m_idx, m_uv) vt_write_feedback(m_idx, m_uv)

const float VT_POOL_TEXELS = float(VT_POOL_TILES_X) * VT_STORED_PAGE_SIZE; // pool is square.

// Samples one integer VT mip: page-table (indirection) lookup -> physical page in the pool -> sample.
// UV is wrapped with fract() to emulate the REPEAT addressing StandardMaterial3D textures use by
// default (the direct path's material_sampler is REPEAT); a 1-texel seam can appear at the exact
// wrap boundary, acceptable for S0a (none in the no-op content).
vec4 vt_sample_mip(uint vt_id, vec2 uv, int mip) {
	VTMeta m = vt_meta.data[vt_id];
	int max_mip = max(int(min(m.mip_count, uint(VT_INDIRECTION_MIPS))) - 1, 0);
	int floor_mip = int(m.resident_mip_floor); // mips >= this are always resident (the streaming base).
	mip = clamp(mip, 0, max_mip);

	ivec2 mip_size = max(ivec2(int(m.width) >> mip, int(m.height) >> mip), ivec2(1));
	vec2 texel_coord = fract(uv) * vec2(mip_size); // fractional texel position within the mip
	ivec2 page = ivec2(texel_coord) / int(VT_PAGE_SIZE);
	uvec4 entry = texelFetch(usampler2DArray(vt_indirection, vt_point_sampler), ivec3(page, int(vt_id)), mip);

	// S0c streaming fallback: a finer page may not be resident yet (still streaming, or evicted). Drop
	// to the always-resident floor mip (coarser, never a hole) - entry.z != 0 means resident. The
	// feedback path still reports the FINE mip the fragment wanted, so it streams in for next frame.
	if (entry.z == 0u && mip < floor_mip) {
		mip = floor_mip;
		mip_size = max(ivec2(int(m.width) >> mip, int(m.height) >> mip), ivec2(1));
		texel_coord = fract(uv) * vec2(mip_size);
		page = ivec2(texel_coord) / int(VT_PAGE_SIZE);
		entry = texelFetch(usampler2DArray(vt_indirection, vt_point_sampler), ivec3(page, int(vt_id)), mip);
	}

	vec2 in_page = texel_coord - vec2(page * int(VT_PAGE_SIZE)); // [0, VT_PAGE_SIZE), fractional kept
	vec2 pool_texel = vec2(entry.xy) * VT_STORED_PAGE_SIZE + vec2(VT_PAGE_BORDER) + in_page;
	return textureLod(sampler2D(vt_page_pool, vt_pool_sampler), pool_texel / VT_POOL_TEXELS, 0.0);
}

// Trilinear virtual-texture sample: derivative-based LOD (matching what hardware would pick for the
// full-resolution source texture), blending the two bracketing VT mips.
vec4 sampleVirtual(uint vt_id, vec2 uv) {
	VTMeta m = vt_meta.data[vt_id];
	vec2 tex_size = vec2(max(m.width, 1u), max(m.height, 1u));
	vec2 dx = dFdx(uv) * tex_size;
	vec2 dy = dFdy(uv) * tex_size;
	float lod = max(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8)), 0.0);
	int mip_lo = int(floor(lod));
	float f = lod - float(mip_lo);
	return mix(vt_sample_mip(vt_id, uv, mip_lo), vt_sample_mip(vt_id, uv, mip_lo + 1), f);
}

#define SAMPLE_MATERIAL_TEX(m_idx, m_uv) sampleVirtual(m_idx, m_uv)

#else // !MESHLET_USE_VIRTUAL_TEXTURES

// Fixed-size descriptor array - MUST equal MeshletStorage::MAX_MATERIAL_TEXTURES (the renderer binds
// exactly that many, padding unused slots with a default white texture). The index is the
// per-instance-flat material_id's field, so it's dynamically uniform per draw the same way Forward+
// indexes lightmap_textures[] - no nonuniformEXT needed.
layout(set = 0, binding = 13) uniform texture2D material_textures[256];
layout(set = 0, binding = 14) uniform sampler material_sampler;

#define SAMPLE_MATERIAL_TEX(m_idx, m_uv) texture(sampler2D(material_textures[m_idx], material_sampler), m_uv)
#define VT_FEEDBACK(m_idx, m_uv) // no-op: no virtual texturing in this variant

#endif // MESHLET_USE_VIRTUAL_TEXTURES

// Volumetric fog (binding 23 + params at 25). Godot applies fog per-fragment during the FORWARD pass,
// but meshlet geometry is drawn in a late pass that Forward+ has already skipped, so without this it
// receives no fog at all - measured as a distance-correlated brightness/blue gap against the baseline
// (far terrain ~0.79 of the baseline, near terrain ~0.98). Sampled with radiance_sampler, which is
// linear+clamp exactly like Forward+'s SAMPLER_LINEAR_CLAMP.
layout(set = 0, binding = 23) uniform texture3D volumetric_fog_texture;
layout(set = 0, binding = 25, std140) uniform FogParams {
	vec4 fog0; // x = volumetric_fog_inv_length, y = detail_spread, zw = viewport size in pixels
	vec4 fog1; // xyz = camera forward (world), w = enabled (0 = off)
}
fog_params;

// Mirrors scene_forward_clustered.glsl's volumetric_fog_process(): the froxel volume is indexed by
// screen UV and a depth slice, with the slice distribution matching the CPU-side detail spread.
vec4 meshlet_volumetric_fog(vec2 screen_uv, float view_z) {
	vec3 fog_pos = vec3(screen_uv, view_z * fog_params.fog0.x);
	if (fog_pos.z < 0.0) {
		return vec4(0.0, 0.0, 0.0, 1.0); // behind the volume: fully transmissive, no inscatter
	} else if (fog_pos.z < 1.0) {
		fog_pos.z = pow(fog_pos.z, fog_params.fog0.y);
	}
	return texture(sampler3D(volumetric_fog_texture, radiance_sampler), fog_pos);
}

// Composite, matching Forward+'s `frag_color.rgb = frag_color.rgb * fog.a + fog.rgb` for the
// volumetric-only case (the texture already stores premultiplied inscatter + transmittance).
// ASSUMES the modern (non-legacy) fog blending path; Forward+ selects that with a specialization
// constant, which this path has no equivalent of.
vec3 meshlet_apply_fog(vec3 color, vec3 world_pos, vec3 camera_position) {
	if (fog_params.fog1.w < 0.5) {
		return color;
	}
	float view_z = dot(world_pos - camera_position, fog_params.fog1.xyz);
	vec2 screen_uv = gl_FragCoord.xy / max(fog_params.fog0.zw, vec2(1.0));
	vec4 fog = meshlet_volumetric_fog(screen_uv, view_z);
	return color * fog.a + fog.rgb;
}

#ifdef MESHLET_TERRAIN
// Terrain-variant fragment inputs. Same params UBO as the vertex stage (binding 21) plus the splat +
// per-material albedo maps packed into one array (22): [0]=splat0, [1]=splat1, [2..6]=material 0..4
// albedo. Sampled through material_sampler (binding 14). T2.3a = albedo only; normals/ORM/triplanar/hex
// are later slices.
layout(set = 0, binding = 21, std140) uniform TerrainParams {
	vec4 tp0; // x = terrain_size, y = height_min, z = height_range, w = height_scale
	vec4 tp_tiles; // per-material tile scale (metres/tile) for materials 0..3
	vec4 tp_extra; // x = tile scale for material 4, y = debug mode, z = splat weight cutoff, w = roughness_min
	vec4 tp_mat; // x = normal_strength, y = macro_variation_strength, z = water_level, w = shore_band
	// T2.3d de-tiling. The two schemes are per-material alternatives: hex when hex_strength > 0
	// (translation-only stochastic tiling, 3 taps/plane - used on rock, where the lattice shows),
	// otherwise the cheaper two-stamp variant blend.
	vec4 tp_var_str; // variant_strength for materials 0..3
	vec4 tp_var_scale; // variant blotch scale for materials 0..3
	vec4 tp_hex; // hex_strength for materials 0..3
	vec4 tp_var4; // x = variant_strength_4, y = variant_scale_4, z = hex_strength_4, w = variant_blend_width
	vec4 tp_var_rot; // x = variant_rotation, yzw = variant_offset
	vec4 tp_det; // per-material detail displacement depth, materials 0..3
	vec4 tp_det2; // x = detail_depth_4, y = tessellation_distance, z = tessellation_fade, w = detail_mip_bias
}
terrain_params;
// [0]=splat0, [1]=splat1, [2..6]=mat0..4 albedo, [7..11]=mat0..4 ORM, [12..16]=mat0..4 normal,
// [17]=macro_variation, [18..22]=mat0..4 height (read by the tessellation evaluation stage).
// Fixed size - the renderer binds exactly this many, padding gaps with white.
layout(set = 0, binding = 22) uniform texture2D terrain_textures[TERRAIN_SLOT_COUNT];

// T2.3b triplanar sample, mirroring terrain_viewer.gdshader's tri(): project the material from the
// three world planes and blend by the tri-weights. A macro rather than a function so the descriptor
// array is always indexed by a literal - no dynamically-uniform-indexing question to answer.
// m_ts is repeats per METRE (tiles / terrain_size), matching the gdshader's ts0..ts4. Getting this
// wrong is what made T2.3a's terrain washed out: it divided world position BY the tile count instead
// of multiplying by tiles/terrain_size, stretching every material ~15x and blurring it to mush.
#define TERRAIN_TRI(m_idx, m_wp, m_bw, m_ts) ( \
		texture(sampler2D(terrain_textures[m_idx], material_sampler), (m_wp).zy * (m_ts)).rgb * (m_bw).x + \
		texture(sampler2D(terrain_textures[m_idx], material_sampler), (m_wp).xz * (m_ts)).rgb * (m_bw).y + \
		texture(sampler2D(terrain_textures[m_idx], material_sampler), (m_wp).xy * (m_ts)).rgb * (m_bw).z)

// Selects a slot by LITERAL index, so the descriptor array is never dynamically indexed. The helpers
// below take the bare texture2D and build the combined sampler internally: glslang rejects a
// sampler2D() constructor passed as a call argument ("sampler constructor must appear at point of
// use"), so the construction has to happen inside the function that samples.
#define TERRAIN_TEX(m_idx) terrain_textures[m_idx]

// ── T2.3d de-tiling ──────────────────────────────────────────────────────────────────────────
// Ported from terrain_viewer.gdshader. Without this the meshlet path shows the RAW tile repeat that
// the Forward+ baseline hides, which is very visible here because the material tiles at ~4 m.
float terr_hash21(vec2 p) {
	p = fract(p * vec2(123.34, 345.45));
	p += dot(p, p + 34.345);
	return fract(p.x * p.y);
}

float terr_vnoise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = terr_hash21(i);
	float b = terr_hash21(i + vec2(1.0, 0.0));
	float c = terr_hash21(i + vec2(0.0, 1.0));
	float d = terr_hash21(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

vec3 terr_tri(texture2D tex, vec3 wp, vec3 bw, float ts) {
	return texture(sampler2D(tex, material_sampler), wp.zy * ts).rgb * bw.x + texture(sampler2D(tex, material_sampler), wp.xz * ts).rgb * bw.y + texture(sampler2D(tex, material_sampler), wp.xy * ts).rgb * bw.z;
}

// Where the second variant stamp reads from: world position rotated about Y and shifted. The
// ROTATION is what actually breaks the grid - a pure offset would slide the same pattern along,
// leaving its period and orientation intact.
vec3 terr_variant_pos(vec3 wp) {
	float c = cos(terrain_params.tp_var_rot.x);
	float s = sin(terrain_params.tp_var_rot.x);
	return vec3(c * wp.x - s * wp.z + terrain_params.tp_var_rot.y,
			wp.y + terrain_params.tp_var_rot.z,
			s * wp.x + c * wp.z + terrain_params.tp_var_rot.w);
}

// A/B selection mask. TRIPLANAR, and that is essential rather than a nicety: a plain xz mask barely
// changes as you climb a near-vertical face, so it goes constant up cliffs and the patchwork
// collapses. On flat ground bw.y dominates and it reduces to the xz mask.
float terr_variant_mix(vec3 wp, vec3 bw, float scale) {
	float m = terr_vnoise(wp.zy * scale) * bw.x + terr_vnoise(wp.xz * scale) * bw.y + terr_vnoise(wp.xy * scale) * bw.z;
	float bwid = terrain_params.tp_var4.w;
	return smoothstep(0.5 - bwid, 0.5 + bwid, m);
}

vec3 terr_tri_var(texture2D tex, vec3 wp, vec3 wpv, vec3 bw, float ts, float m, float strength) {
	vec3 a = terr_tri(tex, wp, bw, ts);
	if (strength <= 0.0) {
		return a; // disabled materials cost exactly what the plain triplanar path did
	}
	return mix(a, terr_tri(tex, wpv, bw, ts), m * strength);
}

// Same, for a tangent-space NORMAL map, which cannot be blended like plain colour: the variant is
// read from a position rotated by +variant_rotation, so its content lands rotated by
// -variant_rotation and its normal has to be turned to match, or variant patches light as though
// lit from a different direction than the ground they sit on.
vec3 terr_tri_var_normal(texture2D tex, vec3 wp, vec3 wpv, vec3 bw, float ts, float m, float strength) {
	vec3 a = terr_tri(tex, wp, bw, ts);
	if (strength <= 0.0) {
		return a;
	}
	vec3 b = terr_tri(tex, wpv, bw, ts);
	float c = cos(terrain_params.tp_var_rot.x);
	float s = sin(terrain_params.tp_var_rot.x);
	vec2 n = b.xy * 2.0 - 1.0;
	n = vec2(c * n.x + s * n.y, c * n.y - s * n.x);
	b.xy = n * 0.5 + 0.5;
	return mix(a, b, m * strength);
}

// Hex stochastic tiling (Heitz-Neyret): every hex cell gets its OWN random offset into the texture,
// so no two cells share a stamp and there is no period left to see - unlike the variant blend, which
// has only two stamps and both still tile. Translation-only (no per-cell rotation), so normals need
// no reorientation and directional textures keep their grain.
vec2 terr_hash22(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.xx + p3.yz) * p3.zy);
}

void terr_hex_weights(vec2 uv, out vec2 v1, out vec2 v2, out vec2 v3, out vec3 w) {
	vec2 sk = mat2(vec2(1.0, 0.0), vec2(-0.57735027, 1.15470054)) * (uv * 3.464);
	vec2 base = floor(sk);
	vec2 f = fract(sk);
	float cc = 1.0 - f.x - f.y;
	if (cc > 0.0) {
		w = vec3(cc, f.y, f.x);
		v1 = base;
		v2 = base + vec2(0.0, 1.0);
		v3 = base + vec2(1.0, 0.0);
	} else {
		w = vec3(-cc, 1.0 - f.y, 1.0 - f.x);
		v1 = base + vec2(1.0, 1.0);
		v2 = base + vec2(1.0, 0.0);
		v3 = base + vec2(0.0, 1.0);
	}
}

// textureGrad keeps the ORIGINAL uv derivatives, so each offset tap still picks the right mip -
// there are no meaningful derivatives across the random jump.
vec3 terr_hex2D(texture2D tex, vec2 uv) {
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);
	vec2 v1, v2, v3;
	vec3 w;
	terr_hex_weights(uv, v1, v2, v3, w);
	// Sharpen so most of the surface is dominated by a SINGLE tap; this narrows the blend zones where
	// three samples average together and wash out contrast (the classic hex artifact).
	w = w * w * w;
	w /= (w.x + w.y + w.z);
	return w.x * textureGrad(sampler2D(tex, material_sampler), uv + terr_hash22(v1), dx, dy).rgb +
			w.y * textureGrad(sampler2D(tex, material_sampler), uv + terr_hash22(v2), dx, dy).rgb +
			w.z * textureGrad(sampler2D(tex, material_sampler), uv + terr_hash22(v3), dx, dy).rgb;
}

vec3 terr_tri_hex(texture2D tex, vec3 wp, vec3 bw, float ts) {
	return terr_hex2D(tex, wp.zy * ts) * bw.x + terr_hex2D(tex, wp.xz * ts) * bw.y + terr_hex2D(tex, wp.xy * ts) * bw.z;
}

// Per-material selector: hex where the material enables it (rock, where the lattice shows), else the
// cheaper variant. Branch is on a uniform, so it is coherent across the whole draw.
vec3 terr_sample_mat(texture2D tex, vec3 wp, vec3 wpv, vec3 bw, float ts, float m, float vstr, float hstr) {
	if (hstr > 0.0) {
		return terr_tri_hex(tex, wp, bw, ts);
	}
	return terr_tri_var(tex, wp, wpv, bw, ts, m, vstr);
}

vec3 terr_sample_mat_n(texture2D tex, vec3 wp, vec3 wpv, vec3 bw, float ts, float m, float vstr, float hstr) {
	if (hstr > 0.0) {
		return terr_tri_hex(tex, wp, bw, ts); // translation-only, so no reorientation needed
	}
	return terr_tri_var_normal(tex, wp, wpv, bw, ts, m, vstr);
}
#endif

// All meshlet PBR shading (BRDF, light loop, SVOGI cone-trace, ambient) + meshlet_shade(). Included
// AFTER the buffer declarations above, which its functions reference by name.
#include "meshlet_shade_inc.glsl"

#endif

void main() {
#ifndef MESHLET_DEPTH_ONLY
#ifdef MESHLET_TERRAIN
	// T2.3a: splat-blended terrain albedo (top-down tiled UVs), lit through the SHARED meshlet light
	// loop (light_process_* + SVOGI + ambient) with a neutral non-metallic material carrying that
	// albedo - so terrain matches the rest of the meshlet path's lighting. Triplanar/hex/variant/ORM
	// and proper per-material roughness are later T2.3 slices; this is the albedo (colour) win.
	// Debug visualisation short-circuits shading entirely (see the vertex stage): unlit, so what's on
	// screen is exactly the value being probed, with no lighting or GI folded in.
	// Modes 1-9 are colour probes and short-circuit shading; modes 10+ change geometry instead and must
	// render normally so the surface shape is what's being read.
	// A vertex/eval-stage probe wrote a colour (>= 0); blit it unlit so what is on screen is exactly
	// the value being probed. -1 means no probe matched, so shade normally - that also covers the
	// geometry-override modes, which change the surface and are meant to be shaded.
	if (terrain_params.tp_extra.y >= 0.5 && debug_color_interp.r >= 0.0) {
		frag_color = vec4(debug_color_interp, 1.0);
		return;
	}
	vec3 wp = world_pos_interp;
	// The splat lookup stays TOP-DOWN - it is a map of the terrain seen from above, exactly as in the
	// gdshader's fragment (and as the vertex stage's displacement lookup).
	vec2 suv = wp.xz / terrain_params.tp0.x + 0.5;
	vec4 w0 = texture(sampler2D(terrain_textures[TERRAIN_SLOT_SPLAT0], material_sampler), suv);
	float w4 = texture(sampler2D(terrain_textures[TERRAIN_SLOT_SPLAT1], material_sampler), suv).r;

	// T2.3b: triplanar blend weights, sharpened (^4) so flat ground stays a clean top projection and
	// only real slopes pull in the side planes. The gdshader recomputes this normal per-pixel from the
	// displacement map; here it comes interpolated from the vertex stage, which derives it the same way
	// (finite differences on the heightmap) - close enough at this cluster density, and one less set of
	// fetches. Cliffs previously sampled the top projection alone, which smeared every vertical face.
	vec3 n_world = normalize(world_normal_interp);
	vec3 bw = pow(abs(n_world), vec3(4.0));
	bw /= (bw.x + bw.y + bw.z);

	// Repeats per metre, per material (gdshader ts0..ts4 = tiles / terrain_size).
	float inv_size = 1.0 / terrain_params.tp0.x;
	float ts0 = terrain_params.tp_tiles.x * inv_size;
	float ts1 = terrain_params.tp_tiles.y * inv_size;
	float ts2 = terrain_params.tp_tiles.z * inv_size;
	float ts3 = terrain_params.tp_tiles.w * inv_size;
	float ts4 = terrain_params.tp_extra.x * inv_size;

	// Each material is 3 fetches per map across the triplanar planes, and at any given pixel most of
	// the five have ~zero splat weight and contribute nothing - so skip them, exactly as the gdshader
	// does. The weights are normalised out below, so dropping a sub-cutoff material shifts the result
	// by less than the cutoff. Branch is on interpolated splat weights, coherent across large areas.
	// T2.3c: albedo + ORM + tangent-space normal, all three triplanar, per material.
	float cutoff = terrain_params.tp_extra.z;
	vec3 alb = vec3(0.0);
	vec3 orm = vec3(0.0);
	vec3 nrm = vec3(0.0);
	// The variant's read position depends only on world position, so compute it once for all five.
	// The A/B mask is per material (each has its own blotch scale), so it's computed per material.
	vec3 wpv = terr_variant_pos(wp);
	if (w0.r > cutoff) {
		float vm = terr_variant_mix(wp, bw, terrain_params.tp_var_scale.x);
		alb += terr_sample_mat(TERRAIN_TEX(TERRAIN_SLOT_ALBEDO + 0), wp, wpv, bw, ts0, vm, terrain_params.tp_var_str.x, terrain_params.tp_hex.x) * w0.r;
		orm += terr_sample_mat(TERRAIN_TEX(TERRAIN_SLOT_ORM + 0), wp, wpv, bw, ts0, vm, terrain_params.tp_var_str.x, terrain_params.tp_hex.x) * w0.r;
		nrm += terr_sample_mat_n(TERRAIN_TEX(TERRAIN_SLOT_NORMAL + 0), wp, wpv, bw, ts0, vm, terrain_params.tp_var_str.x, terrain_params.tp_hex.x) * w0.r;
	}
	if (w0.g > cutoff) {
		float vm = terr_variant_mix(wp, bw, terrain_params.tp_var_scale.y);
		alb += terr_sample_mat(TERRAIN_TEX(TERRAIN_SLOT_ALBEDO + 1), wp, wpv, bw, ts1, vm, terrain_params.tp_var_str.y, terrain_params.tp_hex.y) * w0.g;
		orm += terr_sample_mat(TERRAIN_TEX(TERRAIN_SLOT_ORM + 1), wp, wpv, bw, ts1, vm, terrain_params.tp_var_str.y, terrain_params.tp_hex.y) * w0.g;
		nrm += terr_sample_mat_n(TERRAIN_TEX(TERRAIN_SLOT_NORMAL + 1), wp, wpv, bw, ts1, vm, terrain_params.tp_var_str.y, terrain_params.tp_hex.y) * w0.g;
	}
	if (w0.b > cutoff) {
		float vm = terr_variant_mix(wp, bw, terrain_params.tp_var_scale.z);
		alb += terr_sample_mat(TERRAIN_TEX(TERRAIN_SLOT_ALBEDO + 2), wp, wpv, bw, ts2, vm, terrain_params.tp_var_str.z, terrain_params.tp_hex.z) * w0.b;
		orm += terr_sample_mat(TERRAIN_TEX(TERRAIN_SLOT_ORM + 2), wp, wpv, bw, ts2, vm, terrain_params.tp_var_str.z, terrain_params.tp_hex.z) * w0.b;
		nrm += terr_sample_mat_n(TERRAIN_TEX(TERRAIN_SLOT_NORMAL + 2), wp, wpv, bw, ts2, vm, terrain_params.tp_var_str.z, terrain_params.tp_hex.z) * w0.b;
	}
	if (w0.a > cutoff) {
		float vm = terr_variant_mix(wp, bw, terrain_params.tp_var_scale.w);
		alb += terr_sample_mat(TERRAIN_TEX(TERRAIN_SLOT_ALBEDO + 3), wp, wpv, bw, ts3, vm, terrain_params.tp_var_str.w, terrain_params.tp_hex.w) * w0.a;
		orm += terr_sample_mat(TERRAIN_TEX(TERRAIN_SLOT_ORM + 3), wp, wpv, bw, ts3, vm, terrain_params.tp_var_str.w, terrain_params.tp_hex.w) * w0.a;
		nrm += terr_sample_mat_n(TERRAIN_TEX(TERRAIN_SLOT_NORMAL + 3), wp, wpv, bw, ts3, vm, terrain_params.tp_var_str.w, terrain_params.tp_hex.w) * w0.a;
	}
	if (w4 > cutoff) {
		float vm = terr_variant_mix(wp, bw, terrain_params.tp_var4.y);
		alb += terr_sample_mat(TERRAIN_TEX(TERRAIN_SLOT_ALBEDO + 4), wp, wpv, bw, ts4, vm, terrain_params.tp_var4.x, terrain_params.tp_var4.z) * w4;
		orm += terr_sample_mat(TERRAIN_TEX(TERRAIN_SLOT_ORM + 4), wp, wpv, bw, ts4, vm, terrain_params.tp_var4.x, terrain_params.tp_var4.z) * w4;
		nrm += terr_sample_mat_n(TERRAIN_TEX(TERRAIN_SLOT_NORMAL + 4), wp, wpv, bw, ts4, vm, terrain_params.tp_var4.x, terrain_params.tp_var4.z) * w4;
	}
	float wsum = w0.r + w0.g + w0.b + w0.a + w4;
	if (wsum > 0.0001) {
		alb /= wsum;
		orm /= wsum;
		nrm /= wsum;
	} else {
		alb = vec3(0.5);
		nrm = vec3(0.5, 0.5, 1.0); // flat tangent-space normal
	}

	// Large-scale brightness variation, baked unique across the whole map (never tiles), to break up
	// the repeat of the tiled materials. mv*2 recentres the stored 0.5-neutral field on 1.0.
	vec3 mv = texture(sampler2D(terrain_textures[TERRAIN_SLOT_MACRO_VARIATION], material_sampler), suv).rgb;
	alb *= mix(vec3(1.0), mv * 2.0, terrain_params.tp_mat.y);

	// Shoreline wetness: darken and smooth the ground from the waterline up to shore_band metres
	// above it, so there's a believable wet margin before the water plane takes over.
	float shore_wet = 1.0 - clamp((wp.y - terrain_params.tp_mat.z) / max(terrain_params.tp_mat.w, 0.0001), 0.0, 1.0);
	alb *= mix(1.0, 0.5, shore_wet);

	// Apply the blended tangent-space normal. The base terrain mesh is a PlaneMesh whose tangent is
	// world +X, so rebuild that frame here: Gram-Schmidt +X against the surface normal, bitangent from
	// the cross product. (Godot's own path gets the same frame from the mesh TANGENT stream, which the
	// meshlet vertex format doesn't carry - see [[meshlet-pbr-derived-tangents]] for the standard
	// path's equivalent.) tp_mat.x is normal_strength, matching NORMAL_MAP_DEPTH.
	// Z is RECONSTRUCTED from XY, not read from the blue channel, exactly as Godot's own NORMAL_MAP
	// path does - and for the same reason: normal maps import as RGTC/BC5, which stores only two
	// channels, so blue carries nothing usable. Reading nrm.z instead produced near-tangent-plane
	// normals and shaded the terrain as a mass of hard cracks (verified: debug mode 19, which drops the
	// normal map entirely, removes them completely).
	vec2 n_xy = nrm.xy * 2.0 - 1.0;
	float n_z = sqrt(max(0.0, 1.0 - dot(n_xy, n_xy)));
	vec3 T = normalize(vec3(1.0, 0.0, 0.0) - n_world * n_world.x);
	vec3 B = cross(n_world, T);
	// normal_strength is applied as Godot applies NORMAL_MAP_DEPTH: a mix between the geometric and
	// mapped normal, not a scaling of the tangent-space XY.
	vec3 n_mapped = normalize(T * n_xy.x + B * n_xy.y + n_world * n_z);
	vec3 N = normalize(mix(n_world, n_mapped, clamp(terrain_params.tp_mat.x, 0.0, 1.0)));
	// 19 = normal map OFF (geometric normal only), to separate "the TBN/normal map is wrong" from
	// "the albedo blend is wrong" when comparing against the Forward+ baseline at the same pose.
	// 20 = unlit albedo, the T2.3a quantity on its own with no lighting or GI folded in.
	int terr_fdbg = int(terrain_params.tp_extra.y + 0.5);
	if (terr_fdbg == 19) {
		N = n_world;
	} else if (terr_fdbg == 20) {
		frag_color = vec4(alb, 1.0);
		return;
	}
	vec3 V = normalize(params.camera_position - wp);
	MeshletMaterial tmat = meshlet_materials.data[0];
	// ORM: r = ambient occlusion, g = roughness, b = metallic (same packing the gdshader reads).
	// Wet ground reads smoother, so the shoreline band pulls roughness down as it darkens albedo.
	tmat.metallic = orm.b;
	tmat.roughness = clamp(max(orm.g, terrain_params.tp_extra.w) * mix(1.0, 0.4, shore_wet), 0.04, 1.0);
	tmat.emission = vec3(0.0);
	vec3 diffuse_light = vec3(0.0);
	vec3 specular_light = vec3(0.0);
	for (uint i = 0u; i < params.light_count; i++) {
		if (lights.data[i].is_directional != 0u) {
			light_process_directional(i, N, V, tmat, alb, diffuse_light, specular_light);
		} else if (lights.data[i].cone_angle < 1.0) {
			light_process_spot(i, wp, N, V, tmat, alb, diffuse_light, specular_light);
		} else {
			light_process_omni(i, wp, N, V, tmat, alb, diffuse_light, specular_light);
		}
	}
	vec3 gi_diffuse = vec3(0.0);
	if (params.svogi_bounds.w > 0.0) {
		float vsz = params.svogi_bounds.w / 32.0;
		gi_diffuse = svogi_hemisphere_gather(wp, N, vsz * 3.0, params.svogi_bounds.w * 2.0, params.svogi_bounds.xyz, params.svogi_bounds.w, params.svogi_params.x);
	}
	vec3 ambient = params.ambient_color.rgb;
	if (params.ambient_color.a > 0.0) {
		// Sky ambient, sampled in the direction of the surface NORMAL (diffuse irradiance) rather than
		// from one fixed texel. This mirrors scene_forward_clustered.glsl's ambient_dir block, using the
		// same shared vec3_to_oct_with_border() so the octahedral convention matches the sky bake
		// exactly. The old fixed (0.5,0.5) fetch gave every surface the same colour regardless of
		// orientation, so upward-facing geometry never picked up the blue of the sky - measured as a
		// consistent blue deficit against Forward+.
		// LIMITATION: Forward+ first maps the direction through radiance_inverse_xform (view -> sky
		// space). Here the normal is already WORLD space, so this is exact whenever sky space == world
		// space (an unrotated sky) and only drifts if the sky is rotated. The meshlet push constant is
		// at the 128-byte Vulkan floor, so there is no room to pass that basis without restructuring.
		vec2 sky_uv = vec3_to_oct_with_border(normalize(N), vec2(params.svogi_params.w, 1.0 - params.svogi_params.w * 2.0));
		vec3 sky_ambient = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(sky_uv, params.svogi_params.z), 0.0).rgb * params.svogi_params.y;
		ambient = mix(ambient, sky_ambient, params.ambient_color.a);
	}
	// Sky specular, through the same shared helper the standard meshlet path uses (defined in
	// meshlet_shade_inc.glsl) so terrain cannot drift from it. Terrain is non-metallic, so f0 is the
	// dielectric IOR term only.
	if (params.ambient_color.a > 0.0) {
		float terr_f0_ior = pow((tmat.ior - 1.0) / (tmat.ior + 1.0), 2.0);
		specular_light += meshlet_sky_specular(N, V, vec3(terr_f0_ior), tmat.roughness, 0.0, params.svogi_params.w, params.svogi_params.y, params.svogi_params.z) * orm.r;
	}

	// AO darkens only the ambient/indirect terms, never the direct light - occluding a surface from
	// the sky should not also dim the sun hitting it.
	frag_color = vec4(meshlet_apply_fog(alb * (diffuse_light + (ambient + gi_diffuse) * orm.r) + specular_light, wp, params.camera_position), 1.0);
#else
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
			false, // gi_precomputed: direct-raster color path has no filtered-GI texture; trace inline.
			vec3(0.0),
			do_discard);
	if (do_discard) {
		discard;
	}
	frag_color = vec4(meshlet_apply_fog(shaded.rgb, world_pos_interp, params.camera_position), shaded.a);
#endif // MESHLET_TERRAIN
#endif // !MESHLET_DEPTH_ONLY
	// MESHLET_DEPTH_ONLY: no color output at all - this variant targets a depth-only
	// framebuffer (Forward+'s real depth pre-pass framebuffer, which has zero color
	// attachments) for the temporal early pass; depth write/test happens via fixed-function
	// state regardless of what (if anything) the fragment shader writes.
}

#[tesc]

#version 450

#VERSION_DEFINES

// T2.4a - tessellation control for the meshlet path, pass-through.
//
// Forwards the vertex stage's clip position and every VS->FS varying unchanged (per control point)
// and sets a uniform tessellation level of 1.0, at which the evaluation stage reproduces the three
// original corners exactly - so a tessellated terrain draw is pixel-identical to the untessellated
// one. Adaptive levels and detail displacement build on this parity baseline, exactly as the
// Forward+ feature did (see scene_forward_clustered.glsl's equivalent stages).
//
// These stages are compiled only for the shader version MeshletRenderer flags as tessellated, so the
// standard/VT/depth-only draws are untouched.

layout(vertices = 3) out;

in gl_PerVertex {
	vec4 gl_Position;
} gl_in[gl_MaxPatchVertices];

out gl_PerVertex {
	vec4 gl_Position;
} gl_out[];

// Must mirror the vertex stage's output block exactly, location for location.
layout(location = 0) in vec3 in_world_normal[];
layout(location = 1) in flat uint in_meshlet_index[];
layout(location = 2) in flat uint in_material_id[];
layout(location = 3) in vec3 in_world_pos[];
layout(location = 4) in flat vec3 in_instance_pos[];
layout(location = 5) in vec2 in_uv[];
#ifdef MESHLET_TERRAIN
layout(location = 6) in vec3 in_debug_color[];
#endif

layout(location = 0) out vec3 tc_world_normal[];
layout(location = 1) out flat uint tc_meshlet_index[];
layout(location = 2) out flat uint tc_material_id[];
layout(location = 3) out vec3 tc_world_pos[];
layout(location = 4) out flat vec3 tc_instance_pos[];
layout(location = 5) out vec2 tc_uv[];
#ifdef MESHLET_TERRAIN
layout(location = 6) out vec3 tc_debug_color[];
#endif

// Per-draw params block, shared with the fragment stage (which reads its fog half). Only .zw is
// needed here: the viewport in pixels. Resources match by set/binding, not by block instance name.
// Reading it from a tessellation stage is safe in THIS path - every meshlet binding lives in set 0
// and the set is built per-draw from this shader's own reflection, so adding the TCS stage flag stays
// self-consistent. The Forward+ tessellation feature cannot do this (its TCS/TES must never touch the
// shared scene sets) and has to smuggle the viewport through spare varying .w lanes instead.
layout(set = 0, binding = 25, std140) uniform FogParams {
	vec4 fog0; // .zw = viewport size in pixels
	vec4 fog1;
}
tess_draw_params;

#define TESS_VIEWPORT_SIZE tess_draw_params.fog0.zw
// Target on-screen length of each generated edge, and the ceiling on subdivision. Max 4 mirrors the
// Forward+ feature's choice: adaptivity should only ever REMOVE work relative to a fixed level.
const float TESS_TARGET_EDGE_PX = 16.0;
const float TESS_MAX_LEVEL = 4.0;

// Clip -> pixel position. w is clamped PER CONTROL POINT (never using the other end of the edge), so
// the function stays symmetric: a control point at or behind the eye yields a huge edge length, which
// saturates to the max level - the right answer for something on top of the camera.
vec2 _tess_screen_pos(vec4 clip, vec2 vp) {
	float w = max(abs(clip.w), 1e-4);
	return (clip.xy / w) * 0.5 * vp;
}

float _tess_edge_level(vec2 a, vec2 b) {
	return clamp(distance(a, b) / TESS_TARGET_EDGE_PX, 1.0, TESS_MAX_LEVEL);
}

void main() {
	// Per-vertex outputs MUST be indexed by the literal gl_InvocationID - glslang rejects a copied
	// int here ("per-vertex output l-value must be indexed with gl_InvocationID").
	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
	tc_world_normal[gl_InvocationID] = in_world_normal[gl_InvocationID];
	tc_meshlet_index[gl_InvocationID] = in_meshlet_index[gl_InvocationID];
	tc_material_id[gl_InvocationID] = in_material_id[gl_InvocationID];
	tc_world_pos[gl_InvocationID] = in_world_pos[gl_InvocationID];
	tc_instance_pos[gl_InvocationID] = in_instance_pos[gl_InvocationID];
	tc_uv[gl_InvocationID] = in_uv[gl_InvocationID];
#ifdef MESHLET_TERRAIN
	tc_debug_color[gl_InvocationID] = in_debug_color[gl_InvocationID];
#endif

	if (gl_InvocationID == 0) {
		// T2.4b - adaptive level from SCREEN-SPACE EDGE LENGTH.
		//
		// CRACK-FREE RULE: each gl_TessLevelOuter[i] must be a symmetric function of ONLY that edge's
		// two control points. A neighbouring patch sharing the edge sees the same two points with
		// bit-identical gl_Position, so distance() gives it bit-identical levels and the surfaces meet
		// exactly. Bringing in the third point (e.g. patch area) would break that and tear every
		// shared edge. outer[i] is the edge OPPOSITE control point i.
		//
		// The viewport arrives through the params UBO rather than a varying: unlike the Forward+
		// tessellation feature, this stage CAN read the descriptor set (everything is set 0, built
		// from this shader's own reflection), so no .w-lane packing is needed.
		vec2 vp = max(TESS_VIEWPORT_SIZE, vec2(1.0));
		vec2 p0 = _tess_screen_pos(gl_in[0].gl_Position, vp);
		vec2 p1 = _tess_screen_pos(gl_in[1].gl_Position, vp);
		vec2 p2 = _tess_screen_pos(gl_in[2].gl_Position, vp);

		gl_TessLevelOuter[0] = _tess_edge_level(p1, p2);
		gl_TessLevelOuter[1] = _tess_edge_level(p2, p0);
		gl_TessLevelOuter[2] = _tess_edge_level(p0, p1);
		// Inner takes the max so a patch with finely-split edges doesn't stay coarse through the middle.
		gl_TessLevelInner[0] = max(gl_TessLevelOuter[0], max(gl_TessLevelOuter[1], gl_TessLevelOuter[2]));
	}
}

#[tese]

#version 450

#VERSION_DEFINES

// T2.4a - tessellation evaluation for the meshlet path, pass-through.
// `cw` matches the pipeline's front face (RD defaults to POLYGON_FRONT_FACE_CLOCKWISE and the meshlet
// pipeline does not override it), so generated triangles keep the same facing as the input patch.

layout(triangles, equal_spacing, cw) in;

// Same push constant the vertex stage uses. Declaring it here lets the evaluation stage re-project a
// displaced vertex with the EXACT transform the vertex stage applied, instead of approximating the
// displacement as a clip-space offset. RD derives the push-constant stage flags from reflection
// across this shader's own stages, so adding the stage stays self-consistent.
layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec3 camera_position;
	uint light_count;
	vec4 ambient_color;
	vec4 svogi_bounds;
	vec4 svogi_params;
}
params;

in gl_PerVertex {
	vec4 gl_Position;
} gl_in[gl_MaxPatchVertices];

out gl_PerVertex {
	vec4 gl_Position;
};

layout(location = 0) in vec3 tc_world_normal[];
layout(location = 1) in flat uint tc_meshlet_index[];
layout(location = 2) in flat uint tc_material_id[];
layout(location = 3) in vec3 tc_world_pos[];
layout(location = 4) in flat vec3 tc_instance_pos[];
layout(location = 5) in vec2 tc_uv[];
#ifdef MESHLET_TERRAIN
layout(location = 6) in vec3 tc_debug_color[];
#endif

// Names match the fragment stage's inputs, which is what the interface is validated against.
layout(location = 0) out vec3 world_normal_interp;
layout(location = 1) out flat uint meshlet_index_interp;
layout(location = 2) out flat uint material_id_interp;
layout(location = 3) out vec3 world_pos_interp;
layout(location = 4) out flat vec3 instance_pos_interp;
layout(location = 5) out vec2 uv_interp;
#ifdef MESHLET_TERRAIN
layout(location = 6) out vec3 debug_color_interp;
#endif

#ifdef MESHLET_TERRAIN
// The evaluation stage reads the terrain params directly - possible here because every meshlet
// binding is in set 0 and the uniform set is built from this shader's own reflection. Needed for the
// tessellation debug view now, and for detail displacement next.
layout(set = 0, binding = 21, std140) uniform TerrainParams {
	vec4 tp0; // x = terrain_size, y = height_min, z = height_range, w = height_scale
	vec4 tp_tiles; // per-material tile scale (world metres per tile) for materials 0..3
	vec4 tp_extra; // x = tile scale for material 4, y = debug mode, z = splat weight cutoff, w = roughness_min
	vec4 tp_mat; // x = normal_strength, y = macro_variation_strength, z = water_level, w = shore_band
	// T2.3d de-tiling. The two schemes are per-material alternatives: hex when hex_strength > 0
	// (translation-only stochastic tiling, 3 taps/plane - used on rock, where the lattice shows),
	// otherwise the cheaper two-stamp variant blend.
	vec4 tp_var_str; // variant_strength for materials 0..3
	vec4 tp_var_scale; // variant blotch scale for materials 0..3
	vec4 tp_hex; // hex_strength for materials 0..3
	vec4 tp_var4; // x = variant_strength_4, y = variant_scale_4, z = hex_strength_4, w = variant_blend_width
	vec4 tp_var_rot; // x = variant_rotation, yzw = variant_offset
	vec4 tp_det; // per-material detail displacement depth, materials 0..3
	vec4 tp_det2; // x = detail_depth_4, y = tessellation_distance, z = tessellation_fade, w = detail_mip_bias
}
terrain_params;
#endif

#ifdef MESHLET_TERRAIN
// The detail height maps + their sampler, read PER GENERATED VERTEX. This path can sample textures
// straight from a tessellation stage because every meshlet binding is in set 0 and the uniform set is
// built from this shader's own reflection - the Forward+ feature had to route around the shared-set
// wall with samplerless texelFetch and a hand-written bilinear filter.
layout(set = 0, binding = 14) uniform sampler material_sampler;
layout(set = 0, binding = 22) uniform texture2D terrain_textures[TERRAIN_SLOT_COUNT];

// Triplanar height tap, mirroring terrain_viewer.gdshader's DETAIL_H. textureLod, never texture():
// a tessellation stage has NO derivatives, so the mip must be explicit or the result is undefined.
// The 0.5 subtraction centres the relief on the base surface so displacement straddles it instead of
// inflating everything outward. -0.5..+0.5 scaled by the material's depth and its splat weight.
#define TERRAIN_DETAIL_H(m_idx, m_wp, m_bw, m_ts, m_wgt, m_depth, m_mip) 	if ((m_wgt) > cutoff && (m_depth) > 0.0) { 		h += ((textureLod(sampler2D(terrain_textures[m_idx], material_sampler), (m_wp).zy * (m_ts), (m_mip)).r * (m_bw).x + 				textureLod(sampler2D(terrain_textures[m_idx], material_sampler), (m_wp).xz * (m_ts), (m_mip)).r * (m_bw).y + 				textureLod(sampler2D(terrain_textures[m_idx], material_sampler), (m_wp).xy * (m_ts), (m_mip)).r * (m_bw).z) - 0.5) * (m_depth) * (m_wgt); 	}
#endif

#define TESS_INTERP(m_a) (gl_TessCoord.x * (m_a)[0] + gl_TessCoord.y * (m_a)[1] + gl_TessCoord.z * (m_a)[2])

void main() {
	gl_Position = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position + gl_TessCoord.z * gl_in[2].gl_Position;

	world_normal_interp = TESS_INTERP(tc_world_normal);
	world_pos_interp = TESS_INTERP(tc_world_pos);

#ifdef MESHLET_TERRAIN
	// T2.4c - detail displacement, evaluated PER GENERATED VERTEX. This is what makes tessellation do
	// something: interpolating a per-corner scalar would be a linear function and could never add a
	// spatial frequency the base triangle doesn't already carry (the exact trap the Forward+ feature
	// fell into and had to undo - see [[adaptive-tessellation]] P4f).
	{
		vec3 wp = world_pos_interp;
		vec3 n = normalize(world_normal_interp);
		float view_depth = gl_Position.w; // descriptor-free view depth, in world units
		float tess_dist = terrain_params.tp_det2.y;
		// Past the cutoff the relief has faded to nothing, so skip the sampling entirely - that guard
		// is the whole perf win. CRACK-FREE because both the fade and the cutoff are per-vertex
		// functions of view depth, so a shared edge's two ends agree, and the fade reaches 0 exactly
		// AT the cutoff so skipped-flat patches meet still-sampled ones seamlessly.
		if (view_depth < tess_dist) {
			float fade = clamp((tess_dist - view_depth) / max(terrain_params.tp_det2.z, 0.0001), 0.0, 1.0);
			// Same sharpened triplanar weights and top-down splat lookup the fragment uses, so relief
			// and texture project identically.
			vec3 bw = pow(abs(n), vec3(4.0));
			bw /= (bw.x + bw.y + bw.z);
			vec2 suv = wp.xz / terrain_params.tp0.x + 0.5;
			vec4 w0 = texture(sampler2D(terrain_textures[TERRAIN_SLOT_SPLAT0], material_sampler), suv);
			float w4 = texture(sampler2D(terrain_textures[TERRAIN_SLOT_SPLAT1], material_sampler), suv).r;

			float inv_size = 1.0 / terrain_params.tp0.x;
			float cutoff = terrain_params.tp_extra.z;
			float mip = terrain_params.tp_det2.w;
			float h = 0.0;
			TERRAIN_DETAIL_H(TERRAIN_SLOT_HEIGHT + 0, wp, bw, terrain_params.tp_tiles.x * inv_size, w0.r, terrain_params.tp_det.x, mip)
			TERRAIN_DETAIL_H(TERRAIN_SLOT_HEIGHT + 1, wp, bw, terrain_params.tp_tiles.y * inv_size, w0.g, terrain_params.tp_det.y, mip)
			TERRAIN_DETAIL_H(TERRAIN_SLOT_HEIGHT + 2, wp, bw, terrain_params.tp_tiles.z * inv_size, w0.b, terrain_params.tp_det.z, mip)
			TERRAIN_DETAIL_H(TERRAIN_SLOT_HEIGHT + 3, wp, bw, terrain_params.tp_tiles.w * inv_size, w0.a, terrain_params.tp_det.w, mip)
			TERRAIN_DETAIL_H(TERRAIN_SLOT_HEIGHT + 4, wp, bw, terrain_params.tp_extra.x * inv_size, w4, terrain_params.tp_det2.x, mip)
			float wsum = w0.r + w0.g + w0.b + w0.a + w4;
			if (wsum > 0.0001) {
				h /= wsum;
			}

			// Offset along the surface normal and RE-PROJECT EXACTLY, reusing the vertex stage's own
			// transform - the push constant is visible here, so there is no need for the clip-space
			// offset approximation the Forward+ TES has to use. No depth bias is re-applied: terrain
			// sets the owns-own-depth bit, so the vertex stage's bias is 0 for this stream.
			world_pos_interp = wp + n * (h * fade);
			gl_Position = params.view_projection * vec4(world_pos_interp - params.camera_position, 1.0);
		}
	}
#endif
	uv_interp = TESS_INTERP(tc_uv);
	// Flat varyings are constant across the patch - take corner 0 rather than interpolating.
	meshlet_index_interp = tc_meshlet_index[0];
	material_id_interp = tc_material_id[0];
	instance_pos_interp = tc_instance_pos[0];
#ifdef MESHLET_TERRAIN
	debug_color_interp = TESS_INTERP(tc_debug_color);
	// Debug mode 21: patch parameterisation (barycentric coordinate as colour). USEFUL FOR PATCH
	// LAYOUT, USELESS FOR LEVEL - gl_TessCoord is continuous across the whole patch, so a subdivided
	// patch renders as a SMOOTHER gradient, not as discrete cells, and looks essentially identical at
	// level 1 and level 4. Read the level with mode 22 instead; I misread this view as "no
	// subdivision" once already.
	// Mode 22: the level the control stage actually chose, quantised (red 1 / green 2 / blue 3 /
	// white >=4). Read from gl_TessLevelInner directly - visualising gl_TessCoord CANNOT show this,
	// because it is continuous across the patch and so looks identical at every level.
	if (int(terrain_params.tp_extra.y + 0.5) == 22) {
		float lvl = gl_TessLevelInner[0];
		debug_color_interp = (lvl < 1.5) ? vec3(1.0, 0.0, 0.0) : ((lvl < 2.5) ? vec3(0.0, 1.0, 0.0) : ((lvl < 3.5) ? vec3(0.0, 0.0, 1.0) : vec3(1.0)));
	}
#endif
}
