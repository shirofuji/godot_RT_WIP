/**************************************************************************/
/*  virtual_texture.h                                                     */
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

#include "core/io/resource_loader.h"
#include "scene/resources/texture.h"

class VirtualTexture2D : public Texture2D {
	GDCLASS(VirtualTexture2D, Texture2D);

private:
	String path_to_file;
	mutable RID texture;
	Image::Format format = Image::FORMAT_RGBA8;
	int w = 0;
	int h = 0;

protected:
	static void _bind_methods();

public:
	virtual Image::Format get_format() const override;
	Error load(const String &p_path);
	String get_load_path() const;

	virtual int get_width() const override;
	virtual int get_height() const override;
	virtual RID get_rid() const override;
	virtual bool has_alpha() const override;

	virtual void set_path(const String &p_path, bool p_take_over) override;

	virtual Ref<Image> get_image() const override;

	VirtualTexture2D();
	~VirtualTexture2D();
};

class ResourceFormatLoaderVirtualTexture2D : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderVirtualTexture2D, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};
