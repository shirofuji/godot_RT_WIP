/**************************************************************************/
/*  meshlet_terrain_contract.h                                            */
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

#include "core/string/string_name.h"

// ============================================================================
// THE MESHLET TERRAIN MATERIAL CONTRACT - the single source of truth.
// ============================================================================
//
// A custom terrain shader joins the meshlet rendering spine by:
//   1. declaring `uniform bool meshlet_terrain = true;` and setting it true on the material
//      (that marker is what _meshlet_material_is_terrain() looks for), and
//   2. declaring the uniforms named in the tables below.
//
// Everything the engine reads off a terrain material is listed HERE, exactly once. The renderer
// builds its texture array and params UBO by walking these tables, and the GLSL slot indices are
// injected into the shader from the same enum - so a name or a slot cannot drift between the C++
// and the shader the way it does when both hardcode their own copy.
//
// Nothing is mandatory. Any uniform a material does not declare falls back to the default recorded
// here, which MUST equal the default declared in the reference shader (terrain_viewer.gdshader) -
// material_get_param returns an empty Variant for an undeclared uniform, so a wrong default here is
// silently substituted rather than reported. That has bitten this path twice: albedo sampled through
// a linear view instead of sRGB, and detail_depth defaulting to 0 (which disables displacement
// outright).
//
// KNOWN LIMITATION, recorded honestly: this is a fixed vocabulary, not a general material system.
// It is a faithful port of one reference shader's parameter set, and a project whose terrain wants
// different parameters still cannot express them. Centralising it here makes the coupling explicit
// and one-line to extend; it does not remove it.

namespace RendererRD {
namespace MeshletTerrainContract {

// Slots in the terrain texture array (shader binding 22). The per-material families are contiguous,
// so shader code indexes them as BASE + material_index. Mirrored into the shader as TERRAIN_SLOT_*
// defines by MeshletRenderer - do not hardcode these numbers in GLSL.
enum TextureSlot : uint32_t {
	SLOT_SPLAT0 = 0, // rgba = weight of materials 0..3
	SLOT_SPLAT1 = 1, // r = weight of material 4
	SLOT_ALBEDO = 2, // + 0..4
	SLOT_ORM = 7, // + 0..4  (r = occlusion, g = roughness, b = metallic)
	SLOT_NORMAL = 12, // + 0..4  (tangent space; z is reconstructed, blue is unused)
	SLOT_MACRO_VARIATION = 17, // map-wide tint, never tiles
	SLOT_HEIGHT = 18, // + 0..4  (detail displacement, read by the tessellation eval stage)
	SLOT_COUNT = 23,
};

// The heightmap that shapes the terrain itself. Bound separately (binding 19), not in the array,
// because the vertex stage needs it before any material data exists.
inline StringName heightmap_uniform() { return StringName("displacement_map"); }

struct TextureEntry {
	const char *name;
	bool srgb; // sRGB VIEW, for anything the reference shader declares `source_color`.
};

// Index == slot. Order is the contract.
static const TextureEntry TEXTURES[SLOT_COUNT] = {
	{ "splat0", false },
	{ "splat1", false },
	{ "mat0_albedo", true },
	{ "mat1_albedo", true },
	{ "mat2_albedo", true },
	{ "mat3_albedo", true },
	{ "mat4_albedo", true },
	{ "mat0_orm", false },
	{ "mat1_orm", false },
	{ "mat2_orm", false },
	{ "mat3_orm", false },
	{ "mat4_orm", false },
	{ "mat0_normal", false },
	{ "mat1_normal", false },
	{ "mat2_normal", false },
	{ "mat3_normal", false },
	{ "mat4_normal", false },
	{ "macro_variation", false },
	{ "mat0_height", false },
	{ "mat1_height", false },
	{ "mat2_height", false },
	{ "mat3_height", false },
	{ "mat4_height", false },
};

// Float offsets into the params UBO. Must match the TerrainParams block in meshlet_render.glsl:
// 11 vec4 = tp0, tp_tiles, tp_extra, tp_mat, tp_var_str, tp_var_scale, tp_hex, tp_var4, tp_var_rot,
// tp_det, tp_det2.
enum ParamIndex : uint32_t {
	P_TERRAIN_SIZE = 0,
	P_DEBUG_MODE = 9, // ENGINE-PROVIDED, not read from the material.
	PARAM_FLOAT_COUNT = 44,
};

enum ParamKind {
	PARAM_FLOAT,
	PARAM_VEC3,
	PARAM_VEC4,
};

struct ParamEntry {
	const char *name;
	ParamKind kind;
	uint32_t index; // first float slot
	float def[4];
};

// Defaults MUST match the reference shader's declared defaults (see the note at the top).
static const ParamEntry PARAMS[] = {
	{ "terrain_size", PARAM_FLOAT, 0, { 1024.0f, 0, 0, 0 } },
	{ "height_min", PARAM_FLOAT, 1, { 0.0f, 0, 0, 0 } },
	{ "height_range", PARAM_FLOAT, 2, { 1.0f, 0, 0, 0 } },
	{ "height_scale", PARAM_FLOAT, 3, { 1.0f, 0, 0, 0 } },
	{ "tiles_0123", PARAM_VEC4, 4, { 250.0f, 120.0f, 220.0f, 120.0f } },
	{ "tiles_4", PARAM_FLOAT, 8, { 160.0f, 0, 0, 0 } },
	// index 9 = P_DEBUG_MODE, engine-provided.
	{ "detail_weight_cutoff", PARAM_FLOAT, 10, { 0.02f, 0, 0, 0 } },
	{ "roughness_min", PARAM_FLOAT, 11, { 0.6f, 0, 0, 0 } },
	{ "normal_strength", PARAM_FLOAT, 12, { 1.0f, 0, 0, 0 } },
	{ "macro_variation_strength", PARAM_FLOAT, 13, { 0.6f, 0, 0, 0 } },
	// Defaults far below any terrain, so the shoreline term is inert when the material omits it.
	{ "water_level", PARAM_FLOAT, 14, { -1.0e30f, 0, 0, 0 } },
	{ "shore_band", PARAM_FLOAT, 15, { 1.0f, 0, 0, 0 } },
	{ "variant_strength_0123", PARAM_VEC4, 16, { 0, 0, 0, 0 } },
	{ "variant_scale_0123", PARAM_VEC4, 20, { 0.012f, 0.12f, 0.02f, 0.012f } },
	{ "hex_strength_0123", PARAM_VEC4, 24, { 0, 0, 0, 0 } },
	{ "variant_strength_4", PARAM_FLOAT, 28, { 0.0f, 0, 0, 0 } },
	{ "variant_scale_4", PARAM_FLOAT, 29, { 0.012f, 0, 0, 0 } },
	{ "hex_strength_4", PARAM_FLOAT, 30, { 0.0f, 0, 0, 0 } },
	{ "variant_blend_width", PARAM_FLOAT, 31, { 0.15f, 0, 0, 0 } },
	{ "variant_rotation", PARAM_FLOAT, 32, { 2.4f, 0, 0, 0 } },
	{ "variant_offset", PARAM_VEC3, 33, { 37.3f, 23.1f, 17.9f, 0 } },
	{ "detail_depth_0123", PARAM_VEC4, 36, { 0.5f, 1.0f, 1.0f, 1.0f } },
	{ "detail_depth_4", PARAM_FLOAT, 40, { 1.0f, 0, 0, 0 } },
	{ "tessellation_distance", PARAM_FLOAT, 41, { 600.0f, 0, 0, 0 } },
	{ "tessellation_fade", PARAM_FLOAT, 42, { 200.0f, 0, 0, 0 } },
	{ "detail_mip_bias", PARAM_FLOAT, 43, { 5.0f, 0, 0, 0 } },
};

static const uint32_t PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);

} // namespace MeshletTerrainContract
} // namespace RendererRD
