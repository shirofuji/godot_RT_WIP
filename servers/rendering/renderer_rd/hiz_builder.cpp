/**************************************************************************/
/*  hiz_builder.cpp                                                      */
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

#include "hiz_builder.h"

using namespace RendererRD;

HiZBuilder *HiZBuilder::singleton = nullptr;

HiZBuilder *HiZBuilder::get_singleton() {
	return singleton;
}

HiZBuilder::HiZBuilder() {
	singleton = this;

	Vector<String> versions;
	versions.push_back("");
	downsample_shader.initialize(versions);
	downsample_shader_version = downsample_shader.version_create();
	downsample_shader_rid = downsample_shader.version_get_shader(downsample_shader_version, 0);
	downsample_pipeline = RD::get_singleton()->compute_pipeline_create(downsample_shader_rid);

	sampler = RD::get_singleton()->sampler_create(RD::SamplerState());
}

HiZBuilder::~HiZBuilder() {
	if (hiz_texture.is_valid()) {
		RD::get_singleton()->free_rid(hiz_texture);
	}
	RD::get_singleton()->free_rid(sampler);
	downsample_shader.version_free(downsample_shader_version);
	singleton = nullptr;
}

void HiZBuilder::_ensure_texture(const Size2i &p_mip0_size, uint32_t p_mip_count) {
	if (hiz_texture.is_valid() && hiz_mip0_size == p_mip0_size && hiz_mip_count == p_mip_count) {
		return;
	}
	if (hiz_texture.is_valid()) {
		RD::get_singleton()->free_rid(hiz_texture);
	}

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	tf.width = MAX(1, p_mip0_size.width);
	tf.height = MAX(1, p_mip0_size.height);
	tf.mipmaps = p_mip_count;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	hiz_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
	hiz_mip0_size = p_mip0_size;
	hiz_mip_count = p_mip_count;

	// Resolve the texture's lazy "pending clear" flag right away, on the texture as a whole,
	// before any per-mip texture_create_shared_from_slice() calls. Each slice snapshots a copy of
	// the parent's state at creation time (RenderingDevice::texture_create_shared_from_slice does
	// `Texture texture = *src_texture;`); if every slice for every mip is created later while the
	// parent is still pending-clear, each slice independently re-triggers the lazy clear the first
	// time it's bound - including mips that were already correctly written by an earlier dispatch
	// in this same build(), silently clobbering them back to the clear value. Clearing here once,
	// before any slice exists, makes every later slice correctly inherit pending_clear=false.
	// Clear value 0.0 = "far" under Godot RD's reversed-Z convention (near=1.0, far=0.0) - this is
	// only a defensive default for the brief window before the first real downsample dispatch
	// writes every mip in full; it doesn't affect steady-state correctness.
	RD::get_singleton()->texture_clear(hiz_texture, Color(0, 0, 0, 0), 0, p_mip_count, 0, 1);
}

HiZBuilder::HiZResult HiZBuilder::build(RID p_source_depth_texture, const Size2i &p_source_size) {
	HiZResult result;

	ERR_FAIL_COND_V(p_source_size.width <= 0 || p_source_size.height <= 0, result);

	Size2i mip0_size((p_source_size.width + 1) / 2, (p_source_size.height + 1) / 2);
	mip0_size.width = MAX(1, mip0_size.width);
	mip0_size.height = MAX(1, mip0_size.height);

	uint32_t mip_count = 1;
	Size2i cur = mip0_size;
	while (cur.width > 1 && cur.height > 1) {
		cur.width = MAX(1, cur.width / 2);
		cur.height = MAX(1, cur.height / 2);
		mip_count++;
	}

	_ensure_texture(mip0_size, mip_count);

	RD::get_singleton()->draw_command_begin_label("Meshlet Hi-Z build");

	Size2i dest_size = mip0_size;
	RID source = p_source_depth_texture;
	Vector<RID> mip_slices_to_free;
	Vector<RID> uniform_sets_to_free;

	struct PushConstant {
		int32_t dest_size[2];
		int32_t pad[2];
	};
	Vector<PushConstant> push_constants;
	Vector<Size2i> dispatch_sizes;

	// Precompute every mip's slice views and uniform set before opening the compute list -
	// uniform_set_create() returns an invalid RID if called while a compute list is active.
	for (uint32_t m = 0; m < mip_count; m++) {
		RID dest = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), hiz_texture, 0, m, 1);
		mip_slices_to_free.push_back(dest);

		PushConstant push_constant;
		push_constant.dest_size[0] = dest_size.width;
		push_constant.dest_size[1] = dest_size.height;
		push_constant.pad[0] = 0;
		push_constant.pad[1] = 0;
		push_constants.push_back(push_constant);
		dispatch_sizes.push_back(dest_size);

		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>{ sampler, source });
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 1, dest);
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(u_source);
		uniforms.push_back(u_dest);
		uniform_sets_to_free.push_back(RD::get_singleton()->uniform_set_create(uniforms, downsample_shader_rid, 0));

		source = dest;
		dest_size.width = MAX(1, dest_size.width / 2);
		dest_size.height = MAX(1, dest_size.height / 2);
	}

	// One compute list for the whole chain, with an explicit barrier between each mip's dispatch:
	// each mip's dispatch reads the previous mip's write, and separate compute_list_begin/end
	// pairs do not guarantee that write is visible yet (confirmed by this self-test catching mip
	// 2+ silently reading stale/default data without a barrier here).
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	for (uint32_t m = 0; m < mip_count; m++) {
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, downsample_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_sets_to_free[m], 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constants[m], sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, dispatch_sizes[m].width, dispatch_sizes[m].height, 1);

		if (m + 1 < mip_count) {
			RD::get_singleton()->compute_list_add_barrier(compute_list);
		}
	}

	RD::get_singleton()->compute_list_end();

	for (int i = 0; i < uniform_sets_to_free.size(); i++) {
		RD::get_singleton()->free_rid(uniform_sets_to_free[i]);
	}
	for (int i = 0; i < mip_slices_to_free.size(); i++) {
		RD::get_singleton()->free_rid(mip_slices_to_free[i]);
	}

	RD::get_singleton()->draw_command_end_label();

	result.texture = hiz_texture;
	result.mip_count = mip_count;
	result.size = mip0_size;
	return result;
}
