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

#ifdef MODE_MIPMAP
void main() {
	// Stub: We will insert anisotropic mipmap filtering logic here
}
#endif
