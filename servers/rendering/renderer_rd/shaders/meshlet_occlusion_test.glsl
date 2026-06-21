#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	uint work_item_count;
	uint max_visible;
	uint hiz_mip_count;
	uint pad0;
	mat4 view_matrix; // World -> view (camera) space.
	float proj_x_scale;
	float proj_y_scale;
	float proj_z_a;
	float proj_z_b;
	float screen_size[2];
	float pad1[2];
}
params;

struct VisibleMeshlet {
	uint instance_index;
	uint meshlet_index;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer WorkItems {
	uint count;
	VisibleMeshlet data[];
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

layout(set = 0, binding = 3) uniform sampler2D hiz_texture;

layout(set = 0, binding = 4, std430) restrict buffer VisibleMeshlets {
	uint count;
	VisibleMeshlet data[];
}
visible_meshlets;

// Tight epsilon for camera-plane (clip_w) degeneracy checks only - unrelated to the occlusion
// margin floor below.
const float EPSILON = 1e-5;

// Floor for the occlusion comparison's adaptive margin (see below) - NOT the same as EPSILON
// above. The adaptive component (derived from a meshlet's own near-to-far device-Z extent)
// shrinks toward zero for small/distant objects, since reversed-Z compresses far-away depth into
// an increasingly tiny numeric range - confirmed via a real stress scene (many small, distant
// instances) where the pure-adaptive version (effectively falling back near a ~1e-5 floor for
// those) reproduced the original self-occlusion bug, just for small/far objects instead of large/
// near ones. 2e-3 was chosen empirically: large enough to absorb that precision gap, but still
// smaller than a genuine occlusion gap between two meaningfully-different-distance objects (the
// self-test's "distance 4 occluder vs. distance 8 occludee" case has a real gap of ~0.006).
const float OCCLUSION_MARGIN_FLOOR = 2e-3;

void main() {
	uint idx = gl_GlobalInvocationID.x;
	// work_items.count is Pass B's real survivor count, read directly here (no CPU readback
	// stall needed). It's an atomic counter that can overflow past the buffer's actual capacity
	// (the writer's bounds check only gates the write, not the increment), so clamp against
	// params.work_item_count (the buffer's allocated capacity, a fixed CPU-known constant).
	if (idx >= min(work_items.count, params.work_item_count)) {
		return;
	}

	VisibleMeshlet item = work_items.data[idx];
	mat4 transform = transforms.data[item.instance_index];
	MeshletDescriptor d = meshlet_descriptors.data[item.meshlet_index];

	vec3 world_center = (transform * vec4(d.bounds_center, 1.0)).xyz;
	float scale_x = length(transform[0].xyz);
	float scale_y = length(transform[1].xyz);
	float scale_z = length(transform[2].xyz);
	float world_radius = d.bounds_radius * max(max(scale_x, scale_y), scale_z);

	vec4 view_pos = params.view_matrix * vec4(world_center, 1.0);

	// Camera looks down -Z in view space, so the point of the sphere nearest the camera has the
	// largest (least negative) view-space Z.
	float nearest_view_z = view_pos.z + world_radius;
	float clip_w_nearest = -nearest_view_z;
	if (clip_w_nearest <= EPSILON) {
		// Nearest point is at/behind the camera plane - the projection math below breaks down;
		// conservatively treat as visible rather than risk a false occlusion (pop-in).
		uint slot = atomicAdd(visible_meshlets.count, 1);
		if (slot < params.max_visible) {
			visible_meshlets.data[slot] = item;
		}
		return;
	}
	float nearest_device_z = (nearest_view_z * params.proj_z_a + params.proj_z_b) / clip_w_nearest;

	// Screen-space footprint, using the (unshifted) sphere center for the sample position/size -
	// an approximation, consistent with Pass B's frustum test's own approximations.
	float clip_w_center = -view_pos.z;
	if (clip_w_center <= EPSILON) {
		uint slot = atomicAdd(visible_meshlets.count, 1);
		if (slot < params.max_visible) {
			visible_meshlets.data[slot] = item;
		}
		return;
	}
	vec2 ndc = vec2(view_pos.x * params.proj_x_scale, view_pos.y * params.proj_y_scale) / clip_w_center;
	vec2 uv = ndc * 0.5 + 0.5;

	// abs() because the engine's depth-corrected projection flips Y (proj_y_scale is negative) -
	// a screen-space radius is a magnitude, never negative. Without this, screen_radius_px was
	// always negative, which the MAX(...,1.0) clamp below then always collapsed to mip 0
	// regardless of the object's actual screen size - not incorrect, but defeats the point of
	// building a multi-mip Hi-Z chain at all.
	float screen_radius_px = abs(world_radius * params.proj_y_scale * params.screen_size[1] * 0.5) / clip_w_center;
	// Hi-Z mip 0 is half the original screen resolution, so a footprint diameter of
	// `screen_radius_px * 2` original-resolution pixels is `screen_radius_px` Hi-Z-mip0 pixels.
	float mip_f = floor(log2(max(screen_radius_px, 1.0)));
	float mip = clamp(mip_f, 0.0, float(params.hiz_mip_count - 1));

	float occluder_depth = textureLod(hiz_texture, uv, mip).x;

	// Adaptive occlusion-comparison epsilon, derived from this meshlet's *own* near-to-far device-Z
	// extent (NOT a fixed constant). Reversed-Z packs precision very unevenly with distance - the
	// same absolute device-Z gap means a tiny world-space distance near the camera but a huge one
	// far away - so a single fixed epsilon is fundamentally wrong: 1e-5 was too tight to absorb a
	// real bounding-sphere-vs-rasterized-depth precision gap (confirmed via live diagnostics, ~0.0013
	// for a meshlet self-testing against its own object's depth) and caused visible front-facing
	// self-occlusion; bumping it to a fixed 1e-2 then broke genuine cross-object occlusion in the
	// self-test (a real, more-distant occluder's device-Z gap can be *smaller* than 1e-2 once both
	// objects are far enough that reversed-Z's precision has compressed). Scaling the margin to this
	// meshlet's own depth extent fixes both: the margin naturally grows with bounding-sphere size at
	// any given distance (absorbing its own imprecision) while staying far smaller than a genuine
	// gap to a separate, meaningfully-more-distant occluder.
	float farthest_view_z = view_pos.z - world_radius;
	float clip_w_farthest = -farthest_view_z;
	float farthest_device_z = clip_w_farthest <= EPSILON ? nearest_device_z : (farthest_view_z * params.proj_z_a + params.proj_z_b) / clip_w_farthest;
	float occlusion_margin = max(abs(nearest_device_z - farthest_device_z) * 0.5, OCCLUSION_MARGIN_FLOOR);

	// Godot RD's reversed-Z convention (near=1.0, far=0.0): occluder_depth is the *farthest* real
	// sample recorded in this footprint (HiZBuilder now min-reduces, not max - see its shader's
	// comment). The object is occluded only if its own nearest point is even farther (a smaller
	// reversed-Z value) than that - i.e. genuinely behind everything drawn in this region, beyond
	// this meshlet's own margin of imprecision. (A prior version of this comparison assumed
	// non-reversed depth and used `>= occluder_depth + EPSILON` - confirmed wrong against the real
	// engine: it culled near/foreground objects instead of distant ones.)
	if (nearest_device_z <= occluder_depth - occlusion_margin) {
		return; // Occluded.
	}

	uint slot = atomicAdd(visible_meshlets.count, 1);
	if (slot < params.max_visible) {
		visible_meshlets.data[slot] = item;
	}
}
