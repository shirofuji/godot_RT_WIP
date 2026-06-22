/**************************************************************************/
/*  meshlet_renderer.cpp                                                 */
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

#include "meshlet_renderer.h"

#include "servers/rendering/renderer_rd/storage_rd/meshlet_storage.h"

using namespace RendererRD;

MeshletRenderer *MeshletRenderer::singleton = nullptr;

MeshletRenderer *MeshletRenderer::get_singleton() {
	return singleton;
}

MeshletRenderer::MeshletRenderer() {
	singleton = this;

	Vector<String> versions;
	versions.push_back(""); // Version 0: normal (debug color + depth).
	versions.push_back("\n#define MESHLET_DEPTH_ONLY\n"); // Version 1: no fragment color output.
	render_shader.initialize(versions);
	render_shader_version = render_shader.version_create();
	render_shader_rid = render_shader.version_get_shader(render_shader_version, 0);
	depth_only_shader_rid = render_shader.version_get_shader(render_shader_version, 1);

	// Vertex-pulling: no per-surface vertex buffer at all, every attribute is fetched manually
	// in the vertex shader body via gl_VertexIndex/gl_InstanceIndex.
	vertex_format = RD::get_singleton()->vertex_format_create(Vector<RD::VertexAttribute>());
	empty_vertex_array = RD::get_singleton()->vertex_array_create(MAX_TRIANGLES_PER_MESHLET * 3, vertex_format, Vector<RID>());

	const uint32_t index_count = MAX_TRIANGLES_PER_MESHLET * 3;
	LocalVector<uint32_t> indices;
	indices.resize(index_count);
	for (uint32_t i = 0; i < index_count; i++) {
		indices[i] = i;
	}
	synthetic_index_buffer = RD::get_singleton()->index_buffer_create(index_count, RD::INDEX_BUFFER_FORMAT_UINT32, Span<uint8_t>((const uint8_t *)indices.ptr(), indices.size() * sizeof(uint32_t)));
	synthetic_index_array = RD::get_singleton()->index_array_create(synthetic_index_buffer, 0, index_count);
}

MeshletRenderer::~MeshletRenderer() {
	if (cached_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(cached_pipeline);
	}
	if (cached_depth_only_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(cached_depth_only_pipeline);
	}
	RD::get_singleton()->free_rid(synthetic_index_buffer);
	RD::get_singleton()->free_rid(empty_vertex_array);
	// vertex_format is a VertexFormatID (cached/interned by RD, not an owned RID) - no free needed.
	render_shader.version_free(render_shader_version);
	singleton = nullptr;
}

void MeshletRenderer::_ensure_pipeline(RD::FramebufferFormatID p_framebuffer_format, bool p_depth_only) {
	RID &cached = p_depth_only ? cached_depth_only_pipeline : cached_pipeline;
	RD::FramebufferFormatID &cached_format = p_depth_only ? cached_depth_only_framebuffer_format : cached_framebuffer_format;

	if (cached.is_valid() && cached_format == p_framebuffer_format) {
		return;
	}
	if (cached.is_valid()) {
		RD::get_singleton()->free_rid(cached);
	}

	RD::PipelineDepthStencilState ds;
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	// GREATER_OR_EQUAL, not LESS_OR_EQUAL: Godot RD's reversed-Z convention (near=1.0, far=0.0,
	// see meshlet_occlusion_test.glsl's comment) means a *larger* device-Z value is nearer, so the
	// nearer fragment must win with a greater-or-equal test, not a less-or-equal one - confirmed by
	// grepping Forward+'s own real pipelines (scene_shader_forward_clustered.cpp,
	// scene_shader_forward_mobile.cpp), which use this exact operator for their main opaque pass.
	// Flipping this to LESS_OR_EQUAL "fixes" a single-isolated-object scratch test by accident (it
	// happens to compensate for a one-off precision bias - see depth_bias below) but would be
	// actively wrong for any scene with other real geometry, since it disagrees with the rest of
	// the scene's own depth convention on every inter-object comparison.
	ds.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;

	// Backface culling left disabled (the RD default) - both front and back faces of every
	// meshlet are rasterized, with the depth test alone deciding the winner. Re-tested with
	// POLYGON_CULL_BACK + CW front_face after the depth-bias fix landed (in case the earlier test
	// was confounded by the missing bias) - confirmed it's a genuine, separate bug: the exact same
	// hole reappears even with the bias in place. Meshlet triangle winding likely isn't reliably
	// consistent across all meshlets from this pipeline (SurfaceTool::build_meshlets/meshoptimizer
	// may not guarantee uniform winding when repartitioning a source index buffer into clusters).
	// Worth investigating directly - fixing it would roughly halve this pipeline's fill-rate cost,
	// real performance work given Phase 6's whole point is hundreds-of-millions-of-polygon scenes.
	RD::PipelineRasterizationState rs;
	rs.cull_mode = RD::POLYGON_CULL_FRONT;

	// Depth bias is handled manually in the vertex shader (see meshlet_render.glsl), not via
	// RD::PipelineRasterizationState's depth_bias_* fields - those scale by Vulkan's own per-
	// fragment minimum-resolvable-difference for the depth format (designed for sub-ULP anti-
	// z-fighting noise), which turned out far too small to move the needle against the systematic
	// ~0.1%+ gap measured here (a constant_factor of 4-100 made no visible difference) - this
	// isn't floating-point noise, it's a consistent offset between two different vertex
	// computation paths producing the device-Z for the same logical surface point (this pipeline's
	// vertex-pulling vs. Forward+'s own real vertex shader, both targeting the same mesh). A
	// direct, fixed-fraction nudge in the shader gives predictable, measured control instead.

	RID shader_rid = p_depth_only ? depth_only_shader_rid : render_shader_rid;
	// create_disabled(0): the depth-only variant targets a framebuffer with zero color
	// attachments (e.g. Forward+'s real depth pre-pass framebuffer) - the color blend state's
	// attachment count must match the framebuffer's actual color attachment count.
	RD::PipelineColorBlendState blend_state = p_depth_only ? RD::PipelineColorBlendState::create_disabled(0) : RD::PipelineColorBlendState::create_disabled();

	cached = RD::get_singleton()->render_pipeline_create(shader_rid, p_framebuffer_format, vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, rs, RD::PipelineMultisampleState(), ds, blend_state);
	cached_format = p_framebuffer_format;
}

void MeshletRenderer::render(const MeshletCuller::CullResult &p_visible, const MeshletCuller::IndirectDrawResult &p_draws, RID p_transforms_buffer, RID p_material_ids_buffer, RID p_framebuffer, RD::FramebufferFormatID p_framebuffer_format, const Rect2i &p_viewport, const Projection &p_projection, const Transform3D &p_camera_transform, const Vector3 &p_light_direction, bool p_clear, bool p_depth_only) {
	ERR_FAIL_NULL(MeshletStorage::get_singleton());

	_ensure_pipeline(p_framebuffer_format, p_depth_only);

	RD::DrawListID draw_list;
	if (p_clear) {
		// 0.0 = far under Godot RD's reversed-Z convention (near=1.0, far=0.0) - matches the
		// pipeline's GREATER_OR_EQUAL depth compare op (see _ensure_pipeline()): clearing to 1.0
		// (the old, non-reversed-style "far" value) would make every real fragment's device-Z
		// (always < 1.0) fail the depth test against it, since nothing can be "more near" than
		// the maximum possible value.
		if (p_depth_only) {
			draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_CLEAR_DEPTH, Vector<Color>(), 0.0f);
		} else {
			Vector<Color> clear_colors;
			clear_colors.push_back(Color(0, 0, 0, 1));
			draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_CLEAR_ALL, clear_colors, 0.0f);
		}
	} else {
		draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL);
	}

	if (p_visible.is_valid() && p_draws.is_valid() && p_draws.max_draw_count > 0) {
		MeshletStorage *storage = MeshletStorage::get_singleton();

		Vector<RD::Uniform> uniforms;
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 0;
			u.append_id(p_visible.visible_buffer);
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
			u.append_id(storage->get_meshlet_descriptor_buffer_rid());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 3;
			u.append_id(storage->get_meshlet_vertex_buffer_rid());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 4;
			u.append_id(storage->get_meshlet_triangle_buffer_rid());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 5;
			u.append_id(storage->get_vertex_position_buffer_rid());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 6;
			u.append_id(storage->get_vertex_attribute_buffer_rid());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 7;
			u.append_id(p_material_ids_buffer);
			uniforms.push_back(u);
		}
		if (!p_depth_only) {
			// Binding 8 (MeshletMaterials) only exists in the normal shader version's descriptor
			// set layout - the depth-only fragment variant has no material lookup at all (guarded
			// out via #ifndef MESHLET_DEPTH_ONLY in meshlet_render.glsl), so its compiled shader
			// doesn't declare this binding; including it here for that variant would mismatch the
			// shader's actual layout.
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 8;
			u.append_id(storage->get_meshlet_material_buffer_rid());
			uniforms.push_back(u);
		}
		RID render_shader_rid_to_use = p_depth_only ? depth_only_shader_rid : render_shader_rid;
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, render_shader_rid_to_use, 0);

		struct RenderPushConstant {
			float view_projection[16];
			float light_direction[3];
			float pad0;
		};
		RenderPushConstant push_constant;
		Projection vp = p_projection * Projection(p_camera_transform.affine_inverse());
		for (int col = 0; col < 4; col++) {
			for (int row = 0; row < 4; row++) {
				push_constant.view_projection[col * 4 + row] = vp.columns[col][row];
			}
		}
		Vector3 light_dir = p_light_direction.normalized();
		push_constant.light_direction[0] = light_dir.x;
		push_constant.light_direction[1] = light_dir.y;
		push_constant.light_direction[2] = light_dir.z;
		push_constant.pad0 = 0;

		RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, p_depth_only ? cached_depth_only_pipeline : cached_pipeline);
		RD::get_singleton()->draw_list_bind_vertex_array(draw_list, empty_vertex_array);
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set, 0);
		RD::get_singleton()->draw_list_bind_index_array(draw_list, synthetic_index_array);
		RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(push_constant));
		RD::get_singleton()->draw_list_set_viewport(draw_list, Rect2(p_viewport));
		RD::get_singleton()->draw_list_draw_indirect_count(draw_list, true, p_draws.command_buffer, 0, p_draws.count_buffer, 0, p_draws.max_draw_count, sizeof(MeshletCuller::IndirectCommand));

		RD::get_singleton()->free_rid(uniform_set);
	}

	RD::get_singleton()->draw_list_end();
}
