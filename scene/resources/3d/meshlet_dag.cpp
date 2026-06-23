/**************************************************************************/
/*  meshlet_dag.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "meshlet_dag.h"

#include "core/templates/hash_map.h"
#include "scene/resources/surface_tool.h"

#include <float.h>
#include <utility>

namespace {

// Triangle count we aim to keep per group when simplifying one level into the next. ~4 clusters of
// ~124 tris merged and halved -> ~250 tris -> ~2-3 parent clusters, a ~2x reduction per level.
constexpr uint32_t GROUP_SIZE = 4;

// Bounding sphere (centroid + max radius) of the unique vertices referenced by a flat index list.
void compute_bounds(const LocalVector<uint32_t> &p_indices, const Vector3 *p_positions, Vector3 &r_center, float &r_radius) {
	if (p_indices.is_empty()) {
		r_center = Vector3();
		r_radius = 0.0f;
		return;
	}
	Vector3 sum;
	for (uint32_t idx : p_indices) {
		sum += p_positions[idx];
	}
	r_center = sum / (float)p_indices.size();
	float max_sq = 0.0f;
	for (uint32_t idx : p_indices) {
		float d = (p_positions[idx] - r_center).length_squared();
		if (d > max_sq) {
			max_sq = d;
		}
	}
	r_radius = Math::sqrt(max_sq);
}

// Runs meshoptimizer's clusterizer over a flat index list and converts each produced meshlet into a
// MeshletDAG::Cluster holding flat global indices + a bounding sphere. Only geometry/bounds are
// filled; LOD-cut fields are set by the caller.
void split_into_clusters(const PackedVector3Array &p_positions, const LocalVector<uint32_t> &p_indices, uint32_t p_max_vertices, uint32_t p_max_triangles, LocalVector<MeshletDAG::Cluster> &r_out) {
	if (p_indices.size() < 3) {
		return;
	}
	PackedInt32Array indices;
	indices.resize(p_indices.size());
	for (uint32_t i = 0; i < p_indices.size(); i++) {
		indices.write[i] = (int)p_indices[i];
	}
	PackedInt32Array meshlet_vertices;
	PackedByteArray meshlet_triangles;
	Vector<SurfaceTool::MeshletBounds> bounds;
	Vector<SurfaceTool::Meshlet> meshlets = SurfaceTool::build_meshlets(p_positions, indices, p_max_vertices, p_max_triangles, 0.5f, meshlet_vertices, meshlet_triangles, bounds);

	const Vector3 *positions = p_positions.ptr();
	for (int m = 0; m < meshlets.size(); m++) {
		const SurfaceTool::Meshlet &ml = meshlets[m];
		MeshletDAG::Cluster c;
		c.indices.reserve(ml.triangle_count * 3);
		for (uint32_t t = 0; t < ml.triangle_count * 3; t++) {
			uint8_t local = meshlet_triangles[ml.triangle_offset + t];
			uint32_t global = (uint32_t)meshlet_vertices[ml.vertex_offset + local];
			c.indices.push_back(global);
		}
		compute_bounds(c.indices, positions, c.bounds_center, c.bounds_radius);
		r_out.push_back(c);
	}
}

// Spatially groups a working set of clusters into groups of ~GROUP_SIZE by sorting their centroids
// along a Morton (Z-order) curve so spatially-near clusters end up adjacent, then chunking. This is
// the simple first-cut partitioner: it keeps groups reasonably compact (so the locked inter-group
// boundary stays small and simplification is effective) without needing a full graph partitioner.
// Adjacency-based grouping (minimizing shared locked edges) is a later quality improvement.
uint32_t morton_part(uint32_t x) {
	x &= 0x3ff;
	x = (x | (x << 16)) & 0x030000ff;
	x = (x | (x << 8)) & 0x0300f00f;
	x = (x | (x << 4)) & 0x030c30c3;
	x = (x | (x << 2)) & 0x09249249;
	return x;
}

void group_clusters(const LocalVector<uint32_t> &p_working, const LocalVector<MeshletDAG::Cluster> &p_clusters, LocalVector<LocalVector<uint32_t>> &r_groups) {
	uint32_t n = p_working.size();
	if (n == 0) {
		return;
	}
	// Bounds of the working set's centroids, to quantize into the Morton grid.
	Vector3 mn(FLT_MAX, FLT_MAX, FLT_MAX);
	Vector3 mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	for (uint32_t ci : p_working) {
		Vector3 c = p_clusters[ci].bounds_center;
		mn = mn.min(c);
		mx = mx.max(c);
	}
	Vector3 ext = mx - mn;
	for (int a = 0; a < 3; a++) {
		if (ext[a] <= 0.0f) {
			ext[a] = 1.0f;
		}
	}

	struct Keyed {
		uint64_t key;
		uint32_t cluster;
	};
	struct KeyedSort {
		bool operator()(const Keyed &a, const Keyed &b) const { return a.key < b.key; }
	};
	LocalVector<Keyed> keyed;
	keyed.resize(n);
	for (uint32_t i = 0; i < n; i++) {
		Vector3 c = p_clusters[p_working[i]].bounds_center;
		uint32_t qx = (uint32_t)(CLAMP((c.x - mn.x) / ext.x, 0.0f, 1.0f) * 1023.0f);
		uint32_t qy = (uint32_t)(CLAMP((c.y - mn.y) / ext.y, 0.0f, 1.0f) * 1023.0f);
		uint32_t qz = (uint32_t)(CLAMP((c.z - mn.z) / ext.z, 0.0f, 1.0f) * 1023.0f);
		keyed[i].key = morton_part(qx) | ((uint64_t)morton_part(qy) << 1) | ((uint64_t)morton_part(qz) << 2);
		keyed[i].cluster = p_working[i];
	}
	keyed.sort_custom<KeyedSort>();

	for (uint32_t i = 0; i < n; i += GROUP_SIZE) {
		LocalVector<uint32_t> group;
		for (uint32_t j = i; j < MIN(i + GROUP_SIZE, n); j++) {
			group.push_back(keyed[j].cluster);
		}
		// std::move: LocalVector's copy constructor is explicit (so a by-value push_back of an
		// lvalue won't copy-initialize); moving the just-built group avoids that and is what we want
		// anyway since `group` is rebuilt fresh each iteration.
		r_groups.push_back(std::move(group));
	}
}

// Per-vertex lock array (1 = locked) for a merged group: locks every vertex on the group's outer
// boundary, i.e. endpoints of any edge used by exactly one of the group's triangles. Because a true
// inter-group boundary edge is used once in each of the two groups it separates, both groups lock
// the same vertices independently -> the simplification can't move them on either side -> the seam
// stays watertight. (Also locks mesh-open-border vertices, which is correct.) Sized to the full
// shared vertex array and indexed by global vertex id.
void compute_boundary_lock(const LocalVector<uint32_t> &p_merged, uint32_t p_vertex_count, LocalVector<uint8_t> &r_lock) {
	r_lock.clear();
	r_lock.resize(p_vertex_count);
	memset(r_lock.ptr(), 0, p_vertex_count);

	HashMap<uint64_t, uint32_t> edge_count;
	uint32_t tri_count = p_merged.size() / 3;
	for (uint32_t t = 0; t < tri_count; t++) {
		uint32_t a = p_merged[t * 3 + 0];
		uint32_t b = p_merged[t * 3 + 1];
		uint32_t c = p_merged[t * 3 + 2];
		uint32_t e[3][2] = { { a, b }, { b, c }, { c, a } };
		for (int i = 0; i < 3; i++) {
			uint32_t lo = MIN(e[i][0], e[i][1]);
			uint32_t hi = MAX(e[i][0], e[i][1]);
			uint64_t key = ((uint64_t)lo << 32) | (uint64_t)hi;
			edge_count[key] = edge_count.has(key) ? edge_count[key] + 1 : 1;
		}
	}
	for (const KeyValue<uint64_t, uint32_t> &kv : edge_count) {
		if (kv.value == 1) {
			uint32_t lo = (uint32_t)(kv.key >> 32);
			uint32_t hi = (uint32_t)(kv.key & 0xffffffff);
			r_lock[lo] = 1;
			r_lock[hi] = 1;
		}
	}
}

} // namespace

bool MeshletDAG::build(const PackedVector3Array &p_positions, const PackedInt32Array &p_indices, LocalVector<Cluster> &r_clusters, uint32_t p_max_vertices, uint32_t p_max_triangles) {
	r_clusters.clear();
	if (SurfaceTool::build_meshlets_func == nullptr || SurfaceTool::simplify_with_attrib_func == nullptr) {
		return false;
	}
	if (p_indices.size() < 3 || p_positions.is_empty()) {
		return false;
	}

	const uint32_t vertex_count = (uint32_t)p_positions.size();
	const Vector3 *positions = p_positions.ptr();

	// meshoptimizer takes float* positions; real_t may be double (precision=double build), so build
	// a packed float copy once up front (mirrors SurfaceTool::build_meshlets()'s own conversion).
	LocalVector<float> positions_f;
	positions_f.resize(vertex_count * 3);
	for (uint32_t i = 0; i < vertex_count; i++) {
		positions_f[i * 3 + 0] = (float)positions[i].x;
		positions_f[i * 3 + 1] = (float)positions[i].y;
		positions_f[i * 3 + 2] = (float)positions[i].z;
	}

	// --- Level 0: split the input into full-resolution clusters. ---
	LocalVector<uint32_t> lod0_indices;
	lod0_indices.resize(p_indices.size());
	for (int i = 0; i < p_indices.size(); i++) {
		lod0_indices[i] = (uint32_t)p_indices[i];
	}
	LocalVector<Cluster> level0;
	split_into_clusters(p_positions, lod0_indices, p_max_vertices, p_max_triangles, level0);
	if (level0.is_empty()) {
		return false;
	}
	LocalVector<uint32_t> working; // indices into r_clusters of the current (finest unmerged) level.
	for (Cluster &c : level0) {
		c.self_error = 0.0f;
		c.self_lod_center = c.bounds_center;
		c.self_lod_radius = c.bounds_radius;
		c.parent_error = FLT_MAX; // Overwritten below if/when this cluster gets merged.
		working.push_back(r_clusters.size());
		r_clusters.push_back(c);
	}

	// --- Coarsen level by level until a single cluster remains (or simplification stalls). ---
	const int MAX_LEVELS = 24; // Safety bound; log2(triangles)-ish in practice.
	for (int level = 0; level < MAX_LEVELS && working.size() > 1; level++) {
		LocalVector<LocalVector<uint32_t>> groups;
		group_clusters(working, r_clusters, groups);

		LocalVector<uint32_t> next;
		for (const LocalVector<uint32_t> &group : groups) {
			// Merge the group's triangles into one index list.
			LocalVector<uint32_t> merged;
			float child_max_error = 0.0f;
			for (uint32_t ci : group) {
				const Cluster &c = r_clusters[ci];
				for (uint32_t idx : c.indices) {
					merged.push_back(idx);
				}
				child_max_error = MAX(child_max_error, c.self_error);
			}
			uint32_t merged_tris = merged.size() / 3;
			if (merged_tris < 2) {
				// Nothing meaningful to simplify - carry the lone cluster up unchanged as a root.
				for (uint32_t ci : group) {
					next.push_back(ci);
				}
				continue;
			}

			// Group LOD bounding sphere (shared by this group's clusters as their parent bound and
			// by the parents we produce as their self bound - this shared sphere is what keeps the
			// cut consistent across the group).
			Vector3 group_center;
			float group_radius;
			compute_bounds(merged, positions, group_center, group_radius);

			// Lock the group's outer boundary, then simplify the interior to ~half the triangles.
			LocalVector<uint8_t> lock;
			compute_boundary_lock(merged, vertex_count, lock);

			LocalVector<uint32_t> simplified;
			simplified.resize(merged.size());
			float sim_error = 0.0f;
			size_t result_count = SurfaceTool::simplify_with_attrib_func(
					(unsigned int *)simplified.ptr(), (const unsigned int *)merged.ptr(), merged.size(),
					positions_f.ptr(), vertex_count, sizeof(float) * 3,
					nullptr, 0, nullptr, 0, // No attributes.
					lock.ptr(),
					(size_t)((merged_tris / 2) * 3), FLT_MAX,
					SurfaceTool::SIMPLIFY_ERROR_ABSOLUTE, &sim_error);
			simplified.resize(result_count);

			// Monotonic error: a parent LOD can never have less error than any child it replaces.
			float group_error = MAX(sim_error, child_max_error);

			// Record this group as the children's parent LOD.
			for (uint32_t ci : group) {
				r_clusters[ci].parent_error = group_error;
				r_clusters[ci].parent_lod_center = group_center;
				r_clusters[ci].parent_lod_radius = group_radius;
			}

			// If simplification couldn't reduce the group (e.g. almost everything boundary-locked),
			// don't re-split it into ~the same clusters - carry the children up as roots instead, so
			// the level actually shrinks and the loop terminates.
			if (result_count == 0 || (result_count / 3) >= merged_tris) {
				for (uint32_t ci : group) {
					r_clusters[ci].parent_error = FLT_MAX; // It's effectively a root after all.
					next.push_back(ci);
				}
				continue;
			}

			// Re-split the simplified group into coarser parent clusters.
			LocalVector<Cluster> parents;
			split_into_clusters(p_positions, simplified, p_max_vertices, p_max_triangles, parents);
			for (Cluster &pc : parents) {
				pc.self_error = group_error;
				pc.self_lod_center = group_center;
				pc.self_lod_radius = group_radius;
				pc.parent_error = FLT_MAX; // Overwritten if merged at the next level.
				next.push_back(r_clusters.size());
				r_clusters.push_back(pc);
			}
		}

		if (next.size() >= working.size()) {
			// No net progress this level (couldn't coarsen further) - the remaining clusters are
			// roots. Leave their parent_error at FLT_MAX (set above) and stop.
			break;
		}
		working = next;
	}

	return true;
}
