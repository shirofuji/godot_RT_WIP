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
	if (idx == 0u) {
		// Reset node allocator (0 is root)
		alloc_counter = 1u;

		// Clear root node
		nodes[0].children_base_index = 0u;
		nodes[0].child_mask = 0u;
		nodes[0].albedo = 0u;
		nodes[0].normal = 0u;
		nodes[0].emission = 0u;
		nodes[0].pad[0] = 0u; // depth
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

// Per-instance material slot (mirrors meshlet_render.glsl's InstanceMaterialIds binding exactly -
// same resolution-at-scan-time data, see RenderForwardClustered::_meshlet_scan_render_list()).
layout(set = 1, binding = 7, std430) restrict readonly buffer InstanceMaterialIds {
	uint data[];
} instance_material_ids;

// Mirrors MeshletStorage::MeshletMaterialGPU's std430 layout exactly (see meshlet_render.glsl's
// own MeshletMaterial struct) - only albedo/emission are used here; texture indices are ignored
// since this voxelization pass has no UVs to sample with.
struct MeshletMaterial {
	vec4 albedo;
	vec3 emission;
	float metallic;
	float roughness;
	float specular;
	uint albedo_texture_index;
	uint normal_texture_index;
	uint orm_texture_index;
	uint emission_texture_index;
	uint flags;
	float alpha_scissor_threshold;
	vec2 uv1_scale;
	vec2 uv1_offset;
};

layout(set = 1, binding = 8, std430) restrict readonly buffer MeshletMaterials {
	MeshletMaterial data[];
} meshlet_materials;

layout(push_constant, std430) uniform Params {
	vec3 bounds_center;
	float bounds_half_size;
	uint max_nodes;
	uint pad[3];
} params;

uint fetch_triangle_local_vertex(uint p_byte_index) {
	uint word = meshlet_triangles.data[p_byte_index / 4u];
	return (word >> ((p_byte_index % 4u) * 8u)) & 0xFFu;
}

void main() {
	uint meshlet_idx = gl_GlobalInvocationID.x;
	if (meshlet_idx >= visible_meshlets.count) {
		return;
	}

	VisibleMeshlet item = visible_meshlets.data[meshlet_idx];
	mat4 transform = transforms.data[item.instance_index];
	MeshletDescriptor d = meshlet_descriptors.data[item.meshlet_index];

	for (uint i = 0u; i < d.triangle_count; i++) {
		uint i0 = fetch_triangle_local_vertex(d.triangle_offset + i * 3u + 0u);
		uint i1 = fetch_triangle_local_vertex(d.triangle_offset + i * 3u + 1u);
		uint i2 = fetch_triangle_local_vertex(d.triangle_offset + i * 3u + 2u);
		
		uint g0 = meshlet_vertex_remap.data[d.vertex_remap_offset + i0];
		uint g1 = meshlet_vertex_remap.data[d.vertex_remap_offset + i1];
		uint g2 = meshlet_vertex_remap.data[d.vertex_remap_offset + i2];
		
		vec3 v0 = (transform * vec4(vertex_positions.data[g0].xyz, 1.0)).xyz;
		vec3 v1 = (transform * vec4(vertex_positions.data[g1].xyz, 1.0)).xyz;
		vec3 v2 = (transform * vec4(vertex_positions.data[g2].xyz, 1.0)).xyz;
		
		// Approximate voxel insertion with the centroid of the triangle
		vec3 tri_centroid = (v0 + v1 + v2) / 3.0;
		vec3 normal = normalize(cross(v1 - v0, v2 - v0));
		
		// Skip if outside bounds
		vec3 diff = abs(tri_centroid - params.bounds_center);
		if (diff.x > params.bounds_half_size || diff.y > params.bounds_half_size || diff.z > params.bounds_half_size) {
			continue;
		}
		
		uint node_idx = 0u;
		vec3 current_center = params.bounds_center;
		float current_half_size = params.bounds_half_size;
		
		// Traverse and build up to 7 levels deep (root + 6)
		for (uint depth = 0u; depth < 6u; depth++) {
			bvec3 is_pos = greaterThan(tri_centroid, current_center);
			uint child_idx = (is_pos.x ? 1u : 0u) | ((is_pos.y ? 1u : 0u) << 1) | ((is_pos.z ? 1u : 0u) << 2);
			
			vec3 offset = vec3(is_pos.x ? 1.0 : -1.0, is_pos.y ? 1.0 : -1.0, is_pos.z ? 1.0 : -1.0);
			current_half_size *= 0.5;
			current_center += offset * current_half_size;
			
			uint base_idx = nodes[node_idx].children_base_index;
			if (base_idx == 0u) {
				uint new_base = atomicAdd(alloc_counter, 8u);
				if (new_base >= params.max_nodes - 8u) {
					break; // Out of memory
				}
				// Zero the 8 new children and stamp their depth (depth+1, since these are this
				// node's children) BEFORE publishing children_base_index below - nothing else can
				// reach these slots via the tree until the atomicCompSwap succeeds, so this
				// ordering is race-free. The buffer isn't cleared between region-update cycles
				// beyond the root node (see MODE_CLEAR), so these slots may hold stale data from a
				// previous cycle that allocated the same index range for a differently-shaped tree
				// - without this, a stale child_mask/children_base_index could make the cone-trace
				// traversal follow a dangling pointer into garbage data, or a stale depth value
				// could make the mipmap pass skip/misprocess this node entirely.
				for (uint c = 0u; c < 8u; c++) {
					nodes[new_base + c].children_base_index = 0u;
					nodes[new_base + c].child_mask = 0u;
					nodes[new_base + c].albedo = 0u;
					nodes[new_base + c].normal = 0u;
					nodes[new_base + c].emission = 0u;
					nodes[new_base + c].pad[0] = depth + 1u;
				}
				uint old_val = atomicCompSwap(nodes[node_idx].children_base_index, 0u, new_base);
				if (old_val != 0u) {
					// Another thread allocated first, use theirs - our own new_base allocation
					// above is now wasted/leaked space for this clear cycle, which is acceptable
					// given MAX_NODES' ample headroom for this prototype's scale.
					base_idx = old_val;
				} else {
					base_idx = new_base;
				}
			}
			
			atomicOr(nodes[node_idx].child_mask, 1u << child_idx);
			node_idx = base_idx + child_idx;
		}
		
		// Leaf node insertion - real material albedo/emission (same per-instance resolution the
		// meshlet renderer itself uses, see InstanceMaterialIds/MeshletMaterials above), replacing
		// the original hardcoded-white placeholder. Without this, SVOGI could only ever bounce
		// flat white light regardless of the actual surface's color.
		uint material_id = instance_material_ids.data[item.instance_index];
		vec4 albedo = meshlet_materials.data[material_id].albedo;
		uvec4 albedo_bytes = uvec4(clamp(albedo, vec4(0.0), vec4(1.0)) * 255.0 + 0.5);
		uint packed_color = (albedo_bytes.r << 24u) | (albedo_bytes.g << 16u) | (albedo_bytes.b << 8u) | albedo_bytes.a;
		atomicMax(nodes[node_idx].albedo, packed_color); // Use atomicMax to ensure it writes if 0

		vec3 emission = meshlet_materials.data[material_id].emission;
		uvec3 emission_bytes = uvec3(clamp(emission, vec3(0.0), vec3(1.0)) * 255.0 + 0.5);
		uint packed_emission = (emission_bytes.r << 16u) | (emission_bytes.g << 8u) | emission_bytes.b;
		atomicMax(nodes[node_idx].emission, packed_emission);

		// Pack normal: 8-bit xyz mapped to 0-255
		vec3 n = normal * 0.5 + 0.5;
		uint nx = uint(n.x * 255.0) & 0xFFu;
		uint ny = uint(n.y * 255.0) & 0xFFu;
		uint nz = uint(n.z * 255.0) & 0xFFu;
		uint packed_normal = (nx << 16u) | (ny << 8u) | nz;
		atomicMax(nodes[node_idx].normal, packed_normal);
	}
}
#endif
