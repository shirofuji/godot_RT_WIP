/**************************************************************************/
/*  meshlet_software_rasterizer.cpp                                      */
/**************************************************************************/

#include "meshlet_software_rasterizer.h"
#include "servers/rendering/renderer_rd/storage_rd/meshlet_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

MeshletSoftwareRasterizer *MeshletSoftwareRasterizer::singleton = nullptr;

MeshletSoftwareRasterizer *MeshletSoftwareRasterizer::get_singleton() {
	return singleton;
}

MeshletSoftwareRasterizer::MeshletSoftwareRasterizer() {
	singleton = this;

	int64_supported = RD::get_singleton()->has_feature(RD::SUPPORTS_BUFFER_ATOMIC_INT64);

	// Rasterize shader: variant 0 = int64 visbuffer, variant 1 = 32-bit fallback.
	Vector<String> rasterize_versions;
	rasterize_versions.push_back(""); // 0: int64.
	rasterize_versions.push_back("\n#define MESHLET_VISBUFFER_FALLBACK\n"); // 1: fallback.
	rasterize_shader.initialize(rasterize_versions);
	rasterize_shader_version = rasterize_shader.version_create();

	// The fallback variant works everywhere; always build it. The int64 variant needs the device
	// feature (its `#extension ... : require` maps to a SPIR-V capability the device must support), so
	// only compile + pipeline it when the capability is present.
	rasterize_pipeline_fallback = RD::get_singleton()->compute_pipeline_create(rasterize_shader.version_get_shader(rasterize_shader_version, 1));
	if (int64_supported) {
		rasterize_pipeline_int64 = RD::get_singleton()->compute_pipeline_create(rasterize_shader.version_get_shader(rasterize_shader_version, 0));
	}

	Vector<String> args_versions;
	args_versions.push_back("");
	dispatch_args_shader.initialize(args_versions);
	dispatch_args_shader_version = dispatch_args_shader.version_create();
	dispatch_args_shader_rid = dispatch_args_shader.version_get_shader(dispatch_args_shader_version, 0);
	dispatch_args_pipeline = RD::get_singleton()->compute_pipeline_create(dispatch_args_shader_rid);

	// VkDispatchIndirectCommand {x,y,z} - written by the args shader, read by dispatch_indirect.
	dispatch_args_buffer = RD::get_singleton()->storage_buffer_create(sizeof(uint32_t) * 3, Span<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);

	// --- Hardware visbuffer raster (P3): vertex-pulling + side-effect fragment into the same visbuffer. ---
	Vector<String> hw_versions;
	hw_versions.push_back(""); // 0: int64.
	hw_versions.push_back("\n#define MESHLET_VISBUFFER_FALLBACK\n"); // 1: fallback.
	hw_raster_shader.initialize(hw_versions);
	hw_raster_shader_version = hw_raster_shader.version_create();

	// Vertex-pulling: no per-vertex attributes; gl_VertexIndex/gl_InstanceIndex drive manual fetches.
	const uint32_t MAX_TRIANGLES_PER_MESHLET = 124; // Matches MeshletRenderer / the meshlet bake cap.
	const uint32_t hw_index_count = MAX_TRIANGLES_PER_MESHLET * 3;
	hw_vertex_format = RD::get_singleton()->vertex_format_create(Vector<RD::VertexAttribute>());
	hw_empty_vertex_array = RD::get_singleton()->vertex_array_create(hw_index_count, hw_vertex_format, Vector<RID>());
	LocalVector<uint32_t> hw_indices;
	hw_indices.resize(hw_index_count);
	for (uint32_t i = 0; i < hw_index_count; i++) {
		hw_indices[i] = i;
	}
	hw_synthetic_index_buffer = RD::get_singleton()->index_buffer_create(hw_index_count, RD::INDEX_BUFFER_FORMAT_UINT32, Span<uint8_t>((const uint8_t *)hw_indices.ptr(), hw_indices.size() * sizeof(uint32_t)));
	hw_synthetic_index_array = RD::get_singleton()->index_array_create(hw_synthetic_index_buffer, 0, hw_index_count);

	hw_framebuffer_format = RD::get_singleton()->framebuffer_format_create_empty();

	RD::PipelineRasterizationState hw_rs; // No cull (POLYGON_CULL_DISABLED default) - atomicMax picks nearest.
	RD::PipelineDepthStencilState hw_ds; // Depth test off (default) - the atomicMax IS the depth test.
	RD::PipelineColorBlendState hw_blend = RD::PipelineColorBlendState::create_disabled(0); // Zero color attachments.
	hw_raster_pipeline_fallback = RD::get_singleton()->render_pipeline_create(hw_raster_shader.version_get_shader(hw_raster_shader_version, 1), hw_framebuffer_format, hw_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, hw_rs, RD::PipelineMultisampleState(), hw_ds, hw_blend);
	if (int64_supported) {
		hw_raster_pipeline_int64 = RD::get_singleton()->render_pipeline_create(hw_raster_shader.version_get_shader(hw_raster_shader_version, 0), hw_framebuffer_format, hw_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, hw_rs, RD::PipelineMultisampleState(), hw_ds, hw_blend);
	}
}

MeshletSoftwareRasterizer::~MeshletSoftwareRasterizer() {
	if (rasterize_pipeline_int64.is_valid()) {
		RD::get_singleton()->free_rid(rasterize_pipeline_int64);
	}
	if (rasterize_pipeline_fallback.is_valid()) {
		RD::get_singleton()->free_rid(rasterize_pipeline_fallback);
	}
	rasterize_shader.version_free(rasterize_shader_version);

	if (dispatch_args_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(dispatch_args_pipeline);
	}
	dispatch_args_shader.version_free(dispatch_args_shader_version);
	if (dispatch_args_buffer.is_valid()) {
		RD::get_singleton()->free_rid(dispatch_args_buffer);
	}

	if (hw_raster_pipeline_int64.is_valid()) {
		RD::get_singleton()->free_rid(hw_raster_pipeline_int64);
	}
	if (hw_raster_pipeline_fallback.is_valid()) {
		RD::get_singleton()->free_rid(hw_raster_pipeline_fallback);
	}
	hw_raster_shader.version_free(hw_raster_shader_version);
	if (hw_empty_vertex_array.is_valid()) {
		RD::get_singleton()->free_rid(hw_empty_vertex_array);
	}
	if (hw_synthetic_index_array.is_valid()) {
		RD::get_singleton()->free_rid(hw_synthetic_index_array);
	}
	if (hw_synthetic_index_buffer.is_valid()) {
		RD::get_singleton()->free_rid(hw_synthetic_index_buffer);
	}
	if (hw_framebuffer.is_valid()) {
		RD::get_singleton()->free_rid(hw_framebuffer);
	}
	// hw_vertex_format is a cached/interned VertexFormatID, not an owned RID - no free.

	if (visbuffer_u64.is_valid()) {
		RD::get_singleton()->free_rid(visbuffer_u64);
	}
	if (vis_depth_u32.is_valid()) {
		RD::get_singleton()->free_rid(vis_depth_u32);
	}
	if (vis_payload_u32.is_valid()) {
		RD::get_singleton()->free_rid(vis_payload_u32);
	}

	singleton = nullptr;
}

void MeshletSoftwareRasterizer::_ensure_visbuffer(const Size2i &p_screen_size, bool p_int64) {
	bool have = visbuffer_dims == p_screen_size && visbuffer_is_int64 == p_int64 &&
			(p_int64 ? visbuffer_u64.is_valid() : (vis_depth_u32.is_valid() && vis_payload_u32.is_valid()));
	if (have) {
		return;
	}

	// Free whatever is currently allocated (dims and/or layout changed).
	if (visbuffer_u64.is_valid()) {
		RD::get_singleton()->free_rid(visbuffer_u64);
		visbuffer_u64 = RID();
	}
	if (vis_depth_u32.is_valid()) {
		RD::get_singleton()->free_rid(vis_depth_u32);
		vis_depth_u32 = RID();
	}
	if (vis_payload_u32.is_valid()) {
		RD::get_singleton()->free_rid(vis_payload_u32);
		vis_payload_u32 = RID();
	}

	uint32_t pixel_count = (uint32_t)MAX(1, p_screen_size.x * p_screen_size.y);
	if (p_int64) {
		visbuffer_u64 = RD::get_singleton()->storage_buffer_create(pixel_count * sizeof(uint64_t));
	} else {
		vis_depth_u32 = RD::get_singleton()->storage_buffer_create(pixel_count * sizeof(uint32_t));
		vis_payload_u32 = RD::get_singleton()->storage_buffer_create(pixel_count * sizeof(uint32_t));
	}
	visbuffer_dims = p_screen_size;
	visbuffer_is_int64 = p_int64;
}

void MeshletSoftwareRasterizer::rasterize(const RendererRD::MeshletCuller::CullResult &p_list, RID p_transforms_buffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform, bool p_force_fallback) {
	if (!p_list.is_valid()) {
		return;
	}
	RendererRD::MeshletStorage *ms = RendererRD::MeshletStorage::get_singleton();
	if (!ms) {
		return;
	}
	if (p_screen_size.x <= 0 || p_screen_size.y <= 0) {
		return;
	}

	bool use_int64 = int64_supported && !p_force_fallback && rasterize_pipeline_int64.is_valid();
	_ensure_visbuffer(p_screen_size, use_int64);

	uint32_t pixel_count = (uint32_t)(p_screen_size.x * p_screen_size.y);

	// Clear the visbuffer to 0 (reverse-Z: 0 = far / empty; any real fragment's depth beats it).
	if (use_int64) {
		RD::get_singleton()->buffer_clear(visbuffer_u64, 0, pixel_count * sizeof(uint64_t));
	} else {
		RD::get_singleton()->buffer_clear(vis_depth_u32, 0, pixel_count * sizeof(uint32_t));
		RD::get_singleton()->buffer_clear(vis_payload_u32, 0, pixel_count * sizeof(uint32_t));
	}

	// Pass 1: build the indirect dispatch args from the (GPU-side) software count.
	{
		LocalVector<RD::Uniform> uniforms;
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 0;
			u.append_id(p_list.visible_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 1;
			u.append_id(dispatch_args_buffer);
			uniforms.push_back(u);
		}
		RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(dispatch_args_shader_rid, 0, uniforms);

		DispatchArgsPushConstant pc;
		pc.max_visible = p_list.max_visible;
		pc.pad0 = 0;
		pc.pad1 = 0;
		pc.pad2 = 0;

		RD::ComputeListID cl = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(cl, dispatch_args_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(cl, set, 0);
		RD::get_singleton()->compute_list_set_push_constant(cl, &pc, sizeof(DispatchArgsPushConstant));
		RD::get_singleton()->compute_list_dispatch(cl, 1, 1, 1);
		RD::get_singleton()->compute_list_end();
	}

	// Pass 2: rasterize the meshlet list into the visbuffer (indirect: one workgroup per meshlet).
	{
		RID shader_rid = rasterize_shader.version_get_shader(rasterize_shader_version, use_int64 ? 0 : 1);

		LocalVector<RD::Uniform> uniforms;
		auto add_ssbo = [&](uint32_t p_binding, RID p_buffer) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = p_binding;
			u.append_id(p_buffer);
			uniforms.push_back(u);
		};
		add_ssbo(0, p_list.visible_buffer);
		add_ssbo(1, p_transforms_buffer);
		add_ssbo(2, ms->get_meshlet_descriptor_buffer_rid());
		add_ssbo(3, ms->get_meshlet_vertex_buffer_rid());
		add_ssbo(4, ms->get_meshlet_triangle_buffer_rid());
		add_ssbo(5, ms->get_vertex_position_buffer_rid());
		if (use_int64) {
			add_ssbo(6, visbuffer_u64);
		} else {
			add_ssbo(6, vis_depth_u32);
			add_ssbo(7, vis_payload_u32);
		}
		RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 0, uniforms);

		Projection view_projection = p_projection * Projection(p_camera_transform.affine_inverse());
		RasterizePushConstant pc;
		for (int col = 0; col < 4; col++) {
			for (int row = 0; row < 4; row++) {
				pc.view_projection_matrix[col * 4 + row] = view_projection.columns[col][row];
			}
		}
		pc.viewport_width = (uint32_t)p_screen_size.x;
		pc.viewport_height = (uint32_t)p_screen_size.y;
		pc.max_visible = p_list.max_visible;
		pc.pad = 0;

		RD::ComputeListID cl = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(cl, use_int64 ? rasterize_pipeline_int64 : rasterize_pipeline_fallback);
		RD::get_singleton()->compute_list_bind_uniform_set(cl, set, 0);
		RD::get_singleton()->compute_list_set_push_constant(cl, &pc, sizeof(RasterizePushConstant));
		RD::get_singleton()->compute_list_dispatch_indirect(cl, dispatch_args_buffer, 0);
		RD::get_singleton()->compute_list_end();
	}
}

void MeshletSoftwareRasterizer::_ensure_hw_framebuffer(const Size2i &p_screen_size) {
	if (hw_framebuffer.is_valid() && hw_framebuffer_dims == p_screen_size) {
		return;
	}
	if (hw_framebuffer.is_valid()) {
		RD::get_singleton()->free_rid(hw_framebuffer);
	}
	// Attachment-less framebuffer: its size is the raster area; the fragment shader only writes the
	// visbuffer SSBO (needs SUPPORTS_FRAGMENT_SHADER_WITH_ONLY_SIDE_EFFECTS - true on Vulkan).
	hw_framebuffer = RD::get_singleton()->framebuffer_create_empty(p_screen_size, RD::TEXTURE_SAMPLES_1, hw_framebuffer_format);
	hw_framebuffer_dims = p_screen_size;
}

void MeshletSoftwareRasterizer::rasterize_hardware(const RendererRD::MeshletCuller::CullResult &p_list, RID p_transforms_buffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform, bool p_clear, bool p_force_fallback) {
	if (!p_list.is_valid()) {
		return;
	}
	RendererRD::MeshletStorage *ms = RendererRD::MeshletStorage::get_singleton();
	RendererRD::MeshletCuller *culler = RendererRD::MeshletCuller::get_singleton();
	if (!ms || !culler) {
		return;
	}
	if (p_screen_size.x <= 0 || p_screen_size.y <= 0) {
		return;
	}

	bool use_int64 = int64_supported && !p_force_fallback && hw_raster_pipeline_int64.is_valid();
	_ensure_visbuffer(p_screen_size, use_int64);

	uint32_t pixel_count = (uint32_t)(p_screen_size.x * p_screen_size.y);
	if (p_clear) {
		if (use_int64) {
			RD::get_singleton()->buffer_clear(visbuffer_u64, 0, pixel_count * sizeof(uint64_t));
		} else {
			RD::get_singleton()->buffer_clear(vis_depth_u32, 0, pixel_count * sizeof(uint32_t));
			RD::get_singleton()->buffer_clear(vis_payload_u32, 0, pixel_count * sizeof(uint32_t));
		}
	}

	// One indirect draw command per visible meshlet (index_count = its triangle_count*3, firstInstance
	// = its slot). draw_indirect_count reads the real draw count off the GPU.
	RendererRD::MeshletCuller::IndirectDrawResult draws = culler->emit_indirect_draws(p_list, p_list.max_visible);
	if (!draws.is_valid() || draws.max_draw_count == 0) {
		return;
	}

	_ensure_hw_framebuffer(p_screen_size);

	RID shader_rid = hw_raster_shader.version_get_shader(hw_raster_shader_version, use_int64 ? 0 : 1);
	LocalVector<RD::Uniform> uniforms;
	auto add_ssbo = [&](uint32_t p_binding, RID p_buffer) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = p_binding;
		u.append_id(p_buffer);
		uniforms.push_back(u);
	};
	add_ssbo(0, p_list.visible_buffer);
	add_ssbo(1, p_transforms_buffer);
	add_ssbo(2, ms->get_meshlet_descriptor_buffer_rid());
	add_ssbo(3, ms->get_meshlet_vertex_buffer_rid());
	add_ssbo(4, ms->get_meshlet_triangle_buffer_rid());
	add_ssbo(5, ms->get_vertex_position_buffer_rid());
	if (use_int64) {
		add_ssbo(6, visbuffer_u64);
	} else {
		add_ssbo(6, vis_depth_u32);
		add_ssbo(7, vis_payload_u32);
	}
	RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 0, uniforms);

	Projection view_projection = p_projection * Projection(p_camera_transform.affine_inverse());
	HwRasterPushConstant pc;
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			pc.view_projection_matrix[col * 4 + row] = view_projection.columns[col][row];
		}
	}
	pc.viewport_width = (uint32_t)p_screen_size.x;
	pc.viewport_height = (uint32_t)p_screen_size.y;
	pc.pad0 = 0;
	pc.pad1 = 0;

	RD::DrawListID dl = RD::get_singleton()->draw_list_begin(hw_framebuffer, RD::DRAW_DEFAULT_ALL);
	RD::get_singleton()->draw_list_bind_render_pipeline(dl, use_int64 ? hw_raster_pipeline_int64 : hw_raster_pipeline_fallback);
	RD::get_singleton()->draw_list_bind_uniform_set(dl, set, 0);
	RD::get_singleton()->draw_list_bind_vertex_array(dl, hw_empty_vertex_array);
	RD::get_singleton()->draw_list_bind_index_array(dl, hw_synthetic_index_array);
	RD::get_singleton()->draw_list_set_push_constant(dl, &pc, sizeof(HwRasterPushConstant));
	RD::get_singleton()->draw_list_set_viewport(dl, Rect2(0, 0, p_screen_size.x, p_screen_size.y));
	RD::get_singleton()->draw_list_draw_indirect_count(dl, true, draws.command_buffer, 0, draws.count_buffer, 0, draws.max_draw_count, sizeof(RendererRD::MeshletCuller::IndirectCommand));
	RD::get_singleton()->draw_list_end();
}
