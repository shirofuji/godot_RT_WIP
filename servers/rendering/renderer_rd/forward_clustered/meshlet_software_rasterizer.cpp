/**************************************************************************/
/*  meshlet_software_rasterizer.cpp                                      */
/**************************************************************************/

#include "meshlet_software_rasterizer.h"
#include "servers/rendering/renderer_rd/storage_rd/meshlet_storage.h"
#include "servers/rendering/renderer_rd/meshlet_culler.h"

MeshletSoftwareRasterizer *MeshletSoftwareRasterizer::singleton = nullptr;

MeshletSoftwareRasterizer *MeshletSoftwareRasterizer::get_singleton() {
	return singleton;
}

MeshletSoftwareRasterizer::MeshletSoftwareRasterizer() {
	singleton = this;

	Vector<String> rasterize_versions;
	rasterize_versions.push_back("");
	rasterize_shader.initialize(rasterize_versions);
	rasterize_shader_version = rasterize_shader.version_create();
	rasterize_shader_rid = rasterize_shader.version_get_shader(rasterize_shader_version, 0);

	rasterize_pipeline = RD::get_singleton()->compute_pipeline_create(rasterize_shader_rid);

	Vector<String> versions;
	versions.push_back("");
	blit_shader.initialize(versions);
	blit_shader_version = blit_shader.version_create();
	blit_shader_rid = blit_shader.version_get_shader(blit_shader_version, 0);
}

MeshletSoftwareRasterizer::~MeshletSoftwareRasterizer() {
	if (rasterize_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(rasterize_pipeline);
	}
	rasterize_shader.version_free(rasterize_shader_version);

	if (blit_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(blit_pipeline);
	}
	blit_shader.version_free(blit_shader_version);

	if (depth_buffer.is_valid()) {
		RD::get_singleton()->free_rid(depth_buffer);
	}

	singleton = nullptr;
}

RID MeshletSoftwareRasterizer::get_blit_pipeline(RD::FramebufferFormatID p_framebuffer_format) {
	if (blit_pipeline.is_valid() && blit_framebuffer_format == p_framebuffer_format) {
		return blit_pipeline;
	}
	if (blit_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(blit_pipeline);
	}

	RD::PipelineDepthStencilState ds;
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL; // Reverse Z

	RD::PipelineRasterizationState rs;
	rs.cull_mode = RD::POLYGON_CULL_DISABLED;

	RD::PipelineColorBlendState blend;

	RID shader_rid = blit_shader.version_get_shader(blit_shader_version, 0);

	RD::PipelineMultisampleState multisample_state;

	blit_pipeline = RD::get_singleton()->render_pipeline_create(
			shader_rid,
			p_framebuffer_format,
			RD::INVALID_ID,
			RD::RENDER_PRIMITIVE_TRIANGLES,
			rs, multisample_state, ds, blend);

	blit_framebuffer_format = p_framebuffer_format;
	return blit_pipeline;
}

void MeshletSoftwareRasterizer::dispatch(RendererRD::MeshletCuller::CullResult p_cull_result, RID p_transforms_buffer, RID p_depth_framebuffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform) {
	if (!p_cull_result.is_valid()) {
		return;
	}

	RendererRD::MeshletStorage *meshlet_storage = RendererRD::MeshletStorage::get_singleton();
	if (!meshlet_storage) {
		return;
	}

	uint32_t needed_depth_size = p_screen_size.x * p_screen_size.y * sizeof(uint32_t);
	if (depth_buffer_size != needed_depth_size) {
		if (depth_buffer.is_valid()) {
			RD::get_singleton()->free_rid(depth_buffer);
		}
		depth_buffer = RD::get_singleton()->storage_buffer_create(needed_depth_size);
		depth_buffer_size = needed_depth_size;
	}

	RD::get_singleton()->buffer_clear(depth_buffer, 0, depth_buffer_size);

	// Compute pass
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, rasterize_pipeline);

	Vector<RD::Uniform> uniforms;
	
	// Set 1, binding 1: visible_meshlets
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 1;
		u.append_id(p_cull_result.visible_buffer);
		uniforms.push_back(u);
	}

	// Set 1, binding 2: transforms
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 2;
		u.append_id(p_transforms_buffer);
		uniforms.push_back(u);
	}

	// Set 1, binding 3: meshlet_descriptors
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 3;
		u.append_id(meshlet_storage->get_meshlet_descriptor_buffer_rid());
		uniforms.push_back(u);
	}

	// Set 1, binding 4: meshlet_vertex_remap
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 4;
		u.append_id(meshlet_storage->get_meshlet_vertex_buffer_rid());
		uniforms.push_back(u);
	}

	// Set 1, binding 5: meshlet_triangles
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 5;
		u.append_id(meshlet_storage->get_meshlet_triangle_buffer_rid());
		uniforms.push_back(u);
	}

	// Set 1, binding 6: meshlet_vertex_positions
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 6;
		u.append_id(meshlet_storage->get_vertex_position_buffer_rid());
		uniforms.push_back(u);
	}

	RID uniform_set_1 = RD::get_singleton()->uniform_set_create(uniforms, rasterize_shader_rid, 1);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_1, 1);

	// Set 2, binding 0: depth_buffer
	Vector<RD::Uniform> depth_uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 0;
		u.append_id(depth_buffer);
		depth_uniforms.push_back(u);
	}
	RID uniform_set_2 = RD::get_singleton()->uniform_set_create(depth_uniforms, rasterize_shader_rid, 2);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_2, 2);

	MeshletSoftwareRasterizePushConstant push_constant;
	// Compute view_projection
	Transform3D view = p_camera_transform.affine_inverse();
	Projection view_projection = p_projection * view;
	
	// Create float array from projection matrix
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			push_constant.view_projection_matrix[i * 4 + j] = view_projection.columns[i][j];
		}
	}
	push_constant.viewport_size[0] = p_screen_size.x;
	push_constant.viewport_size[1] = p_screen_size.y;

	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(MeshletSoftwareRasterizePushConstant));

	RD::get_singleton()->compute_list_dispatch(compute_list, p_cull_result.max_visible, 1, 1);
	RD::get_singleton()->compute_list_end();

	// Blit pass
	RD::FramebufferFormatID blit_fb_format = RD::get_singleton()->framebuffer_get_format(p_depth_framebuffer);
	RID current_blit_pipeline = get_blit_pipeline(blit_fb_format);

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_depth_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0, Rect2i(0, 0, p_screen_size.x, p_screen_size.y));
	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, current_blit_pipeline);
	
	Vector<RD::Uniform> blit_uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 0;
		u.append_id(depth_buffer);
		blit_uniforms.push_back(u);
	}
	RID blit_uniform_set = RD::get_singleton()->uniform_set_create(blit_uniforms, blit_shader_rid, 0);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, blit_uniform_set, 0);

	MeshletDepthBlitPushConstant blit_push_constant;
	blit_push_constant.width = p_screen_size.x;
	RD::get_singleton()->draw_list_set_push_constant(draw_list, &blit_push_constant, sizeof(MeshletDepthBlitPushConstant));

	RD::get_singleton()->draw_list_draw(draw_list, false, 1, 3);
	RD::get_singleton()->draw_list_end();
}
