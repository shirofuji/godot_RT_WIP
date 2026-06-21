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
#include "servers/rendering/renderer_rd/shaders/meshlet_hiz_downsample.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Builds a max-reduced hierarchical depth (Hi-Z) mip chain from a source depth texture, for use
// by MeshletCuller's occlusion test. Standalone and independent of SSEffects's own SSR Hi-Z
// buffer (servers/rendering/renderer_rd/effects/ss_effects.cpp) - that one is tied to SSR's
// buffer lifecycle/feature toggle and built from the final resolved depth; this one needs to run
// from an occluder depth pre-pass at a different point in the frame (Phase 5 wires that up).
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

	RID hiz_texture;
	Size2i hiz_mip0_size;
	uint32_t hiz_mip_count = 0;

	void _ensure_texture(const Size2i &p_mip0_size, uint32_t p_mip_count);

public:
	static HiZBuilder *get_singleton();

	HiZResult build(RID p_source_depth_texture, const Size2i &p_source_size);

	HiZBuilder();
	~HiZBuilder();
};

} // namespace RendererRD
