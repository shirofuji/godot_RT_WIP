#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	uint work_item_count;
	uint max_visible;
	float projection_scale; // |P[1][1]| * viewport_height * 0.5: world length -> screen px at unit dist.
	float lod_threshold; // Max acceptable screen-space cluster error, in pixels. <=0 projection_scale = LOD cut off (leaf-only).
	vec4 planes[6]; // xyz = normal, w = d; matches core/math/Plane: distance_to(p) = dot(normal,p) - d.
	vec3 camera_position;
	float pad2;
}
params;

struct WorkItem {
	uint instance_index;
	uint meshlet_index;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer WorkItems {
	uint count;
	WorkItem data[];
}
work_items;

layout(set = 0, binding = 1, std430) restrict readonly buffer Transforms {
	mat4 data[];
}
transforms;

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

layout(set = 0, binding = 2, std430) restrict readonly buffer MeshletDescriptors {
	MeshletDescriptor data[];
}
meshlet_descriptors;

struct VisibleMeshlet {
	uint instance_index;
	uint meshlet_index;
};

layout(set = 0, binding = 3, std430) restrict buffer VisibleMeshlets {
	uint count;
	VisibleMeshlet data[];
}
visible_meshlets;

// Per-meshlet Nanite-style LOD-cut data (parallel to meshlet_descriptors, same index). Mirrors
// MeshletStorage::MeshletLODGPU (48 bytes / 3 vec4 slots). self_* is the LOD this cluster is valid
// at, parent_* the coarser LOD that replaces it. For surfaces baked without a DAG, every record is
// all-zero (self_error 0), which the LOD test below treats as "always selectable".
struct MeshletLOD {
	vec3 self_center;
	float self_error;
	vec3 parent_center;
	float parent_error;
	float self_radius;
	float parent_radius;
	float pad0;
	float pad1;
};

layout(set = 0, binding = 4, std430) restrict readonly buffer MeshletLODs {
	MeshletLOD data[];
}
meshlet_lods;

void main() {
	uint idx = gl_GlobalInvocationID.x;
	// Read the real survivor count from Pass A's own output buffer rather than a CPU-supplied
	// push constant - lets the caller dispatch a fixed upper-bound thread count (cheap, no GPU
	// readback stall) instead of having to read Pass A's atomic counter back to the CPU first.
	// work_items.count is an atomic counter that can overflow past the buffer's actual capacity
	// (Pass A's bounds check only gates the write, not the increment) - clamp against
	// params.work_item_count (repurposed here as the buffer's allocated capacity, still a fixed
	// CPU-known constant) to avoid reading out of bounds.
	if (idx >= min(work_items.count, params.work_item_count)) {
		return;
	}

	WorkItem item = work_items.data[idx];
	mat4 transform = transforms.data[item.instance_index];
	MeshletDescriptor d = meshlet_descriptors.data[item.meshlet_index];

	MeshletLOD lod = meshlet_lods.data[item.meshlet_index];

	// Conservative radius scale: largest axis scale of the transform's basis (exact for uniform
	// scale, conservative - never under-estimates - for non-uniform scale). Also scales the LOD
	// error/bounds below (object-space lengths) into world space.
	float scale_x = length(transform[0].xyz);
	float scale_y = length(transform[1].xyz);
	float scale_z = length(transform[2].xyz);
	float max_scale = max(max(scale_x, scale_y), scale_z);

	// Continuous per-cluster LOD cut (Nanite-style). Keep a cluster iff its own simplification error
	// is acceptable on screen (<= threshold) AND the coarser parent LOD that would replace it is NOT
	// (parent error > threshold) - so exactly one LOD per region survives, and because every cluster
	// in a group shares the same LOD sphere on each side with monotonic error, the surviving subset
	// is watertight across mixed LODs. Errors/bounds are object-space; transform centers, scale
	// lengths by max_scale, and project to screen pixels as error * projection_scale / distance (to
	// the bound's nearest point). projection_scale <= 0 disables the cut (e.g. contexts with no
	// projection info) -> fall back to leaf-only (LOD-0). Non-DAG surfaces have all-zero LOD records
	// (self_error 0, parent_error 0), which both paths keep unconditionally.
	if (params.projection_scale <= 0.0) {
		if (lod.self_error != 0.0) {
			return; // No projection info: render the finest level only.
		}
	} else {
		const float ROOT_ERROR = 1e30; // parent_error sentinel (FLT_MAX) = no coarser parent.
		float self_px = 0.0;
		if (lod.self_error > 0.0) {
			vec3 self_c = (transform * vec4(lod.self_center, 1.0)).xyz;
			float self_d = max(length(self_c - params.camera_position) - lod.self_radius * max_scale, 0.001);
			self_px = (lod.self_error * max_scale) * params.projection_scale / self_d;
		}
		bool is_root = lod.parent_error >= ROOT_ERROR;
		float parent_px = ROOT_ERROR;
		if (!is_root && lod.parent_error > 0.0) {
			vec3 par_c = (transform * vec4(lod.parent_center, 1.0)).xyz;
			float par_d = max(length(par_c - params.camera_position) - lod.parent_radius * max_scale, 0.001);
			parent_px = (lod.parent_error * max_scale) * params.projection_scale / par_d;
		}
		bool lower_ok = self_px <= params.lod_threshold; // this cluster's own error is fine
		bool upper_ok = (lod.parent_error <= 0.0) || is_root || (parent_px > params.lod_threshold); // parent too coarse / none
		if (!(lower_ok && upper_ok)) {
			return; // Not the right LOD for this view.
		}
	}

	vec3 world_center = (transform * vec4(d.bounds_center, 1.0)).xyz;
	float world_radius = d.bounds_radius * max_scale;

	// Normal-cone backface rejection (meshoptimizer's apex-free formula - see
	// meshopt_computeMeshletBounds's docs): cull if facing away from the camera everywhere on
	// the meshlet's bounding sphere. Needs the inverse-transpose of the model matrix, not the
	// model matrix itself, to transform a normal/cone-axis direction correctly under non-uniform
	// scale - mat3(transform) directly was tried here as a cheaper approximation under the
	// assumption that cone_axis (an aggregate direction, not a precise surface normal) wouldn't be
	// sensitive to the error, but that assumption broke down for combined rotation + non-uniform
	// scale: confirmed live, an instance with both produced a normalized cone_axis skewed enough
	// to make the cone test wrongly classify nearly the entire meshlet set as backfacing, leaving
	// only a thin ring of silhouette-adjacent meshlets visible (the same shader-level bug class as
	// meshlet_render.glsl's per-vertex normal fix, just earlier in the pipeline - in the cull pass,
	// not the render pass).
	// Inverse-transpose of the model matrix via the cofactor-matrix identity, not a literal
	// transpose(inverse()) - see meshlet_render.glsl's vertex-shader normal computation for the
	// full derivation. normalize(cofactor * axis) * sign(det) is identical to
	// normalize(transpose(inverse(M)) * axis) for every transform, without the matrix inverse.
	mat3 cone_m = mat3(transform);
	vec3 cone_cofactor0 = cross(cone_m[1], cone_m[2]);
	mat3 cone_normal_matrix = mat3(cone_cofactor0, cross(cone_m[2], cone_m[0]), cross(cone_m[0], cone_m[1]));
	float cone_det_sign = sign(dot(cone_m[0], cone_cofactor0));
	vec3 world_cone_axis = normalize(cone_normal_matrix * d.cone_axis) * cone_det_sign;
	vec3 to_meshlet = world_center - params.camera_position;
	float dist = length(to_meshlet);
	if (dist > 0.0001) {
		if (dot(to_meshlet, world_cone_axis) >= d.cone_cutoff * dist + world_radius) {
			return; // Backfacing - culled.
		}
	}

	for (int i = 0; i < 6; i++) {
		vec4 plane = params.planes[i];
		float dist_to_plane = dot(plane.xyz, world_center) - plane.w;
		if (dist_to_plane >= world_radius) {
			return; // Fully outside this frustum plane - culled.
		}
	}

	uint slot = atomicAdd(visible_meshlets.count, 1);
	if (slot < params.max_visible) {
		visible_meshlets.data[slot].instance_index = item.instance_index;
		visible_meshlets.data[slot].meshlet_index = item.meshlet_index;
	}
}
