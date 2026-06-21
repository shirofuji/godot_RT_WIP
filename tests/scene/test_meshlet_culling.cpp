/**************************************************************************/
/*  test_meshlet_culling.cpp                                             */
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

TEST_FORCE_LINK(test_meshlet_culling)

#ifndef _3D_DISABLED

#include "core/math/projection.h"
#include "core/templates/a_hash_map.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/surface_tool.h"
#include "servers/rendering/renderer_rd/meshlet_culler.h"
#include "servers/rendering/renderer_rd/storage_rd/meshlet_storage.h"

namespace TestMeshletCulling {

static void _transform_to_mat4_columns(const Transform3D &p_transform, float r_out[16]) {
	r_out[0] = p_transform.basis.rows[0].x;
	r_out[1] = p_transform.basis.rows[1].x;
	r_out[2] = p_transform.basis.rows[2].x;
	r_out[3] = 0.0f;

	r_out[4] = p_transform.basis.rows[0].y;
	r_out[5] = p_transform.basis.rows[1].y;
	r_out[6] = p_transform.basis.rows[2].y;
	r_out[7] = 0.0f;

	r_out[8] = p_transform.basis.rows[0].z;
	r_out[9] = p_transform.basis.rows[1].z;
	r_out[10] = p_transform.basis.rows[2].z;
	r_out[11] = 0.0f;

	r_out[12] = p_transform.origin.x;
	r_out[13] = p_transform.origin.y;
	r_out[14] = p_transform.origin.z;
	r_out[15] = 1.0f;
}

// Mirrors meshlet_cull.glsl's math exactly, operating on orthonormal (no-scale) transforms only
// - matching the shader's own noted approximation for non-uniform scale, which this test doesn't
// exercise.
static bool _cpu_reference_visible(const Transform3D &p_transform, const RendererRD::MeshletStorage::MeshletDescriptorGPU &p_descriptor, const Vector<Plane> &p_planes, const Vector3 &p_camera_position) {
	Vector3 center(p_descriptor.bounds_center[0], p_descriptor.bounds_center[1], p_descriptor.bounds_center[2]);
	Vector3 cone_axis(p_descriptor.cone_axis[0], p_descriptor.cone_axis[1], p_descriptor.cone_axis[2]);

	Vector3 world_center = p_transform.xform(center);
	float world_radius = p_descriptor.bounds_radius; // No-scale transforms in this test.

	Vector3 world_cone_axis = p_transform.basis.xform(cone_axis).normalized();
	Vector3 to_meshlet = world_center - p_camera_position;
	float dist = to_meshlet.length();
	if (dist > 0.0001f) {
		if (world_cone_axis.dot(to_meshlet) >= p_descriptor.cone_cutoff * dist + world_radius) {
			return false; // Backfacing.
		}
	}

	for (int i = 0; i < p_planes.size(); i++) {
		float dist_to_plane = p_planes[i].normal.dot(world_center) - p_planes[i].d;
		if (dist_to_plane >= world_radius) {
			return false; // Outside this frustum plane.
		}
	}

	return true;
}

TEST_CASE("[SceneTree][Meshlet] GPU meshlet culling exactly matches a CPU frustum+cone reference") {
	RendererRD::MeshletStorage *storage = RendererRD::MeshletStorage::get_singleton();
	RendererRD::MeshletCuller *culler = RendererRD::MeshletCuller::get_singleton();
	if (!storage || !culler || !SurfaceTool::build_meshlets_func) {
		// Godot's --test harness always brings up RasterizerDummy (Main::test_setup() in
		// main.cpp), never a real RenderingDevice, so this never actually runs here - see
		// servers/rendering/renderer_rd/meshlet_selftest.h for the real verification path
		// (`godot.exe --headless --quit --meshlet-selftest`), which is what actually exercises
		// this GPU-dependent code against the same CPU reference.
		return;
	}

	// Real sphere geometry: meshlets face every direction, so a single instance already
	// exercises cone rejection (the half facing away from the camera) alongside frustum
	// rejection (whole instances placed outside the view).
	Ref<SphereMesh> sphere;
	sphere.instantiate();
	sphere->set_radius(1.0f);
	sphere->set_height(2.0f);
	sphere->set_radial_segments(12);
	sphere->set_rings(12);
	Array arrays = sphere->surface_get_arrays(0);
	PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
	PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];

	PackedInt32Array meshlet_vertices;
	PackedByteArray meshlet_triangles;
	Vector<SurfaceTool::MeshletBounds> bounds_st;
	Vector<SurfaceTool::Meshlet> meshlets_st = SurfaceTool::build_meshlets(vertices, indices, 64, 124, 0.5f, meshlet_vertices, meshlet_triangles, bounds_st);
	REQUIRE(meshlets_st.size() > 0);

	Vector<RenderingServerTypes::MeshletInfo> meshlets_info;
	meshlets_info.resize(meshlets_st.size());
	memcpy(meshlets_info.ptrw(), meshlets_st.ptr(), sizeof(SurfaceTool::Meshlet) * meshlets_st.size());
	Vector<RenderingServerTypes::MeshletBoundsInfo> bounds_info;
	bounds_info.resize(bounds_st.size());
	memcpy(bounds_info.ptrw(), bounds_st.ptr(), sizeof(SurfaceTool::MeshletBounds) * bounds_st.size());

	RendererRD::MeshletStorage::UploadResult upload = storage->upload_mesh_meshlets(vertices, PackedVector3Array(), PackedVector2Array(), meshlets_info, meshlet_vertices, meshlet_triangles, bounds_info);
	REQUIRE(upload.is_valid());

	// Hand-placed instances: one at the origin (camera looks down -Z from (0,0,5), so this is
	// squarely in view), one far to the left (outside the frustum), one directly behind the
	// camera (outside, behind the near plane), and one close in front (in view, used alongside
	// the origin one to confirm cone rejection differs per-instance based on position).
	Vector<Transform3D> instance_transforms;
	instance_transforms.push_back(Transform3D(Basis(), Vector3(0, 0, 0)));
	instance_transforms.push_back(Transform3D(Basis(), Vector3(50, 0, 0)));
	instance_transforms.push_back(Transform3D(Basis(), Vector3(0, 0, 10)));
	instance_transforms.push_back(Transform3D(Basis(), Vector3(0, 0, 2)));

	LocalVector<float> transforms_data;
	transforms_data.resize(instance_transforms.size() * 16);
	for (int i = 0; i < instance_transforms.size(); i++) {
		_transform_to_mat4_columns(instance_transforms[i], &transforms_data[i * 16]);
	}
	RID transforms_buffer = RD::get_singleton()->storage_buffer_create(transforms_data.size() * sizeof(float));
	RD::get_singleton()->buffer_update(transforms_buffer, 0, transforms_data.size() * sizeof(float), transforms_data.ptr());

	Vector<RendererRD::MeshletCuller::InstanceMeshletRange> ranges;
	for (int i = 0; i < instance_transforms.size(); i++) {
		RendererRD::MeshletCuller::InstanceMeshletRange r;
		r.instance_index = i;
		r.meshlet_offset = upload.meshlet_range.offset;
		r.meshlet_count = upload.meshlet_range.count;
		ranges.push_back(r);
	}

	Transform3D camera_xform(Basis(), Vector3(0, 0, 5));
	Projection projection = Projection::create_perspective(70.0f, 1.0f, 0.05f, 20.0f);
	Vector<Plane> planes = projection.get_projection_planes(camera_xform);
	REQUIRE(planes.size() == 6);

	RendererRD::MeshletCuller::CullResult result = culler->cull(transforms_buffer, ranges, planes, camera_xform.origin);
	REQUIRE(result.is_valid());
	Vector<RendererRD::MeshletCuller::VisibleMeshlet> gpu_visible = culler->debug_read_visible(result);

	// Build the CPU reference set over every (instance, meshlet) pair and compare exactly.
	AHashMap<uint64_t, bool> expected;
	int expected_count = 0;
	for (int i = 0; i < instance_transforms.size(); i++) {
		for (uint32_t m = 0; m < upload.meshlet_range.count; m++) {
			uint32_t global_meshlet_index = upload.meshlet_range.offset + m;
			RendererRD::MeshletStorage::MeshletDescriptorGPU descriptor = storage->debug_get_meshlet_descriptor(global_meshlet_index);
			if (_cpu_reference_visible(instance_transforms[i], descriptor, planes, camera_xform.origin)) {
				uint64_t key = (uint64_t(i) << 32) | global_meshlet_index;
				expected[key] = true;
				expected_count++;
			}
		}
	}

	CHECK_MESSAGE(expected_count > 0, "Test scene must have at least some expected-visible meshlets.");
	CHECK_MESSAGE(expected_count < (int)(instance_transforms.size() * upload.meshlet_range.count), "Test scene must also cull some meshlets (cone and/or frustum), otherwise this test doesn't exercise rejection.");

	CHECK(gpu_visible.size() == expected_count);

	AHashMap<uint64_t, bool> seen;
	for (int i = 0; i < gpu_visible.size(); i++) {
		uint64_t key = (uint64_t(gpu_visible[i].instance_index) << 32) | gpu_visible[i].meshlet_index;
		CHECK_MESSAGE(expected.has(key), "GPU produced a visible meshlet the CPU reference did not expect (false positive).");
		CHECK_MESSAGE(!seen.has(key), "GPU produced the same (instance, meshlet) pair twice.");
		seen[key] = true;
	}
	CHECK_MESSAGE(seen.size() == expected.size(), "GPU must produce every CPU-expected (instance, meshlet) pair (no false negatives).");

	storage->free_mesh_meshlets(upload);
	RD::get_singleton()->free_rid(transforms_buffer);
}

} // namespace TestMeshletCulling

#endif // _3D_DISABLED
