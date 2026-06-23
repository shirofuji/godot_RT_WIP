/**************************************************************************/
/*  meshlet_culler.cpp                                                   */
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

#include "meshlet_culler.h"

#include "servers/rendering/renderer_rd/storage_rd/meshlet_storage.h"

using namespace RendererRD;

MeshletCuller *MeshletCuller::singleton = nullptr;

MeshletCuller *MeshletCuller::get_singleton() {
	return singleton;
}

MeshletCuller::MeshletCuller() {
	singleton = this;

	{
		Vector<String> versions;
		versions.push_back("");
		expand_shader.initialize(versions);
		expand_shader_version = expand_shader.version_create();
		expand_shader_rid = expand_shader.version_get_shader(expand_shader_version, 0);
		expand_pipeline = RD::get_singleton()->compute_pipeline_create(expand_shader_rid);
	}
	{
		Vector<String> versions;
		versions.push_back("");
		cull_shader.initialize(versions);
		cull_shader_version = cull_shader.version_create();
		cull_shader_rid = cull_shader.version_get_shader(cull_shader_version, 0);
		cull_pipeline = RD::get_singleton()->compute_pipeline_create(cull_shader_rid);
	}
	{
		Vector<String> versions;
		versions.push_back("");
		occlusion_shader.initialize(versions);
		occlusion_shader_version = occlusion_shader.version_create();
		occlusion_shader_rid = occlusion_shader.version_get_shader(occlusion_shader_version, 0);
		occlusion_pipeline = RD::get_singleton()->compute_pipeline_create(occlusion_shader_rid);
	}

	{
		Vector<String> versions;
		versions.push_back("");
		emit_draws_shader.initialize(versions);
		emit_draws_shader_version = emit_draws_shader.version_create();
		emit_draws_shader_rid = emit_draws_shader.version_get_shader(emit_draws_shader_version, 0);
		emit_draws_pipeline = RD::get_singleton()->compute_pipeline_create(emit_draws_shader_rid);
	}

	hiz_sampler = RD::get_singleton()->sampler_create(RD::SamplerState());
}

MeshletCuller::~MeshletCuller() {
	if (ranges_buffer.is_valid()) {
		RD::get_singleton()->free_rid(ranges_buffer);
	}
	if (work_items_buffer.is_valid()) {
		RD::get_singleton()->free_rid(work_items_buffer);
	}
	if (visible_buffer.is_valid()) {
		RD::get_singleton()->free_rid(visible_buffer);
	}
	if (occluded_buffer.is_valid()) {
		RD::get_singleton()->free_rid(occluded_buffer);
	}
	if (command_buffer.is_valid()) {
		RD::get_singleton()->free_rid(command_buffer);
	}
	if (draw_count_buffer.is_valid()) {
		RD::get_singleton()->free_rid(draw_count_buffer);
	}
	RD::get_singleton()->free_rid(hiz_sampler);
	expand_shader.version_free(expand_shader_version);
	cull_shader.version_free(cull_shader_version);
	occlusion_shader.version_free(occlusion_shader_version);
	emit_draws_shader.version_free(emit_draws_shader_version);
	singleton = nullptr;
}

void MeshletCuller::_ensure_work_items_capacity(uint32_t p_capacity) {
	if (work_items_capacity >= p_capacity) {
		return;
	}
	if (work_items_buffer.is_valid()) {
		RD::get_singleton()->free_rid(work_items_buffer);
	}
	// Leading uint counter + VisibleMeshlet-sized (2x uint32) entries.
	uint32_t size_bytes = sizeof(uint32_t) + p_capacity * sizeof(uint32_t) * 2;
	work_items_buffer = RD::get_singleton()->storage_buffer_create(size_bytes);
	work_items_capacity = p_capacity;
}

void MeshletCuller::_ensure_visible_capacity(uint32_t p_capacity) {
	if (visible_capacity >= p_capacity) {
		return;
	}
	if (visible_buffer.is_valid()) {
		RD::get_singleton()->free_rid(visible_buffer);
	}
	uint32_t size_bytes = sizeof(uint32_t) + p_capacity * sizeof(uint32_t) * 2;
	visible_buffer = RD::get_singleton()->storage_buffer_create(size_bytes);
	visible_capacity = p_capacity;
}

void MeshletCuller::_ensure_occluded_capacity(uint32_t p_capacity) {
	if (occluded_capacity >= p_capacity) {
		return;
	}
	if (occluded_buffer.is_valid()) {
		RD::get_singleton()->free_rid(occluded_buffer);
	}
	uint32_t size_bytes = sizeof(uint32_t) + p_capacity * sizeof(uint32_t) * 2;
	occluded_buffer = RD::get_singleton()->storage_buffer_create(size_bytes);
	occluded_capacity = p_capacity;
}

void MeshletCuller::_ensure_command_capacity(uint32_t p_capacity) {
	if (command_capacity >= p_capacity) {
		return;
	}
	if (command_buffer.is_valid()) {
		RD::get_singleton()->free_rid(command_buffer);
	}
	uint32_t size_bytes = p_capacity * sizeof(IndirectCommand);
	command_buffer = RD::get_singleton()->storage_buffer_create(size_bytes, Span<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	command_capacity = p_capacity;
}

MeshletCuller::CullResult MeshletCuller::cull(RID p_transforms_buffer, const Vector<InstanceMeshletRange> &p_ranges, const Vector<Plane> &p_frustum_planes, const Vector3 &p_camera_position, uint32_t p_max_work_items, uint32_t p_max_visible) {
	CullResult result;

	ERR_FAIL_COND_V(p_frustum_planes.size() != 6, result);
	ERR_FAIL_NULL_V(MeshletStorage::get_singleton(), result);

	_ensure_work_items_capacity(p_max_work_items);
	_ensure_visible_capacity(p_max_visible);

	// Zero the atomic counters before this call's dispatches.
	uint32_t zero = 0;
	RD::get_singleton()->buffer_update(work_items_buffer, 0, sizeof(uint32_t), &zero);
	RD::get_singleton()->buffer_update(visible_buffer, 0, sizeof(uint32_t), &zero);

	result.visible_buffer = visible_buffer;
	result.max_visible = p_max_visible;

	if (p_ranges.is_empty()) {
		return result;
	}

	if (ranges_buffer.is_valid()) {
		RD::get_singleton()->free_rid(ranges_buffer);
	}
	uint32_t ranges_size_bytes = p_ranges.size() * sizeof(InstanceMeshletRange);
	ranges_buffer = RD::get_singleton()->storage_buffer_create(ranges_size_bytes);
	RD::get_singleton()->buffer_update(ranges_buffer, 0, ranges_size_bytes, p_ranges.ptr());

	// --- Pass A: expand instance-surface ranges into a flat per-meshlet worklist. ---
	{
		Vector<RD::Uniform> uniforms;
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 0;
			u.append_id(ranges_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 1;
			u.append_id(work_items_buffer);
			uniforms.push_back(u);
		}
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, expand_shader_rid, 0);

		struct ExpandPushConstant {
			uint32_t range_count;
			uint32_t max_work_items;
			uint32_t pad0;
			uint32_t pad1;
		};
		ExpandPushConstant push_constant;
		push_constant.range_count = p_ranges.size();
		push_constant.max_work_items = p_max_work_items;
		push_constant.pad0 = 0;
		push_constant.pad1 = 0;

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, expand_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_ranges.size(), 1, 1);
		RD::get_singleton()->compute_list_end();

		RD::get_singleton()->free_rid(uniform_set);
	}

	// --- Pass B: cull each worklist entry, append survivors. ---
	// No CPU readback of Pass A's survivor count: dispatch the full fixed capacity
	// (p_max_work_items) instead - cheap on the GPU (excess threads exit immediately, see the
	// shader's own clamped bounds check against work_items.count) and avoids a synchronous
	// GPU->CPU pipeline stall every frame, which was the dominant cost wiring this into the live
	// render loop at scale (see project memory: this was a known correctness-first tradeoff from
	// Phase 3, finally fixed here once a real stress scene exposed the cost).
	{
		Vector<RD::Uniform> uniforms;
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 0;
			u.append_id(work_items_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 1;
			u.append_id(p_transforms_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 2;
			u.append_id(MeshletStorage::get_singleton()->get_meshlet_descriptor_buffer_rid());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 3;
			u.append_id(visible_buffer);
			uniforms.push_back(u);
		}
		{
			// Binding 4: per-meshlet LOD-cut data (parallel to descriptors) for DAG cluster-LOD
			// selection - see meshlet_cull.glsl's MeshletLODs.
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 4;
			u.append_id(MeshletStorage::get_singleton()->get_meshlet_lod_buffer_rid());
			uniforms.push_back(u);
		}
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, cull_shader_rid, 0);

		struct CullPushConstant {
			uint32_t work_item_count;
			uint32_t max_visible;
			uint32_t pad0;
			uint32_t pad1;
			float planes[6][4];
			float camera_position[3];
			float pad2;
		};
		CullPushConstant push_constant;
		push_constant.work_item_count = p_max_work_items; // Capacity, not the real count - see shader.
		push_constant.max_visible = p_max_visible;
		push_constant.pad0 = 0;
		push_constant.pad1 = 0;
		for (int i = 0; i < 6; i++) {
			push_constant.planes[i][0] = p_frustum_planes[i].normal.x;
			push_constant.planes[i][1] = p_frustum_planes[i].normal.y;
			push_constant.planes[i][2] = p_frustum_planes[i].normal.z;
			push_constant.planes[i][3] = p_frustum_planes[i].d;
		}
		push_constant.camera_position[0] = p_camera_position.x;
		push_constant.camera_position[1] = p_camera_position.y;
		push_constant.camera_position[2] = p_camera_position.z;
		push_constant.pad2 = 0;

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, cull_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_max_work_items, 1, 1);
		RD::get_singleton()->compute_list_end();

		RD::get_singleton()->free_rid(uniform_set);
	}

	return result;
}

static void transform_to_mat4_columns(const Transform3D &p_transform, float r_out[16]) {
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

MeshletCuller::CullResult MeshletCuller::occlude(RID p_transforms_buffer, const CullResult &p_frustum_result, RID p_hiz_texture, uint32_t p_hiz_mip_count, const Transform3D &p_camera_transform, const Projection &p_projection, const Size2i &p_screen_size, uint32_t p_max_visible) {
	CullResult result;

	ERR_FAIL_COND_V(!p_frustum_result.is_valid(), result);
	ERR_FAIL_COND_V(!p_hiz_texture.is_valid() || p_hiz_mip_count == 0, result);

	_ensure_occluded_capacity(p_max_visible);

	uint32_t zero = 0;
	RD::get_singleton()->buffer_update(occluded_buffer, 0, sizeof(uint32_t), &zero);

	result.visible_buffer = occluded_buffer;
	result.max_visible = p_max_visible;

	// No CPU readback of Pass B's survivor count - dispatch p_frustum_result's full fixed buffer
	// capacity instead (see the shader's own clamped bounds check against work_items.count).
	Vector<RD::Uniform> uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 0;
		u.append_id(p_frustum_result.visible_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 1;
		u.append_id(p_transforms_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 2;
		u.append_id(MeshletStorage::get_singleton()->get_meshlet_descriptor_buffer_rid());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>{ hiz_sampler, p_hiz_texture });
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 4;
		u.append_id(occluded_buffer);
		uniforms.push_back(u);
	}
	RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, occlusion_shader_rid, 0);

	struct OcclusionPushConstant {
		uint32_t work_item_count;
		uint32_t max_visible;
		uint32_t hiz_mip_count;
		uint32_t pad0;
		float view_matrix[16];
		float proj_x_scale;
		float proj_y_scale;
		float proj_z_a;
		float proj_z_b;
		float screen_size[2];
		float pad1[2];
	};
	OcclusionPushConstant push_constant;
	push_constant.work_item_count = p_frustum_result.max_visible; // Capacity, not the real count.
	push_constant.max_visible = p_max_visible;
	push_constant.hiz_mip_count = p_hiz_mip_count;
	push_constant.pad0 = 0;
	transform_to_mat4_columns(p_camera_transform.affine_inverse(), push_constant.view_matrix);
	push_constant.proj_x_scale = p_projection.columns[0][0];
	push_constant.proj_y_scale = p_projection.columns[1][1];
	push_constant.proj_z_a = p_projection.columns[2][2];
	push_constant.proj_z_b = p_projection.columns[3][2];
	push_constant.screen_size[0] = (float)p_screen_size.width;
	push_constant.screen_size[1] = (float)p_screen_size.height;
	push_constant.pad1[0] = 0;
	push_constant.pad1[1] = 0;

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, occlusion_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_frustum_result.max_visible, 1, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->free_rid(uniform_set);

	return result;
}

MeshletCuller::IndirectDrawResult MeshletCuller::emit_indirect_draws(const CullResult &p_result, uint32_t p_max_draws) {
	IndirectDrawResult result;

	ERR_FAIL_COND_V(!p_result.is_valid(), result);
	ERR_FAIL_NULL_V(MeshletStorage::get_singleton(), result);

	_ensure_command_capacity(p_max_draws);
	result.command_buffer = command_buffer;
	result.max_draw_count = p_max_draws;

	// Lazily create a persistent 4-byte SSBO for the draw count. The emit-draws compute shader
	// writes the actual visible count here; vkCmdDrawIndexedIndirectCount reads it at draw time
	// so the GPU only processes the real number of draws, not the full p_max_draws capacity.
	if (!draw_count_buffer.is_valid()) {
		draw_count_buffer = RD::get_singleton()->storage_buffer_create(sizeof(uint32_t), Span<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	result.count_buffer = draw_count_buffer;

	Vector<RD::Uniform> uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 0;
		u.append_id(p_result.visible_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 1;
		u.append_id(MeshletStorage::get_singleton()->get_meshlet_descriptor_buffer_rid());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 2;
		u.append_id(command_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 3;
		u.append_id(draw_count_buffer);
		uniforms.push_back(u);
	}
	RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, emit_draws_shader_rid, 0);

	struct EmitDrawsPushConstant {
		uint32_t max_draws;
		uint32_t max_visible_capacity;
		uint32_t pad0;
		uint32_t pad1;
	};
	EmitDrawsPushConstant push_constant;
	push_constant.max_draws = p_max_draws;
	push_constant.max_visible_capacity = p_result.max_visible;
	push_constant.pad0 = 0;
	push_constant.pad1 = 0;

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, emit_draws_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_max_draws, 1, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->free_rid(uniform_set);

	return result;
}

Vector<MeshletCuller::IndirectCommand> MeshletCuller::debug_read_commands(const IndirectDrawResult &p_result) {
	Vector<IndirectCommand> out;
	if (!p_result.is_valid() || p_result.max_draw_count == 0) {
		return out;
	}
	Vector<uint8_t> data = RD::get_singleton()->buffer_get_data(p_result.command_buffer, 0, p_result.max_draw_count * sizeof(IndirectCommand));
	out.resize(p_result.max_draw_count);
	if ((uint32_t)data.size() >= p_result.max_draw_count * sizeof(IndirectCommand)) {
		memcpy(out.ptrw(), data.ptr(), p_result.max_draw_count * sizeof(IndirectCommand));
	}
	return out;
}

Vector<MeshletCuller::VisibleMeshlet> MeshletCuller::debug_read_visible(const CullResult &p_result) {
	Vector<VisibleMeshlet> out;
	if (!p_result.is_valid()) {
		return out;
	}

	Vector<uint8_t> count_data = RD::get_singleton()->buffer_get_data(p_result.visible_buffer, 0, sizeof(uint32_t));
	uint32_t count = 0;
	if ((uint32_t)count_data.size() >= sizeof(uint32_t)) {
		memcpy(&count, count_data.ptr(), sizeof(uint32_t));
	}
	count = MIN(count, p_result.max_visible);
	if (count == 0) {
		return out;
	}

	Vector<uint8_t> data = RD::get_singleton()->buffer_get_data(p_result.visible_buffer, sizeof(uint32_t), count * sizeof(VisibleMeshlet));
	out.resize(count);
	if ((uint32_t)data.size() >= count * sizeof(VisibleMeshlet)) {
		memcpy(out.ptrw(), data.ptr(), count * sizeof(VisibleMeshlet));
	}
	return out;
}
