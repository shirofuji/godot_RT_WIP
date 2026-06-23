#[compute]

#version 450

#VERSION_DEFINES

#define MAX_NODES 16777216

struct Node {
	uint children_base_index;
	uint child_mask;
	uint albedo;
	uint normal;
	uint emission;
	uint pad[3]; // pad[0] = this node's depth (root = 0), stamped at allocation time in
			// svogi_voxelize.glsl - lets this pass find "every node at depth N" with a single
			// linear scan per level, without needing a separate per-level node list.
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict buffer OctreeNodes {
	Node nodes[];
};

layout(set = 0, binding = 1, std430) restrict readonly buffer AtomicCounter {
	uint alloc_counter;
	uint alloc_pad[3];
};

#ifdef MODE_MIPMAP

// Dispatched once per octree level, walking from the deepest leaf level up to the root (one level
// per dispatch, target_depth decreasing) - a parent's children must already hold their own final
// (possibly already-aggregated, if they're internal nodes themselves) values before the parent
// can aggregate them, so levels cannot be processed out of order or in a single pass.
layout(push_constant, std430) uniform Params {
	uint target_depth;
	uint pad[3];
}
params;

vec4 unpack_rgba8(uint p_packed) {
	return vec4(
			float((p_packed >> 24u) & 0xFFu),
			float((p_packed >> 16u) & 0xFFu),
			float((p_packed >> 8u) & 0xFFu),
			float(p_packed & 0xFFu)) /
			255.0;
}

uint pack_rgba8(vec4 p_color) {
	uvec4 c = uvec4(clamp(p_color, vec4(0.0), vec4(1.0)) * 255.0 + 0.5);
	return (c.r << 24u) | (c.g << 16u) | (c.b << 8u) | c.a;
}

// Matches svogi_voxelize.glsl's normal packing exactly: 3x8-bit, no alpha, (nx<<16)|(ny<<8)|nz,
// where nx/ny/nz are normal*0.5+0.5 mapped from [-1,1] to [0,1] before quantizing to 8 bits.
vec3 unpack_normal(uint p_packed) {
	vec3 n01 = vec3(
			float((p_packed >> 16u) & 0xFFu),
			float((p_packed >> 8u) & 0xFFu),
			float(p_packed & 0xFFu)) /
			255.0;
	return n01 * 2.0 - 1.0;
}

uint pack_normal(vec3 p_normal) {
	vec3 n01 = clamp(p_normal * 0.5 + 0.5, vec3(0.0), vec3(1.0));
	uvec3 c = uvec3(n01 * 255.0 + 0.5);
	return (c.r << 16u) | (c.g << 8u) | c.b;
}

void main() {
	uint idx = gl_GlobalInvocationID.x;
	// alloc_counter only ever grows monotonically within one clear-to-clear cycle (see
	// svogi_voxelize.glsl's MODE_CLEAR/MODE_VOXELIZE), so this is always a safe upper bound for
	// which slots are actually part of the current tree - reading it here instead of passing a
	// CPU-side node count avoids a GPU->CPU readback/sync between voxelize and this pass. Node 0
	// (the root) is included in this range and DOES need processing when target_depth==0, to
	// aggregate its own depth-1 children into itself.
	if (idx >= alloc_counter) {
		return;
	}
	if (nodes[idx].pad[0] != params.target_depth) {
		return;
	}
	uint mask = nodes[idx].child_mask;
	uint base = nodes[idx].children_base_index;
	if (mask == 0u || base == 0u) {
		return; // Leaf - already holds its own real data from voxelize, nothing to aggregate.
	}

	vec4 albedo_accum = vec4(0.0);
	vec3 normal_accum = vec3(0.0);
	vec4 emission_accum = vec4(0.0);
	uint count = 0u;
	for (uint c = 0u; c < 8u; c++) {
		if ((mask & (1u << c)) == 0u) {
			continue;
		}
		uint child_idx = base + c;
		albedo_accum += unpack_rgba8(nodes[child_idx].albedo);
		normal_accum += unpack_normal(nodes[child_idx].normal);
		emission_accum += unpack_rgba8(nodes[child_idx].emission);
		count++;
	}
	if (count == 0u) {
		return;
	}

	nodes[idx].albedo = pack_rgba8(albedo_accum / float(count));
	float normal_len = length(normal_accum);
	nodes[idx].normal = pack_normal(normal_len > 0.0001 ? (normal_accum / normal_len) : vec3(0.0, 0.0, 1.0));
	nodes[idx].emission = pack_rgba8(emission_accum / float(count));
}
#endif
