/**************************************************************************/
/*  virtual_texture.cpp                                                   */
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

#include "virtual_texture.h"

#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/virtual_texture_file.h"

Image::Format VirtualTexture2D::get_format() const {
	return format;
}

Error VirtualTexture2D::load(const String &p_path) {
	Ref<VirtualTextureFile> file;
	file.instantiate();
	Error err = file->open(p_path);
	if (err != OK) {
		return err;
	}

	const VirtualTextureFile::Header &header = file->get_header();
	w = header.width;
	h = header.height;
	if (header.format == VirtualTextureFile::FORMAT_RGBA8) {
		format = Image::FORMAT_RGBA8;
	} else {
		return ERR_FILE_CORRUPT;
	}

	// Extract coarse fallback texture for non-VT samplers
	uint32_t coarse_mip = header.mip_count > 0 ? header.mip_count - 1 : 0;
	PackedByteArray page_data = file->read_page(coarse_mip, 0, 0);

	Ref<Image> fallback_image;
	if (!page_data.is_empty()) {
		// Coarsest mip's actual size is w >> coarse_mip and h >> coarse_mip.
		// However, it can't be smaller than 1.
		int coarse_w = MAX(1, w >> coarse_mip);
		int coarse_h = MAX(1, h >> coarse_mip);

		fallback_image = Image::create_empty(coarse_w, coarse_h, false, Image::FORMAT_RGBA8);
		
		// The page data contains VirtualTextureFile::STORED_PAGE_SIZE x STORED_PAGE_SIZE with borders.
		// We copy the non-border area (starting at PAGE_BORDER, PAGE_BORDER).
		const uint8_t *src = page_data.ptr();
		for (int y = 0; y < coarse_h; y++) {
			for (int x = 0; x < coarse_w; x++) {
				int src_x = VirtualTextureFile::PAGE_BORDER + x;
				int src_y = VirtualTextureFile::PAGE_BORDER + y;
				int src_idx = (src_y * VirtualTextureFile::STORED_PAGE_SIZE + src_x) * 4;
				fallback_image->set_pixel(x, y, Color(
					src[src_idx] / 255.0f,
					src[src_idx + 1] / 255.0f,
					src[src_idx + 2] / 255.0f,
					src[src_idx + 3] / 255.0f
				));
			}
		}
	} else {
		// Fallback in case of read error: 4x4 magenta image
		fallback_image = Image::create_empty(4, 4, false, Image::FORMAT_RGBA8);
		fallback_image->fill(Color(1, 0, 1, 1));
	}

	texture = RenderingServer::get_singleton()->texture_2d_create(fallback_image);
	
	// Tag the coarse fallback RID with the SVT path so the renderer can intercept it!
	RenderingServer::get_singleton()->texture_set_path(texture, p_path);

	path_to_file = p_path;

	return OK;
}

String VirtualTexture2D::get_load_path() const {
	return path_to_file;
}

int VirtualTexture2D::get_width() const {
	return w;
}

int VirtualTexture2D::get_height() const {
	return h;
}

RID VirtualTexture2D::get_rid() const {
	if (texture.is_valid()) {
		return texture;
	}
	return RID();
}

bool VirtualTexture2D::has_alpha() const {
	return true;
}

void VirtualTexture2D::set_path(const String &p_path, bool p_take_over) {
	if (texture.is_valid()) {
		RenderingServer::get_singleton()->texture_set_path(texture, p_path);
	}

	Resource::set_path(p_path, p_take_over);
}

Ref<Image> VirtualTexture2D::get_image() const {
	return Ref<Image>(); // Not trivial to provide the full image for SVT.
}

void VirtualTexture2D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &VirtualTexture2D::load);
}

VirtualTexture2D::VirtualTexture2D() {
}

VirtualTexture2D::~VirtualTexture2D() {
	if (texture.is_valid()) {
		ERR_FAIL_NULL(RenderingServer::get_singleton());
		RenderingServer::get_singleton()->free(texture);
	}
}

// ResourceFormatLoaderVirtualTexture2D

Ref<Resource> ResourceFormatLoaderVirtualTexture2D::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<VirtualTexture2D> vt;
	vt.instantiate();
	Error err = vt->load(p_path);
	if (r_error) {
		*r_error = err;
	}
	if (err != OK) {
		return Ref<Resource>();
	}

	return vt;
}

void ResourceFormatLoaderVirtualTexture2D::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("svt");
}

bool ResourceFormatLoaderVirtualTexture2D::handles_type(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "VirtualTexture2D");
}

String ResourceFormatLoaderVirtualTexture2D::get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "svt") {
		return "VirtualTexture2D";
	}
	return "";
}
