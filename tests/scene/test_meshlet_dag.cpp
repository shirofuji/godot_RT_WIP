/**************************************************************************/
/*  test_meshlet_dag.cpp                                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_meshlet_dag)

#ifndef _3D_DISABLED

#include "scene/resources/3d/meshlet_dag.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/mesh.h"
#include "scene/resources/surface_tool.h"

#include <float.h>

namespace TestMeshletDAG {

// A sphere dense enough (~2k triangles) to split into many LOD-0 meshlets, so the DAG actually
// coarsens across several levels rather than bottoming out at a single cluster.
static void get_dense_sphere(PackedVector3Array &r_vertices, PackedInt32Array &r_indices) {
	Ref<SphereMesh> sphere = memnew(SphereMesh);
	sphere->set_radius(1.0f);
	sphere->set_height(2.0f);
	sphere->set_radial_segments(32);
	sphere->set_rings(32);

	Array arrays = sphere->surface_get_arrays(0);
	r_vertices = arrays[Mesh::ARRAY_VERTEX];
	r_indices = arrays[Mesh::ARRAY_INDEX];
}

TEST_CASE("[SceneTree][MeshletDAG] Builds a multi-level cluster DAG with monotonic error") {
	if (SurfaceTool::build_meshlets_func == nullptr || SurfaceTool::simplify_with_attrib_func == nullptr) {
		WARN("meshoptimizer hooks not registered - skipping MeshletDAG test.");
		return;
	}

	PackedVector3Array vertices;
	PackedInt32Array indices;
	get_dense_sphere(vertices, indices);
	REQUIRE(indices.size() >= 3);
	REQUIRE(indices.size() % 3 == 0);

	LocalVector<MeshletDAG::Cluster> clusters;
	bool ok = MeshletDAG::build(vertices, indices, clusters);
	CHECK_MESSAGE(ok, "DAG build should succeed on a valid dense mesh.");
	REQUIRE_MESSAGE(clusters.size() > 0, "DAG must produce at least one cluster.");

	// Classify clusters by level role.
	uint32_t leaf_count = 0; // self_error == 0 -> finest LOD (the input split into meshlets).
	uint32_t root_count = 0; // parent_error == FLT_MAX -> coarsest, never merged further.
	uint32_t parent_count = 0; // self_error > 0 -> produced by simplifying a coarser level.
	const uint32_t vertex_count = (uint32_t)vertices.size();

	for (const MeshletDAG::Cluster &c : clusters) {
		// Every cluster must hold real, in-range triangle geometry.
		CHECK_MESSAGE(c.indices.size() >= 3, "Every cluster must have at least one triangle.");
		CHECK_MESSAGE(c.indices.size() % 3 == 0, "Cluster index count must be a multiple of 3.");
		for (uint32_t idx : c.indices) {
			CHECK_MESSAGE(idx < vertex_count, "Cluster references a valid (in-range) shared vertex.");
		}
		// The cut invariant that makes mixed-LOD selection watertight: a cluster can never be
		// replaced by a parent whose error is smaller than its own.
		CHECK_MESSAGE(c.parent_error >= c.self_error, "parent_error must be >= self_error (monotonic).");
		CHECK_MESSAGE(c.self_error >= 0.0f, "self_error must be non-negative.");
		CHECK_MESSAGE(c.bounds_radius > 0.0f, "Cluster must have a non-degenerate bounding sphere.");

		if (c.self_error == 0.0f) {
			leaf_count++;
		} else {
			parent_count++;
		}
		if (c.parent_error == FLT_MAX) {
			root_count++;
		}
	}

	// A real DAG (not just a flat meshlet split) must have coarsened at least once...
	CHECK_MESSAGE(parent_count > 0, "DAG must contain coarser parent clusters, not just LOD-0 leaves.");
	CHECK_MESSAGE(leaf_count > 0, "DAG must contain LOD-0 leaf clusters.");
	CHECK_MESSAGE(root_count > 0, "DAG must terminate in at least one root cluster.");
	// ...and the coarsest level must be strictly smaller than the finest (genuine reduction).
	CHECK_MESSAGE(root_count < leaf_count, "There must be fewer roots than leaves (the DAG narrows toward the top).");

	// The coarsest level (roots) must hold far fewer triangles than the finest (LOD 0) - i.e.
	// coarsening genuinely reduced geometry up the DAG, not just re-clustered it. (Note: the SUM of
	// all coarse levels is expected to be ~= the leaf level for a pyramid that halves per level - a
	// geometric series - so the meaningful reduction check is finest-vs-coarsest, not finest-vs-sum.
	// How aggressively each level halves depends on grouping quality: the current spatial Morton
	// partitioner leaves a fair amount of locked inter-group boundary, so per-level reduction is
	// modest; adjacency-based grouping is the planned quality improvement.)
	uint64_t leaf_tris = 0;
	uint64_t root_tris = 0;
	for (const MeshletDAG::Cluster &c : clusters) {
		if (c.self_error == 0.0f) {
			leaf_tris += c.indices.size() / 3;
		}
		if (c.parent_error == FLT_MAX) {
			root_tris += c.indices.size() / 3;
		}
	}
	CHECK_MESSAGE(root_tris < leaf_tris, "Coarsest (root) level must hold fewer triangles than LOD 0.");

	// --- flatten_to_arrays(): the cluster pool must convert losslessly into meshlet upload form. ---
	Vector<SurfaceTool::Meshlet> meshlets;
	PackedInt32Array meshlet_vertices;
	PackedByteArray meshlet_triangles;
	Vector<SurfaceTool::MeshletBounds> bounds;
	LocalVector<MeshletDAG::ClusterLOD> lods;
	MeshletDAG::flatten_to_arrays(clusters, vertices, meshlets, meshlet_vertices, meshlet_triangles, bounds, lods);

	REQUIRE_MESSAGE(meshlets.size() == (int)clusters.size(), "One meshlet per cluster.");
	REQUIRE_MESSAGE(lods.size() == clusters.size(), "One LOD-cut record per cluster.");
	REQUIRE_MESSAGE(bounds.size() == (int)clusters.size(), "One bound per cluster.");

	for (uint32_t c = 0; c < clusters.size(); c++) {
		const SurfaceTool::Meshlet &ml = meshlets[c];
		const MeshletDAG::Cluster &cl = clusters[c];
		CHECK_MESSAGE(ml.triangle_count == cl.indices.size() / 3, "Meshlet triangle count matches the cluster.");
		CHECK_MESSAGE(ml.vertex_count <= 64, "Meshlet vertex count within cap.");
		CHECK_MESSAGE(ml.triangle_count <= 124, "Meshlet triangle count within cap.");

		// Reconstruct this meshlet's global triangle indices and compare to the cluster's, in order.
		bool match = true;
		for (uint32_t t = 0; t < ml.triangle_count * 3; t++) {
			uint8_t local = meshlet_triangles[ml.triangle_offset + t];
			int global = meshlet_vertices[ml.vertex_offset + local];
			CHECK_MESSAGE((uint32_t)global < vertex_count, "Remapped meshlet vertex is in range.");
			if ((uint32_t)global != cl.indices[t]) {
				match = false;
			}
		}
		CHECK_MESSAGE(match, "Reconstructed meshlet triangles must equal the cluster's flat indices.");

		// LOD-cut data must survive the conversion exactly.
		CHECK(lods[c].self_error == cl.self_error);
		CHECK(lods[c].parent_error == cl.parent_error);
	}
}

} // namespace TestMeshletDAG

#endif // _3D_DISABLED
