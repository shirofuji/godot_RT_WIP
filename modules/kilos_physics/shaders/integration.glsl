#[compute]
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct RigidBodyData {
    vec4 position;          // xyz = position, w = sleep state
    vec4 rotation;          // quaternion (x,y,z,w)
    vec4 linear_velocity;   // xyz = velocity, w = mass (0 if static)
    vec4 angular_velocity;  // xyz = velocity, w = inverse mass
    vec4 center_of_mass;
    vec4 principal_inertia; // xyz = inertia, w = inverse inertia scale
};

layout(set = 0, binding = 0, std430) restrict buffer RigidBodyBuffer {
    RigidBodyData data[];
} rigid_bodies;

layout(push_constant, std430) uniform Params {
    vec4 gravity;
    float delta_time;
    float linear_damp;
    float angular_damp;
    uint body_count;
} params;

// Basic quaternion multiplication
vec4 quat_mult(vec4 q1, vec4 q2) {
    return vec4(
        q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y,
        q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x,
        q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w,
        q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z
    );
}

void main() {
    uint global_id = gl_GlobalInvocationID.x;
    if (global_id >= params.body_count) return;

    RigidBodyData body = rigid_bodies.data[global_id];

    // Static bodies have mass == 0
    if (body.linear_velocity.w == 0.0) return;

    // Save the pre-integration position (PBD "previous" position). The collision
    // solve corrects .position in place, then the finalize pass recovers velocity
    // as (position - center_of_mass) / dt. Harmless when collision is disabled.
    body.center_of_mass.xyz = body.position.xyz;

    // Apply Gravity
    body.linear_velocity.xyz += params.gravity.xyz * params.delta_time;

    // Damping
    body.linear_velocity.xyz *= max(0.0, 1.0 - params.linear_damp * params.delta_time);
    body.angular_velocity.xyz *= max(0.0, 1.0 - params.angular_damp * params.delta_time);

    // Integrate Position
    body.position.xyz += body.linear_velocity.xyz * params.delta_time;

    // Integrate Rotation
    vec4 w = vec4(body.angular_velocity.xyz * params.delta_time * 0.5, 0.0);
    vec4 dq = quat_mult(w, body.rotation);
    body.rotation += dq;
    
    // Normalize quaternion
    float len = length(body.rotation);
    if (len > 0.0) {
        body.rotation /= len;
    }

    rigid_bodies.data[global_id] = body;
}
