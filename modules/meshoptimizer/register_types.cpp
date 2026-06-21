/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "scene/resources/surface_tool.h"

#include <thirdparty/meshoptimizer/meshoptimizer.h>

// SurfaceTool::Meshlet/MeshletBounds are plain layout-compatible mirrors of meshopt_Meshlet/
// meshopt_Bounds (see scene/resources/surface_tool.h) so this translation unit is the only
// place that needs to know about the thirdparty struct layout.
static_assert(sizeof(SurfaceTool::Meshlet) == sizeof(meshopt_Meshlet), "SurfaceTool::Meshlet must mirror meshopt_Meshlet's layout.");
static_assert(sizeof(SurfaceTool::MeshletBounds) == sizeof(meshopt_Bounds), "SurfaceTool::MeshletBounds must mirror meshopt_Bounds's layout.");

static size_t meshoptimizer_build_meshlets(SurfaceTool::Meshlet *p_meshlets, unsigned int *p_meshlet_vertices, unsigned char *p_meshlet_triangles, const unsigned int *p_indices, size_t p_index_count, const float *p_vertex_positions, size_t p_vertex_count, size_t p_vertex_positions_stride, size_t p_max_vertices, size_t p_max_triangles, float p_cone_weight) {
	return meshopt_buildMeshlets(reinterpret_cast<meshopt_Meshlet *>(p_meshlets), p_meshlet_vertices, p_meshlet_triangles, p_indices, p_index_count, p_vertex_positions, p_vertex_count, p_vertex_positions_stride, p_max_vertices, p_max_triangles, p_cone_weight);
}

static SurfaceTool::MeshletBounds meshoptimizer_compute_meshlet_bounds(const unsigned int *p_meshlet_vertices, const unsigned char *p_meshlet_triangles, size_t p_triangle_count, const float *p_vertex_positions, size_t p_vertex_count, size_t p_vertex_positions_stride) {
	meshopt_Bounds bounds = meshopt_computeMeshletBounds(p_meshlet_vertices, p_meshlet_triangles, p_triangle_count, p_vertex_positions, p_vertex_count, p_vertex_positions_stride);
	SurfaceTool::MeshletBounds result;
	memcpy(&result, &bounds, sizeof(result));
	return result;
}

void initialize_meshoptimizer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	SurfaceTool::optimize_vertex_cache_func = meshopt_optimizeVertexCache;
	SurfaceTool::optimize_vertex_fetch_remap_func = meshopt_optimizeVertexFetchRemap;
	SurfaceTool::simplify_func = meshopt_simplify;
	SurfaceTool::simplify_with_attrib_func = meshopt_simplifyWithAttributes;
	SurfaceTool::simplify_scale_func = meshopt_simplifyScale;
	SurfaceTool::generate_remap_func = meshopt_generateVertexRemap;
	SurfaceTool::remap_vertex_func = meshopt_remapVertexBuffer;
	SurfaceTool::remap_index_func = meshopt_remapIndexBuffer;
	SurfaceTool::build_meshlets_func = meshoptimizer_build_meshlets;
	SurfaceTool::build_meshlets_bound_func = meshopt_buildMeshletsBound;
	SurfaceTool::compute_meshlet_bounds_func = meshoptimizer_compute_meshlet_bounds;
}

void uninitialize_meshoptimizer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	SurfaceTool::optimize_vertex_cache_func = nullptr;
	SurfaceTool::optimize_vertex_fetch_remap_func = nullptr;
	SurfaceTool::simplify_func = nullptr;
	SurfaceTool::simplify_scale_func = nullptr;
	SurfaceTool::generate_remap_func = nullptr;
	SurfaceTool::remap_vertex_func = nullptr;
	SurfaceTool::remap_index_func = nullptr;
	SurfaceTool::build_meshlets_func = nullptr;
	SurfaceTool::build_meshlets_bound_func = nullptr;
	SurfaceTool::compute_meshlet_bounds_func = nullptr;
}
