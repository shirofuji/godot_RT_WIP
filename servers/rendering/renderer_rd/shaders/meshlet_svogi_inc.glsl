// SVOGI octree cone-trace, factored out of meshlet_shade_inc.glsl so BOTH the resolve shading path
// AND the standalone half-res GI trace pass (meshlet_svogi_gi_trace.glsl) share the exact same
// traversal. Pure extraction - behavior identical to the previous inline copy.
//
// The includer MUST, BEFORE including this file:
//   1. include meshlet_shade_types_inc.glsl (SVOGINode struct + M_PI),
//   2. declare the octree node buffer (any set/binding; name must match):
//        buffer svogi_nodes { SVOGINode data[]; }

// Descend from the root toward `p`, stopping after `level` subdivisions (level in [1,6]; 6 = leaf), and
// return that node's LIT RADIANCE (albedo field holds surface_albedo*direct_light + emission, written by
// svogi_voxelize.glsl). Returns rgb=0 (with present=false) for empty space / unallocated children, so a
// trilinear tap that lands in a hole contributes nothing. Internal nodes hold mipmap-aggregated data
// (svogi_mipmap.glsl), so a coarse level is a valid pre-filtered sample, not just "empty".
vec3 svogi_read_level(vec3 p, int level, vec3 bounds_center, float bounds_half, out bool present) {
	present = false;
	vec3 d = abs(p - bounds_center);
	if (d.x > bounds_half || d.y > bounds_half || d.z > bounds_half) {
		return vec3(0.0);
	}
	uint node_idx = 0u;
	vec3 current_center = bounds_center;
	float current_half = bounds_half;
	for (int depth = 0; depth < level; depth++) {
		bvec3 is_pos = greaterThan(p, current_center);
		uint child_idx = (is_pos.x ? 1u : 0u) | ((is_pos.y ? 1u : 0u) << 1) | ((is_pos.z ? 1u : 0u) << 2);
		if ((svogi_nodes.data[node_idx].child_mask & (1u << child_idx)) == 0u) {
			return vec3(0.0);
		}
		uint base_idx = svogi_nodes.data[node_idx].children_base_index;
		if (base_idx == 0u) {
			return vec3(0.0);
		}
		node_idx = base_idx + child_idx;
		vec3 offset = vec3(is_pos.x ? 1.0 : -1.0, is_pos.y ? 1.0 : -1.0, is_pos.z ? 1.0 : -1.0);
		current_half *= 0.5;
		current_center += offset * current_half;
	}
	uint radiance_packed = svogi_nodes.data[node_idx].albedo;
	if (radiance_packed == 0u) {
		return vec3(0.0);
	}
	present = true;
	return vec3(
				   float((radiance_packed >> 24u) & 0xFFu),
				   float((radiance_packed >> 16u) & 0xFFu),
				   float((radiance_packed >> 8u) & 0xFFu)) /
			255.0;
}

// Trilinear voxel sample at world position `p` for a voxel roughly `target_size` across. Returns
// premultiplied (radiance-weighted-by-coverage) rgb + coverage in a. Interpolating the 8 surrounding
// voxels of the chosen level - instead of snapping to the single nearest voxel - is what removes the
// blocky per-voxel speckle and, more importantly, makes the traced radiance change CONTINUOUSLY as the
// camera moves (the nearest-voxel version snaps at cell boundaries, which is the shimmer/focus-then-fade
// the temporal filter was fighting). Empty voxels contribute 0 radiance AND 0 coverage, so edges fall off.
vec4 svogi_sample_trilinear(vec3 p, float target_size, vec3 bounds_center, float bounds_half, float energy) {
	float bounds_full = bounds_half * 2.0;
	// Pick the level whose voxel size ~= target_size (leaf = level 6 = bounds_full/64).
	int level = clamp(int(round(log2(bounds_full / max(target_size, 1e-3)))), 1, 6);
	float cell = bounds_full / float(1 << level); // voxel full size at this level.
	vec3 bmin = bounds_center - vec3(bounds_half);
	vec3 gp = (p - bmin) / cell - 0.5; // continuous grid coords, aligned to cell centers.
	vec3 g0 = floor(gp);
	vec3 f = gp - g0;

	vec3 acc = vec3(0.0);
	float coverage = 0.0;
	for (int i = 0; i < 8; i++) {
		vec3 corner = g0 + vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		vec3 cell_center = bmin + (corner + 0.5) * cell;
		bool present;
		vec3 rad = svogi_read_level(cell_center, level, bounds_center, bounds_half, present);
		float wx = ((i & 1) == 1) ? f.x : 1.0 - f.x;
		float wy = (((i >> 1) & 1) == 1) ? f.y : 1.0 - f.y;
		float wz = (((i >> 2) & 1) == 1) ? f.z : 1.0 - f.z;
		float w = wx * wy * wz;
		acc += rad * w; // rad is 0 for empty voxels.
		coverage += present ? w : 0.0;
	}
	return vec4(acc * energy, coverage);
}

// Diffuse cone-march of the SVOGI octree, in absolute world space. Returns rgb = accumulated
// incident radiance, a = coverage.
vec4 svogi_cone_trace(vec3 pos, vec3 dir, float tan_half_angle, float max_distance, float bias, vec3 bounds_center, float bounds_half, float energy) {
	vec4 color = vec4(0.0);
	float dist = bias;

	while (dist < max_distance && color.a < 0.95) {
		float diameter = max(1.0, 2.0 * tan_half_angle * dist);
		vec3 sample_pos = pos + dir * dist;

		vec3 d = abs(sample_pos - bounds_center);
		if (d.x > bounds_half || d.y > bounds_half || d.z > bounds_half) {
			break; // Left the octree's bounds.
		}

		// Trilinear tap at this cone level. Premultiplied front-to-back compositing: s.rgb is already
		// radiance*coverage, s.a is coverage.
		vec4 s = svogi_sample_trilinear(sample_pos, diameter, bounds_center, bounds_half, energy);
		if (s.a > 0.0) {
			float a = 1.0 - color.a;
			color.rgb += a * s.rgb;
			color.a += a * s.a;
		}
		dist += max(0.5, diameter * 0.5);
	}

	return color;
}

// Branchless orthonormal basis from a unit normal (Duff et al. 2017).
void svogi_basis(vec3 n, out vec3 t, out vec3 b) {
	float s = n.z >= 0.0 ? 1.0 : -1.0;
	float a = -1.0 / (s + n.z);
	float bb = n.x * n.y * a;
	t = vec3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
	b = vec3(bb, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted 6-cone hemisphere gather of indirect diffuse from the octree.
vec3 svogi_hemisphere_gather(vec3 pos, vec3 normal, float surface_offset, float max_distance, vec3 bounds_center, float bounds_half, float energy) {
	vec3 t, b;
	svogi_basis(normal, t, b);

	const vec3 CONE_DIRS[6] = vec3[](
			vec3(0.0, 0.0, 1.0),
			vec3(0.0, 0.866025, 0.5),
			vec3(0.823639, 0.267617, 0.5),
			vec3(0.509037, -0.700629, 0.5),
			vec3(-0.509037, -0.700629, 0.5),
			vec3(-0.823639, 0.267617, 0.5));
	const float CONE_WEIGHTS[6] = float[](0.25, 0.15, 0.15, 0.15, 0.15, 0.15);

	// Push the shared cone origin off the surface along the normal before tracing (clears local
	// curvature uniformly, avoiding the dark-petal/pinwheel artifact).
	vec3 start = pos + normal * surface_offset;

	vec3 acc = vec3(0.0);
	for (int i = 0; i < 6; i++) {
		vec3 dir = normalize(t * CONE_DIRS[i].x + b * CONE_DIRS[i].y + normal * CONE_DIRS[i].z);
		acc += CONE_WEIGHTS[i] * svogi_cone_trace(start, dir, 0.577, max_distance, surface_offset * 0.5, bounds_center, bounds_half, energy).rgb;
	}
	return acc;
}
