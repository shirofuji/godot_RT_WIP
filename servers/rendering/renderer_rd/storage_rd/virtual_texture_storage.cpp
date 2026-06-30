/**************************************************************************/
/*  virtual_texture_storage.cpp                                          */
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

#include "virtual_texture_storage.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"

using namespace RendererRD;

VirtualTextureStorage *VirtualTextureStorage::singleton = nullptr;

VirtualTextureStorage *VirtualTextureStorage::get_singleton() {
	return singleton;
}

VirtualTextureStorage::VirtualTextureStorage() {
	singleton = this;
}

VirtualTextureStorage::~VirtualTextureStorage() {
	if (initialized) {
		RD *rd = RD::get_singleton();
		if (page_pool_texture.is_valid()) {
			rd->free_rid(page_pool_texture);
		}
		if (indirection_texture.is_valid()) {
			rd->free_rid(indirection_texture);
		}
		if (vt_metadata_buffer.is_valid()) {
			rd->free_rid(vt_metadata_buffer);
		}
		if (blit_pipeline.is_valid()) {
			rd->free_rid(blit_pipeline);
		}
		if (blit_source_sampler.is_valid()) {
			rd->free_rid(blit_source_sampler);
		}
		blit_shader.version_free(blit_shader_version);
	}
	singleton = nullptr;
}

bool VirtualTextureStorage::is_enabled() {
	// Resolved once and cached (mirrors render_forward_clustered.cpp's meshlet force_enabled/
	// force_disabled statics). --vt-disable wins over everything (hard off for A/B); --vt-enable
	// forces on regardless of the project setting; otherwise the project setting decides.
	static bool initialized = false;
	static bool enabled = false;
	if (!initialized) {
		const List<String> &args = OS::get_singleton()->get_cmdline_args();
		bool force_on = args.find("--vt-enable") != nullptr;
		bool force_off = args.find("--vt-disable") != nullptr;
		enabled = !force_off && (force_on || bool(GLOBAL_GET("rendering/virtual_texture/enabled")));
		initialized = true;
	}
	return enabled;
}

bool VirtualTextureStorage::_ensure_initialized() {
	if (initialized) {
		return true;
	}
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false; // Headless without a real device (e.g. doctest) - VT is unavailable.
	}

	// Blit compute pipeline.
	Vector<String> versions;
	versions.push_back("");
	blit_shader.initialize(versions);
	blit_shader_version = blit_shader.version_create();
	blit_shader_rid = blit_shader.version_get_shader(blit_shader_version, 0);
	blit_pipeline = rd->compute_pipeline_create(blit_shader_rid);
	blit_source_sampler = rd->sampler_create(RD::SamplerState()); // Nearest/clamp - blit uses texelFetch.

	// Physical page pool: one RGBA8 atlas, sampled by the render shader and written by the blit's
	// imageStore. Single mip (each virtual-texture mip lives in its own pages within this one level).
	{
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		tf.width = POOL_TILES_X * STORED_PAGE_SIZE;
		tf.height = POOL_TILES_Y * STORED_PAGE_SIZE;
		tf.texture_type = RD::TEXTURE_TYPE_2D;
		tf.mipmaps = 1;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		page_pool_texture = rd->texture_create(tf, RD::TextureView());
		ERR_FAIL_COND_V(page_pool_texture.is_null(), false);
		rd->texture_clear(page_pool_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}

	// Indirection (page-table) array: one mipped layer per virtual texture. RGBA8_UINT texel =
	// (tile_x, tile_y, resident, 0). Filled per layer via texture_update at registration.
	{
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8G8B8A8_UINT;
		tf.width = INDIRECTION_DIM;
		tf.height = INDIRECTION_DIM;
		tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		tf.array_layers = MAX_VIRTUAL_TEXTURES;
		tf.mipmaps = INDIRECTION_MIPS;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		indirection_texture = rd->texture_create(tf, RD::TextureView());
		ERR_FAIL_COND_V(indirection_texture.is_null(), false);
	}

	// Per-virtual-texture metadata SSBO (base size + mip count), indexed by vt_id.
	{
		Vector<uint8_t> zero;
		zero.resize_initialized(MAX_VIRTUAL_TEXTURES * sizeof(VTMetadataGPU));
		vt_metadata_buffer = rd->storage_buffer_create(zero.size(), zero);
		ERR_FAIL_COND_V(vt_metadata_buffer.is_null(), false);
	}

	// Free-list of pool tiles (all free initially). Popped from the back; S0a never returns them.
	free_tiles.resize(POOL_TILES_X * POOL_TILES_Y);
	for (uint32_t i = 0; i < free_tiles.size(); i++) {
		free_tiles[i] = (POOL_TILES_X * POOL_TILES_Y - 1) - i; // So index 0 is popped first.
	}

	initialized = true;
	return true;
}

uint32_t VirtualTextureStorage::_allocate_tile() {
	if (free_tiles.is_empty()) {
		return 0xFFFFFFFF;
	}
	uint32_t tile = free_tiles[free_tiles.size() - 1];
	free_tiles.remove_at(free_tiles.size() - 1);
	return tile;
}

void VirtualTextureStorage::_free_tile(uint32_t p_tile_index) {
	free_tiles.push_back(p_tile_index);
}

void VirtualTextureStorage::_blit_source_page_to_tile(const RID &p_source, uint32_t p_mip, uint32_t p_page_x, uint32_t p_page_y, uint32_t p_tile_index) {
	// Note: kept as a discrete helper for clarity, but register_virtual_texture() batches all of a
	// texture's page blits under one uniform set / compute list for efficiency, so this is currently
	// only a reference for the per-page push-constant layout (see the batched loop below). Left
	// unused-but-declared deliberately; S0c's on-demand streaming will call a batched form too.
	(void)p_source;
	(void)p_mip;
	(void)p_page_x;
	(void)p_page_y;
	(void)p_tile_index;
}

void VirtualTextureStorage::_write_indirection(uint32_t p_vt_id, uint32_t p_mip, uint32_t p_page_x, uint32_t p_page_y, uint32_t p_tile_index) {
	// Same note as _blit_source_page_to_tile: indirection is built CPU-side per layer and uploaded in
	// one texture_update by register_virtual_texture(); this per-texel signature is retained for S0c,
	// which will patch single texels as pages stream in/out.
	(void)p_vt_id;
	(void)p_mip;
	(void)p_page_x;
	(void)p_page_y;
	(void)p_tile_index;
}

uint32_t VirtualTextureStorage::register_virtual_texture(const RID &p_source_rd_texture) {
	if (p_source_rd_texture.is_null()) {
		return 0xFFFFFFFF;
	}
	HashMap<RID, uint32_t>::Iterator it = source_rid_to_vt_id.find(p_source_rd_texture);
	if (it != source_rid_to_vt_id.end()) {
		return it->value;
	}
	if (!_ensure_initialized()) {
		return 0xFFFFFFFF;
	}
	if (virtual_textures.size() >= MAX_VIRTUAL_TEXTURES) {
		WARN_PRINT_ONCE("VirtualTextureStorage: virtual-texture table full (MAX_VIRTUAL_TEXTURES); extra textures fall back to scalar PBR factors.");
		return 0xFFFFFFFF;
	}

	RD *rd = RD::get_singleton();
	RD::TextureFormat src_fmt = rd->texture_get_format(p_source_rd_texture);
	uint32_t width = src_fmt.width;
	uint32_t height = src_fmt.height;
	if (width == 0 || height == 0) {
		return 0xFFFFFFFF;
	}
	if (width > MAX_TEXTURE_SIZE || height > MAX_TEXTURE_SIZE) {
		WARN_PRINT_ONCE("VirtualTextureStorage: source texture exceeds MAX_TEXTURE_SIZE; page grid is clamped (sampling may be approximate for oversized textures).");
	}

	// Mips we can represent: the source's own mip count, capped at the indirection chain depth. (The
	// mip tail below INDIRECTION_MIPS is not paged in S0a - the shader clamps minification to the
	// coarsest represented mip; harmless for the screen-filling content S0a is verified against.)
	uint32_t mip_count = MIN(src_fmt.mipmaps, INDIRECTION_MIPS);
	mip_count = MAX(mip_count, 1u);

	uint32_t vt_id = virtual_textures.size();

	VirtualTexture vt;
	vt.source = p_source_rd_texture;
	vt.width = width;
	vt.height = height;
	vt.mip_count = mip_count;
	vt.vt_id = vt_id;

	// Precompute per-mip byte offsets into one layer's packed mip chain (mip0 then mip1 ...).
	uint32_t mip_offsets[INDIRECTION_MIPS];
	uint32_t layer_bytes = 0;
	for (uint32_t m = 0; m < INDIRECTION_MIPS; m++) {
		mip_offsets[m] = layer_bytes;
		uint32_t dim = MAX(INDIRECTION_DIM >> m, 1u);
		layer_bytes += dim * dim * 4; // RGBA8_UINT.
	}
	Vector<uint8_t> indirection_data;
	indirection_data.resize_initialized(layer_bytes); // Zero = not resident.
	uint8_t *ind_ptr = indirection_data.ptrw();

	// One uniform set for all of this texture's page blits (source + pool are constant per texture).
	RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>{ blit_source_sampler, p_source_rd_texture });
	RD::Uniform u_pool(RD::UNIFORM_TYPE_IMAGE, 1, page_pool_texture);
	Vector<RD::Uniform> uniforms;
	uniforms.push_back(u_source);
	uniforms.push_back(u_pool);
	RID blit_set = rd->uniform_set_create(uniforms, blit_shader_rid, 0);

	struct BlitPush {
		int32_t src_page_origin[2];
		int32_t pool_tile_origin[2];
		int32_t src_mip;
		int32_t pad0;
		int32_t pad1;
		int32_t pad2;
	};

	rd->draw_command_begin_label("VT register (resident blit)");
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, blit_pipeline);
	rd->compute_list_bind_uniform_set(cl, blit_set, 0);

	bool out_of_tiles = false;
	for (uint32_t m = 0; m < mip_count && !out_of_tiles; m++) {
		uint32_t mip_w = MAX(width >> m, 1u);
		uint32_t mip_h = MAX(height >> m, 1u);
		uint32_t ind_dim = MAX(INDIRECTION_DIM >> m, 1u);
		uint32_t pages_x = MIN((mip_w + PAGE_SIZE - 1) / PAGE_SIZE, ind_dim);
		uint32_t pages_y = MIN((mip_h + PAGE_SIZE - 1) / PAGE_SIZE, ind_dim);

		for (uint32_t py = 0; py < pages_y && !out_of_tiles; py++) {
			for (uint32_t px = 0; px < pages_x; px++) {
				uint32_t tile = _allocate_tile();
				if (tile == 0xFFFFFFFF) {
					WARN_PRINT_ONCE("VirtualTextureStorage: page pool full while making a texture resident (S0a holds everything resident - this is expected at scene scale and is what S0c's eviction fixes). Remaining pages fall back to not-resident.");
					out_of_tiles = true;
					break;
				}
				vt.resident_tiles.push_back(tile);

				uint32_t tile_x = tile % POOL_TILES_X;
				uint32_t tile_y = tile / POOL_TILES_X;

				BlitPush push;
				push.src_page_origin[0] = int32_t(px * PAGE_SIZE);
				push.src_page_origin[1] = int32_t(py * PAGE_SIZE);
				push.pool_tile_origin[0] = int32_t(tile_x * STORED_PAGE_SIZE);
				push.pool_tile_origin[1] = int32_t(tile_y * STORED_PAGE_SIZE);
				push.src_mip = int32_t(m);
				push.pad0 = push.pad1 = push.pad2 = 0;
				rd->compute_list_set_push_constant(cl, &push, sizeof(BlitPush));
				rd->compute_list_dispatch_threads(cl, STORED_PAGE_SIZE, STORED_PAGE_SIZE, 1);

				// Indirection texel for this page (mip m, page px,py) -> this tile, resident.
				uint8_t *texel = ind_ptr + mip_offsets[m] + (py * ind_dim + px) * 4;
				texel[0] = uint8_t(tile_x);
				texel[1] = uint8_t(tile_y);
				texel[2] = 1; // Resident.
				texel[3] = 0;
			}
		}
	}

	rd->compute_list_end();
	rd->draw_command_end_label();
	rd->free_rid(blit_set);

	// Upload this texture's full page table into its indirection layer, and its metadata.
	rd->texture_update(indirection_texture, vt_id, indirection_data);

	VTMetadataGPU meta;
	meta.width = width;
	meta.height = height;
	meta.mip_count = mip_count;
	meta.resident_mip_floor = 0; // S0a: everything resident.
	rd->buffer_update(vt_metadata_buffer, vt_id * sizeof(VTMetadataGPU), sizeof(VTMetadataGPU), &meta);

	virtual_textures.push_back(vt);
	source_rid_to_vt_id[p_source_rd_texture] = vt_id;
	return vt_id;
}

void VirtualTextureStorage::free_virtual_texture(uint32_t p_vt_id) {
	if (p_vt_id >= virtual_textures.size()) {
		return;
	}
	VirtualTexture &vt = virtual_textures[p_vt_id];
	for (uint32_t i = 0; i < vt.resident_tiles.size(); i++) {
		_free_tile(vt.resident_tiles[i]);
	}
	vt.resident_tiles.clear();
	// Leaves the slot in place (append-only vt_id space in S0a); a full free-list of vt_ids is an
	// S0c concern alongside indirection-layer reuse.
}

RID VirtualTextureStorage::get_page_pool_texture_rid() {
	_ensure_initialized();
	return page_pool_texture;
}

RID VirtualTextureStorage::get_indirection_texture_rid() {
	_ensure_initialized();
	return indirection_texture;
}

RID VirtualTextureStorage::get_vt_metadata_buffer_rid() {
	_ensure_initialized();
	return vt_metadata_buffer;
}
