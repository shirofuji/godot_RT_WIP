/**************************************************************************/
/*  hiz_builder.h                                                        */
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

#include "core/math/rect2i.h"
#include "core/object/ref_counted.h"
#include "core/templates/rid.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_hiz_downsample.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderSceneBuffersRD;

// Named-texture scope/name for the meshlet system's own Hi-Z buffers, living in
// RenderSceneBuffersRD (the same per-viewport frame-persistent-resource mechanism SSR's own
// Hi-Z uses - see servers/rendering/renderer_rd/effects/ss_effects.h's RB_SCOPE_SSR/RB_HIZ).
// Two distinct names (not "curr"/"prev") since the caller alternates which one is "this frame's
// write target" vs "last frame's read source" every frame - see RenderForwardClustered's
// meshlet_hiz_a_is_current.
#define RB_SCOPE_MESHLET_HIZ SNAME("meshlet_hiz")
#define RB_MESHLET_HIZ_A SNAME("hiz_a")
#define RB_MESHLET_HIZ_B SNAME("hiz_b")

namespace RendererRD {

// Builds a max-reduced hierarchical depth (Hi-Z) mip chain from a source depth texture, for use
// by MeshletCuller's occlusion test. Standalone and independent of SSEffects's own SSR Hi-Z
// buffer (servers/rendering/renderer_rd/effects/ss_effects.cpp) - that one is tied to SSR's
// buffer lifecycle/feature toggle and built from the final resolved depth; this one needs to run
// from an occluder depth pre-pass at a different point in the frame, and (for temporal two-pass
// occlusion) needs to persist across frames, which a single per-instance texture member can't do.
//
// HiZBuilder itself is now stateless w.r.t. the actual Hi-Z texture - it only owns the
// downsample shader/pipeline/sampler (cheap, fine as a singleton). The destination texture lives
// in the caller-supplied RenderSceneBuffersRD under RB_SCOPE_MESHLET_HIZ, via the same
// create_texture()/get_texture_slice() named-texture mechanism SSR's RB_HIZ uses - this also
// means resize invalidation is automatic (RenderSceneBuffersRD::configure() unconditionally wipes
// all named textures on reconfigure/resize, the same lifecycle every other RB_SCOPE_* buffer
// relies on - no manual size-tracking needed here).
class HiZBuilder {
public:
	struct HiZResult {
		RID texture; // R32_SFLOAT, mip 0 = max-reduced half-resolution of the source.
		uint32_t mip_count = 0;
		Size2i size; // Size of mip 0 (half the source's size, rounded up).

		bool is_valid() const { return texture.is_valid(); }
	};

private:
	static HiZBuilder *singleton;

	MeshletHizDownsampleShaderRD downsample_shader;
	RID downsample_shader_version;
	RID downsample_shader_rid;
	RID downsample_pipeline;

	RID sampler;

public:
	static HiZBuilder *get_singleton();

	// p_dest_name: one of RB_MESHLET_HIZ_A/RB_MESHLET_HIZ_B (or any other StringName, but those
	// two are what RenderForwardClustered's curr/prev swap expects). Builds (or rebuilds) the
	// full mip chain into p_render_buffers's named texture every call - the named-texture cache
	// makes repeated calls with the same (context, name, format) cheap (returns the cached RID
	// immediately), but the mip *contents* are always freshly computed from p_source_depth_texture
	// since this has no "is the source unchanged" detection of its own.
	HiZResult build_into(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_dest_name, RID p_source_depth_texture, const Size2i &p_source_size);

	HiZBuilder();
	~HiZBuilder();
};

} // namespace RendererRD
