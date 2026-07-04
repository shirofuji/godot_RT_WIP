/**************************************************************************/
/*  meshlet_software_rasterizer.cpp                                      */
/**************************************************************************/

#include "meshlet_software_rasterizer.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/meshlet_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
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

	RD::PipelineRasterizationState hw_rs;
	// CULL_BACK, matching MeshletRenderer::render()'s pipeline. Originally POLYGON_CULL_DISABLED on the
	// theory that atomicMax would just keep the nearest of the front+back faces - but in practice
	// keeping back faces let a back face win the depth in patches (confirmed: full visbuffer coverage
	// but blocky wrong-facing normals), showing the dark mesh interior as "holes". Culling back faces
	// (as the working color path does) leaves only front surfaces in the visbuffer.
	hw_rs.cull_mode = RD::POLYGON_CULL_BACK;
	RD::PipelineDepthStencilState hw_ds; // Depth test off (default) - the atomicMax IS the depth test.
	RD::PipelineColorBlendState hw_blend = RD::PipelineColorBlendState::create_disabled(0); // Zero color attachments.
	hw_raster_pipeline_fallback = RD::get_singleton()->render_pipeline_create(hw_raster_shader.version_get_shader(hw_raster_shader_version, 1), hw_framebuffer_format, hw_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, hw_rs, RD::PipelineMultisampleState(), hw_ds, hw_blend);
	if (int64_supported) {
		hw_raster_pipeline_int64 = RD::get_singleton()->render_pipeline_create(hw_raster_shader.version_get_shader(hw_raster_shader_version, 0), hw_framebuffer_format, hw_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, hw_rs, RD::PipelineMultisampleState(), hw_ds, hw_blend);
	}

	// --- Material resolve (P4): shades the visbuffer into out_color. ---
	Vector<String> resolve_versions;
	resolve_versions.push_back(""); // 0: int64.
	resolve_versions.push_back("\n#define MESHLET_VISBUFFER_FALLBACK\n"); // 1: fallback.
	resolve_shader.initialize(resolve_versions);
	resolve_shader_version = resolve_shader.version_create();
	resolve_pipeline_fallback = RD::get_singleton()->compute_pipeline_create(resolve_shader.version_get_shader(resolve_shader_version, 1));
	if (int64_supported) {
		resolve_pipeline_int64 = RD::get_singleton()->compute_pipeline_create(resolve_shader.version_get_shader(resolve_shader_version, 0));
	}

	RD::SamplerState radiance_ss;
	radiance_ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	radiance_ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
	radiance_ss.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	radiance_ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	radiance_ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	radiance_ss.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	resolve_radiance_sampler = RD::get_singleton()->sampler_create(radiance_ss);

	RD::SamplerState material_ss;
	material_ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	material_ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
	material_ss.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	material_ss.repeat_u = RD::SAMPLER_REPEAT_MODE_REPEAT;
	material_ss.repeat_v = RD::SAMPLER_REPEAT_MODE_REPEAT;
	material_ss.repeat_w = RD::SAMPLER_REPEAT_MODE_REPEAT;
	material_ss.use_anisotropy = true;
	material_ss.anisotropy_max = 4.0f;
	resolve_material_sampler = RD::get_singleton()->sampler_create(material_ss);

	// Fragment resolve (P5): shader now; pipelines are created lazily per framebuffer format.
	Vector<String> resolve_raster_versions;
	resolve_raster_versions.push_back(""); // 0: int64.
	resolve_raster_versions.push_back("\n#define MESHLET_VISBUFFER_FALLBACK\n"); // 1: fallback.
	resolve_raster_shader.initialize(resolve_raster_versions);
	resolve_raster_shader_version = resolve_raster_shader.version_create();
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

	if (resolve_pipeline_int64.is_valid()) {
		RD::get_singleton()->free_rid(resolve_pipeline_int64);
	}
	if (resolve_pipeline_fallback.is_valid()) {
		RD::get_singleton()->free_rid(resolve_pipeline_fallback);
	}
	resolve_shader.version_free(resolve_shader_version);
	if (resolve_raster_pipeline_int64.is_valid()) {
		RD::get_singleton()->free_rid(resolve_raster_pipeline_int64);
	}
	if (resolve_raster_pipeline_fallback.is_valid()) {
		RD::get_singleton()->free_rid(resolve_raster_pipeline_fallback);
	}
	resolve_raster_shader.version_free(resolve_raster_shader_version);
	if (out_color.is_valid()) {
		RD::get_singleton()->free_rid(out_color);
	}
	if (resolve_radiance_sampler.is_valid()) {
		RD::get_singleton()->free_rid(resolve_radiance_sampler);
	}
	if (resolve_material_sampler.is_valid()) {
		RD::get_singleton()->free_rid(resolve_material_sampler);
	}

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

void MeshletSoftwareRasterizer::_ensure_out_color(const Size2i &p_screen_size) {
	if (out_color.is_valid() && out_color_dims == p_screen_size) {
		return;
	}
	if (out_color.is_valid()) {
		RD::get_singleton()->free_rid(out_color);
	}
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	tf.width = (uint32_t)p_screen_size.x;
	tf.height = (uint32_t)p_screen_size.y;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	out_color = RD::get_singleton()->texture_create(tf, RD::TextureView());
	out_color_dims = p_screen_size;
}

void MeshletSoftwareRasterizer::resolve(const RendererRD::MeshletCuller::CullResult &p_sw_list, const RendererRD::MeshletCuller::CullResult &p_hw_list, RID p_transforms_buffer, RID p_material_ids_buffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform, RID p_lights_buffer, uint32_t p_light_count, const Color &p_ambient_color, float p_sky_mix, RID p_svogi_octree, const Vector3 &p_svogi_center, float p_svogi_half, float p_svogi_energy, RID p_radiance_texture, float p_radiance_exposure, float p_max_roughness_lod) {
	RendererRD::MeshletStorage *ms = RendererRD::MeshletStorage::get_singleton();
	RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
	if (!ms || !ts) {
		return;
	}
	if (p_screen_size.x <= 0 || p_screen_size.y <= 0) {
		return;
	}

	// Resolve the layout the last raster wrote (int64 vs 32-bit fallback).
	bool use_int64 = visbuffer_is_int64;
	RID pipeline = use_int64 ? resolve_pipeline_int64 : resolve_pipeline_fallback;
	if (pipeline.is_null()) {
		return;
	}
	if (use_int64 ? visbuffer_u64.is_null() : (vis_depth_u32.is_null() || vis_payload_u32.is_null())) {
		return;
	}

	_ensure_out_color(p_screen_size);
	// Clear to transparent black: the resolve only writes pixels that have a visbuffer fragment, so
	// uncovered pixels keep this (in the live frame, out_color would be the scene color to composite on).
	RD::get_singleton()->texture_clear(out_color, Color(0, 0, 0, 0), 0, 1, 0, 1);

	RID standin = ms->get_meshlet_material_buffer_rid(); // Any valid storage buffer for unused list/svogi bindings.
	RID sw_buf = p_sw_list.is_valid() ? p_sw_list.visible_buffer : standin;
	RID hw_buf = p_hw_list.is_valid() ? p_hw_list.visible_buffer : standin;
	RID svogi_buf = p_svogi_octree.is_valid() ? p_svogi_octree : standin;
	RID radiance_tex = p_radiance_texture.is_valid() ? p_radiance_texture : ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
	RID default_white = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);

	LocalVector<RD::Uniform> uniforms;
	auto add_ssbo = [&](uint32_t p_binding, RID p_buffer) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = p_binding;
		u.append_id(p_buffer);
		uniforms.push_back(u);
	};
	if (use_int64) {
		add_ssbo(0, visbuffer_u64);
	} else {
		add_ssbo(0, vis_depth_u32);
		add_ssbo(1, vis_payload_u32);
	}
	add_ssbo(2, sw_buf);
	add_ssbo(3, hw_buf);
	add_ssbo(4, p_transforms_buffer);
	add_ssbo(5, ms->get_meshlet_descriptor_buffer_rid());
	add_ssbo(6, ms->get_meshlet_vertex_buffer_rid());
	add_ssbo(7, ms->get_meshlet_triangle_buffer_rid());
	add_ssbo(8, ms->get_vertex_position_buffer_rid());
	add_ssbo(9, ms->get_vertex_attribute_buffer_rid());
	add_ssbo(10, p_material_ids_buffer);
	add_ssbo(11, ms->get_meshlet_material_buffer_rid());
	add_ssbo(12, p_lights_buffer);
	add_ssbo(13, svogi_buf);
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 14;
		u.append_id(radiance_tex);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		u.binding = 15;
		u.append_id(resolve_radiance_sampler);
		uniforms.push_back(u);
	}
	{
		const Vector<RID> &tex_table = ms->get_material_texture_rids();
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 16;
		for (uint32_t i = 0; i < RendererRD::MeshletStorage::MAX_MATERIAL_TEXTURES; i++) {
			u.append_id(i < (uint32_t)tex_table.size() ? tex_table[i] : default_white);
		}
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		u.binding = 17;
		u.append_id(resolve_material_sampler);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 18;
		u.append_id(out_color);
		uniforms.push_back(u);
	}

	RID shader_rid = resolve_shader.version_get_shader(resolve_shader_version, use_int64 ? 0 : 1);
	RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 0, uniforms);

	Projection view_projection = p_projection * Projection(p_camera_transform.affine_inverse());
	ResolvePushConstant pc;
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			pc.view_projection_matrix[col * 4 + row] = view_projection.columns[col][row];
		}
	}
	pc.camera_position[0] = p_camera_transform.origin.x;
	pc.camera_position[1] = p_camera_transform.origin.y;
	pc.camera_position[2] = p_camera_transform.origin.z;
	pc.light_count = p_light_count;
	pc.ambient_color[0] = p_ambient_color.r;
	pc.ambient_color[1] = p_ambient_color.g;
	pc.ambient_color[2] = p_ambient_color.b;
	pc.ambient_color[3] = p_sky_mix;
	bool svogi_active = p_svogi_octree.is_valid() && p_svogi_half > 0.0f;
	pc.svogi_bounds[0] = svogi_active ? (float)p_svogi_center.x : 0.0f;
	pc.svogi_bounds[1] = svogi_active ? (float)p_svogi_center.y : 0.0f;
	pc.svogi_bounds[2] = svogi_active ? (float)p_svogi_center.z : 0.0f;
	pc.svogi_bounds[3] = svogi_active ? p_svogi_half : 0.0f;
	pc.svogi_params[0] = svogi_active ? p_svogi_energy : 0.0f;
	pc.svogi_params[1] = p_radiance_exposure;
	pc.svogi_params[2] = p_max_roughness_lod;
	pc.svogi_params[3] = 0.0f;

	RD::ComputeListID cl = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(cl, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, set, 0);
	RD::get_singleton()->compute_list_set_push_constant(cl, &pc, sizeof(ResolvePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(cl, (uint32_t)p_screen_size.x, (uint32_t)p_screen_size.y, 1);
	RD::get_singleton()->compute_list_end();
}

uint32_t MeshletSoftwareRasterizer::debug_visbuffer_coverage(const Size2i &p_screen_size) {
	uint32_t pixel_count = (uint32_t)(p_screen_size.x * p_screen_size.y);
	if (pixel_count == 0) {
		return 0;
	}
	uint32_t covered = 0;
	if (visbuffer_is_int64) {
		if (visbuffer_u64.is_null()) {
			return 0;
		}
		Vector<uint8_t> bytes = RD::get_singleton()->buffer_get_data(visbuffer_u64, 0, pixel_count * sizeof(uint64_t));
		const uint64_t *v = (const uint64_t *)bytes.ptr();
		for (uint32_t i = 0; i < pixel_count; i++) {
			if (v[i] != 0) {
				covered++;
			}
		}
	} else {
		if (vis_depth_u32.is_null()) {
			return 0;
		}
		Vector<uint8_t> bytes = RD::get_singleton()->buffer_get_data(vis_depth_u32, 0, pixel_count * sizeof(uint32_t));
		const uint32_t *v = (const uint32_t *)bytes.ptr();
		for (uint32_t i = 0; i < pixel_count; i++) {
			if (v[i] != 0) {
				covered++;
			}
		}
	}
	return covered;
}

void MeshletSoftwareRasterizer::_ensure_resolve_raster_pipeline(RD::FramebufferFormatID p_fb_format) {
	if (resolve_raster_pipeline_format == p_fb_format && resolve_raster_pipeline_fallback.is_valid()) {
		return;
	}
	if (resolve_raster_pipeline_int64.is_valid()) {
		RD::get_singleton()->free_rid(resolve_raster_pipeline_int64);
		resolve_raster_pipeline_int64 = RID();
	}
	if (resolve_raster_pipeline_fallback.is_valid()) {
		RD::get_singleton()->free_rid(resolve_raster_pipeline_fallback);
		resolve_raster_pipeline_fallback = RID();
	}

	RD::PipelineRasterizationState rs; // Fullscreen triangle - no cull.
	RD::PipelineDepthStencilState ds;
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL; // Reverse-Z: write when nearer-or-equal.
	RD::PipelineColorBlendState blend = RD::PipelineColorBlendState::create_disabled(); // One opaque color attachment.

	// INVALID_FORMAT_ID vertex format: this is a procedural fullscreen draw (gl_VertexIndex only, no
	// vertex array bound), so the pipeline must declare no vertex input.
	resolve_raster_pipeline_fallback = RD::get_singleton()->render_pipeline_create(resolve_raster_shader.version_get_shader(resolve_raster_shader_version, 1), p_fb_format, RD::INVALID_FORMAT_ID, RD::RENDER_PRIMITIVE_TRIANGLES, rs, RD::PipelineMultisampleState(), ds, blend);
	if (int64_supported) {
		resolve_raster_pipeline_int64 = RD::get_singleton()->render_pipeline_create(resolve_raster_shader.version_get_shader(resolve_raster_shader_version, 0), p_fb_format, RD::INVALID_FORMAT_ID, RD::RENDER_PRIMITIVE_TRIANGLES, rs, RD::PipelineMultisampleState(), ds, blend);
	}
	resolve_raster_pipeline_format = p_fb_format;
}

void MeshletSoftwareRasterizer::resolve_raster(RID p_target_framebuffer, const RendererRD::MeshletCuller::CullResult &p_sw_list, const RendererRD::MeshletCuller::CullResult &p_hw_list, RID p_transforms_buffer, RID p_material_ids_buffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform, RID p_lights_buffer, uint32_t p_light_count, const Color &p_ambient_color, float p_sky_mix, RID p_svogi_octree, const Vector3 &p_svogi_center, float p_svogi_half, float p_svogi_energy, RID p_radiance_texture, float p_radiance_exposure, float p_max_roughness_lod, bool p_clear) {
	RendererRD::MeshletStorage *ms = RendererRD::MeshletStorage::get_singleton();
	RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
	if (!ms || !ts || !p_target_framebuffer.is_valid()) {
		return;
	}
	if (p_screen_size.x <= 0 || p_screen_size.y <= 0) {
		return;
	}
	bool use_int64 = visbuffer_is_int64;
	if (use_int64 ? visbuffer_u64.is_null() : (vis_depth_u32.is_null() || vis_payload_u32.is_null())) {
		return;
	}

	RD::FramebufferFormatID fb_format = RD::get_singleton()->framebuffer_get_format(p_target_framebuffer);
	_ensure_resolve_raster_pipeline(fb_format);
	RID pipeline = use_int64 ? resolve_raster_pipeline_int64 : resolve_raster_pipeline_fallback;
	if (pipeline.is_null()) {
		return;
	}

	RID standin = ms->get_meshlet_material_buffer_rid();
	RID sw_buf = p_sw_list.is_valid() ? p_sw_list.visible_buffer : standin;
	RID hw_buf = p_hw_list.is_valid() ? p_hw_list.visible_buffer : standin;
	RID svogi_buf = p_svogi_octree.is_valid() ? p_svogi_octree : standin;
	RID radiance_tex = p_radiance_texture.is_valid() ? p_radiance_texture : ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
	RID default_white = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);

	LocalVector<RD::Uniform> uniforms;
	auto add_ssbo = [&](uint32_t p_binding, RID p_buffer) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = p_binding;
		u.append_id(p_buffer);
		uniforms.push_back(u);
	};
	if (use_int64) {
		add_ssbo(0, visbuffer_u64);
	} else {
		add_ssbo(0, vis_depth_u32);
		add_ssbo(1, vis_payload_u32);
	}
	add_ssbo(2, sw_buf);
	add_ssbo(3, hw_buf);
	add_ssbo(4, p_transforms_buffer);
	add_ssbo(5, ms->get_meshlet_descriptor_buffer_rid());
	add_ssbo(6, ms->get_meshlet_vertex_buffer_rid());
	add_ssbo(7, ms->get_meshlet_triangle_buffer_rid());
	add_ssbo(8, ms->get_vertex_position_buffer_rid());
	add_ssbo(9, ms->get_vertex_attribute_buffer_rid());
	add_ssbo(10, p_material_ids_buffer);
	add_ssbo(11, ms->get_meshlet_material_buffer_rid());
	add_ssbo(12, p_lights_buffer);
	add_ssbo(13, svogi_buf);
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 14;
		u.append_id(radiance_tex);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		u.binding = 15;
		u.append_id(resolve_radiance_sampler);
		uniforms.push_back(u);
	}
	{
		const Vector<RID> &tex_table = ms->get_material_texture_rids();
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 16;
		for (uint32_t i = 0; i < RendererRD::MeshletStorage::MAX_MATERIAL_TEXTURES; i++) {
			u.append_id(i < (uint32_t)tex_table.size() ? tex_table[i] : default_white);
		}
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		u.binding = 17;
		u.append_id(resolve_material_sampler);
		uniforms.push_back(u);
	}

	RID shader_rid = resolve_raster_shader.version_get_shader(resolve_raster_shader_version, use_int64 ? 0 : 1);
	RID set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 0, uniforms);

	Projection view_projection = p_projection * Projection(p_camera_transform.affine_inverse());
	ResolvePushConstant pc;
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			pc.view_projection_matrix[col * 4 + row] = view_projection.columns[col][row];
		}
	}
	pc.camera_position[0] = p_camera_transform.origin.x;
	pc.camera_position[1] = p_camera_transform.origin.y;
	pc.camera_position[2] = p_camera_transform.origin.z;
	// Debug visualization mode from --meshlet-visbuffer-debug=N, packed into the top 4 bits of
	// light_count (0 = normal shading; see meshlet_visbuffer_resolve_inc.glsl for the modes).
	static uint32_t dbg_mode = []() {
		for (const String &a : OS::get_singleton()->get_cmdline_args()) {
			if (a.begins_with("--meshlet-visbuffer-debug=")) {
				return (uint32_t)CLAMP(a.get_slicec('=', 1).to_int(), 0, 15);
			}
		}
		return (uint32_t)0;
	}();
	pc.light_count = (p_light_count & 0x0FFFFFFFu) | (dbg_mode << 28);
	pc.ambient_color[0] = p_ambient_color.r;
	pc.ambient_color[1] = p_ambient_color.g;
	pc.ambient_color[2] = p_ambient_color.b;
	pc.ambient_color[3] = p_sky_mix;
	bool svogi_active = p_svogi_octree.is_valid() && p_svogi_half > 0.0f;
	pc.svogi_bounds[0] = svogi_active ? (float)p_svogi_center.x : 0.0f;
	pc.svogi_bounds[1] = svogi_active ? (float)p_svogi_center.y : 0.0f;
	pc.svogi_bounds[2] = svogi_active ? (float)p_svogi_center.z : 0.0f;
	pc.svogi_bounds[3] = svogi_active ? p_svogi_half : 0.0f;
	pc.svogi_params[0] = svogi_active ? p_svogi_energy : 0.0f;
	pc.svogi_params[1] = p_radiance_exposure;
	pc.svogi_params[2] = p_max_roughness_lod;
	// .w packs the viewport dims (width<<16 | height) for the fragment (no imageSize available there).
	uint32_t packed_dims = ((uint32_t)p_screen_size.x << 16) | ((uint32_t)p_screen_size.y & 0xFFFFu);
	memcpy(&pc.svogi_params[3], &packed_dims, sizeof(uint32_t));

	Vector<Color> clear_colors;
	clear_colors.push_back(Color(0, 0, 0, 0));
	RD::DrawFlags flags = p_clear ? RD::DRAW_CLEAR_ALL : RD::DRAW_DEFAULT_ALL;
	// clear_depth 0.0 = reverse-Z far, so any resolved fragment (depth > 0) passes GREATER_OR_EQUAL.
	RD::DrawListID dl = RD::get_singleton()->draw_list_begin(p_target_framebuffer, flags, clear_colors, 0.0f);
	RD::get_singleton()->draw_list_bind_render_pipeline(dl, pipeline);
	RD::get_singleton()->draw_list_bind_uniform_set(dl, set, 0);
	RD::get_singleton()->draw_list_set_push_constant(dl, &pc, sizeof(ResolvePushConstant));
	RD::get_singleton()->draw_list_set_viewport(dl, Rect2(0, 0, p_screen_size.x, p_screen_size.y));
	RD::get_singleton()->draw_list_draw(dl, false, 1, 3); // Fullscreen triangle, 3 procedural verts.
	RD::get_singleton()->draw_list_end();
}
