#[compute]
#version 450

// Spatial-hash broad phase: hash each body's cell (floor(pos / cell_size)) into a
// fixed-size table of fixed-capacity buckets. Cell coords are unbounded, so this is
// independent of world extent (unlike a dense uniform grid). cell_size = 2*radius,
// so any colliding pair lies within the 27-cell (3x3x3) neighbourhood.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct RigidBodyData {
    vec4 position;
    vec4 rotation;
    vec4 linear_velocity; // w = mass (0 => static)
    vec4 angular_velocity;
    vec4 center_of_mass;
    vec4 principal_inertia;
};

// NOTE: no readonly/writeonly qualifiers - the Writable flag is part of the
// descriptor format, and these buffers share uniform sets with the other physics
// passes (which bind them read-write), so all bindings must be plain read-write.
layout(set = 0, binding = 0, std430) restrict buffer Bodies {
    RigidBodyData data[];
} bodies;

layout(set = 1, binding = 0, std430) restrict buffer GridCounts {
    uint data[];
} counts;

layout(set = 1, binding = 1, std430) restrict buffer GridBodies {
    uint data[];
} grid;

layout(push_constant, std430) uniform Params {
    float cell_size;
    uint table_mask;   // table_size - 1 (table_size is a power of two)
    uint max_per_cell;
    uint body_count;
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
        return; // static bodies don't occupy the grid (P3b is body-body dynamic)
    }

    ivec3 cell = ivec3(floor(b.position.xyz / params.cell_size));
    uint h = hash_cell(cell);
    uint slot = atomicAdd(counts.data[h], 1u);
    if (slot < params.max_per_cell) {
        grid.data[h * params.max_per_cell + slot] = i;
    }
}
