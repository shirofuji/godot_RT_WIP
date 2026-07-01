/**************************************************************************/
/*  virtual_texture_storage.h                                            */
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

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/virtual_texture_blit.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Software virtual-texturing (SVT) residency manager. Owns one physical page-pool atlas and a
// per-virtual-texture indirection (page-table) texture; the renderer samples through these instead
// of binding source textures directly, so the resident VRAM footprint is bounded by the pool size
// regardless of how many/how-large the source textures are.
//
// PHASING (see plan precious-wobbling-wilkinson.md):
//   * S0a (THIS phase): pool + indirection + register_virtual_texture() makes EVERY page of a
//     source texture resident immediately (no streaming, no eviction). Goal is a pixel no-op vs the
//     old direct material_textures[] sampling - it proves the indirection lookup + pool sampling
//     math (page borders, mip blend) in isolation. The pool is sized generously here; it does NOT
//     yet save VRAM (it holds everything), that's S0c's job.
//   * S0b (next): GPU feedback - the render pass writes which (vt_id, mip, page) it sampled; the
//     resident-set bookkeeping below (allocated_tiles / a future LRU) drives nothing yet but is
//     laid out so S0c can.
//   * S0c (next): make_resident()/evict() under a budget (the pool size becomes a project setting);
//     register_virtual_texture() stops making everything resident and instead registers metadata
//     only, with pages streamed in on demand from feedback.
//
// Coverage in S0 is the meshlet path only (it owns its fragment shader and already had an indirection
// table, MeshletStorage::material_texture_rids, which this replaces). S2 extends VT sampling to all
// engine textures via shader_compiler.cpp - out of scope here.
class VirtualTextureStorage {
public:
	// --- Page / pool geometry. The shader needs these to translate virtual UV -> physical UV, so any
	// change here must be mirrored in the meshlet render shader's VT-sample helper. ---

	// Texels of real content per page side. 128 is the SVT-standard tile size (good page-table
	// granularity vs. per-page copy overhead).
	static constexpr uint32_t PAGE_SIZE = 128;
	// Replicated-edge border per side, so bilinear (and limited anisotropic) filtering inside a page
	// never reaches across a tile seam into an unrelated neighbour in the atlas.
	static constexpr uint32_t PAGE_BORDER = 4;
	// What's actually stored per page in the atlas (content + both borders).
	static constexpr uint32_t STORED_PAGE_SIZE = PAGE_SIZE + 2 * PAGE_BORDER; // 136

	// Pool atlas dimensions in TILES. The physical pool texture is (dim * STORED_PAGE_SIZE) square; the
	// pool size IS the VRAM budget. Derived at init from the `rendering/virtual_texture/pool_size_mb`
	// project setting via get_pool_tiles_dim() (both this storage's allocation AND the meshlet VT
	// shader variant's VT_POOL_TILES_X define read that one accessor, so they always agree). Tile
	// coords are packed into the indirection texel as bytes, so the dim is clamped to [8, 255].
	// Bytes per stored tile (RGBA8). Default 64x64 tiles ~= 289 MB.
	static constexpr uint32_t POOL_TILE_BYTES = STORED_PAGE_SIZE * STORED_PAGE_SIZE * 4;
	static constexpr uint32_t POOL_TILES_DIM_MIN = 8;
	static constexpr uint32_t POOL_TILES_DIM_MAX = 255; // indirection stores tile x/y as uint8.
	// Reads the project setting and returns the pool tiles-per-side (square). Static so the meshlet
	// renderer can inject the matching VT_POOL_TILES_X shader define without needing the instance.
	static uint32_t get_pool_tiles_dim();

	// Max distinct virtual textures = layers in the indirection array. Matches the old
	// MeshletStorage::MAX_MATERIAL_TEXTURES so the meshlet binding model (one bounded array) carries
	// over 1:1.
	static constexpr uint32_t MAX_VIRTUAL_TEXTURES = 256;

	// Indirection texel sentinel: page not resident (sample must fall back to a coarser resident mip).
	static constexpr uint32_t PAGE_NOT_RESIDENT = 0xFFu;

	// Largest source texture side the page table is sized for. The indirection array's base mip is
	// MAX_TEXTURE_SIZE/PAGE_SIZE texels per side (one texel per page); since both the texture mip
	// chain and the indirection mip chain halve together, indirection mip m holds exactly the page
	// grid for texture mip m, so the shader fetches at (page_xy, vt_id) on indirection mip == texture
	// mip with no rescaling. A source larger than this is registered at a capped resolution.
	static constexpr uint32_t MAX_TEXTURE_SIZE = 8192;
	static constexpr uint32_t INDIRECTION_DIM = MAX_TEXTURE_SIZE / PAGE_SIZE; // 64 pages per side.
	static constexpr uint32_t INDIRECTION_MIPS = 7; // log2(64) + 1: 64,32,16,8,4,2,1.

	// S0b GPU feedback: one r32ui texel per FEEDBACK_TILE-pixel screen tile (MUST match
	// VT_FEEDBACK_TILE in meshlet_render.glsl). Packed (mip<<20 | vt_id<<12 | page_y<<6 | page_x);
	// 0xFFFFFFFF = no request.
	static constexpr uint32_t FEEDBACK_TILE = 16;

	// One decoded feedback entry: a (vt_id, mip, page) the rendered frame actually sampled. S0c
	// streams these in; S0b just reports/asserts them.
	struct PageRequest {
		uint32_t vt_id = 0;
		uint32_t mip = 0;
		uint32_t page_x = 0;
		uint32_t page_y = 0;
	};

	static VirtualTextureStorage *get_singleton();

	// The single kill-switch consulted by every VT integration point (texture registration in
	// MeshletStorage, pool/indirection binding + shader-variant selection in the meshlet renderer).
	// Resolves the --vt-disable / --vt-enable cmdline overrides over the rendering/virtual_texture/
	// enabled project setting, mirroring the meshlet path's force_disabled/force_enabled pattern in
	// render_forward_clustered.cpp. Resolved once and cached. When this is false the entire VT path is
	// inert: no pool/indirection textures are created, the renderer binds + samples the original
	// material_textures[] array, and behaviour is byte-identical to a build without VT.
	static bool is_enabled();

	// Registers a source RD texture as a virtual texture, returning its vt_id (= indirection-array
	// layer), or 0xFFFFFFFF if invalid or the table is full. Deduped by source RID (many materials
	// share one texture). In S0a this synchronously makes ALL of the source's pages resident: it
	// allocates pool tiles, blits each page (with border replication) from the source mips, and
	// writes the indirection texels. The returned vt_id is what a material stores in place of the old
	// per-texture slot index, and what the shader passes to the VT-sample helper.
	uint32_t register_virtual_texture(const RID &p_source_rd_texture);

	// Records that p_normal_vt and p_orm_vt are the same material's normal/ORM siblings of the albedo
	// p_albedo_vt, so update_streaming() streams their matching pages together (GPU feedback only
	// reports albedo). Invalid/0xFFFFFFFF sibling ids are skipped. Idempotent per registration.
	void link_material_siblings(uint32_t p_albedo_vt, uint32_t p_normal_vt, uint32_t p_orm_vt);

	// Releases a virtual texture's pool tiles and clears its indirection layer. Safe to call on an
	// unregistered/invalid vt_id (no-op).
	void free_virtual_texture(uint32_t p_vt_id);

	// --- Bindings the renderer hands to its uniform set (replacing the material_textures[256] array +
	// its sampler). The pool is sampled with a clamp sampler (borders handle cross-page filtering);
	// the indirection is sampled point/nearest. ---
	// The pool geometry (POOL_TILES_*, PAGE_*, STORED_PAGE_SIZE) the shader's VT-sample helper needs
	// is all compile-time, mirrored as #defines in meshlet_render.glsl - so no per-frame constants
	// buffer is required; only these three GPU resources are bound.
	RID get_page_pool_texture_rid();
	RID get_indirection_texture_rid();
	RID get_vt_metadata_buffer_rid(); // Per-vt_id VTMetadataGPU (base size + mip count) for the shader.

	// S0b GPU feedback. The renderer binds the feedback image (binding 18, r32ui) in the VT shader
	// variant; get_feedback_image() lazily (re)creates it sized to the render target in FEEDBACK_TILE
	// tiles. clear_feedback() resets it to "no request" (call before the pass; it's a texture op, so
	// not inside a draw list). read_feedback_requests() reads it back and decodes the deduped set of
	// pages the frame sampled - a GPU-readback stall, so S0b/tests call it directly; S0c reads last
	// frame's to avoid the stall.
	RID get_feedback_image(const Size2i &p_render_size);
	void clear_feedback();
	Vector<PageRequest> read_feedback_requests(); // SYNCHRONOUS readback (stalls) - used by selftests.

	// Stall-free per-frame feedback drain (replaces the synchronous read_feedback_requests()+
	// update_streaming() in the render path). Call once per frame after the VT color draw:
	//   poll_pending_streams(); // apply any async readback that has completed (GPU work, safe point)
	//   request_feedback_async(); // queue THIS frame's feedback for async readback (no CPU stall)
	// The async readback's callback (fires ~2-3 frames later, render thread, at the normal frame fence)
	// only DECODES into pending_stream_requests (CPU only); the actual streaming GPU work runs in the
	// next frame's poll_pending_streams() at a valid command point. Net: VT paging is a few frames
	// latent (fine for texture streaming) but never blocks the pipeline.
	void request_feedback_async();
	void poll_pending_streams();

	// S0c: the per-frame residency drain. Given the page set the frame sampled (read_feedback_requests),
	// streams in any requested fine (non-base) pages not yet resident - blitting them into the pool and
	// patching the indirection - and evicts the least-recently-used streamed pages when the pool is full
	// (the pool size IS the VRAM budget). Bounded to MAX_STREAMS_PER_FRAME new pages per call.
	// Always-resident base mips are never streamed or evicted.
	void update_streaming(const Vector<PageRequest> &p_requests);

	// std430, mirrored by the shader's VTMeta struct. One entry per registered virtual texture,
	// indexed by vt_id; lets the shader recover each mip's texel dimensions to map a UV to a page +
	// in-page offset. resident_mip_floor is the coarsest mip guaranteed resident (S0a: always 0,
	// everything resident; S0c: raised as fine mips get evicted, giving the shader a safe fallback).
	struct VTMetadataGPU {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t mip_count = 0;
		uint32_t resident_mip_floor = 0;
	};

	VirtualTextureStorage();
	~VirtualTextureStorage();

private:
	static VirtualTextureStorage *singleton;

	// Lazily creates the pool + indirection GPU textures on first registration (RenderingDevice must
	// exist by then). Returns false if RD is unavailable (e.g. headless without a real device).
	bool _ensure_initialized();
	bool initialized = false;

	uint32_t pool_tiles_dim = 64; // Pool tiles per side (square). Set from get_pool_tiles_dim() at init.

	RID page_pool_texture; // 2D, (pool_tiles_dim*STORED_PAGE_SIZE) square.
	RID indirection_texture; // 2D_ARRAY, MAX_VIRTUAL_TEXTURES layers, mipped to the page-grid chain.
	RID vt_metadata_buffer; // Storage buffer of VTMetadataGPU[MAX_VIRTUAL_TEXTURES].

	RID feedback_image; // S0b: r32ui, render size / FEEDBACK_TILE; one (vt_id,mip,page) per tile.
	Size2i feedback_dims; // Current feedback image dimensions, in tiles.

	// Async feedback drain state (see request_feedback_async/poll_pending_streams). Render-thread only,
	// so no locking. feedback_async_in_flight bounds it to one outstanding readback at a time.
	bool feedback_async_in_flight = false;
	bool pending_stream_valid = false;
	Vector<PageRequest> pending_stream_requests;
	static void _feedback_async_callback(const PackedByteArray &p_data); // fires on the render thread.
	void _ingest_feedback_data(const PackedByteArray &p_data); // decode -> pending_stream_requests.

	// Blit pipeline (virtual_texture_blit.glsl): copies a source page -> a pool tile with borders.
	VirtualTextureBlitShaderRD blit_shader;
	RID blit_shader_version;
	RID blit_shader_rid;
	RID blit_pipeline;
	RID blit_source_sampler; // For the blit's texelFetch source binding (combined sampler required).

	// Free-list of physical pool tiles, addressed by linear index ty*POOL_TILES_X + tx. S0a allocates
	// and never frees (everything resident); S0c turns this into the LRU-evictable resident set.
	LocalVector<uint32_t> free_tiles;
	uint32_t _allocate_tile(); // 0xFFFFFFFF if the pool is full.
	void _free_tile(uint32_t p_tile_index);

	struct VirtualTexture {
		RID source;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t mip_count = 0;
		uint32_t vt_id = 0xFFFFFFFF;
		uint32_t resident_mip_floor = 0; // Mips >= this are the always-resident base (never evicted).
		LocalVector<uint32_t> resident_tiles; // Base pool tiles, released in free_virtual_texture().
		Vector<uint8_t> indirection_cpu; // CPU shadow of this layer's page table, for per-page patching.
		// Sibling VTs of the SAME material (albedo's siblings = its normal + ORM). GPU feedback only
		// reports the albedo VT (one per screen tile), but siblings share the same UV -> same
		// (mip,page), so streaming an albedo page also streams its siblings' matching pages, keeping a
		// material's PBR set resident together. Empty for non-albedo VTs. See link_material_siblings().
		LocalVector<uint32_t> siblings;
	};
	LocalVector<VirtualTexture> virtual_textures; // Indexed by vt_id.
	HashMap<RID, uint32_t> source_rid_to_vt_id; // Dedup.

	// S0c streaming / LRU-eviction state.
	static constexpr uint32_t MAX_STREAMS_PER_FRAME = 128; // Cap new pages blitted per update_streaming.
	static constexpr uint64_t NO_STREAMED_PAGE = 0xFFFFFFFFFFFFFFFFull; // tile_page_key sentinel.
	HashMap<uint64_t, uint32_t> streamed_page_to_tile; // page_key -> tile, for resident streamed pages.
	LocalVector<uint64_t> tile_page_key; // Per pool tile: the streamed page it holds, or NO_STREAMED_PAGE
			// (a base tile / free tile) - only != sentinel tiles are LRU-evictable.
	LocalVector<uint32_t> tile_last_used; // Per pool tile: last stream_frame it was sampled (LRU key).
	uint32_t stream_frame = 0;

	static uint64_t _page_key(uint32_t p_vt_id, uint32_t p_mip, uint32_t p_page_x, uint32_t p_page_y);
	static uint32_t _indirection_mip_offset(uint32_t p_mip); // Byte offset of mip p_mip within one layer.
	uint32_t _stream_alloc_tile(HashSet<uint32_t> &r_dirty_layers); // Alloc a tile, evicting LRU if full.
	void _set_page_indirection(uint32_t p_vt_id, uint32_t p_mip, uint32_t p_page_x, uint32_t p_page_y, uint32_t p_tile, bool p_resident, HashSet<uint32_t> &r_dirty_layers);

	// Blits one source page (PAGE_SIZE^2 of the given source mip at page coord, plus replicated
	// borders -> STORED_PAGE_SIZE^2) into the pool tile. Implemented as a compute dispatch
	// (virtual_texture_blit.glsl) so border clamping is done in-shader and the same path serves S0c's
	// on-demand streaming. Writes nothing to the indirection (caller does that once the tile is set).
	void _blit_source_page_to_tile(const RID &p_source, uint32_t p_mip, uint32_t p_page_x, uint32_t p_page_y, uint32_t p_tile_index);

	// Writes one indirection texel (vt layer, mip, page coord) = the tile's atlas position, or the
	// PAGE_NOT_RESIDENT sentinel.
	void _write_indirection(uint32_t p_vt_id, uint32_t p_mip, uint32_t p_page_x, uint32_t p_page_y, uint32_t p_tile_index);
};

} // namespace RendererRD
