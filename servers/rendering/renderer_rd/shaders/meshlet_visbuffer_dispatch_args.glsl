#[compute]

#version 450

#VERSION_DEFINES

// Builds the indirect dispatch arguments for the visbuffer software rasterizer: one workgroup per
// software-visible meshlet. Reads the software worklist's atomic count (clamped to the buffer
// capacity, since the counter can overrun it) and writes {groupCountX = count, 1, 1} so the
// rasterizer dispatches exactly the real number of meshlets instead of the fixed capacity. One
// invocation total.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

struct VisibleMeshlet {
	uint instance_index;
	uint meshlet_index;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer VisibleMeshlets {
	uint count;
	VisibleMeshlet data[];
}
visible_meshlets;

layout(set = 0, binding = 1, std430) restrict writeonly buffer DispatchArgs {
	uint x;
	uint y;
	uint z;
}
dispatch_args;

layout(push_constant, std430) uniform Params {
	uint max_visible;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	dispatch_args.x = min(visible_meshlets.count, params.max_visible);
	dispatch_args.y = 1u;
	dispatch_args.z = 1u;
}
