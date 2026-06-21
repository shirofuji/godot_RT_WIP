/**************************************************************************/
/*  meshlet_renderer.h                                                   */
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

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/renderer_rd/meshlet_culler.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_render.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Renders a MeshletCuller::IndirectDrawResult via one indirect multi-draw, using a
// vertex-pulling shader (no per-surface VBO - every attribute is fetched manually from
// MeshletStorage's global buffers, indexed via gl_InstanceIndex/gl_VertexIndex against the
// shared synthetic index buffer). Shading is deliberately a basic per-meshlet debug
// visualization (pseudo-random color per meshlet * simple N.L), not real PBR materials -
// this exists to prove the geometry/culling/indirect-draw pipeline renders correct, properly
// positioned pixels; wiring real materials in is separate, later work.
class MeshletRenderer {
public:
	static constexpr uint32_t MAX_TRIANGLES_PER_MESHLET = 124; // Matches Phase 2's meshlet cap.

private:
	static MeshletRenderer *singleton;

	MeshletRenderShaderRD render_shader;
	RID render_shader_version;
	RID render_shader_rid;

	RD::VertexFormatID vertex_format = 0; // Empty - all attribute fetch is manual (vertex-pulling).
	RID empty_vertex_array; // Zero attributes/buffers, but RD still requires *some* vertex array
			// bound whenever the pipeline's vertex format isn't INVALID_ID - even an
			// intentionally-empty one.
	RID synthetic_index_buffer; // 0, 1, 2, ..., MAX_TRIANGLES_PER_MESHLET*3-1.
	RID synthetic_index_array;

	RD::FramebufferFormatID cached_framebuffer_format = RD::INVALID_FORMAT_ID;
	RID cached_pipeline;

	void _ensure_pipeline(RD::FramebufferFormatID p_framebuffer_format);

public:
	static MeshletRenderer *get_singleton();

	// p_visible must be the exact CullResult that p_draws was generated from
	// (MeshletCuller::emit_indirect_draws(p_visible)) - the vertex shader looks up
	// (instance_index, meshlet_index) from p_visible's buffer via gl_InstanceIndex, while
	// p_draws.command_buffer only supplies the draw parameters themselves. p_framebuffer must
	// have been created with a depth attachment (depth test/write is always enabled) plus one
	// color attachment. p_transforms_buffer: SSBO of mat4, one per instance, indexed the same way
	// as p_visible. p_clear: true clears color+depth first (standalone/offscreen use, e.g. tests);
	// false draws additively on top of whatever's already in p_framebuffer, depth-testing against
	// its existing depth contents - required when compositing into a framebuffer another pass
	// (e.g. Forward+'s real opaque pass) already rendered into this frame.
	void render(const MeshletCuller::CullResult &p_visible, const MeshletCuller::IndirectDrawResult &p_draws, RID p_transforms_buffer, RID p_framebuffer, RD::FramebufferFormatID p_framebuffer_format, const Rect2i &p_viewport, const Projection &p_projection, const Transform3D &p_camera_transform, const Vector3 &p_light_direction, bool p_clear = true);

	MeshletRenderer();
	~MeshletRenderer();
};

} // namespace RendererRD
