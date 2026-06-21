/**************************************************************************/
/*  rendering_server_types.h                                              */
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

#include "core/io/image.h"
#include "core/math/aabb.h"
#include "core/math/rect2.h"
#include "core/math/rect2i.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "core/string/ustring.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"
#include "servers/rendering/rendering_server_enums.h"

#include <cstdint>

template <typename T>
class Vector;

namespace RenderingServerTypes {

/* TEXTURE API */

typedef void (*TextureDetectCallback)(void *);
typedef void (*TextureDetectRoughnessCallback)(void *, const String &, RSE::TextureDetectRoughnessChannel);

struct TextureInfo {
	RID texture;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	Image::Format format;
	int64_t bytes;
	String path;
	RSE::TextureType type;
};

/* SHADER API */

struct ShaderNativeSourceCode {
	struct Version {
		struct Stage {
			String name;
			String code;
		};
		Vector<Stage> stages;
	};
	Vector<Version> versions;
};

/* MESH API */

// Plain mirrors of meshoptimizer's meshopt_Meshlet/meshopt_Bounds (also mirrored independently as
// SurfaceTool::Meshlet/MeshletBounds in scene/resources/surface_tool.h). Kept as a separate,
// scene-independent copy here so SurfaceData/MeshletStorage - both servers-layer - don't need to
// include a scene/ header; conversion between the two mirrors is a plain memcpy since their
// layouts are identical.
struct MeshletInfo {
	uint32_t vertex_offset = 0;
	uint32_t triangle_offset = 0;
	uint32_t vertex_count = 0;
	uint32_t triangle_count = 0;
};

struct MeshletBoundsInfo {
	float center[3] = { 0, 0, 0 };
	float radius = 0;
	float cone_apex[3] = { 0, 0, 0 };
	float cone_axis[3] = { 0, 0, 0 };
	float cone_cutoff = 0;
	int8_t cone_axis_s8[3] = { 0, 0, 0 };
	int8_t cone_cutoff_s8 = 0;
};

struct SurfaceData {
	RSE::PrimitiveType primitive = RSE::PRIMITIVE_MAX;

	uint64_t format = RSE::ARRAY_FLAG_FORMAT_CURRENT_VERSION;
	Vector<uint8_t> vertex_data; // Vertex, Normal, Tangent (change with skinning, blendshape).
	Vector<uint8_t> attribute_data; // Color, UV, UV2, Custom0-3.
	Vector<uint8_t> skin_data; // Bone index, Bone weight.
	uint32_t vertex_count = 0;
	Vector<uint8_t> index_data;
	uint32_t index_count = 0;

	AABB aabb;
	struct LOD {
		float edge_length = 0.0f;
		Vector<uint8_t> index_data;

		Vector<MeshletInfo> meshlets;
		PackedInt32Array meshlet_vertices;
		PackedByteArray meshlet_triangles;
		Vector<MeshletBoundsInfo> meshlet_bounds;
	};
	Vector<LOD> lods;
	Vector<AABB> bone_aabbs;

	// Meshlets for the surface's own (full resolution) geometry; built once by
	// RenderingServer::mesh_create_surface_data_from_arrays() so every mesh creation path
	// (primitives, procedural SurfaceTool/ArrayMesh meshes, and imported meshes) gets meshlets
	// uniformly, without each caller needing to bake them separately.
	Vector<MeshletInfo> meshlets;
	PackedInt32Array meshlet_vertices;
	PackedByteArray meshlet_triangles;
	Vector<MeshletBoundsInfo> meshlet_bounds;

	// Raw (uncompressed) per-vertex source data the meshlets above are indexed against; kept
	// separately from vertex_data because that one may already be quantized/compressed by the
	// time it reaches the renderer, while MeshletStorage's global buffers want full precision.
	PackedVector3Array meshlet_positions;
	PackedVector3Array meshlet_normals;
	PackedVector2Array meshlet_uvs;

	// Transforms used in runtime bone AABBs compute.
	// Since bone AABBs is saved in Mesh space, but bones is in Skeleton space.
	Transform3D mesh_to_skeleton_xform;

	Vector<uint8_t> blend_shape_data;

	Vector4 uv_scale;

	RID material;
};

struct MeshInfo {
	RID mesh;
	String path;
	uint32_t vertex_buffer_size = 0;
	uint32_t attribute_buffer_size = 0;
	uint32_t skin_buffer_size = 0;
	uint32_t index_buffer_size = 0;
	uint32_t blend_shape_buffer_size = 0;
	uint32_t lod_index_buffers_size = 0;
	uint64_t vertex_count = 0;
};

/* STATUS INFORMATION */

struct FrameProfileArea {
	String name;
	double gpu_msec;
	double cpu_msec;
};

/* COMPOSITOR */

struct BlitToScreen {
	RID render_target;
	Rect2 src_rect = Rect2(0.0, 0.0, 1.0, 1.0);
	Rect2i dst_rect;

	struct {
		bool use_layer = false;
		uint32_t layer = 0;
	} multi_view;

	struct {
		//lens distorted parameters for VR
		bool apply = false;
		Vector2 eye_center;
		float k1 = 0.0;
		float k2 = 0.0;

		float upscale = 1.0;
		float aspect_ratio = 1.0;
	} lens_distortion;
};

/* BACKGROUND */

// Helper for RSE::SplashStretchMode, put here for convenience.
inline Rect2 get_splash_stretched_screen_rect(const Size2 &p_image_size, const Size2 &p_window_size, RSE::SplashStretchMode p_stretch_mode) {
	Size2 imgsize = p_image_size;
	Rect2 screenrect;
	switch (p_stretch_mode) {
		case RSE::SPLASH_STRETCH_MODE_DISABLED: {
			screenrect.size = imgsize;
			screenrect.position = ((p_window_size - screenrect.size) / 2.0).floor();
		} break;
		case RSE::SPLASH_STRETCH_MODE_KEEP: {
			if (p_window_size.width > p_window_size.height) {
				// Scale horizontally.
				screenrect.size.y = p_window_size.height;
				screenrect.size.x = imgsize.width * p_window_size.height / imgsize.height;
				screenrect.position.x = (p_window_size.width - screenrect.size.x) / 2;
			} else {
				// Scale vertically.
				screenrect.size.x = p_window_size.width;
				screenrect.size.y = imgsize.height * p_window_size.width / imgsize.width;
				screenrect.position.y = (p_window_size.height - screenrect.size.y) / 2;
			}
		} break;
		case RSE::SPLASH_STRETCH_MODE_KEEP_WIDTH: {
			// Scale vertically.
			screenrect.size.x = p_window_size.width;
			screenrect.size.y = imgsize.height * p_window_size.width / imgsize.width;
			screenrect.position.y = (p_window_size.height - screenrect.size.y) / 2;
		} break;
		case RSE::SPLASH_STRETCH_MODE_KEEP_HEIGHT: {
			// Scale horizontally.
			screenrect.size.y = p_window_size.height;
			screenrect.size.x = imgsize.width * p_window_size.height / imgsize.height;
			screenrect.position.x = (p_window_size.width - screenrect.size.x) / 2;
		} break;
		case RSE::SPLASH_STRETCH_MODE_COVER: {
			double window_aspect = (double)p_window_size.width / p_window_size.height;
			double img_aspect = imgsize.width / imgsize.height;

			if (window_aspect > img_aspect) {
				// Scale vertically.
				screenrect.size.x = p_window_size.width;
				screenrect.size.y = imgsize.height * p_window_size.width / imgsize.width;
				screenrect.position.y = (p_window_size.height - screenrect.size.y) / 2;
			} else {
				// Scale horizontally.
				screenrect.size.y = p_window_size.height;
				screenrect.size.x = imgsize.width * p_window_size.height / imgsize.height;
				screenrect.position.x = (p_window_size.width - screenrect.size.x) / 2;
			}
		} break;
		case RSE::SPLASH_STRETCH_MODE_IGNORE: {
			screenrect.size.x = p_window_size.width;
			screenrect.size.y = p_window_size.height;
		} break;
	}
	return screenrect;
}

/* RENDERING METHOD */

struct RenderInfo {
	int info[RSE::VIEWPORT_RENDER_INFO_TYPE_MAX][RSE::VIEWPORT_RENDER_INFO_MAX] = {};
};

} // namespace RenderingServerTypes
