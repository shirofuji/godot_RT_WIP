// SVOGI octree cone-trace, factored out of meshlet_shade_inc.glsl so BOTH the resolve shading path
// AND the standalone half-res GI trace pass (meshlet_svogi_gi_trace.glsl) share the exact same
// traversal. Pure extraction - behavior identical to the previous inline copy.
//
// The includer MUST, BEFORE including this file:
//   1. include meshlet_shade_types_inc.glsl (SVOGINode struct + M_PI),
//   2. declare the octree node buffer (any set/binding; name must match):
//        buffer svogi_nodes { SVOGINode data[]; }

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

		uint node_idx = 0u;
		vec3 current_center = bounds_center;
		float current_half = bounds_half;
		vec4 voxel_color = vec4(0.0);
		float target_size = max(diameter, current_half / 64.0); // 6 levels: leaf = bounds/64.

		for (uint depth = 0u; depth < 6u; depth++) {
			bvec3 is_pos = greaterThan(sample_pos, current_center);
			uint child_idx = (is_pos.x ? 1u : 0u) | ((is_pos.y ? 1u : 0u) << 1) | ((is_pos.z ? 1u : 0u) << 2);

			if ((svogi_nodes.data[node_idx].child_mask & (1u << child_idx)) == 0u) {
				break; // Empty space.
			}
			uint base_idx = svogi_nodes.data[node_idx].children_base_index;
			if (base_idx == 0u) {
				break; // No children allocated.
			}
			node_idx = base_idx + child_idx;

			vec3 offset = vec3(is_pos.x ? 1.0 : -1.0, is_pos.y ? 1.0 : -1.0, is_pos.z ? 1.0 : -1.0);
			current_half *= 0.5;
			current_center += offset * current_half;

			// Internal nodes hold mipmap-aggregated data (see svogi_mipmap.glsl), so sampling a
			// coarser level for a wide cone is correct, not just empty.
			if (current_half * 2.0 <= target_size || depth == 5u) {
				// The node's "albedo" field actually holds LIT RADIANCE (surface albedo * direct
				// light + emission), written by svogi_voxelize.glsl's direct-light-injection step.
				uint radiance_packed = svogi_nodes.data[node_idx].albedo;
				if (radiance_packed != 0u) {
					vec3 vox_radiance = vec3(
							float((radiance_packed >> 24u) & 0xFFu),
							float((radiance_packed >> 16u) & 0xFFu),
							float((radiance_packed >> 8u) & 0xFFu)) /
							255.0;
					voxel_color = vec4(vox_radiance * energy, 1.0);
				}
				break;
			}
		}

		if (voxel_color.a > 0.0) {
			float a = (1.0 - color.a);
			color += a * voxel_color;
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
