#[compute]

#version 450

// Extensions for 8-bit types if supported, otherwise we unpack manually
// #extension GL_EXT_shader_8bit_storage : require

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct RigidBodyData {
	vec4 position;
	vec4 rotation; // quaternion
	vec4 linear_velocity;
	vec4 angular_velocity;
	vec4 center_of_mass;
	vec4 principal_inertia;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer RigidBodies {
	RigidBodyData data[];
} rigid_bodies;

struct MeshletDescriptorGPU {
	uint surface_index;
	uint vertex_count;
	uint index_count;
	uint vertex_offset;
	
	uint index_offset;
	uint bvh_offset;
	uint pad0;
	uint pad1;
	
	float center[3];
	float radius;
	
	float cone_axis[3];
	float cone_cutoff;
};

layout(set = 1, binding = 0, std430) restrict readonly buffer MeshletDescriptors {
	MeshletDescriptorGPU data[];
} meshlets;

struct CandidatePair {
	uint body_index;
	uint meshlet_index;
};

layout(set = 2, binding = 0, std430) restrict readonly buffer CandidatePairs {
	CandidatePair data[];
} candidates;

layout(set = 2, binding = 1, std430) restrict readonly buffer Counter {
	uint count;
} counter;

struct ContactPoint {
	uint body_index;
	float depth;
	uint pad0;
	uint pad1;
	vec4 position;
	vec4 normal;
};

layout(set = 3, binding = 0, std430) restrict writeonly buffer ContactBuffer {
	ContactPoint data[];
} contacts;

layout(set = 3, binding = 1, std430) restrict buffer ContactCounter {
	uint count;
} contact_counter;

// Global buffers from MeshletStorage
layout(set = 4, binding = 0, std430) restrict readonly buffer GlobalVertices {
	vec4 data[];
} global_vertices;

layout(set = 4, binding = 1, std430) restrict readonly buffer MeshletVertexMap {
	uint data[];
} meshlet_vertex_map;

// Since GLSL doesn't natively support uint8_t buffers without extensions, we pack them in uints
layout(set = 4, binding = 2, std430) restrict readonly buffer MeshletTriangles {
	uint data[];
} meshlet_triangles;

layout(push_constant, std430) uniform Params {
	uint max_contacts;
} params;

// Helper to unpack 8-bit index from packed uint buffer
uint get_triangle_index(uint byte_offset) {
	uint word_idx = byte_offset / 4;
	uint byte_idx = byte_offset % 4;
	uint word = meshlet_triangles.data[word_idx];
	return (word >> (byte_idx * 8)) & 0xFFu;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= counter.count) return;

	CandidatePair pair = candidates.data[index];
	RigidBodyData body = rigid_bodies.data[pair.body_index];
	MeshletDescriptorGPU meshlet = meshlets.data[pair.meshlet_index];

	vec3 sphere_center = body.position.xyz;
	float sphere_radius = 1.0; // TODO: read from shape

	// Perform SAT against every triangle in the meshlet
	uint num_triangles = meshlet.index_count / 3;
	
	// Pre-load all vertices for this meshlet into shared memory to avoid redundant global memory reads?
	// For now, read directly from global memory.

	for (uint i = 0; i < num_triangles; i++) {
		uint tri_start = meshlet.index_offset + (i * 3);
		
		uint i0 = get_triangle_index(tri_start + 0);
		uint i1 = get_triangle_index(tri_start + 1);
		uint i2 = get_triangle_index(tri_start + 2);

		uint g0 = meshlet_vertex_map.data[meshlet.vertex_offset + i0];
		uint g1 = meshlet_vertex_map.data[meshlet.vertex_offset + i1];
		uint g2 = meshlet_vertex_map.data[meshlet.vertex_offset + i2];

		vec3 v0 = global_vertices.data[g0].xyz;
		vec3 v1 = global_vertices.data[g1].xyz;
		vec3 v2 = global_vertices.data[g2].xyz;

		// Sphere-Triangle SAT test
		vec3 e0 = v1 - v0;
		vec3 e1 = v2 - v1;
		vec3 e2 = v0 - v2;

		vec3 tri_normal = normalize(cross(e0, -e2));
		
		// 1. Check distance to triangle plane
		float dist_to_plane = dot(sphere_center - v0, tri_normal);
		if (abs(dist_to_plane) > sphere_radius) continue;

		// 2. Check edges (Voronoi region test)
		vec3 center_on_plane = sphere_center - tri_normal * dist_to_plane;
		
		// Barycentric test
		vec3 c0 = cross(e0, center_on_plane - v0);
		vec3 c1 = cross(e1, center_on_plane - v1);
		vec3 c2 = cross(e2, center_on_plane - v2);

		bool inside = dot(c0, tri_normal) >= 0.0 && dot(c1, tri_normal) >= 0.0 && dot(c2, tri_normal) >= 0.0;
		
		float dist_sq = sphere_radius * sphere_radius;
		vec3 closest_point = center_on_plane;
		
		if (!inside) {
			// Find closest point on edges
			vec3 p0 = v0 + clamp(dot(sphere_center - v0, e0) / dot(e0, e0), 0.0, 1.0) * e0;
			vec3 p1 = v1 + clamp(dot(sphere_center - v1, e1) / dot(e1, e1), 0.0, 1.0) * e1;
			vec3 p2 = v2 + clamp(dot(sphere_center - v2, e2) / dot(e2, e2), 0.0, 1.0) * e2;

			float d0 = dot(sphere_center - p0, sphere_center - p0);
			float d1 = dot(sphere_center - p1, sphere_center - p1);
			float d2 = dot(sphere_center - p2, sphere_center - p2);
			
			float min_d = min(min(d0, d1), d2);
			if (min_d > dist_sq) continue;
			
			if (min_d == d0) closest_point = p0;
			else if (min_d == d1) closest_point = p1;
			else closest_point = p2;
		}

		// Calculate penetration depth and normal
		vec3 dir = sphere_center - closest_point;
		float len = length(dir);
		
		if (len < sphere_radius && len > 0.0001) {
			float penetration_depth = sphere_radius - len;
			vec3 collision_normal = dir / len;
			
			uint out_idx = atomicAdd(contact_counter.count, 1);
			if (out_idx < params.max_contacts) {
				contacts.data[out_idx].body_index = pair.body_index;
				contacts.data[out_idx].depth = penetration_depth;
				contacts.data[out_idx].position = vec4(closest_point, 1.0);
				contacts.data[out_idx].normal = vec4(collision_normal, 0.0);
			} else {
				atomicAdd(contact_counter.count, -1);
			}
		}
	}
}
