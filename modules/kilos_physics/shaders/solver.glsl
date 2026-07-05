#[compute]

#version 450

#extension GL_EXT_shader_atomic_float : enable

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct RigidBodyData {
	vec4 position;
	vec4 rotation;
	vec4 linear_velocity;
	vec4 angular_velocity;
	vec4 center_of_mass;
	vec4 principal_inertia;
};

layout(set = 0, binding = 0, std430) restrict buffer RigidBodies {
	RigidBodyData data[];
} rigid_bodies;

struct ContactPoint {
	uint body_index;
	float depth;
	uint pad0;
	uint pad1;
	vec4 position;
	vec4 normal;
};

layout(set = 1, binding = 0, std430) restrict readonly buffer ContactBuffer {
	ContactPoint data[];
} contacts;

layout(set = 1, binding = 1, std430) restrict readonly buffer ContactCounter {
	uint count;
} contact_counter;

layout(push_constant, std430) uniform Params {
	float delta_time;
	float pseudo_velocity_factor;
} params;



void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= contact_counter.count) return;

	ContactPoint contact = contacts.data[index];
	uint b_idx = contact.body_index;
	
	// Mass = 0 means static, we only push dynamic bodies
	if (rigid_bodies.data[b_idx].linear_velocity.w == 0.0) return;

	float inv_mass = rigid_bodies.data[b_idx].angular_velocity.w; // w component is inv_mass
	vec3 n = contact.normal.xyz;
	float d = contact.depth;

	// Baumgarte stabilization (positional correction via pseudo-velocity)
	float slop = 0.01;
	float bias = params.pseudo_velocity_factor * max(d - slop, 0.0) / params.delta_time;

	// Relative velocity at contact (ignoring angular for a pure sphere right now)
	vec3 v = rigid_bodies.data[b_idx].linear_velocity.xyz;
	float vn = dot(v, n);

	// Only apply impulse if moving towards the collision or if deeply penetrated
	float restitution = 0.0; // Inelastic
	float dPt = -(1.0 + restitution) * vn + bias;
	
	if (dPt > 0.0) {
		float impulse = dPt / inv_mass;
		vec3 impulse_vec = n * impulse * inv_mass;

		// Since multiple contacts can hit the same body, we must atomically add the velocity!
		// atomicAddFloat(rigid_bodies.data[b_idx].linear_velocity.x, impulse_vec.x);
		// atomicAddFloat(rigid_bodies.data[b_idx].linear_velocity.y, impulse_vec.y);
		// atomicAddFloat(rigid_bodies.data[b_idx].linear_velocity.z, impulse_vec.z);

		// But we can also just atomically push the position directly to resolve it instantly (Non-linear Gauss-Seidel)
		// For AAA physics, iterative velocity solvers are better, but direct position pushing avoids jitter
		
		atomicAdd(rigid_bodies.data[b_idx].position.x, n.x * d * 0.5);
		atomicAdd(rigid_bodies.data[b_idx].position.y, n.y * d * 0.5);
		atomicAdd(rigid_bodies.data[b_idx].position.z, n.z * d * 0.5);
		
		// Nullify velocity along the normal to stop bouncing
		if (vn < 0.0) {
			atomicAdd(rigid_bodies.data[b_idx].linear_velocity.x, -n.x * vn);
			atomicAdd(rigid_bodies.data[b_idx].linear_velocity.y, -n.y * vn);
			atomicAdd(rigid_bodies.data[b_idx].linear_velocity.z, -n.z * vn);
		}
	}
}
