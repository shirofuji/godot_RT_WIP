#[compute]
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct RigidBodyData {
    vec4 position;          // xyz = corrected position (after solve)
    vec4 rotation;
    vec4 linear_velocity;   // w = mass (0 => static)
    vec4 angular_velocity;
    vec4 center_of_mass;    // xyz = previous position
    vec4 principal_inertia;
};

layout(set = 0, binding = 0, std430) restrict buffer Bodies {
    RigidBodyData data[];
} bodies;

layout(push_constant, std430) uniform Params {
    float inv_dt;
    float damping_factor; // precomputed (1 - damping*dt), clamped to [0,1]
    uint body_count;
    uint pad1;
} params;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= params.body_count) {
        return;
    }

    RigidBodyData b = bodies.data[i];
    if (b.linear_velocity.w == 0.0) {
        return; // static
    }

    // PBD velocity recovery: the constraint solve moved .position, so the true
    // post-step velocity is (corrected - previous) / dt. Light damping bleeds off
    // the energy injected when overlaps are resolved, so piles settle instead of
    // jittering/drifting.
    vec3 v = (b.position.xyz - b.center_of_mass.xyz) * params.inv_dt * params.damping_factor;
    bodies.data[i].linear_velocity.xyz = v;
}
