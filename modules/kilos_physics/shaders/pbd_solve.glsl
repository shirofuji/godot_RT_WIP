#[compute]
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct RigidBodyData {
    vec4 position;          // xyz = predicted position (corrected in place)
    vec4 rotation;
    vec4 linear_velocity;   // w = mass (0 => static)
    vec4 angular_velocity;
    vec4 center_of_mass;    // xyz = previous position
    vec4 principal_inertia;
};

layout(set = 0, binding = 0, std430) restrict buffer Bodies {
    RigidBodyData data[];
} bodies;

// No readonly qualifier: the Writable flag is part of the descriptor format and
// these sets are shared with grid_build (which writes them).
layout(set = 1, binding = 0, std430) restrict buffer GridCounts {
    uint data[];
} counts;

layout(set = 1, binding = 1, std430) restrict buffer GridBodies {
    uint data[];
} grid;

layout(push_constant, std430) uniform Params {
    float radius;
    float ground_y;
    float cell_size;
    uint body_count;
    uint table_mask;    // table_size - 1
    uint max_per_cell;
    uint pad0;
    uint pad1;
} params;

uint hash_cell(ivec3 c) {
    uint ux = uint(c.x) * 73856093u;
    uint uy = uint(c.y) * 19349663u;
    uint uz = uint(c.z) * 83492791u;
    return (ux ^ uy ^ uz) & params.table_mask;
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= params.body_count) {
        return;
    }

    RigidBodyData b = bodies.data[i];
    if (b.linear_velocity.w == 0.0) {
        return; // static
    }

    vec3 p = b.position.xyz;
    const float min_dist = 2.0 * params.radius;

    // Body-body: scan the 27-cell neighbourhood, push apart overlapping spheres.
    // Jacobi-style: read neighbours, accumulate, write self once.
    vec3 correction = vec3(0.0);
    ivec3 base_cell = ivec3(floor(p / params.cell_size));
    for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                uint h = hash_cell(base_cell + ivec3(dx, dy, dz));
                uint cnt = min(counts.data[h], params.max_per_cell);
                for (uint k = 0u; k < cnt; k++) {
                    uint j = grid.data[h * params.max_per_cell + k];
                    if (j == i) {
                        continue;
                    }
                    vec3 d = p - bodies.data[j].position.xyz;
                    float dist = length(d);
                    if (dist < min_dist && dist > 1e-5) {
                        float pen = min_dist - dist;
                        correction += (d / dist) * (pen * 0.5);
                    }
                }
            }
        }
    }
    p += correction;

    // Ground-plane constraint.
    float min_y = params.ground_y + params.radius;
    if (p.y < min_y) {
        p.y = min_y;
    }

    bodies.data[i].position.xyz = p;
}
