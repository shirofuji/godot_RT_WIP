/**************************************************************************/
/*  meshlet_culler.h                                                     */
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

#include "core/math/plane.h"
#include "core/math/projection.h"
#include "core/math/rect2i.h"
#include "core/math/transform_3d.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_cull.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_cull_expand.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_emit_draws.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_occlusion_test.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Two-pass GPU-driven meshlet frustum culling: walks meshlets reachable from a set of visible
// instances, rejects backfacing ones via their normal cone, frustum-tests the survivors'
// world-space bounding spheres, and appends survivors to a compact list via an atomic counter.
//
// This is deliberately generic and self-contained (no dependency on Forward+'s actual per-frame
// instance list yet) - mirrors how MeshletStorage was built and proven standalone before any
// engine wiring. Hooking real Forward+ instance/transform data into this is later work.
class MeshletCuller {
public:
	// CPU-built input to the expand pass: one entry per visible instance-surface.
	struct InstanceMeshletRange {
		uint32_t instance_index = 0; // Index into the caller-provided transforms buffer.
		uint32_t meshlet_offset = 0; // Into MeshletStorage's global meshlet descriptor buffer.
		uint32_t meshlet_count = 0;
		uint32_t _pad = 0;
	};

	struct VisibleMeshlet {
		uint32_t instance_index = 0;
		uint32_t meshlet_index = 0; // Global index into MeshletStorage's descriptor buffer.
	};

	struct CullResult {
		RID visible_buffer; // SSBO: { uint count; VisibleMeshlet data[]; }, sized max_visible.
		uint32_t max_visible = 0;

		bool is_valid() const { return visible_buffer.is_valid(); }
	};

	// Mirrors VkDrawIndexedIndirectCommand's layout exactly (20 bytes).
	struct IndirectCommand {
		uint32_t index_count = 0;
		uint32_t instance_count = 0;
		uint32_t first_index = 0;
		int32_t vertex_offset = 0;
		uint32_t first_instance = 0;
	};

	struct IndirectDrawResult {
		RID command_buffer; // SSBO of IndirectCommand, sized max_draws.
		uint32_t draw_count = 0; // CPU-known actual count - pass directly as draw_list_draw_indirect's p_draw_count.

		bool is_valid() const { return command_buffer.is_valid(); }
	};

private:
	static MeshletCuller *singleton;

	MeshletCullExpandShaderRD expand_shader;
	RID expand_shader_version;
	RID expand_shader_rid;
	RID expand_pipeline;

	MeshletCullShaderRD cull_shader;
	RID cull_shader_version;
	RID cull_shader_rid;
	RID cull_pipeline;

	MeshletOcclusionTestShaderRD occlusion_shader;
	RID occlusion_shader_version;
	RID occlusion_shader_rid;
	RID occlusion_pipeline;

	MeshletEmitDrawsShaderRD emit_draws_shader;
	RID emit_draws_shader_version;
	RID emit_draws_shader_rid;
	RID emit_draws_pipeline;

	RID hiz_sampler;

	// Transient scratch buffers: recreated (not grown-with-copy) whenever too small, since their
	// contents don't need to persist across calls - unlike MeshletStorage's persistent allocator.
	RID ranges_buffer;
	RID work_items_buffer;
	uint32_t work_items_capacity = 0;
	RID visible_buffer;
	uint32_t visible_capacity = 0;
	RID occluded_buffer; // occlude()'s own output buffer - distinct from visible_buffer, since
			// occlude() reads a CullResult (often visible_buffer itself) as input.
	uint32_t occluded_capacity = 0;
	RID command_buffer;
	uint32_t command_capacity = 0;

	void _ensure_work_items_capacity(uint32_t p_capacity);
	void _ensure_visible_capacity(uint32_t p_capacity);
	void _ensure_occluded_capacity(uint32_t p_capacity);
	void _ensure_command_capacity(uint32_t p_capacity);

public:
	static MeshletCuller *get_singleton();

	// p_transforms_buffer: SSBO of mat4, one per instance, indexed by InstanceMeshletRange::instance_index.
	// p_frustum_planes: exactly 6 planes (e.g. from Projection::get_projection_planes()).
	CullResult cull(RID p_transforms_buffer, const Vector<InstanceMeshletRange> &p_ranges, const Vector<Plane> &p_frustum_planes, const Vector3 &p_camera_position, uint32_t p_max_work_items = 1 << 16, uint32_t p_max_visible = 1 << 16);

	// Tests p_frustum_result's survivors (e.g. from cull() above) against a Hi-Z depth pyramid
	// (see HiZBuilder) and returns a new, final CullResult. p_camera_transform/p_projection
	// describe the same camera the Hi-Z texture's source depth was rendered with.
	CullResult occlude(RID p_transforms_buffer, const CullResult &p_frustum_result, RID p_hiz_texture, uint32_t p_hiz_mip_count, const Transform3D &p_camera_transform, const Projection &p_projection, const Size2i &p_screen_size, uint32_t p_max_visible = 1 << 16);

	// Converts p_result's survivors into an indirect draw command buffer, one command per
	// surviving meshlet, with each command's index_count set to that specific meshlet's actual
	// triangle_count * 3 (looked up from MeshletStorage's descriptor buffer - meshlets rarely hit
	// the 124-triangle cap, so a fixed global index count would process out-of-range/degenerate
	// triangles for most draws) and firstInstance set to that meshlet's slot in p_result's
	// visible-list buffer (instanceCount=1, so the vertex shader's gl_InstanceIndex equals
	// firstInstance for every vertex of that draw - the standard vertex-pulling trick). Reads
	// p_result's count back on the CPU (a stall, same correctness-first tradeoff as the Pass A->B
	// and B->C handoffs) so the result's draw_count is usable directly as
	// draw_list_draw_indirect's p_draw_count.
	IndirectDrawResult emit_indirect_draws(const CullResult &p_result, uint32_t p_max_draws = 1 << 16);

	// Read-back helper (stalls the GPU - test/debug use only, never call this per-frame).
	Vector<VisibleMeshlet> debug_read_visible(const CullResult &p_result);
	Vector<IndirectCommand> debug_read_commands(const IndirectDrawResult &p_result);

	MeshletCuller();
	~MeshletCuller();
};

} // namespace RendererRD
