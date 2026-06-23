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
	uint pad[3];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict buffer OctreeNodes {
	Node nodes[];
};

layout(set = 0, binding = 1, std430) restrict buffer AtomicCounter {
	uint alloc_counter;
	uint alloc_pad[3];
};

#ifdef MODE_CLEAR
void main() {
	uint idx = gl_GlobalInvocationID.x;
	if (idx == 0) {
		// Reset node allocator (0 is root)
		alloc_counter = 1;
		
		// Clear root node
		nodes[0].children_base_index = 0;
		nodes[0].child_mask = 0;
		nodes[0].albedo = 0;
		nodes[0].normal = 0;
		nodes[0].emission = 0;
	}
}
#endif

#ifdef MODE_VOXELIZE

struct VisibleMeshlet {
	uint instance_index;
	uint meshlet_index;
};

layout(set = 1, binding = 0, std430) restrict readonly buffer VisibleMeshlets {
	uint count;
	VisibleMeshlet data[];
} visible_meshlets;

layout(set = 1, binding = 1, std430) restrict readonly buffer Transforms {
	mat4 data[];
} transforms;

struct MeshletDescriptor {
	vec3 bounds_center;
	float bounds_radius;
	vec3 cone_axis;
	float cone_cutoff;
	uint vertex_remap_offset;
	uint triangle_offset;
	uint vertex_count;
	uint triangle_count;
};

layout(set = 1, binding = 2, std430) restrict readonly buffer MeshletDescriptors {
	MeshletDescriptor data[];
} meshlet_descriptors;

layout(set = 1, binding = 3, std430) restrict readonly buffer MeshletVertexRemap {
	uint data[];
} meshlet_vertex_remap;

layout(set = 1, binding = 4, std430) restrict readonly buffer MeshletTriangles {
	uint data[];
} meshlet_triangles;

layout(set = 1, binding = 5, std430) restrict readonly buffer VertexPositions {
	vec4 data[];
} vertex_positions;

layout(set = 1, binding = 6, std430) restrict readonly buffer VertexAttributes {
	vec4 data[];
} vertex_attributes;

layout(push_constant, std430) uniform Params {
	vec3 bounds_center;
	float bounds_half_size;
	uint max_nodes;
	uint pad[3];
} params;

uint fetch_triangle_local_vertex(uint p_byte_index) {
	uint word = meshlet_triangles.data[p_byte_index / 4];
	return (word >> ((p_byte_index % 4) * 8)) & 0xFFu;
}

void main() {
	uint meshlet_idx = gl_GlobalInvocationID.x;
	if (meshlet_idx >= visible_meshlets.count) {
		return;
	}

	VisibleMeshlet item = visible_meshlets.data[meshlet_idx];
	mat4 transform = transforms.data[item.instance_index];
	MeshletDescriptor d = meshlet_descriptors.data[item.meshlet_index];

	for (uint i = 0; i < d.triangle_count; i++) {
		uint i0 = fetch_triangle_local_vertex(d.triangle_offset + i * 3 + 0);
		uint i1 = fetch_triangle_local_vertex(d.triangle_offset + i * 3 + 1);
		uint i2 = fetch_triangle_local_vertex(d.triangle_offset + i * 3 + 2);
		
		uint g0 = meshlet_vertex_remap.data[d.vertex_remap_offset + i0];
		uint g1 = meshlet_vertex_remap.data[d.vertex_remap_offset + i1];
		uint g2 = meshlet_vertex_remap.data[d.vertex_remap_offset + i2];
		
		vec3 v0 = (transform * vec4(vertex_positions.data[g0].xyz, 1.0)).xyz;
		vec3 v1 = (transform * vec4(vertex_positions.data[g1].xyz, 1.0)).xyz;
		vec3 v2 = (transform * vec4(vertex_positions.data[g2].xyz, 1.0)).xyz;
		
		// TODO: Compute voxel intersection with triangle (v0, v1, v2)
		// and atomically insert into octree.
	}
}
#endif
