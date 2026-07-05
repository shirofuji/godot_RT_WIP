// Meshlet shading data layouts + constants, shared by the meshlet color fragment shader
// (meshlet_render.glsl) and - once the visibility-buffer path lands - the material-resolve compute
// pass. Structs mirror their C++ counterparts (std430) exactly; include this BEFORE declaring the
// storage buffers that use these struct types, then include meshlet_shade_inc.glsl AFTER the buffer
// declarations (the shading functions reference those buffers by name).

const float M_PI = 3.14159265358979323846;
const uint MESHLET_TEXTURE_NONE = 0xFFFFFFFFu;

// Mirrors MeshletStorage::MeshletMaterialGPU exactly (std430 layout) - a flattened snapshot of the
// subset of StandardMaterial3D/ORMMaterial3D parameters this pipeline reads.
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

// Mirrors RenderForwardClustered::MeshletLightGPU exactly (std430 layout, 64 bytes) - real scene
// lights (directional/omni/spot) extracted CPU-side via LightStorage's public getters. Color is
// energy-premultiplied. No spatial culling: every light up to MESHLET_MAX_LIGHTS is evaluated for
// every fragment unconditionally.
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

// SVOGI sparse-voxel-octree node. Mirrors svogi_voxelize.glsl's Node struct exactly (32 bytes). The
// octree is built in ABSOLUTE world space; the shading functions cone-trace it directly in world
// space (see meshlet_shade_inc.glsl's svogi_cone_trace).
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
