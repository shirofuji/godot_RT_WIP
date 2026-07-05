/**************************************************************************/
/*  test_virtual_texture_file.cpp                                        */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_virtual_texture_file)

#include "core/io/dir_access.h"
#include "servers/rendering/virtual_texture_file.h"
#include "tests/test_utils.h"

namespace TestVirtualTextureFile {

// Independently recompute the expected bordered page bytes for one page of a single-mip RGBA8 image,
// using the same clamp-to-edge border rule as VirtualTextureFile::bake(). If read_page() matches this,
// the bake -> file -> read round-trip (content AND replicated borders) is correct.
static PackedByteArray expected_page(const Ref<Image> &p_mip, uint32_t p_px, uint32_t p_py) {
	const int PS = (int)VirtualTextureFile::PAGE_SIZE;
	const int BD = (int)VirtualTextureFile::PAGE_BORDER;
	const int SP = (int)VirtualTextureFile::STORED_PAGE_SIZE;
	const int mw = p_mip->get_width();
	const int mh = p_mip->get_height();
	const PackedByteArray src = p_mip->get_data();
	const uint8_t *s = src.ptr();

	PackedByteArray out;
	out.resize(VirtualTextureFile::PAGE_BYTES);
	uint8_t *d = out.ptrw();
	for (int oy = 0; oy < SP; oy++) {
		const int sy = CLAMP((int)p_py * PS - BD + oy, 0, mh - 1);
		for (int ox = 0; ox < SP; ox++) {
			const int sx = CLAMP((int)p_px * PS - BD + ox, 0, mw - 1);
			const uint8_t *sp = &s[((int64_t)sy * mw + sx) * 4];
			uint8_t *dp = &d[(oy * SP + ox) * 4];
			dp[0] = sp[0];
			dp[1] = sp[1];
			dp[2] = sp[2];
			dp[3] = sp[3];
		}
	}
	return out;
}

static Ref<Image> make_pattern_image(int p_w, int p_h) {
	PackedByteArray data;
	data.resize(p_w * p_h * 4);
	uint8_t *p = data.ptrw();
	for (int y = 0; y < p_h; y++) {
		for (int x = 0; x < p_w; x++) {
			const int i = (y * p_w + x) * 4;
			p[i + 0] = (uint8_t)(x & 255);
			p[i + 1] = (uint8_t)(y & 255);
			p[i + 2] = (uint8_t)((x * 3 + y * 7) & 255);
			p[i + 3] = 255;
		}
	}
	return Image::create_from_data(p_w, p_h, false, Image::FORMAT_RGBA8, data);
}

TEST_CASE("[VirtualTextureFile] bake -> read round-trip (content + borders) at every mip/page") {
	const int W = 256;
	const int H = 256;
	const String path = TestUtils::get_data_path("test_virtual_texture_file.svt");

	Ref<Image> src = make_pattern_image(W, H);
	REQUIRE(VirtualTextureFile::bake(src, path) == OK);

	VirtualTextureFile vt;
	REQUIRE(vt.open(path) == OK);

	// Header. 256 has 9 mips (256..1); the format caps paged mips to MAX_MIPS (7).
	CHECK(vt.get_header().width == (uint32_t)W);
	CHECK(vt.get_header().height == (uint32_t)H);
	CHECK(vt.get_header().mip_count == VirtualTextureFile::MAX_MIPS);
	CHECK(vt.get_header().format == (uint32_t)VirtualTextureFile::FORMAT_RGBA8);

	// Reference mip chain, generated the same way the baker does.
	Ref<Image> mipsrc = src->duplicate();
	mipsrc->generate_mipmaps();

	// mip0 (256) = 2x2 pages; mip1 (128) = 1x1; deeper mips (<128) = 1x1.
	CHECK(vt.get_pages_x(0) == 2);
	CHECK(vt.get_pages_y(0) == 2);
	CHECK(vt.get_pages_x(1) == 1);
	CHECK(vt.get_pages_y(1) == 1);

	bool all_pages_match = true;
	for (uint32_t m = 0; m < vt.get_header().mip_count; m++) {
		Ref<Image> mip = mipsrc->get_image_from_mipmap(m);
		for (uint32_t py = 0; py < vt.get_pages_y(m); py++) {
			for (uint32_t px = 0; px < vt.get_pages_x(m); px++) {
				const PackedByteArray got = vt.read_page(m, px, py);
				if (got.size() != (int)VirtualTextureFile::PAGE_BYTES) {
					all_pages_match = false;
					continue;
				}
				if (got != expected_page(mip, px, py)) {
					all_pages_match = false;
				}
			}
		}
	}
	CHECK_MESSAGE(all_pages_match, "Every baked page (all mips) must match the clamped-border reference.");

	// Spot-check the border rule directly: mip0 page (0,0) stored texel (0,0) reads from clamp(-4,-4) =
	// source(0,0). The pattern makes source(0,0) = (0,0,0,255).
	const PackedByteArray p00 = vt.read_page(0, 0, 0);
	REQUIRE(p00.size() == (int)VirtualTextureFile::PAGE_BYTES);
	CHECK(p00[0] == 0); // R = x = 0
	CHECK(p00[1] == 0); // G = y = 0
	CHECK(p00[3] == 255); // A

	// The content origin of page (0,0) sits at stored offset (BORDER, BORDER) = source(0,0) too, and its
	// neighbour one texel in (+1 x) must be source(1,0) = R=1.
	const int SP = (int)VirtualTextureFile::STORED_PAGE_SIZE;
	const int BD = (int)VirtualTextureFile::PAGE_BORDER;
	CHECK(p00[(BD * SP + (BD + 1)) * 4 + 0] == 1); // R at content (1,0) = x = 1

	vt.close();
	DirAccess::remove_absolute(path);
}

TEST_CASE("[VirtualTextureFile] rejects a non-.svt file") {
	const String path = TestUtils::get_data_path("test_virtual_texture_not_svt.bin");
	{
		Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_32(0xDEADBEEF);
		f->store_32(1234);
		f->close();
	}
	VirtualTextureFile vt;
	ERR_PRINT_OFF;
	const Error e = vt.open(path);
	ERR_PRINT_ON;
	CHECK(e != OK);
	CHECK_FALSE(vt.is_open());
	DirAccess::remove_absolute(path);
}

} // namespace TestVirtualTextureFile
