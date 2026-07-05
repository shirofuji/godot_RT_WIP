/**************************************************************************/
/*  virtual_texture_file.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                                */
/*                        https://godotengine.org                          */
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

#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"

// On-disk tiled page file (`.svt`) for Streaming Virtual Texturing (SVT S1, import-baked path). A source
// image is baked offline into bordered mip pages so the runtime page provider can stream a single page
// disk -> pool without ever uploading the full texture to VRAM (the whole point of S1 - see the SVT memory).
//
// Page geometry MUST stay in lockstep with RendererRD::VirtualTextureStorage (PAGE_SIZE / PAGE_BORDER /
// STORED_PAGE_SIZE / INDIRECTION_MIPS): each stored page is STORED_PAGE_SIZE^2 RGBA8 with borders already
// baked in (border texels = clamped source neighbours, exactly what the runtime blit compute shader
// produces today), so a page drops straight into a pool atlas tile via a plain texture upload.
//
// This class is deliberately RenderingDevice-free (pure Image + FileAccess) so the baker runs at import
// time in the editor and the format is unit-testable offline under `--test` (no GPU needed).
class VirtualTextureFile : public RefCounted {
public:
	static constexpr uint32_t MAGIC = 0x54565347u; // "GSVT" tag.
	static constexpr uint32_t VERSION = 1;
	static constexpr uint32_t PAGE_SIZE = 128; // Content texels per side. MUST match VirtualTextureStorage.
	static constexpr uint32_t PAGE_BORDER = 4; // Replicated border texels per edge.
	static constexpr uint32_t STORED_PAGE_SIZE = PAGE_SIZE + 2 * PAGE_BORDER; // 136.
	static constexpr uint32_t PAGE_BYTES = STORED_PAGE_SIZE * STORED_PAGE_SIZE * 4; // RGBA8, 73984.
	static constexpr uint32_t MAX_MIPS = 7; // Matches VirtualTextureStorage::INDIRECTION_MIPS (64..1 page grid).

	enum Format {
		FORMAT_RGBA8 = 0,
	};

	struct Header {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t mip_count = 0;
		uint32_t format = FORMAT_RGBA8;
	};

	// Bake a source image to `p_path`. Decompresses/converts to RGBA8, generates the mip chain (capped at
	// MAX_MIPS levels), splits each mip into STORED_PAGE_SIZE^2 bordered pages and writes header + page
	// table + payloads. Editor/import-time use.
	static Error bake(const Ref<Image> &p_source, const String &p_path);

	// --- Reader: metadata + random per-page access (runtime provider). ---
	Error open(const String &p_path);
	bool is_open() const { return file.is_valid(); }
	const Header &get_header() const { return header; }
	uint32_t get_pages_x(uint32_t p_mip) const { return p_mip < header.mip_count ? pages_x[p_mip] : 0; }
	uint32_t get_pages_y(uint32_t p_mip) const { return p_mip < header.mip_count ? pages_y[p_mip] : 0; }
	// Reads one STORED_PAGE_SIZE^2 RGBA8 page (PAGE_BYTES). Returns an empty array on any error.
	PackedByteArray read_page(uint32_t p_mip, uint32_t p_page_x, uint32_t p_page_y);
	void close();

private:
	Mutex file_mutex;
	Ref<FileAccess> file;
	Header header;
	uint32_t pages_x[MAX_MIPS] = {};
	uint32_t pages_y[MAX_MIPS] = {};
	uint64_t mip_offset[MAX_MIPS] = {};
};
