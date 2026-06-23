/**************************************************************************/
/*  meshlet_selftest.cpp                                                 */
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

#include "meshlet_selftest.h"

#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "core/math/projection.h"
#include "core/os/os.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/surface_tool.h"
#include "servers/rendering/renderer_rd/hiz_builder.h"
#include "servers/rendering/renderer_rd/meshlet_culler.h"
#include "servers/rendering/renderer_rd/meshlet_renderer.h"
#include "servers/rendering/renderer_rd/storage_rd/meshlet_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace {

int g_failures = 0;

void check(bool p_condition, const String &p_message) {
	if (p_condition) {
		print_line("MESHLET_SELFTEST: PASS: " + p_message);
	} else {
		print_line("MESHLET_SELFTEST: FAIL: " + p_message);
		g_failures++;
	}
}

void get_sphere_geometry(PackedVector3Array &r_vertices, PackedInt32Array &r_indices) {
	Ref<SphereMesh> sphere;
	sphere.instantiate();
	sphere->set_radius(1.0f);
	sphere->set_height(2.0f);
	sphere->set_radial_segments(12);
	sphere->set_rings(12);
	Array arrays = sphere->surface_get_arrays(0);
	r_vertices = arrays[Mesh::ARRAY_VERTEX];
	r_indices = arrays[Mesh::ARRAY_INDEX];
}

void transform_to_mat4_columns(const Transform3D &p_transform, float r_out[16]) {
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

bool cpu_reference_visible(const Transform3D &p_transform, const RendererRD::MeshletStorage::MeshletDescriptorGPU &p_descriptor, const Vector<Plane> &p_planes, const Vector3 &p_camera_position) {
	Vector3 center(p_descriptor.bounds_center[0], p_descriptor.bounds_center[1], p_descriptor.bounds_center[2]);
	Vector3 cone_axis(p_descriptor.cone_axis[0], p_descriptor.cone_axis[1], p_descriptor.cone_axis[2]);

	Vector3 world_center = p_transform.xform(center);
	float world_radius = p_descriptor.bounds_radius;

	Vector3 world_cone_axis = p_transform.basis.xform(cone_axis).normalized();
	Vector3 to_meshlet = world_center - p_camera_position;
	float dist = to_meshlet.length();
	if (dist > 0.0001f) {
		if (world_cone_axis.dot(to_meshlet) >= p_descriptor.cone_cutoff * dist + world_radius) {
			return false;
		}
	}

	for (int i = 0; i < p_planes.size(); i++) {
		float dist_to_plane = p_planes[i].normal.dot(world_center) - p_planes[i].d;
		if (dist_to_plane >= world_radius) {
			return false;
		}
	}
	return true;
}

void test_meshlet_storage_round_trip() {
	RendererRD::MeshletStorage *storage = RendererRD::MeshletStorage::get_singleton();
	check(storage != nullptr, "MeshletStorage singleton exists");
	if (!storage) {
		return;
	}

	PackedVector3Array vertices;
	PackedInt32Array indices;
	get_sphere_geometry(vertices, indices);

	PackedInt32Array meshlet_vertices;
	PackedByteArray meshlet_triangles;
	Vector<SurfaceTool::MeshletBounds> bounds_st;
	Vector<SurfaceTool::Meshlet> meshlets_st = SurfaceTool::build_meshlets(vertices, indices, 64, 124, 0.5f, meshlet_vertices, meshlet_triangles, bounds_st);
	check(meshlets_st.size() > 0, "build_meshlets produced at least one meshlet");

	Vector<RenderingServerTypes::MeshletInfo> meshlets_info;
	meshlets_info.resize(meshlets_st.size());
	memcpy(meshlets_info.ptrw(), meshlets_st.ptr(), sizeof(SurfaceTool::Meshlet) * meshlets_st.size());
	Vector<RenderingServerTypes::MeshletBoundsInfo> bounds_info;
	bounds_info.resize(bounds_st.size());
	memcpy(bounds_info.ptrw(), bounds_st.ptr(), sizeof(SurfaceTool::MeshletBounds) * bounds_st.size());

	RendererRD::MeshletStorage::UploadResult upload = storage->upload_mesh_meshlets(vertices, PackedVector3Array(), PackedVector2Array(), meshlets_info, meshlet_vertices, meshlet_triangles, bounds_info);
	check(upload.is_valid(), "MeshletStorage::upload_mesh_meshlets returned a valid result");
	check(upload.meshlet_range.count == (uint32_t)meshlets_st.size(), "Uploaded meshlet count matches CPU-built meshlet count");

	bool all_match = true;
	for (int m = 0; m < meshlets_st.size(); m++) {
		RendererRD::MeshletStorage::MeshletDescriptorGPU descriptor = storage->debug_get_meshlet_descriptor(upload.meshlet_range.offset + m);
		if (descriptor.vertex_count != meshlets_st[m].vertex_count || descriptor.triangle_count != meshlets_st[m].triangle_count) {
			all_match = false;
			break;
		}
		if (meshlets_st[m].vertex_count > 0) {
			uint32_t global_remap = storage->debug_get_meshlet_vertex_remap(descriptor.vertex_remap_offset);
			Vector3 global_position = storage->debug_get_vertex_position(global_remap);
			Vector3 expected = vertices[meshlet_vertices[meshlets_st[m].vertex_offset]];
			if (!global_position.is_equal_approx(expected)) {
				all_match = false;
				break;
			}
		}
	}
	check(all_match, "Every readback descriptor/vertex round-trips correctly through real GPU buffers");

	storage->free_mesh_meshlets(upload);
}

void test_meshlet_culler_vs_cpu_reference() {
	RendererRD::MeshletStorage *storage = RendererRD::MeshletStorage::get_singleton();
	RendererRD::MeshletCuller *culler = RendererRD::MeshletCuller::get_singleton();
	check(culler != nullptr, "MeshletCuller singleton exists");
	if (!storage || !culler) {
		return;
	}

	PackedVector3Array vertices;
	PackedInt32Array indices;
	get_sphere_geometry(vertices, indices);

	PackedInt32Array meshlet_vertices;
	PackedByteArray meshlet_triangles;
	Vector<SurfaceTool::MeshletBounds> bounds_st;
	Vector<SurfaceTool::Meshlet> meshlets_st = SurfaceTool::build_meshlets(vertices, indices, 64, 124, 0.5f, meshlet_vertices, meshlet_triangles, bounds_st);

	Vector<RenderingServerTypes::MeshletInfo> meshlets_info;
	meshlets_info.resize(meshlets_st.size());
	memcpy(meshlets_info.ptrw(), meshlets_st.ptr(), sizeof(SurfaceTool::Meshlet) * meshlets_st.size());
	Vector<RenderingServerTypes::MeshletBoundsInfo> bounds_info;
	bounds_info.resize(bounds_st.size());
	memcpy(bounds_info.ptrw(), bounds_st.ptr(), sizeof(SurfaceTool::MeshletBounds) * bounds_st.size());

	RendererRD::MeshletStorage::UploadResult upload = storage->upload_mesh_meshlets(vertices, PackedVector3Array(), PackedVector2Array(), meshlets_info, meshlet_vertices, meshlet_triangles, bounds_info);

	Vector<Transform3D> instance_transforms;
	instance_transforms.push_back(Transform3D(Basis(), Vector3(0, 0, 0)));
	instance_transforms.push_back(Transform3D(Basis(), Vector3(50, 0, 0)));
	instance_transforms.push_back(Transform3D(Basis(), Vector3(0, 0, 10)));
	instance_transforms.push_back(Transform3D(Basis(), Vector3(0, 0, 2)));

	LocalVector<float> transforms_data;
	transforms_data.resize(instance_transforms.size() * 16);
	for (int i = 0; i < instance_transforms.size(); i++) {
		transform_to_mat4_columns(instance_transforms[i], &transforms_data[i * 16]);
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

	RendererRD::MeshletCuller::CullResult result = culler->cull(transforms_buffer, ranges, planes, camera_xform.origin);
	check(result.is_valid(), "MeshletCuller::cull returned a valid result");
	Vector<RendererRD::MeshletCuller::VisibleMeshlet> gpu_visible = culler->debug_read_visible(result);

	HashSet<uint64_t> expected;
	for (int i = 0; i < instance_transforms.size(); i++) {
		for (uint32_t m = 0; m < upload.meshlet_range.count; m++) {
			uint32_t global_meshlet_index = upload.meshlet_range.offset + m;
			RendererRD::MeshletStorage::MeshletDescriptorGPU descriptor = storage->debug_get_meshlet_descriptor(global_meshlet_index);
			if (cpu_reference_visible(instance_transforms[i], descriptor, planes, camera_xform.origin)) {
				expected.insert((uint64_t(i) << 32) | global_meshlet_index);
			}
		}
	}

	check(!expected.is_empty(), "Test scene has at least some expected-visible meshlets");

	HashSet<uint64_t> seen;
	bool false_positive = false;
	bool duplicate = false;
	for (int i = 0; i < gpu_visible.size(); i++) {
		uint64_t key = (uint64_t(gpu_visible[i].instance_index) << 32) | gpu_visible[i].meshlet_index;
		if (!expected.has(key)) {
			false_positive = true;
		}
		if (seen.has(key)) {
			duplicate = true;
		}
		seen.insert(key);
	}
	check(!false_positive, "GPU produced no false-positive visible meshlets");
	check(!duplicate, "GPU produced no duplicate visible meshlets");
	check(seen.size() == expected.size(), "GPU produced every CPU-expected visible meshlet (no false negatives)");

	storage->free_mesh_meshlets(upload);
	RD::get_singleton()->free_rid(transforms_buffer);
}

// Mirrors meshlet_occlusion_test.glsl's math exactly, sampling pre-read-back Hi-Z mip data
// instead of issuing a GPU texture fetch.
bool cpu_occlusion_visible(const Transform3D &p_transform, const RendererRD::MeshletStorage::MeshletDescriptorGPU &p_descriptor, const Transform3D &p_view_matrix, float p_proj_x_scale, float p_proj_y_scale, float p_proj_z_a, float p_proj_z_b, const Size2i &p_screen_size, const Vector<Vector<float>> &p_hiz_mip_data, const Vector<Size2i> &p_hiz_mip_sizes) {
	// Tight epsilon for camera-plane (clip_w) degeneracy checks only - see
	// meshlet_occlusion_test.glsl's comment for why the occlusion comparison itself uses an
	// adaptive margin instead of a fixed epsilon.
	const float EPSILON = 1e-5f;
	// See meshlet_occlusion_test.glsl's comment - the pure-adaptive margin shrinks toward zero for
	// small/distant objects (reversed-Z compresses far depth into a tiny numeric range), which
	// reproduced the self-occlusion bug for those instead of large/near ones. Empirically chosen:
	// large enough to absorb that, smaller than a genuine cross-object occlusion gap.
	const float OCCLUSION_MARGIN_FLOOR = 2e-3f;

	Vector3 center(p_descriptor.bounds_center[0], p_descriptor.bounds_center[1], p_descriptor.bounds_center[2]);
	Vector3 world_center = p_transform.xform(center);
	float world_radius = p_descriptor.bounds_radius;

	Vector3 view_pos = p_view_matrix.xform(world_center);

	float nearest_view_z = view_pos.z + world_radius;
	float clip_w_nearest = -nearest_view_z;
	if (clip_w_nearest <= EPSILON) {
		return true;
	}
	float nearest_device_z = (nearest_view_z * p_proj_z_a + p_proj_z_b) / clip_w_nearest;

	float clip_w_center = -view_pos.z;
	if (clip_w_center <= EPSILON) {
		return true;
	}
	Vector2 ndc(view_pos.x * p_proj_x_scale / clip_w_center, view_pos.y * p_proj_y_scale / clip_w_center);
	Vector2 uv = ndc * 0.5f + Vector2(0.5f, 0.5f);

	float screen_radius_px = Math::abs(world_radius * p_proj_y_scale * p_screen_size.height * 0.5f) / clip_w_center;
	int mip_count = p_hiz_mip_data.size();
	int mip = CLAMP((int)Math::floor(Math::log2(MAX(screen_radius_px, 1.0f))), 0, mip_count - 1);

	Size2i mip_size = p_hiz_mip_sizes[mip];
	int px = CLAMP((int)Math::floor(uv.x * mip_size.width), 0, mip_size.width - 1);
	int py = CLAMP((int)Math::floor(uv.y * mip_size.height), 0, mip_size.height - 1);
	float occluder_depth = p_hiz_mip_data[mip][py * mip_size.width + px];

	// Adaptive margin from this meshlet's own near-to-far device-Z extent - see
	// meshlet_occlusion_test.glsl's comment for why a fixed epsilon doesn't work under reversed-Z.
	float farthest_view_z = view_pos.z - world_radius;
	float clip_w_farthest = -farthest_view_z;
	float farthest_device_z = clip_w_farthest <= EPSILON ? nearest_device_z : (farthest_view_z * p_proj_z_a + p_proj_z_b) / clip_w_farthest;
	float occlusion_margin = MAX(Math::abs(nearest_device_z - farthest_device_z) * 0.5f, OCCLUSION_MARGIN_FLOOR);

	// Reversed-Z (near=1.0, far=0.0, see meshlet_occlusion_test.glsl's comment) - occluded if
	// nearest_device_z is farther (smaller) than the recorded farthest-occluder-sample value,
	// beyond this meshlet's own margin of imprecision.
	if (nearest_device_z <= occluder_depth - occlusion_margin) {
		return false; // Occluded.
	}
	return true;
}

void test_hiz_occlusion_vs_cpu_reference() {
	RendererRD::MeshletStorage *storage = RendererRD::MeshletStorage::get_singleton();
	RendererRD::MeshletCuller *culler = RendererRD::MeshletCuller::get_singleton();
	RendererRD::HiZBuilder *hiz_builder = RendererRD::HiZBuilder::get_singleton();
	check(hiz_builder != nullptr, "HiZBuilder singleton exists");
	if (!storage || !culler || !hiz_builder) {
		return;
	}

	// Synthetic 64x64 source depth: a square "occluder" in the middle of the screen at a known
	// near-ish depth, far/clear depth (1.0) everywhere else.
	const int screen_size = 64;
	Transform3D camera_xform(Basis(), Vector3(0, 0, 5));
	Transform3D view_matrix = camera_xform.affine_inverse();
	// Use Godot RD's actual reversed-Z depth convention (RenderSceneDataRD::get_cam_projection()'s
	// depth correction - near=1.0, far=0.0), not the raw OpenGL-convention projection - this self
	// test's synthetic depth data and CPU reference math must match what the real engine/shaders
	// now assume (see meshlet_occlusion_test.glsl/meshlet_hiz_downsample.glsl's comments). A prior
	// version of this test used the raw projection directly, which was internally consistent but
	// never caught the real engine's convention being different - confirmed wrong against a real
	// scene (near/foreground objects were wrongly culled instead of distant ones).
	// Near=1.0 (not the usual 0.05) is deliberate here: under reversed-Z, a device-Z value's
	// magnitude for a given world distance scales with the near-plane value (precision is
	// "front-loaded" relative to near, not to the camera in absolute terms) - this test's
	// occluder/occludee distances (4 and 8 world units) only produced a genuine device-Z gap of
	// ~0.006 at near=0.05, which is smaller than meshlet_occlusion_test.glsl's
	// OCCLUSION_MARGIN_FLOOR (5e-2) and made this test's intentionally-occluded instance wrongly
	// pass as visible once that margin was raised to handle precision drift at real-world scale.
	// near=1.0 produces a gap of ~0.13 for the same world distances (safely > the margin) without
	// moving any of the test's hand-placed instances or changing the qualitative occluder-in-
	// front-of/behind-instances story. All test instances stay well clear of this new near plane
	// (closest is instance C at distance 2, still 2x near).
	Projection raw_projection = Projection::create_perspective(70.0f, 1.0f, 1.0f, 20.0f);
	Projection depth_correction;
	depth_correction.set_depth_correction();
	Projection projection = depth_correction * raw_projection;
	float proj_x_scale = projection.columns[0][0];
	float proj_y_scale = projection.columns[1][1];
	float proj_z_a = projection.columns[2][2];
	float proj_z_b = projection.columns[3][2];

	auto device_depth_for_world_z = [&](float p_world_z) -> float {
		float view_z = p_world_z - camera_xform.origin.z;
		return (view_z * proj_z_a + proj_z_b) / (-view_z);
	};

	float occluder_depth = device_depth_for_world_z(1.0f); // World Z=1, distance 4 from camera.

	LocalVector<float> depth_pixels;
	depth_pixels.resize(screen_size * screen_size);
	for (int y = 0; y < screen_size; y++) {
		for (int x = 0; x < screen_size; x++) {
			bool in_occluder_rect = x >= 16 && x < 48 && y >= 16 && y < 48;
			depth_pixels[y * screen_size + x] = in_occluder_rect ? occluder_depth : 0.0f; // 0.0 = far under reversed-Z.
		}
	}

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	tf.width = screen_size;
	tf.height = screen_size;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	RID source_depth_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
	Vector<uint8_t> depth_bytes;
	depth_bytes.resize(depth_pixels.size() * sizeof(float));
	memcpy(depth_bytes.ptrw(), depth_pixels.ptr(), depth_bytes.size());
	RD::get_singleton()->texture_update(source_depth_texture, 0, depth_bytes);

	// HiZBuilder now needs a RenderSceneBuffersRD to own its destination texture (for temporal
	// two-pass persistence in the live engine) - a bare, unconfigured instance is fine here since
	// build_into() always passes explicit sizes/layers, never falling back to
	// RenderSceneBuffersRD's own internal_size/view_count (which would be unset without configure()).
	Ref<RenderSceneBuffersRD> hiz_rb;
	hiz_rb.instantiate();
	RendererRD::HiZBuilder::HiZResult hiz = hiz_builder->build_into(hiz_rb, RB_MESHLET_HIZ_A, source_depth_texture, Size2i(screen_size, screen_size));
	check(hiz.is_valid(), "HiZBuilder::build returned a valid result");
	if (!hiz.is_valid()) {
		RD::get_singleton()->free_rid(source_depth_texture);
		return;
	}

	// Read back every Hi-Z mip for the CPU reference. Uses texture_copy() into a fresh,
	// non-sliced, single-mip texture per level rather than texture_create_shared_from_slice() +
	// texture_get_data() - the latter does not appear to respect the slice's base_mipmap offset
	// for mips beyond the first (texture_get_data's internal subresource indexing uses the local
	// 0-based mip index of the slice's own `mipmaps` count, not the underlying image's real mip
	// index), so every "mip >= 1" slice silently read back mip 0's data instead. texture_copy()
	// takes the source mip as an explicit parameter and isn't affected by this.
	Vector<Vector<float>> hiz_mip_data;
	Vector<Size2i> hiz_mip_sizes;
	Size2i cur_size = hiz.size;
	for (uint32_t m = 0; m < hiz.mip_count; m++) {
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32_SFLOAT;
		tf.width = MAX(1, cur_size.width);
		tf.height = MAX(1, cur_size.height);
		tf.usage_bits = RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		RID dst = RD::get_singleton()->texture_create(tf, RD::TextureView());
		RD::get_singleton()->texture_copy(hiz.texture, dst, Vector3(0, 0, 0), Vector3(0, 0, 0), Vector3(cur_size.width, cur_size.height, 1), m, 0, 0, 0);
		Vector<uint8_t> bytes = RD::get_singleton()->texture_get_data(dst, 0);
		RD::get_singleton()->free_rid(dst);
		Vector<float> floats;
		floats.resize(cur_size.width * cur_size.height);
		if ((int)bytes.size() >= floats.size() * (int)sizeof(float)) {
			memcpy(floats.ptrw(), bytes.ptr(), floats.size() * sizeof(float));
		}
		hiz_mip_data.push_back(floats);
		hiz_mip_sizes.push_back(cur_size);
		cur_size.width = MAX(1, cur_size.width / 2);
		cur_size.height = MAX(1, cur_size.height / 2);
	}

	// Sphere meshlets, reused for three hand-placed instances:
	//   A: world Z=-3 (distance 8, behind the occluder at distance 4, same screen position) -> occluded.
	//   B: world Z=-3 but offset in X, outside the occluder's screen rectangle -> visible.
	//   C: world Z=3 (distance 2, in front of the occluder) -> visible.
	PackedVector3Array vertices;
	PackedInt32Array indices;
	get_sphere_geometry(vertices, indices);
	PackedInt32Array meshlet_vertices;
	PackedByteArray meshlet_triangles;
	Vector<SurfaceTool::MeshletBounds> bounds_st;
	Vector<SurfaceTool::Meshlet> meshlets_st = SurfaceTool::build_meshlets(vertices, indices, 64, 124, 0.5f, meshlet_vertices, meshlet_triangles, bounds_st);
	Vector<RenderingServerTypes::MeshletInfo> meshlets_info;
	meshlets_info.resize(meshlets_st.size());
	memcpy(meshlets_info.ptrw(), meshlets_st.ptr(), sizeof(SurfaceTool::Meshlet) * meshlets_st.size());
	Vector<RenderingServerTypes::MeshletBoundsInfo> bounds_info;
	bounds_info.resize(bounds_st.size());
	memcpy(bounds_info.ptrw(), bounds_st.ptr(), sizeof(SurfaceTool::MeshletBounds) * bounds_st.size());
	RendererRD::MeshletStorage::UploadResult upload = storage->upload_mesh_meshlets(vertices, PackedVector3Array(), PackedVector2Array(), meshlets_info, meshlet_vertices, meshlet_triangles, bounds_info);

	Vector<Transform3D> instance_transforms;
	instance_transforms.push_back(Transform3D(Basis(), Vector3(0, 0, -3))); // A: occluded.
	instance_transforms.push_back(Transform3D(Basis(), Vector3(4, 0, -3))); // B: visible (beside).
	instance_transforms.push_back(Transform3D(Basis(), Vector3(0, 0, 3))); // C: visible (in front).

	LocalVector<float> transforms_data;
	transforms_data.resize(instance_transforms.size() * 16);
	for (int i = 0; i < instance_transforms.size(); i++) {
		transform_to_mat4_columns(instance_transforms[i], &transforms_data[i * 16]);
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

	Vector<Plane> planes = projection.get_projection_planes(camera_xform);
	RendererRD::MeshletCuller::CullResult frustum_result = culler->cull(transforms_buffer, ranges, planes, camera_xform.origin);
	RendererRD::MeshletCuller::CullResult occlusion_result = culler->occlude(transforms_buffer, frustum_result, hiz.texture, hiz.mip_count, camera_xform, projection, Size2i(screen_size, screen_size));
	check(occlusion_result.is_valid(), "MeshletCuller::occlude returned a valid result");
	Vector<RendererRD::MeshletCuller::VisibleMeshlet> gpu_visible = culler->debug_read_visible(occlusion_result);

	HashSet<uint64_t> expected;
	for (int i = 0; i < instance_transforms.size(); i++) {
		for (uint32_t m = 0; m < upload.meshlet_range.count; m++) {
			uint32_t global_meshlet_index = upload.meshlet_range.offset + m;
			RendererRD::MeshletStorage::MeshletDescriptorGPU descriptor = storage->debug_get_meshlet_descriptor(global_meshlet_index);
			// Must pass the frustum test too (occlude() only receives frustum survivors).
			if (!cpu_reference_visible(instance_transforms[i], descriptor, planes, camera_xform.origin)) {
				continue;
			}
			bool occ_visible = cpu_occlusion_visible(instance_transforms[i], descriptor, view_matrix, proj_x_scale, proj_y_scale, proj_z_a, proj_z_b, Size2i(screen_size, screen_size), hiz_mip_data, hiz_mip_sizes);
			if (occ_visible) {
				expected.insert((uint64_t(i) << 32) | global_meshlet_index);
			}
		}
	}

	check(!expected.is_empty(), "Hi-Z test scene has at least some expected-visible meshlets");

	// Instance A (index 0) must be fully occluded; instances B and C (indices 1, 2) must not be.
	bool instance_a_fully_occluded = true;
	for (const uint64_t &key : expected) {
		if ((key >> 32) == 0) {
			instance_a_fully_occluded = false;
		}
	}
	check(instance_a_fully_occluded, "CPU reference: instance directly behind the occluder is fully occluded");

	HashSet<uint64_t> seen;
	bool false_positive = false;
	bool duplicate = false;
	for (int i = 0; i < gpu_visible.size(); i++) {
		uint64_t key = (uint64_t(gpu_visible[i].instance_index) << 32) | gpu_visible[i].meshlet_index;
		if (!expected.has(key)) {
			false_positive = true;
		}
		if (seen.has(key)) {
			duplicate = true;
		}
		seen.insert(key);
	}
	check(!false_positive, "GPU Hi-Z occlusion produced no false-positive visible meshlets");
	check(!duplicate, "GPU Hi-Z occlusion produced no duplicate visible meshlets");
	check(seen.size() == expected.size(), "GPU Hi-Z occlusion produced every CPU-expected visible meshlet (no false negatives/pop-in)");

	bool gpu_instance_a_occluded = true;
	for (int i = 0; i < gpu_visible.size(); i++) {
		if (gpu_visible[i].instance_index == 0) {
			gpu_instance_a_occluded = false;
		}
	}
	check(gpu_instance_a_occluded, "GPU: instance directly behind the occluder produced no visible meshlets");

	storage->free_mesh_meshlets(upload);
	RD::get_singleton()->free_rid(transforms_buffer);
	RD::get_singleton()->free_rid(source_depth_texture);
}

// Temporal two-pass Hi-Z occlusion (see project plan/memory): proves the actual point of running
// two passes instead of one - that an instance wrongly occluded by a *stale* Hi-Z (the early
// pass's situation, testing against last frame's data) gets correctly recovered once re-tested
// against a *fresh* Hi-Z built after whatever was blocking it is gone (the late pass's situation).
// Tests MeshletCuller::occlude() directly against two hand-built depth buffers representing
// "last frame" and "this frame," at the same level of abstraction as
// test_hiz_occlusion_vs_cpu_reference() above, rather than simulating RenderForwardClustered's
// full _render_scene flow (which would need much heavier RenderDataRD/viewport scaffolding to
// construct here).
void test_temporal_hiz_two_pass_disocclusion_recovery() {
	RendererRD::MeshletStorage *storage = RendererRD::MeshletStorage::get_singleton();
	RendererRD::MeshletCuller *culler = RendererRD::MeshletCuller::get_singleton();
	RendererRD::HiZBuilder *hiz_builder = RendererRD::HiZBuilder::get_singleton();
	if (!storage || !culler || !hiz_builder) {
		return;
	}

	const int screen_size = 64;
	Transform3D camera_xform(Basis(), Vector3(0, 0, 5));
	Projection raw_projection = Projection::create_perspective(70.0f, 1.0f, 1.0f, 20.0f);
	Projection depth_correction;
	depth_correction.set_depth_correction();
	Projection projection = depth_correction * raw_projection;
	float proj_z_a = projection.columns[2][2];
	float proj_z_b = projection.columns[3][2];

	auto device_depth_for_world_z = [&](float p_world_z) -> float {
		float view_z = p_world_z - camera_xform.origin.z;
		return (view_z * proj_z_a + proj_z_b) / (-view_z);
	};

	float occluder_depth = device_depth_for_world_z(1.0f); // World Z=1, distance 4 from camera.

	// "Last frame": an occluder blocks the screen rect where instance D will project.
	LocalVector<float> stale_depth_pixels;
	stale_depth_pixels.resize(screen_size * screen_size);
	for (int y = 0; y < screen_size; y++) {
		for (int x = 0; x < screen_size; x++) {
			bool in_occluder_rect = x >= 16 && x < 48 && y >= 16 && y < 48;
			stale_depth_pixels[y * screen_size + x] = in_occluder_rect ? occluder_depth : 0.0f;
		}
	}
	// "This frame": the occluder is gone (disocclusion) - everywhere reads far/clear.
	LocalVector<float> fresh_depth_pixels;
	fresh_depth_pixels.resize(screen_size * screen_size);
	for (uint32_t i = 0; i < fresh_depth_pixels.size(); i++) {
		fresh_depth_pixels[i] = 0.0f;
	}

	auto make_depth_texture = [&](const LocalVector<float> &p_pixels) -> RID {
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32_SFLOAT;
		tf.width = screen_size;
		tf.height = screen_size;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		RID tex = RD::get_singleton()->texture_create(tf, RD::TextureView());
		Vector<uint8_t> bytes;
		bytes.resize(p_pixels.size() * sizeof(float));
		memcpy(bytes.ptrw(), p_pixels.ptr(), bytes.size());
		RD::get_singleton()->texture_update(tex, 0, bytes);
		return tex;
	};
	RID stale_depth_texture = make_depth_texture(stale_depth_pixels);
	RID fresh_depth_texture = make_depth_texture(fresh_depth_pixels);

	// Same RenderSceneBuffersRD, two different named textures - mirrors how the live engine
	// alternates RB_MESHLET_HIZ_A/RB_MESHLET_HIZ_B roles across frames rather than overwriting one
	// texture in place (see render_forward_clustered.h's meshlet_hiz_a_is_current comment).
	Ref<RenderSceneBuffersRD> hiz_rb;
	hiz_rb.instantiate();
	RendererRD::HiZBuilder::HiZResult stale_hiz = hiz_builder->build_into(hiz_rb, RB_MESHLET_HIZ_A, stale_depth_texture, Size2i(screen_size, screen_size));
	RendererRD::HiZBuilder::HiZResult fresh_hiz = hiz_builder->build_into(hiz_rb, RB_MESHLET_HIZ_B, fresh_depth_texture, Size2i(screen_size, screen_size));
	check(stale_hiz.is_valid() && fresh_hiz.is_valid(), "Temporal test: both stale and fresh Hi-Z built successfully");
	if (!stale_hiz.is_valid() || !fresh_hiz.is_valid()) {
		RD::get_singleton()->free_rid(stale_depth_texture);
		RD::get_singleton()->free_rid(fresh_depth_texture);
		return;
	}

	// Instance D: world Z=-3 (distance 8), same screen position as the occluder rect above -
	// occluded against the stale Hi-Z, must be recovered against the fresh one.
	PackedVector3Array vertices;
	PackedInt32Array indices;
	get_sphere_geometry(vertices, indices);
	PackedInt32Array meshlet_vertices;
	PackedByteArray meshlet_triangles;
	Vector<SurfaceTool::MeshletBounds> bounds_st;
	Vector<SurfaceTool::Meshlet> meshlets_st = SurfaceTool::build_meshlets(vertices, indices, 64, 124, 0.5f, meshlet_vertices, meshlet_triangles, bounds_st);
	Vector<RenderingServerTypes::MeshletInfo> meshlets_info;
	meshlets_info.resize(meshlets_st.size());
	memcpy(meshlets_info.ptrw(), meshlets_st.ptr(), sizeof(SurfaceTool::Meshlet) * meshlets_st.size());
	Vector<RenderingServerTypes::MeshletBoundsInfo> bounds_info;
	bounds_info.resize(bounds_st.size());
	memcpy(bounds_info.ptrw(), bounds_st.ptr(), sizeof(SurfaceTool::MeshletBounds) * bounds_st.size());
	RendererRD::MeshletStorage::UploadResult upload = storage->upload_mesh_meshlets(vertices, PackedVector3Array(), PackedVector2Array(), meshlets_info, meshlet_vertices, meshlet_triangles, bounds_info);

	Transform3D instance_d_transform(Basis(), Vector3(0, 0, -3));
	LocalVector<float> transforms_data;
	transforms_data.resize(16);
	transform_to_mat4_columns(instance_d_transform, transforms_data.ptr());
	RID transforms_buffer = RD::get_singleton()->storage_buffer_create(transforms_data.size() * sizeof(float));
	RD::get_singleton()->buffer_update(transforms_buffer, 0, transforms_data.size() * sizeof(float), transforms_data.ptr());

	Vector<RendererRD::MeshletCuller::InstanceMeshletRange> ranges;
	RendererRD::MeshletCuller::InstanceMeshletRange r;
	r.instance_index = 0;
	r.meshlet_offset = upload.meshlet_range.offset;
	r.meshlet_count = upload.meshlet_range.count;
	ranges.push_back(r);

	Vector<Plane> planes = projection.get_projection_planes(camera_xform);
	RendererRD::MeshletCuller::CullResult frustum_result = culler->cull(transforms_buffer, ranges, planes, camera_xform.origin);

	// Early pass's situation: occlude-test against last frame's (stale) Hi-Z.
	RendererRD::MeshletCuller::CullResult stale_occlusion_result = culler->occlude(transforms_buffer, frustum_result, stale_hiz.texture, stale_hiz.mip_count, camera_xform, projection, Size2i(screen_size, screen_size));
	Vector<RendererRD::MeshletCuller::VisibleMeshlet> stale_visible = culler->debug_read_visible(stale_occlusion_result);
	check(stale_visible.is_empty(), "Temporal test: instance D is wrongly occluded against the stale Hi-Z (expected - this is what the early pass would see)");

	// Late pass's situation: re-test the *same* frustum survivors against a fresh Hi-Z built after
	// the occluder is gone - this is the actual point of two-pass over single-pass.
	RendererRD::MeshletCuller::CullResult fresh_occlusion_result = culler->occlude(transforms_buffer, frustum_result, fresh_hiz.texture, fresh_hiz.mip_count, camera_xform, projection, Size2i(screen_size, screen_size));
	Vector<RendererRD::MeshletCuller::VisibleMeshlet> fresh_visible = culler->debug_read_visible(fresh_occlusion_result);
	check(!fresh_visible.is_empty(), "Temporal test: late pass recovers instance D against the fresh Hi-Z (disocclusion correctly handled)");
	bool all_instance_d = true;
	for (int i = 0; i < fresh_visible.size(); i++) {
		if (fresh_visible[i].instance_index != 0) {
			all_instance_d = false;
		}
	}
	check(all_instance_d, "Temporal test: every fresh-pass-visible meshlet belongs to instance D (no spurious results)");

	storage->free_mesh_meshlets(upload);
	RD::get_singleton()->free_rid(transforms_buffer);
	RD::get_singleton()->free_rid(stale_depth_texture);
	RD::get_singleton()->free_rid(fresh_depth_texture);
}

// Phase 5 visual proof: renders an actual frame through the full pipeline (cull -> occlude ->
// emit indirect draws -> vertex-pulling indirect multi-draw) into an offscreen framebuffer,
// checks the resulting pixels are sane, and saves a PNG so it can also be looked at directly -
// this is the first phase where "does it look right" matters and doctest-style assertions alone
// aren't the full story.
void test_meshlet_render_visual_proof() {
	RendererRD::MeshletStorage *storage = RendererRD::MeshletStorage::get_singleton();
	RendererRD::MeshletCuller *culler = RendererRD::MeshletCuller::get_singleton();
	RendererRD::HiZBuilder *hiz_builder = RendererRD::HiZBuilder::get_singleton();
	RendererRD::MeshletRenderer *renderer = RendererRD::MeshletRenderer::get_singleton();
	check(renderer != nullptr, "MeshletRenderer singleton exists");
	if (!storage || !culler || !hiz_builder || !renderer) {
		return;
	}

	PackedVector3Array vertices;
	PackedInt32Array indices;
	get_sphere_geometry(vertices, indices);
	PackedInt32Array meshlet_vertices;
	PackedByteArray meshlet_triangles;
	Vector<SurfaceTool::MeshletBounds> bounds_st;
	Vector<SurfaceTool::Meshlet> meshlets_st = SurfaceTool::build_meshlets(vertices, indices, 64, 124, 0.5f, meshlet_vertices, meshlet_triangles, bounds_st);
	Vector<RenderingServerTypes::MeshletInfo> meshlets_info;
	meshlets_info.resize(meshlets_st.size());
	memcpy(meshlets_info.ptrw(), meshlets_st.ptr(), sizeof(SurfaceTool::Meshlet) * meshlets_st.size());
	Vector<RenderingServerTypes::MeshletBoundsInfo> bounds_info;
	bounds_info.resize(bounds_st.size());
	memcpy(bounds_info.ptrw(), bounds_st.ptr(), sizeof(SurfaceTool::MeshletBounds) * bounds_st.size());

	PackedVector3Array normals;
	normals.resize(vertices.size());
	for (int i = 0; i < vertices.size(); i++) {
		normals.write[i] = vertices[i].normalized(); // Sphere centered at origin - position is the normal.
	}
	RendererRD::MeshletStorage::UploadResult upload = storage->upload_mesh_meshlets(vertices, normals, PackedVector2Array(), meshlets_info, meshlet_vertices, meshlet_triangles, bounds_info);

	// One instance, squarely in view, unoccluded.
	Transform3D instance_transform(Basis(), Vector3(0, 0, 0));
	LocalVector<float> transforms_data;
	transforms_data.resize(16);
	transform_to_mat4_columns(instance_transform, &transforms_data[0]);
	RID transforms_buffer = RD::get_singleton()->storage_buffer_create(transforms_data.size() * sizeof(float));
	RD::get_singleton()->buffer_update(transforms_buffer, 0, transforms_data.size() * sizeof(float), transforms_data.ptr());

	// Default-constructed MeshletMaterialGPU (flat white, no textures) explicitly uploaded to a
	// known slot - the shader unconditionally indexes meshlet_materials.data[material_id], so this
	// must be a real, populated slot rather than relying on the buffer's uninitialized backing
	// storage.
	uint32_t material_id = storage->upload_material(RID(), RendererRD::MeshletStorage::MeshletMaterialGPU());
	RID material_ids_buffer = RD::get_singleton()->storage_buffer_create(sizeof(uint32_t));
	RD::get_singleton()->buffer_update(material_ids_buffer, 0, sizeof(uint32_t), &material_id);

	Vector<RendererRD::MeshletCuller::InstanceMeshletRange> ranges;
	RendererRD::MeshletCuller::InstanceMeshletRange r;
	r.instance_index = 0;
	r.meshlet_offset = upload.meshlet_range.offset;
	r.meshlet_count = upload.meshlet_range.count;
	ranges.push_back(r);

	Transform3D camera_xform(Basis(), Vector3(0, 0, 5));
	// Depth-corrected (reversed-Z, matching the real engine - see meshlet_occlusion_test.glsl's
	// comment) projection, used consistently for frustum planes, occlude()'s scalars, and the
	// final render() call.
	Projection raw_projection = Projection::create_perspective(70.0f, 1.0f, 0.05f, 20.0f);
	Projection depth_correction;
	depth_correction.set_depth_correction();
	Projection projection = depth_correction * raw_projection;
	Vector<Plane> planes = projection.get_projection_planes(camera_xform);
	RendererRD::MeshletCuller::CullResult frustum_result = culler->cull(transforms_buffer, ranges, planes, camera_xform.origin);

	// Trivial all-far synthetic depth source - guarantees nothing is occluded, isolating this
	// test to proving the render path itself (the occlusion math is already covered separately).
	// 0.0 = far under reversed-Z.
	const int hiz_source_size = 64;
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	tf.width = hiz_source_size;
	tf.height = hiz_source_size;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	RID far_depth_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
	Vector<uint8_t> far_bytes;
	far_bytes.resize(hiz_source_size * hiz_source_size * sizeof(float));
	{
		float *f = (float *)far_bytes.ptrw();
		for (int i = 0; i < hiz_source_size * hiz_source_size; i++) {
			f[i] = 0.0f;
		}
	}
	RD::get_singleton()->texture_update(far_depth_texture, 0, far_bytes);
	Ref<RenderSceneBuffersRD> hiz_rb;
	hiz_rb.instantiate();
	RendererRD::HiZBuilder::HiZResult hiz = hiz_builder->build_into(hiz_rb, RB_MESHLET_HIZ_A, far_depth_texture, Size2i(hiz_source_size, hiz_source_size));

	RendererRD::MeshletCuller::CullResult occlusion_result = culler->occlude(transforms_buffer, frustum_result, hiz.texture, hiz.mip_count, camera_xform, projection, Size2i(hiz_source_size, hiz_source_size));
	check(occlusion_result.is_valid(), "occlude() returned a valid result (trivial all-far Hi-Z)");

	RendererRD::MeshletCuller::IndirectDrawResult draws = culler->emit_indirect_draws(occlusion_result);
	check(draws.is_valid() && draws.max_draw_count > 0, "emit_indirect_draws produced at least one draw command");

	// Offscreen render target.
	const int render_size = 256;
	RD::TextureFormat color_tf;
	color_tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	color_tf.width = render_size;
	color_tf.height = render_size;
	color_tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	RID color_texture = RD::get_singleton()->texture_create(color_tf, RD::TextureView());

	RD::TextureFormat depth_tf;
	depth_tf.format = RD::DATA_FORMAT_D32_SFLOAT;
	depth_tf.width = render_size;
	depth_tf.height = render_size;
	depth_tf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	RID depth_texture = RD::get_singleton()->texture_create(depth_tf, RD::TextureView());

	Vector<RID> attachments;
	attachments.push_back(color_texture);
	attachments.push_back(depth_texture);
	RID framebuffer = RD::get_singleton()->framebuffer_create(attachments);
	RD::FramebufferFormatID framebuffer_format = RD::get_singleton()->framebuffer_get_format(framebuffer);

	// One directional light, pointed straight at the camera-facing side (not the old debug-shading
	// Vector3(-0.5,-1,-0.5)) - since B2 introduced real light/dark sides, the center-pixel-
	// brightness assertion below needs the front-facing point to be guaranteed brightly lit
	// (N.L == 1.0 exactly) regardless of any minor pixel-center-vs-sphere-center alignment nuance.
	// Mirrors RenderForwardClustered::MeshletLightGPU's layout exactly (64 bytes: see
	// meshlet_render.glsl's MeshletLight struct) without depending on that type directly - this
	// file deliberately doesn't include render_forward_clustered.h.
	struct {
		float position[3] = { 0, 0, 0 };
		float inv_radius = 0;
		float direction[3] = { 0, 0, -1 };
		float attenuation = 1;
		// Energy-premultiplied including the *PI compensation RenderForwardClustered::
		// _meshlet_collect_lights() applies for real scene lights (matches LightStorage's own
		// light data setup, compensating for light_compute()'s Lambert 1/PI normalization) - a
		// flat color[3]={1,1,1} without this would render visibly dimmer than a real energy=1.0
		// light actually produces.
		float color[3] = { (float)Math::PI, (float)Math::PI, (float)Math::PI };
		float size = 0;
		float cone_angle = 1;
		float cone_attenuation = 1;
		uint32_t is_directional = 1;
		uint32_t pad0 = 0;
	} light_data;
	RID lights_buffer = RD::get_singleton()->storage_buffer_create(sizeof(light_data));
	RD::get_singleton()->buffer_update(lights_buffer, 0, sizeof(light_data), &light_data);

	renderer->render(occlusion_result, draws, transforms_buffer, material_ids_buffer, framebuffer, framebuffer_format, Rect2i(0, 0, render_size, render_size), projection, camera_xform, lights_buffer, 1);

	Vector<uint8_t> pixels = RD::get_singleton()->texture_get_data(color_texture, 0);
	check((int)pixels.size() == render_size * render_size * 4, "Color texture readback has the expected byte size");

	bool center_is_non_background = false;
	bool corner_is_background = true;
	if ((int)pixels.size() == render_size * render_size * 4) {
		int center_idx = (render_size / 2 * render_size + render_size / 2) * 4;
		if (pixels[center_idx] > 0 || pixels[center_idx + 1] > 0 || pixels[center_idx + 2] > 0) {
			center_is_non_background = true;
		}
		int corner_idx = (2 * render_size + 2) * 4; // Near top-left, outside the sphere's projection.
		if (pixels[corner_idx] != 0 || pixels[corner_idx + 1] != 0 || pixels[corner_idx + 2] != 0) {
			corner_is_background = false;
		}
	}
	check(center_is_non_background, "Center pixel (where the sphere should project) is not the background clear color");
	check(corner_is_background, "Corner pixel (outside the sphere's projection) is still the background clear color");

	if ((int)pixels.size() == render_size * render_size * 4) {
		Ref<Image> image = Image::create_from_data(render_size, render_size, false, Image::FORMAT_RGBA8, pixels);
		String path = "user://meshlet_render_selftest.png";
		Error err = image->save_png(path);
		if (err == OK) {
			print_line("MESHLET_SELFTEST: saved render proof to " + ProjectSettings::get_singleton()->globalize_path(path));
		} else {
			print_line(vformat("MESHLET_SELFTEST: failed to save render proof PNG (error %d)", (int)err));
		}
	}

	storage->free_mesh_meshlets(upload);
	RD::get_singleton()->free_rid(transforms_buffer);
	RD::get_singleton()->free_rid(material_ids_buffer);
	RD::get_singleton()->free_rid(lights_buffer);
	RD::get_singleton()->free_rid(far_depth_texture);
	// Free the framebuffer before the textures it attaches - freeing an attached texture first
	// cascades to free the framebuffer too (same dependency-tracking behavior as vertex
	// buffers/arrays elsewhere in this engine), which would make this a double-free.
	RD::get_singleton()->free_rid(framebuffer);
	RD::get_singleton()->free_rid(color_texture);
	RD::get_singleton()->free_rid(depth_texture);
}

// B5 (PBR verification milestone): proves MeshletStorage::upload_material()'s RID-keyed dedup and
// MeshletMaterialGPU round-tripping work correctly across *multiple distinct real materials* -
// the explicitly-flagged-as-missing check from this project's research (every earlier render test
// only ever exercised a single material). Mirrors the established CPU-reference pattern used for
// culling/occlusion: build known data, upload it, read it back via the GPU, and assert it matches
// exactly - not a render/visual check (that's covered separately by the live multi-material
// .selftest_project scene, see meshlet_overlay_test.tscn's two differently-colored spheres).
void test_meshlet_material_lookup_vs_cpu_reference() {
	RendererRD::MeshletStorage *storage = RendererRD::MeshletStorage::get_singleton();
	if (!storage) {
		return;
	}

	// Three distinct materials, distinguished by albedo color (and, for the third, also
	// metallic/roughness) - enough to catch both "wrong slot" and "fields swapped/overwritten"
	// bugs, not just "is some material found at all".
	Vector<RendererRD::MeshletStorage::MeshletMaterialGPU> expected_materials;
	{
		RendererRD::MeshletStorage::MeshletMaterialGPU m;
		m.albedo[0] = 0.8f;
		m.albedo[1] = 0.1f;
		m.albedo[2] = 0.1f;
		m.albedo[3] = 1.0f;
		expected_materials.push_back(m);
	}
	{
		RendererRD::MeshletStorage::MeshletMaterialGPU m;
		m.albedo[0] = 0.1f;
		m.albedo[1] = 0.8f;
		m.albedo[2] = 0.1f;
		m.albedo[3] = 1.0f;
		expected_materials.push_back(m);
	}
	{
		RendererRD::MeshletStorage::MeshletMaterialGPU m;
		m.albedo[0] = 0.1f;
		m.albedo[1] = 0.1f;
		m.albedo[2] = 0.8f;
		m.albedo[3] = 1.0f;
		m.metallic = 1.0f;
		m.roughness = 0.2f;
		expected_materials.push_back(m);
	}

	// Distinct dummy RIDs as dedup keys - tiny real textures are a convenient way to get distinct,
	// valid RID values that don't collide with each other or with RID() (used elsewhere for "no
	// material") - their texture format is otherwise irrelevant, they're never sampled.
	RD::TextureFormat dummy_format;
	dummy_format.format = RD::DATA_FORMAT_R8_UNORM;
	dummy_format.width = 1;
	dummy_format.height = 1;
	dummy_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
	Vector<RID> material_rids;
	for (int i = 0; i < expected_materials.size(); i++) {
		material_rids.push_back(RD::get_singleton()->texture_create(dummy_format, RD::TextureView()));
	}

	Vector<uint32_t> slots;
	for (int i = 0; i < expected_materials.size(); i++) {
		slots.push_back(storage->upload_material(material_rids[i], expected_materials[i]));
	}

	bool slots_distinct = true;
	for (int i = 0; i < slots.size(); i++) {
		for (int j = i + 1; j < slots.size(); j++) {
			if (slots[i] == slots[j]) {
				slots_distinct = false;
			}
		}
	}
	check(slots_distinct, "upload_material() returned distinct slots for distinct material RIDs");

	bool all_round_trip_correctly = true;
	for (int i = 0; i < slots.size(); i++) {
		RendererRD::MeshletStorage::MeshletMaterialGPU readback = storage->debug_get_material(slots[i]);
		const RendererRD::MeshletStorage::MeshletMaterialGPU &expected = expected_materials[i];
		bool matches = readback.albedo[0] == expected.albedo[0] && readback.albedo[1] == expected.albedo[1] &&
				readback.albedo[2] == expected.albedo[2] && readback.albedo[3] == expected.albedo[3] &&
				readback.metallic == expected.metallic && readback.roughness == expected.roughness;
		if (!matches) {
			all_round_trip_correctly = false;
		}
	}
	check(all_round_trip_correctly, "Every uploaded material's albedo/metallic/roughness round-trips correctly through real GPU buffers, in the correct distinct slot");

	// Re-uploading the same RID (e.g. simulating the same instance re-scanned next frame) must
	// dedup to the *same* slot, not allocate a new one - this is what keeps material upload cheap
	// for the common case of many instances sharing one material.
	uint32_t re_upload_slot = storage->upload_material(material_rids[0], expected_materials[0]);
	check(re_upload_slot == slots[0], "Re-uploading the same material RID dedups to the same slot, not a new one");

	for (int i = 0; i < material_rids.size(); i++) {
		if (material_rids[i].is_valid()) {
			RD::get_singleton()->free_rid(material_rids[i]);
		}
	}
}

} // namespace

void run_meshlet_selftest_if_requested() {
	if (!OS::get_singleton()->get_cmdline_args().find("--meshlet-selftest")) {
		return;
	}

	print_line("MESHLET_SELFTEST: starting");
	g_failures = 0;
	test_meshlet_storage_round_trip();
	test_meshlet_culler_vs_cpu_reference();
	test_hiz_occlusion_vs_cpu_reference();
	test_temporal_hiz_two_pass_disocclusion_recovery();
	test_meshlet_render_visual_proof();
	test_meshlet_material_lookup_vs_cpu_reference();
	if (g_failures == 0) {
		print_line("MESHLET_SELFTEST: all checks passed");
	} else {
		print_line(vformat("MESHLET_SELFTEST: %d check(s) FAILED", g_failures));
	}
}
