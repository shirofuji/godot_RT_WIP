#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	uint max_draws; // Command buffer capacity - dispatch is always exactly this size (a fixed
			// CPU-known constant), so the caller never has to read the real visible count back
			// from the GPU before dispatching.
	uint max_visible_capacity; // visible_meshlets buffer's allocated capacity - visible_meshlets.count
			// is an atomic counter that can overflow past this, so it must be clamped here too.
	uint pad0;
	uint pad1;
}
params;

struct VisibleMeshlet {
	uint instance_index;
	uint meshlet_index;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer VisibleMeshlets {
	uint count;
	VisibleMeshlet data[];
}
visible_meshlets;

// Mirrors MeshletStorage::MeshletDescriptorGPU exactly (only triangle_count is needed here, but
// std430 offsets must match the full struct to read it correctly).
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

layout(set = 0, binding = 1, std430) restrict readonly buffer MeshletDescriptors {
	MeshletDescriptor data[];
}
meshlet_descriptors;

// Mirrors VkDrawIndexedIndirectCommand's layout exactly (20 bytes): indexCount, instanceCount,
// firstIndex, vertexOffset, firstInstance.
struct IndirectCommand {
	uint index_count;
	uint instance_count;
	uint first_index;
	int vertex_offset;
	uint first_instance;
};

layout(set = 0, binding = 2, std430) restrict writeonly buffer IndirectCommands {
	IndirectCommand data[];
}
commands;

void main() {
	uint idx = gl_GlobalInvocationID.x;
	if (idx >= params.max_draws) {
		return;
	}

	uint real_visible_count = min(visible_meshlets.count, params.max_visible_capacity);
	if (idx >= real_visible_count) {
		// Beyond the real visible count for this frame - explicitly mark degenerate
		// (instance_count=0, drawn as a no-op) rather than leaving this slot untouched, since it
		// may hold a real, non-degenerate command left over from a previous frame that had more
		// visible meshlets than this one.
		commands.data[idx].instance_count = 0;
		return;
	}

	// Each meshlet has its own actual triangle_count (up to the 124-triangle cap) - indexCount
	// must reflect that exactly, not a fixed maximum, or the vertex shader would process
	// out-of-range/degenerate triangles for every meshlet smaller than the cap.
	VisibleMeshlet item = visible_meshlets.data[idx];
	uint triangle_count = meshlet_descriptors.data[item.meshlet_index].triangle_count;

	// firstInstance encodes this draw's slot in the visible list (instanceCount=1, so the vertex
	// shader's gl_InstanceIndex equals this value exactly) - the vertex shader uses it to look up
	// (instance_index, meshlet_index) from the same visible_meshlets buffer.
	commands.data[idx].index_count = triangle_count * 3;
	commands.data[idx].instance_count = 1;
	commands.data[idx].first_index = 0;
	commands.data[idx].vertex_offset = 0;
	commands.data[idx].first_instance = idx;
}
