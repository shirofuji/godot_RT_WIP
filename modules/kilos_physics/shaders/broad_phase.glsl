#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct RigidBodyData {
	vec4 position;
	vec4 rotation; // quaternion
	vec4 linear_velocity;
	vec4 angular_velocity;
	vec4 center_of_mass;
	vec4 principal_inertia;
};

layout(set = 0, binding = 0, std430) restrict buffer RigidBodies {
	RigidBodyData data[];
} rigid_bodies;

// Assuming meshlet descriptor structure similar to godot's meshlet_storage
struct MeshletDescriptorGPU {
	uint surface_index;
	uint vertex_count;
	uint index_count;
	uint vertex_offset;
	
	uint index_offset;
	uint bvh_offset;
	uint pad0;
	uint pad1;
	
	float center[3];
	float radius;
	
	float cone_axis[3];
	float cone_cutoff;
};

layout(set = 1, binding = 0, std430) restrict readonly buffer MeshletDescriptors {
	MeshletDescriptorGPU data[];
} meshlets;

struct CandidatePair {
	uint body_index;
	uint meshlet_index;
};

layout(set = 2, binding = 0, std430) restrict writeonly buffer CandidatePairs {
	CandidatePair data[];
} candidates;

layout(set = 2, binding = 1, std430) restrict buffer Counter {
	uint count;
} counter;

layout(push_constant, std430) uniform Params {
	uint body_count;
	uint meshlet_count;
	uint max_candidates;
} params;

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.meshlet_count) return;

	MeshletDescriptorGPU meshlet = meshlets.data[index];
	vec3 m_center = vec3(meshlet.center[0], meshlet.center[1], meshlet.center[2]);
	float m_radius = meshlet.radius;

	for (uint b = 0; b < params.body_count; b++) {
		// A simple broad phase: sphere-sphere test
		// For a real engine, we'd need body bounding sphere radius, using a constant here for now
		vec3 b_pos = rigid_bodies.data[b].position.xyz;
		float b_radius = 1.0; // TODO: read from shape data
		
		float dist = distance(m_center, b_pos);
		if (dist < m_radius + b_radius) {
			// Collision candidate found
			uint c_idx = atomicAdd(counter.count, 1);
			if (c_idx < params.max_candidates) {
				candidates.data[c_idx].body_index = b;
				candidates.data[c_idx].meshlet_index = index;
			} else {
				atomicAdd(counter.count, -1); // Clamp if exceeded
			}
		}
	}
}
