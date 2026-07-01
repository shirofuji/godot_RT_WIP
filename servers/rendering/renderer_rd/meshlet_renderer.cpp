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
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/virtual_texture_storage.h"

using namespace RendererRD;

MeshletRenderer *MeshletRenderer::singleton = nullptr;

MeshletRenderer *MeshletRenderer::get_singleton() {
	return singleton;
}

MeshletRenderer::MeshletRenderer() {
	singleton = this;

	Vector<String> versions;
	versions.push_back(""); // Version 0: normal (direct material_textures[] sampling + depth).
	versions.push_back("\n#define MESHLET_DEPTH_ONLY\n"); // Version 1: no fragment color output.
	// Version 2: VT-sampled color. Inject VT_POOL_TILES_X to match VirtualTextureStorage's runtime pool
	// allocation (from the rendering/virtual_texture/pool_size_mb setting) so the shader's virtual->
	// physical UV mapping uses the correct pool geometry.
	versions.push_back(vformat("\n#define MESHLET_USE_VIRTUAL_TEXTURES\n#define VT_POOL_TILES_X %d\n", (int)VirtualTextureStorage::get_pool_tiles_dim()));
	render_shader.initialize(versions);
	render_shader_version = render_shader.version_create();
	render_shader_rid = render_shader.version_get_shader(render_shader_version, 0);
	depth_only_shader_rid = render_shader.version_get_shader(render_shader_version, 1);
	vt_shader_rid = render_shader.version_get_shader(render_shader_version, 2);

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

	// Sampler for the sky radiance octmap ambient (binding 12). Linear with mipmaps so the LOD-based
	// layer sample is filtered; clamp so the center-texel fetch never wraps.
	RD::SamplerState radiance_ss;
	radiance_ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	radiance_ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
	radiance_ss.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	radiance_ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	radiance_ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	radiance_ss.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	radiance_sampler = RD::get_singleton()->sampler_create(radiance_ss);

	// Material-texture sampler (binding 14): linear with mipmaps, repeat for tiling, anisotropic.
	RD::SamplerState material_ss;
	material_ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	material_ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
	material_ss.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	material_ss.repeat_u = RD::SAMPLER_REPEAT_MODE_REPEAT;
	material_ss.repeat_v = RD::SAMPLER_REPEAT_MODE_REPEAT;
	material_ss.repeat_w = RD::SAMPLER_REPEAT_MODE_REPEAT;
	material_ss.use_anisotropy = true;
	material_ss.anisotropy_max = 4.0f;
	material_sampler = RD::get_singleton()->sampler_create(material_ss);

	// VT page-pool sampler (binding 14 of the VT variant): linear + CLAMP, no mip filtering (the pool
	// is single-mip; sampleVirtual() blends VT mips by sampling two pages itself). Clamp because the
	// pool is an atlas of unrelated pages - repeat would wrap a sample across tile boundaries; the
	// per-page replicated borders are what make intra-page bilinear filtering seamless instead.
	RD::SamplerState pool_ss;
	pool_ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	pool_ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
	pool_ss.mip_filter = RD::SAMPLER_FILTER_NEAREST;
	pool_ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	pool_ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	pool_ss.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	pool_sampler = RD::get_singleton()->sampler_create(pool_ss);

	// VT indirection sampler (binding 16): nearest + clamp. Only ever read via texelFetch (which
	// ignores filtering), but a usampler2DArray binding still needs a sampler RID.
	RD::SamplerState ind_ss;
	ind_ss.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	ind_ss.min_filter = RD::SAMPLER_FILTER_NEAREST;
	ind_ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	ind_ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	ind_ss.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	vt_indirection_sampler = RD::get_singleton()->sampler_create(ind_ss);
}

MeshletRenderer::~MeshletRenderer() {
	if (cached_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(cached_pipeline);
	}
	if (cached_depth_only_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(cached_depth_only_pipeline);
	}
	if (cached_vt_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(cached_vt_pipeline);
	}
	RD::get_singleton()->free_rid(synthetic_index_buffer);
	RD::get_singleton()->free_rid(empty_vertex_array);
	RD::get_singleton()->free_rid(radiance_sampler);
	RD::get_singleton()->free_rid(material_sampler);
	RD::get_singleton()->free_rid(pool_sampler);
	RD::get_singleton()->free_rid(vt_indirection_sampler);
	// vertex_format is a VertexFormatID (cached/interned by RD, not an owned RID) - no free needed.
	render_shader.version_free(render_shader_version);
	singleton = nullptr;
}

void MeshletRenderer::_ensure_pipeline(RD::FramebufferFormatID p_framebuffer_format, bool p_depth_only, bool p_virtual_textures) {
	// Three independently-cached pipelines: depth-only (temporal early pass), VT color (virtual-
	// texture sampling variant), and normal color (direct material_textures[] sampling). VT only
	// applies to the color path, so p_depth_only takes precedence over p_virtual_textures.
	RID *cached_ptr;
	RD::FramebufferFormatID *cached_format_ptr;
	RID variant_shader_rid;
	if (p_depth_only) {
		cached_ptr = &cached_depth_only_pipeline;
		cached_format_ptr = &cached_depth_only_framebuffer_format;
		variant_shader_rid = depth_only_shader_rid;
	} else if (p_virtual_textures) {
		cached_ptr = &cached_vt_pipeline;
		cached_format_ptr = &cached_vt_framebuffer_format;
		variant_shader_rid = vt_shader_rid;
	} else {
		cached_ptr = &cached_pipeline;
		cached_format_ptr = &cached_framebuffer_format;
		variant_shader_rid = render_shader_rid;
	}
	RID &cached = *cached_ptr;
	RD::FramebufferFormatID &cached_format = *cached_format_ptr;

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

	// Backface culling: POLYGON_CULL_FRONT. History: originally disabled outright because meshlet
	// triangle winding isn't reliably consistent across all meshlets from this pipeline
	// (SurfaceTool::build_meshlets/meshoptimizer doesn't guarantee uniform winding when
	// repartitioning a source index buffer into clusters) - CULL_BACK/CULL_FRONT both reproduced
	// the same "holes" bug for that reason at the time. A later fix switched to this
	// (POLYGON_CULL_FRONT), which empirically eliminated the holes for the scenes tested.
	//
	// A real, separate bug surfaced once a *negative-scale* (mirrored) instance was tested:
	// mirroring reverses every triangle's effective winding for that one instance, so this single,
	// pipeline-wide cull state - correct for the rest of the scene - becomes backwards specifically
	// for that instance, producing visibly broken/jagged geometry. POLYGON_CULL_DISABLED (rendering
	// both winding orders, letting depth testing alone decide) was tried as a fix - this pipeline
	// batches many instances with potentially different transforms into one indirect multi-draw, so
	// there's no way to pick a different static cull mode per-instance within a single draw call,
	// and disabling culling entirely seemed like the only option correct regardless of per-instance
	// handedness. That was reverted: rendering both front and back triangles of the same surface
	// caused severe, widespread Z-fighting between them (confirmed even with the depth bias below
	// removed entirely, ruling out bias tuning as a fix) - strictly worse than the negative-scale
	// bug it was meant to solve, since it broke every instance, not just mirrored ones.
	//
	// Excluding negative-scale instances from the meshlet scan entirely (see
	// RenderForwardClustered::_meshlet_scan_render_list's comment) was also tried and reverted -
	// it surfaced a second, not-fully-understood bug where the excluded instance's own separate
	// Forward+ draw would silently fail to write color whenever another qualifying instance
	// engaged the late meshlet pass that frame. Net result: negative-scale instances currently
	// still render with the known, localized jagged-geometry bug described above - a real,
	// pre-existing limitation, not a regression from this investigation. The original inconsistent-
	// meshlet-winding problem this cull mode works around is still worth fixing at the source
	// eventually (in SurfaceTool::build_meshlets or the meshoptimizer call site, to use Godot's
	// standard CCW-front convention) - that would remove the need for this cull-mode workaround
	// and its negative-scale blind spot entirely - but that's a separate, larger undertaking.
	RD::PipelineRasterizationState rs;
	// CULL_BACK, not CULL_FRONT. The history above blamed "inconsistent meshlet winding," but
	// meshopt_buildMeshlets only PARTITIONS the source index buffer - it never reorders a triangle's
	// three vertices - so meshlet winding equals the source mesh's, which for Godot meshes is standard
	// CCW-front. Single-mesh primitives (BoxMesh/SphereMesh, StandardMaterial3D) rendered inside-out
	// under CULL_FRONT - exactly what a backwards cull mode does to correctly-wound geometry - and
	// switching to CULL_BACK fixed them with nothing else inverting (verified in a real scene). The
	// old "holes" the comment blamed on winding were most likely the since-removed local-Z normal flip.
	rs.cull_mode = RD::POLYGON_CULL_BACK;

	// Depth bias is handled manually in the vertex shader (see meshlet_render.glsl), not via
	// RD::PipelineRasterizationState's depth_bias_* fields - those scale by Vulkan's own per-
	// fragment minimum-resolvable-difference for the depth format (designed for sub-ULP anti-
	// z-fighting noise), which turned out far too small to move the needle against the systematic
	// ~0.1%+ gap measured here (a constant_factor of 4-100 made no visible difference) - this
	// isn't floating-point noise, it's a consistent offset between two different vertex
	// computation paths producing the device-Z for the same logical surface point (this pipeline's
	// vertex-pulling vs. Forward+'s own real vertex shader, both targeting the same mesh). A
	// direct, fixed-fraction nudge in the shader gives predictable, measured control instead.

	RID shader_rid = variant_shader_rid;
	// create_disabled(0): the depth-only variant targets a framebuffer with zero color
	// attachments (e.g. Forward+'s real depth pre-pass framebuffer) - the color blend state's
	// attachment count must match the framebuffer's actual color attachment count.
	RD::PipelineColorBlendState blend_state = p_depth_only ? RD::PipelineColorBlendState::create_disabled(0) : RD::PipelineColorBlendState::create_disabled();

	cached = RD::get_singleton()->render_pipeline_create(shader_rid, p_framebuffer_format, vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, rs, RD::PipelineMultisampleState(), ds, blend_state);
	cached_format = p_framebuffer_format;
}

void MeshletRenderer::render(const MeshletCuller::CullResult &p_visible, const MeshletCuller::IndirectDrawResult &p_draws, RID p_transforms_buffer, RID p_material_ids_buffer, RID p_framebuffer, RD::FramebufferFormatID p_framebuffer_format, const Rect2i &p_viewport, const Projection &p_projection, const Transform3D &p_camera_transform, RID p_lights_buffer, uint32_t p_light_count, bool p_clear, bool p_depth_only, const Color &p_ambient_color, RID p_svogi_octree_buffer, const Vector3 &p_svogi_bounds_center, float p_svogi_bounds_half_size, float p_svogi_energy, RID p_radiance_texture, float p_sky_ambient_mix, float p_radiance_exposure, float p_max_roughness_lod) {
	ERR_FAIL_NULL(MeshletStorage::get_singleton());

	// VT sampling applies only to the color path (depth-only samples no material textures). The
	// kill-switch lives in VirtualTextureStorage::is_enabled() (resolves --vt-enable/--vt-disable over
	// the project setting); when off, the normal version-0 shader + direct material_textures[] binding
	// run, byte-identical to a build without VT.
	const bool use_virtual_textures = !p_depth_only && VirtualTextureStorage::is_enabled() && VirtualTextureStorage::get_singleton() != nullptr;

	_ensure_pipeline(p_framebuffer_format, p_depth_only, use_virtual_textures);

	// S0b: the VT shader variant writes per-screen-tile page feedback to binding 18. Fetch + clear it
	// before the draw list opens (texture_update can't run inside a draw list). The caller drains it
	// (VirtualTextureStorage::read_feedback_requests) when it wants the page set; here we just keep it
	// bound and freshly cleared each pass.
	RID vt_feedback_image;
	if (use_virtual_textures) {
		VirtualTextureStorage *vts = VirtualTextureStorage::get_singleton();
		vt_feedback_image = vts->get_feedback_image(p_viewport.size);
		vts->clear_feedback();
	}

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
		if (!p_depth_only) {
			// Binding 9 (Lights) - same depth-only exclusion reasoning as binding 8. Caller
			// guarantees p_lights_buffer is a valid RID whenever p_depth_only is false (see
			// RenderForwardClustered::meshlet_lights_buffer_rid(), which always allocates at least
			// one element's worth of space, even with zero real lights).
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 9;
			u.append_id(p_lights_buffer);
			uniforms.push_back(u);
		}
		if (!p_depth_only) {
			// Binding 10 (SVOGINodes) - same depth-only exclusion reasoning as 8/9. The descriptor
			// set layout always declares this binding for the non-depth-only shader, so it must
			// always be provided; when SVOGI is off / has no octree this frame (octree RID invalid)
			// a harmless already-valid storage buffer stands in - the fragment shader never indexes
			// it in that case (svogi_bounds.w == 0 short-circuits the trace).
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 10;
			u.append_id(p_svogi_octree_buffer.is_valid() ? p_svogi_octree_buffer : storage->get_meshlet_material_buffer_rid());
			uniforms.push_back(u);
		}
		if (!p_depth_only) {
			// Bindings 11/12 (sky radiance octmap + its sampler) - same depth-only exclusion reasoning
			// as 8/9/10; the non-depth-only shader layout always declares them, so a valid texture must
			// always be bound. When the caller has no sky this frame (p_radiance_texture invalid) a
			// 1x1 black texture2DArray stands in - p_sky_ambient_mix is 0 in that case, so the fragment
			// shader's `if (ambient_color.a > 0.0)` never samples it.
			RID radiance = p_radiance_texture;
			if (radiance.is_null()) {
				radiance = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
			}
			RD::Uniform ut;
			ut.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			ut.binding = 11;
			ut.append_id(radiance);
			uniforms.push_back(ut);

			RD::Uniform us;
			us.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			us.binding = 12;
			us.append_id(radiance_sampler);
			uniforms.push_back(us);
		}
		if (!p_depth_only && use_virtual_textures) {
			// VT color variant (version 2): bindings 13-17 are the virtual-texturing resources -
			// page pool (13) + its sampler (14), indirection page-table array (15) + its sampler (16),
			// and the per-VT metadata SSBO (17) - replacing the direct material_textures[] array. The
			// material's *_texture_index fields are vt_ids here (MeshletStorage routed them through
			// VirtualTextureStorage::register_virtual_texture); sampleVirtual() resolves them.
			VirtualTextureStorage *vts = VirtualTextureStorage::get_singleton();
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 13, vts->get_page_pool_texture_rid()));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 14, pool_sampler));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 15, vts->get_indirection_texture_rid()));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 16, vt_indirection_sampler));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, vts->get_vt_metadata_buffer_rid()));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, vt_feedback_image)); // S0b: page-request feedback.
		} else if (!p_depth_only) {
			// Binding 13/14: the material-texture descriptor array + its sampler. The fragment shader
			// indexes material_textures[mat.*_texture_index]. The array is a fixed size
			// (MeshletStorage::MAX_MATERIAL_TEXTURES) - Vulkan requires every declared element to be a
			// valid binding - so pad the live table with a default white texture beyond what's used.
			const Vector<RID> &tex_table = storage->get_material_texture_rids();
			RID default_white = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
			RD::Uniform utex;
			utex.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			utex.binding = 13;
			for (uint32_t i = 0; i < MeshletStorage::MAX_MATERIAL_TEXTURES; i++) {
				utex.append_id(i < (uint32_t)tex_table.size() ? tex_table[i] : default_white);
			}
			uniforms.push_back(utex);

			RD::Uniform usmat;
			usmat.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			usmat.binding = 14;
			usmat.append_id(material_sampler);
			uniforms.push_back(usmat);
		}
		RID render_shader_rid_to_use = p_depth_only ? depth_only_shader_rid : (use_virtual_textures ? vt_shader_rid : render_shader_rid);
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, render_shader_rid_to_use, 0);

		// Matches meshlet_render.glsl's Params push-constant block exactly (128 bytes). The same
		// block is declared (outside any MESHLET_DEPTH_ONLY guard) by both the color and depth-only
		// vertex shaders, so this single struct/size is correct for both pipelines; the depth-only
		// path just leaves the color/ambient/svogi fields zeroed.
		struct RenderPushConstant {
			float view_projection[16];
			float camera_position[3];
			uint32_t light_count;
			float ambient_color[4]; // .rgb = pre-multiplied color*energy (linear), .a = reserved
			float svogi_bounds[4]; // xyz = octree root center (absolute world), w = root half-size (0 = SVOGI off)
			float svogi_params[4]; // x = energy, yzw reserved
		};
		RenderPushConstant push_constant;
		// Camera-relative view-projection: the inverse camera with its translation stripped (basis
		// only). The shader applies this to (world_pos - camera_position) rather than to the absolute
		// world_pos, which is mathematically identical (basis_inv * (world - cam) == full_view * world)
		// but keeps every operand small-magnitude, avoiding the float32 catastrophic cancellation the
		// absolute path suffered for geometry far from the world origin - the root cause of moving/
		// distant meshlet geometry flickering against Forward+'s precise depth pre-pass (see
		// meshlet_render.glsl's gl_Position comment for the full derivation).
		Transform3D camera_rotation_only = p_camera_transform;
		camera_rotation_only.origin = Vector3();
		Projection vp = p_projection * Projection(camera_rotation_only.affine_inverse());
		for (int col = 0; col < 4; col++) {
			for (int row = 0; row < 4; row++) {
				push_constant.view_projection[col * 4 + row] = vp.columns[col][row];
			}
		}
		push_constant.camera_position[0] = p_camera_transform.origin.x;
		push_constant.camera_position[1] = p_camera_transform.origin.y;
		push_constant.camera_position[2] = p_camera_transform.origin.z;
		push_constant.light_count = p_depth_only ? 0 : p_light_count;
		push_constant.ambient_color[0] = p_depth_only ? 0.0f : p_ambient_color.r;
		push_constant.ambient_color[1] = p_depth_only ? 0.0f : p_ambient_color.g;
		push_constant.ambient_color[2] = p_depth_only ? 0.0f : p_ambient_color.b;
		// .a = sky-radiance mix amount: > 0 makes the fragment shader blend the sky octmap ambient
		// over the flat ambient_color (see meshlet_render.glsl). Zeroed for depth-only.
		push_constant.ambient_color[3] = p_depth_only ? 0.0f : p_sky_ambient_mix;
		// svogi_bounds.w (root half-size) == 0 signals "SVOGI off / no data" to the shader, which
		// then skips the trace entirely - so zero it whenever depth-only or no octree was supplied.
		bool svogi_active = !p_depth_only && p_svogi_bounds_half_size > 0.0f;
		push_constant.svogi_bounds[0] = svogi_active ? (float)p_svogi_bounds_center.x : 0.0f;
		push_constant.svogi_bounds[1] = svogi_active ? (float)p_svogi_bounds_center.y : 0.0f;
		push_constant.svogi_bounds[2] = svogi_active ? (float)p_svogi_bounds_center.z : 0.0f;
		push_constant.svogi_bounds[3] = svogi_active ? p_svogi_bounds_half_size : 0.0f;
		push_constant.svogi_params[0] = svogi_active ? p_svogi_energy : 0.0f;
		// .y = sky-radiance exposure*energy scale, .z = MAX_ROUGHNESS_LOD layer to sample (see the
		// radiance octmap binding in meshlet_render.glsl). Zeroed for depth-only.
		push_constant.svogi_params[1] = p_depth_only ? 0.0f : p_radiance_exposure;
		push_constant.svogi_params[2] = p_depth_only ? 0.0f : p_max_roughness_lod;
		push_constant.svogi_params[3] = 0.0f;

		RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, p_depth_only ? cached_depth_only_pipeline : (use_virtual_textures ? cached_vt_pipeline : cached_pipeline));
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
