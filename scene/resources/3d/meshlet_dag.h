/**************************************************************************/
/*  meshlet_dag.h                                                         */
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

#pragma once

#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// Builds a Nanite-style meshlet DAG (directed acyclic graph of cluster LODs) from a triangle mesh.
//
// The DAG is the data structure that makes crack-free continuous per-cluster LOD possible: starting
// from the full-resolution meshlets (LOD 0), adjacent meshlets are repeatedly grouped, each group's
// triangles merged and simplified to ~half while its OUTER boundary vertices are locked (so adjacent
// groups stay watertight), and the simplified result re-split into coarser parent meshlets. Each
// cluster records the error of the LOD it belongs to (self_error) and the error of the coarser LOD
// it gets merged into next (parent_error); because error is propagated monotonically (parent_error
// >= self_error) and every cluster in a group shares the same LOD bounding sphere on each side, a
// runtime "cut" of [self_error <= screen_threshold < parent_error] selects a watertight subset of
// clusters across mixed LODs.
//
// All clusters across all levels reference one shared vertex position array (simplification only
// collapses existing vertices, never adds new ones), so the whole DAG can be uploaded as a single
// cluster pool over the surface's existing vertex buffer.
class MeshletDAG {
public:
	struct Cluster {
		// Renderable geometry: a flat triangle list (3 entries per triangle), each entry a global
		// index into the shared vertex position array. Cluster-sized (<= MAX_TRIANGLES triangles).
		LocalVector<uint32_t> indices;

		// Bounding sphere of this cluster's own geometry - for frustum / cone / Hi-Z culling.
		Vector3 bounds_center;
		float bounds_radius = 0.0f;

		// LOD cut data. self_* is the LOD group this cluster belongs to (the group it was produced
		// by simplifying); parent_* is the coarser LOD group it gets simplified into next. A cluster
		// is the right LOD to draw when projected_error(parent) > threshold >= projected_error(self).
		// LOD-0 leaves: self_error 0. Roots (coarsest): parent_error +inf (always pass the upper test).
		Vector3 self_lod_center;
		float self_lod_radius = 0.0f;
		float self_error = 0.0f;
		Vector3 parent_lod_center;
		float parent_lod_radius = 0.0f;
		float parent_error = 0.0f; // Set to FLT_MAX for roots in build().
	};

	// Builds the DAG over p_positions (shared vertex array) from p_indices (the LOD-0 triangle list,
	// 3 indices per triangle). Appends every cluster across every level to r_clusters. Returns false
	// (and leaves r_clusters as a single-level fallback) if the meshoptimizer hooks aren't available
	// or the input is degenerate. p_max_vertices/p_max_triangles follow the meshlet cap used
	// elsewhere in the renderer (64 / 124).
	static bool build(const PackedVector3Array &p_positions, const PackedInt32Array &p_indices, LocalVector<Cluster> &r_clusters, uint32_t p_max_vertices = 64, uint32_t p_max_triangles = 124);
};
