#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	uint range_count;
	uint max_work_items;
	uint pad0;
	uint pad1;
}
params;

struct InstanceMeshletRange {
	uint instance_index;
	uint meshlet_offset;
	uint meshlet_count;
	uint pad;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Ranges {
	InstanceMeshletRange data[];
}
ranges;

struct WorkItem {
	uint instance_index;
	uint meshlet_index;
};

layout(set = 0, binding = 1, std430) restrict buffer WorkItems {
	uint count;
	WorkItem data[];
}
work_items;

void main() {
	uint idx = gl_GlobalInvocationID.x;
	if (idx >= params.range_count) {
		return;
	}

	InstanceMeshletRange r = ranges.data[idx];
	for (uint i = 0; i < r.meshlet_count; i++) {
		uint slot = atomicAdd(work_items.count, 1);
		if (slot < params.max_work_items) {
			work_items.data[slot].instance_index = r.instance_index;
			work_items.data[slot].meshlet_index = r.meshlet_offset + i;
		}
	}
}
