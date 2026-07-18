#[compute]

#version 450

#VERSION_DEFINES

// Point-injection voxelization for the SVOGI octree. Companion to svogi_voxelize.glsl (which voxelizes
// meshlet TRIANGLES): this pass takes a flat buffer of world-space surface VOXELS - {position, normal,
// albedo} - and injects each into the SAME octree, so geometry that has no meshlet data (e.g. the
// runtime marching-cubes terrain, whose voxel grid is the source) still contributes GI. Runs AFTER the
// meshlet voxelize into the same buffer (no clear between), so flora + terrain coexist in one octree.
// The octree topology build (atomic alloc + publish) and the LIT-RADIANCE store mirror svogi_voxelize's
// MODE_VOXELIZE exactly.

#define MAX_NODES 16777216

struct Node {
	uint children_base_index;
	uint child_mask;
	uint albedo;
	uint normal;
	uint emission;
	uint pad[3];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict coherent buffer OctreeNodes {
	Node nodes[];
};

layout(set = 0, binding = 1, std430) restrict coherent buffer AtomicCounter {
	uint alloc_counter;
	uint alloc_pad[3];
};

// Each point: pos_albedo.xyz = world position, pos_albedo.w = albedo as uintBitsToFloat(rgba8),
// normal.xyz = surface normal (for the sun N.L), normal.w unused. 32 bytes, std430-clean.
struct GIVoxelPoint {
	vec4 pos_albedo;
	vec4 normal;
};

layout(set = 1, binding = 0, std430) restrict readonly buffer Points {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
	GIVoxelPoint data[];
}
points;

// Mirrors RenderForwardClustered::MeshletLightGPU / meshlet_render.glsl's MeshletLight (64 bytes) - same
// light buffer the meshlet voxelize is handed, so terrain voxels get the same direct lighting as flora.
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

layout(set = 1, binding = 1, std430) restrict readonly buffer Lights {
	MeshletLight data[];
} lights;

layout(push_constant, std430) uniform Params {
	vec3 bounds_center;
	float bounds_half_size;
	uint max_nodes;
	uint light_count;
	uint pad[2];
} params;

float voxel_omni_attenuation(float distance, float inv_range, float decay) {
	float nd = distance * inv_range;
	nd *= nd;
	nd *= nd; // nd^4
	nd = max(1.0 - nd, 0.0);
	nd *= nd; // nd^2
	return nd * pow(max(distance, 0.0001), -decay);
}

// Diffuse-only direct lighting for a voxel (mirrors svogi_voxelize.glsl's voxel_direct_light).
vec3 voxel_direct_light(vec3 p_pos, vec3 p_normal) {
	vec3 acc = vec3(0.0);
	for (uint i = 0u; i < params.light_count; i++) {
		vec3 L;
		float attenuation = 1.0;
		if (lights.data[i].is_directional != 0u) {
			L = normalize(-lights.data[i].direction);
		} else {
			vec3 rel = lights.data[i].position - p_pos;
			float len = length(rel);
			L = rel / max(len, 0.0001);
			attenuation = voxel_omni_attenuation(len, lights.data[i].inv_radius, lights.data[i].attenuation);
			if (lights.data[i].cone_angle < 1.0) {
				float cone_angle = lights.data[i].cone_angle;
				float scos = max(dot(-L, lights.data[i].direction), cone_angle);
				float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - cone_angle));
				attenuation *= 1.0 - pow(spot_rim, lights.data[i].cone_attenuation);
			}
		}
		float ndotl = max(dot(p_normal, L), 0.0);
		acc += lights.data[i].color * (ndotl * attenuation);
	}
	return acc;
}

void main() {
	uint idx = gl_GlobalInvocationID.x;
	if (idx >= points.count) {
		return;
	}

	GIVoxelPoint p = points.data[idx];
	vec3 world_pos = p.pos_albedo.xyz;
	vec3 normal = p.normal.xyz;
	uint albedo_packed = floatBitsToUint(p.pos_albedo.w);
	vec3 albedo = vec3(
			float((albedo_packed >> 24u) & 0xFFu),
			float((albedo_packed >> 16u) & 0xFFu),
			float((albedo_packed >> 8u) & 0xFFu)) /
			255.0;

	vec3 diff = abs(world_pos - params.bounds_center);
	if (diff.x > params.bounds_half_size || diff.y > params.bounds_half_size || diff.z > params.bounds_half_size) {
		return;
	}

	uint node_idx = 0u;
	vec3 current_center = params.bounds_center;
	float current_half_size = params.bounds_half_size;

	for (uint depth = 0u; depth < 6u; depth++) {
		bvec3 is_pos = greaterThan(world_pos, current_center);
		uint child_idx = (is_pos.x ? 1u : 0u) | ((is_pos.y ? 1u : 0u) << 1) | ((is_pos.z ? 1u : 0u) << 2);

		vec3 offset = vec3(is_pos.x ? 1.0 : -1.0, is_pos.y ? 1.0 : -1.0, is_pos.z ? 1.0 : -1.0);
		current_half_size *= 0.5;
		current_center += offset * current_half_size;

		uint base_idx = atomicOr(nodes[node_idx].children_base_index, 0u);
		if (base_idx == 0u) {
			uint new_base = atomicAdd(alloc_counter, 8u);
			if (new_base >= params.max_nodes - 8u) {
				break;
			}
			for (uint c = 0u; c < 8u; c++) {
				nodes[new_base + c].children_base_index = 0u;
				nodes[new_base + c].child_mask = 0u;
				nodes[new_base + c].albedo = 0u;
				nodes[new_base + c].normal = 0u;
				nodes[new_base + c].emission = 0u;
				nodes[new_base + c].pad[0] = depth + 1u;
			}
			memoryBarrierBuffer();
			uint old_val = atomicCompSwap(nodes[node_idx].children_base_index, 0u, new_base);
			base_idx = (old_val != 0u) ? old_val : new_base;
		}

		atomicOr(nodes[node_idx].child_mask, 1u << child_idx);
		node_idx = base_idx + child_idx;
	}

	// LIT RADIANCE = albedo * direct light (same field/format the cone trace + mipmap read).
	vec3 radiance = albedo * voxel_direct_light(world_pos, normal);
	vec3 radiance_clamped = clamp(radiance, vec3(0.0), vec3(1.0));
	uvec3 radiance_bytes = uvec3(radiance_clamped * 255.0 + 0.5);
	uint packed_color = (radiance_bytes.r << 24u) | (radiance_bytes.g << 16u) | (radiance_bytes.b << 8u) | 255u;

	// Luminance-keyed "brightest wins" CAS (matches svogi_voxelize.glsl).
	const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
	float new_luma = dot(radiance_clamped, LUMA);
	uint prev = nodes[node_idx].albedo;
	while (true) {
		vec3 old_rgb = vec3(
				float((prev >> 24u) & 0xFFu),
				float((prev >> 16u) & 0xFFu),
				float((prev >> 8u) & 0xFFu)) /
				255.0;
		if (dot(old_rgb, LUMA) >= new_luma) {
			break;
		}
		uint got = atomicCompSwap(nodes[node_idx].albedo, prev, packed_color);
		if (got == prev) {
			break;
		}
		prev = got;
	}

	// Normal (matches svogi_voxelize.glsl's packing: 3x8-bit, (nx<<16)|(ny<<8)|nz, n*0.5+0.5).
	vec3 n01 = normal * 0.5 + 0.5;
	uint nx = uint(n01.x * 255.0) & 0xFFu;
	uint ny = uint(n01.y * 255.0) & 0xFFu;
	uint nz = uint(n01.z * 255.0) & 0xFFu;
	atomicMax(nodes[node_idx].normal, (nx << 16u) | (ny << 8u) | nz);
}
