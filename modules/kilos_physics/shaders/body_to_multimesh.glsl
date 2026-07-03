#[compute]
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct RigidBodyData {
    vec4 position;          // xyz = position
    vec4 rotation;          // quaternion (x,y,z,w)
    vec4 linear_velocity;
    vec4 angular_velocity;
    vec4 center_of_mass;
    vec4 principal_inertia;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Bodies {
    RigidBodyData data[];
} bodies;

// MultiMesh instance transform buffer (Godot layout: row-major 3x4 per instance,
// stride in floats given by params.stride; transform occupies the first 12 floats).
layout(set = 1, binding = 0, std430) restrict writeonly buffer MultiMeshBuffer {
    float data[];
} mm;

layout(push_constant, std430) uniform Params {
    uint base;    // first body slot in this range
    uint count;   // number of instances
    uint stride;  // floats per instance in the multimesh buffer (12 for transform-only)
    uint pad;
} params;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= params.count) {
        return;
    }

    RigidBodyData b = bodies.data[params.base + i];
    vec3 p = b.position.xyz;

    // Build the rotation matrix ROWS from the unit quaternion, matching Godot's
    // Basis(Quaternion) convention (see _multimesh_instance_set_transform).
    float x = b.rotation.x, y = b.rotation.y, z = b.rotation.z, w = b.rotation.w;
    vec3 r0 = vec3(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - w * z),       2.0 * (x * z + w * y));
    vec3 r1 = vec3(2.0 * (x * y + w * z),       1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - w * x));
    vec3 r2 = vec3(2.0 * (x * z - w * y),       2.0 * (y * z + w * x),       1.0 - 2.0 * (x * x + y * y));

    uint o = i * params.stride;
    mm.data[o + 0] = r0.x; mm.data[o + 1] = r0.y; mm.data[o + 2] = r0.z; mm.data[o + 3] = p.x;
    mm.data[o + 4] = r1.x; mm.data[o + 5] = r1.y; mm.data[o + 6] = r1.z; mm.data[o + 7] = p.y;
    mm.data[o + 8] = r2.x; mm.data[o + 9] = r2.y; mm.data[o + 10] = r2.z; mm.data[o + 11] = p.z;
}
