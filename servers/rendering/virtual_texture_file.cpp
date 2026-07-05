/**************************************************************************/
/*  virtual_texture_file.cpp                                              */
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

#include "virtual_texture_file.h"

Error VirtualTextureFile::bake(const Ref<Image> &p_source, const String &p_path) {
	ERR_FAIL_COND_V_MSG(p_source.is_null() || p_source->is_empty(), ERR_INVALID_PARAMETER, "VT bake: null/empty source image.");

	// Normalize to an uncompressed RGBA8 image with a full mip chain (the page grid indexes mips).
	Ref<Image> img = p_source->duplicate();
	if (img->is_compressed()) {
		Error de = img->decompress();
		ERR_FAIL_COND_V_MSG(de != OK, de, "VT bake: source is compressed and could not be decompressed.");
	}
	if (img->get_format() != Image::FORMAT_RGBA8) {
		img->convert(Image::FORMAT_RGBA8);
	}
	if (!img->has_mipmaps()) {
		img->generate_mipmaps();
	}

	const uint32_t width = img->get_width();
	const uint32_t height = img->get_height();
	const uint32_t total_mips = (uint32_t)img->get_mipmap_count() + 1;
	const uint32_t mip_count = MIN(total_mips, MAX_MIPS);

	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_CANT_CREATE, "VT bake: cannot open '" + p_path + "' for writing.");

	// Header.
	f->store_32(MAGIC);
	f->store_32(VERSION);
	f->store_32(width);
	f->store_32(height);
	f->store_32(mip_count);
	f->store_32(FORMAT_RGBA8);

	// Page grid per mip, then the table (pages_x, pages_y per mip). Payloads follow immediately.
	uint32_t px_count[MAX_MIPS] = {};
	uint32_t py_count[MAX_MIPS] = {};
	for (uint32_t m = 0; m < mip_count; m++) {
		const uint32_t mw = MAX(width >> m, 1u);
		const uint32_t mh = MAX(height >> m, 1u);
		px_count[m] = MAX((mw + PAGE_SIZE - 1) / PAGE_SIZE, 1u);
		py_count[m] = MAX((mh + PAGE_SIZE - 1) / PAGE_SIZE, 1u);
		f->store_32(px_count[m]);
		f->store_32(py_count[m]);
	}

	// Payloads: per mip, row-major pages, each STORED_PAGE_SIZE^2 RGBA8 with clamped-neighbour borders.
	LocalVector<uint8_t> page;
	page.resize(PAGE_BYTES);
	for (uint32_t m = 0; m < mip_count; m++) {
		Ref<Image> mip_img = img->get_image_from_mipmap(m);
		const PackedByteArray mdata = mip_img->get_data();
		const uint8_t *src = mdata.ptr();
		const int mw = mip_img->get_width();
		const int mh = mip_img->get_height();

		for (uint32_t pyi = 0; pyi < py_count[m]; pyi++) {
			for (uint32_t pxi = 0; pxi < px_count[m]; pxi++) {
				for (uint32_t oy = 0; oy < STORED_PAGE_SIZE; oy++) {
					const int sy = CLAMP((int)(pyi * PAGE_SIZE) - (int)PAGE_BORDER + (int)oy, 0, mh - 1);
					for (uint32_t ox = 0; ox < STORED_PAGE_SIZE; ox++) {
						const int sx = CLAMP((int)(pxi * PAGE_SIZE) - (int)PAGE_BORDER + (int)ox, 0, mw - 1);
						const uint8_t *s = &src[((int64_t)sy * mw + sx) * 4];
						uint8_t *d = &page[(oy * STORED_PAGE_SIZE + ox) * 4];
						d[0] = s[0];
						d[1] = s[1];
						d[2] = s[2];
						d[3] = s[3];
					}
				}
				f->store_buffer(page.ptr(), PAGE_BYTES);
			}
		}
	}

	f->close();
	return OK;
}

Error VirtualTextureFile::open(const String &p_path) {
	close();
	file = FileAccess::open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_CANT_OPEN, "VT open: cannot open '" + p_path + "'.");

	const uint32_t magic = file->get_32();
	const uint32_t version = file->get_32();
	if (magic != MAGIC || version != VERSION) {
		close();
		ERR_FAIL_V_MSG(ERR_FILE_UNRECOGNIZED, "VT open: bad magic/version in '" + p_path + "'.");
	}
	header.width = file->get_32();
	header.height = file->get_32();
	header.mip_count = file->get_32();
	header.format = file->get_32();
	if (header.mip_count == 0 || header.mip_count > MAX_MIPS) {
		close();
		ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, "VT open: invalid mip_count in '" + p_path + "'.");
	}

	for (uint32_t m = 0; m < header.mip_count; m++) {
		pages_x[m] = file->get_32();
		pages_y[m] = file->get_32();
	}

	uint64_t off = file->get_position(); // Payloads start right after the table.
	for (uint32_t m = 0; m < header.mip_count; m++) {
		mip_offset[m] = off;
		off += (uint64_t)pages_x[m] * pages_y[m] * PAGE_BYTES;
	}
	return OK;
}

PackedByteArray VirtualTextureFile::read_page(uint32_t p_mip, uint32_t p_page_x, uint32_t p_page_y) {
	PackedByteArray out;
	ERR_FAIL_COND_V_MSG(file.is_null(), out, "VT read_page: file not open.");
	ERR_FAIL_COND_V(p_mip >= header.mip_count, out);
	ERR_FAIL_COND_V(p_page_x >= pages_x[p_mip] || p_page_y >= pages_y[p_mip], out);

	const uint64_t idx = (uint64_t)p_page_y * pages_x[p_mip] + p_page_x;
	const uint64_t off = mip_offset[p_mip] + idx * PAGE_BYTES;
	
	MutexLock lock(file_mutex);
	file->seek(off);
	out.resize(PAGE_BYTES);
	const uint64_t read = file->get_buffer(out.ptrw(), PAGE_BYTES);
	ERR_FAIL_COND_V_MSG(read != PAGE_BYTES, PackedByteArray(), "VT read_page: short read (truncated file?).");
	return out;
}

void VirtualTextureFile::close() {
	if (file.is_valid()) {
		file.unref();
	}
	header = Header();
	for (uint32_t m = 0; m < MAX_MIPS; m++) {
		pages_x[m] = 0;
		pages_y[m] = 0;
		mip_offset[m] = 0;
	}
}
