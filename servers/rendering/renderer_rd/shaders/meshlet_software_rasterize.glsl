#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_shader_atomic_int64 : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : enable

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;



layout(set = 2, binding = 0, std430) restrict buffer DepthBuffer {
	uint data[];
} depth_buffer;

struct MeshletDescriptor {
	float offset_x;
	float offset_y;
	float offset_z;
	float radius;
	float cone_apex_x;
	float cone_apex_y;
	float cone_apex_z;
	float cone_axis_x;
	float cone_axis_y;
	float cone_axis_z;
	float cone_cutoff;
	uint vertex_count;
	uint triangle_count;
	uint vertex_offset;
	uint triangle_offset;
};

struct VisibleMeshlet {
	uint instance_index;
	uint meshlet_index;
};

layout(set = 1, binding = 1, std430) restrict readonly buffer VisibleMeshlets {
	uint count;
	VisibleMeshlet data[];
} visible_meshlets;

layout(set = 1, binding = 2, std430) restrict readonly buffer Transforms {
	mat4 data[];
} transforms;

layout(set = 1, binding = 3, std430) restrict readonly buffer MeshletDescriptors {
	MeshletDescriptor data[];
} meshlet_descriptors;

layout(set = 1, binding = 4, std430) restrict readonly buffer MeshletVertexRemap {
	uint data[];
} meshlet_vertex_remap;

layout(set = 1, binding = 5, std430) restrict readonly buffer MeshletTriangles {
	uint data[];
} meshlet_triangles;

layout(set = 1, binding = 6, std430) restrict readonly buffer MeshletVertexPositions {
	vec4 data[];
} meshlet_vertex_positions;

layout(push_constant, std430) uniform PushConstant {
	mat4 view_projection_matrix;
	vec2 viewport_size;
} push_constant;

void main() {
	uint visible_meshlet_id = gl_WorkGroupID.x;
	uint triangle_id = gl_LocalInvocationID.x;
	
	if (visible_meshlet_id >= visible_meshlets.count) {
		return;
	}

	uint instance_id = visible_meshlets.data[visible_meshlet_id].instance_index;
	uint meshlet_id = visible_meshlets.data[visible_meshlet_id].meshlet_index;

	mat4 instance_transform = transforms.data[instance_id];

	MeshletDescriptor desc = meshlet_descriptors.data[meshlet_id];
	
	if (triangle_id >= desc.triangle_count) {
		return;
	}
	
	// Fetch Triangle Vertices
	uint tri_idx = desc.triangle_offset + triangle_id;
	uint tri_data = meshlet_triangles.data[tri_idx];
	uint v0_idx = desc.vertex_offset + (tri_data & 0xFF);
	uint v1_idx = desc.vertex_offset + ((tri_data >> 8) & 0xFF);
	uint v2_idx = desc.vertex_offset + ((tri_data >> 16) & 0xFF);
	
	uint v0_global = meshlet_vertex_remap.data[v0_idx];
	uint v1_global = meshlet_vertex_remap.data[v1_idx];
	uint v2_global = meshlet_vertex_remap.data[v2_idx];
	
	vec3 p0 = meshlet_vertex_positions.data[v0_global].xyz;
	vec3 p1 = meshlet_vertex_positions.data[v1_global].xyz;
	vec3 p2 = meshlet_vertex_positions.data[v2_global].xyz;
	
	// Transform & Project to screen space
	vec4 p0_clip = push_constant.view_projection_matrix * instance_transform * vec4(p0, 1.0);
	vec4 p1_clip = push_constant.view_projection_matrix * instance_transform * vec4(p1, 1.0);
	vec4 p2_clip = push_constant.view_projection_matrix * instance_transform * vec4(p2, 1.0);
	
	if (p0_clip.w <= 0.0 || p1_clip.w <= 0.0 || p2_clip.w <= 0.0) {
		return;
	}
	
	// Perspective divide
	p0_clip.xyz /= p0_clip.w;
	p1_clip.xyz /= p1_clip.w;
	p2_clip.xyz /= p2_clip.w;
	
	// Viewport transform
	vec2 screen0 = (p0_clip.xy * 0.5 + 0.5) * push_constant.viewport_size;
	vec2 screen1 = (p1_clip.xy * 0.5 + 0.5) * push_constant.viewport_size;
	vec2 screen2 = (p2_clip.xy * 0.5 + 0.5) * push_constant.viewport_size;
	
	// Backface culling
	vec2 e0 = screen1 - screen0;
	vec2 e1 = screen2 - screen0;
	if (e0.x * e1.y - e0.y * e1.x <= 0.0) {
		return; // Culled
	}
	
	// Bounding Box Test (Screen Space)
	vec2 min_bound = min(min(screen0, screen1), screen2);
	vec2 max_bound = max(max(screen0, screen1), screen2);

	vec2 viewport_size = push_constant.viewport_size;

	// Clamp to screen bounds
	int min_x = max(0, int(floor(min_bound.x)));
	int min_y = max(0, int(floor(min_bound.y)));
	int max_x = min(int(viewport_size.x) - 1, int(ceil(max_bound.x)));
	int max_y = min(int(viewport_size.y) - 1, int(ceil(max_bound.y)));

	// Rasterize
	for (int y = min_y; y <= max_y; y++) {
		for (int x = min_x; x <= max_x; x++) {
			vec2 p = vec2(x, y) + 0.5;

			// Edge functions
			vec2 v0 = screen1 - screen0;
			vec2 w0 = p - screen0;
			float cross0 = v0.x * w0.y - v0.y * w0.x;
			
			vec2 v1 = screen2 - screen1;
			vec2 w1 = p - screen1;
			float cross1 = v1.x * w1.y - v1.y * w1.x;
			
			vec2 v2 = screen0 - screen2;
			vec2 w2 = p - screen2;
			float cross2 = v2.x * w2.y - v2.y * w2.x;
			
			if (cross0 >= 0.0 && cross1 >= 0.0 && cross2 >= 0.0) {
				// Barycentric coordinates
				float area = cross0 + cross1 + cross2;
				float b0 = cross1 / area;
				float b1 = cross2 / area;
				float b2 = cross0 / area;
				
				// Interpolate depth
				float z = b0 * p0_clip.z + b1 * p1_clip.z + b2 * p2_clip.z;
				
				// Depth is [0.0, 1.0]. Float [0.0, 1.0] has same ordering as its IEEE 754 uint representation.
				uint z_uint = floatBitsToUint(z);
				
				// Write to DepthBuffer using atomicMax (Reverse-Z means near=1.0, far=0.0)
				// atomicMax effectively does greater-or-equal depth testing for Reverse-Z!
				int pixel_index = y * int(viewport_size.x) + x;
				atomicMax(depth_buffer.data[pixel_index], z_uint);
			}
		}
	}
}
