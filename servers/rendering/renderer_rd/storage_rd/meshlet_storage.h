/**************************************************************************/
/*  meshlet_storage.h                                                    */
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
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_types.h"

namespace RendererRD {

// Owns the large persistent, engine-wide GPU buffers backing the meshlet pipeline: vertex
// positions/attributes and meshlet remap/triangle/descriptor data for every imported mesh,
// addressed by global vertex index / meshlet index (bindless-style) rather than per-mesh RIDs.
// This is additive: it does not replace MeshStorage's existing per-surface vertex/index buffers,
// which keep working for the non-meshlet rendering path.
class MeshletStorage {
public:
	struct Range {
		uint32_t offset = 0;
		uint32_t count = 0;

		bool is_valid() const { return count > 0; }
	};

	// std430 layout, matches the GLSL-side MeshletDescriptor consumed by Phase 3's culling shader.
	struct MeshletDescriptorGPU {
		float bounds_center[3] = { 0, 0, 0 };
		float bounds_radius = 0;
		float cone_axis[3] = { 0, 0, 0 };
		float cone_cutoff = 0;
		uint32_t vertex_remap_offset = 0; // Into the global meshlet-vertex buffer.
		uint32_t triangle_offset = 0; // Byte offset into the global meshlet-triangle buffer.
		uint32_t vertex_count = 0;
		uint32_t triangle_count = 0;
	};

	// std430 layout, matches the GLSL-side MeshletMaterial struct (see meshlet_render.glsl) - a
	// flattened snapshot of the subset of StandardMaterial3D/ORMMaterial3D parameters Forward+'s
	// own fragment shader reads into local variables, not a generic shader-graph evaluator. Custom
	// ShaderMaterial/visual-shader materials can't be represented here and fall back to normal
	// Forward+ rendering (see _meshlet_scan_render_list's qualifying-mesh filtering).
	struct MeshletMaterialGPU {
		float albedo[4] = { 1, 1, 1, 1 }; // rgb + alpha.
		float emission[3] = { 0, 0, 0 };
		float metallic = 0.0f;
		float roughness = 1.0f;
		float specular = 0.5f;
		uint32_t albedo_texture_index = 0xFFFFFFFF; // Index into the bindless texture array
				// (meshlet_render.glsl's material_textures[]); 0xFFFFFFFF = none, use the flat
				// albedo color above instead.
		uint32_t normal_texture_index = 0xFFFFFFFF;
		uint32_t orm_texture_index = 0xFFFFFFFF; // Packed occlusion/roughness/metallic (Godot's
				// existing ORM convention).
		uint32_t emission_texture_index = 0xFFFFFFFF;
		uint32_t flags = 0; // Bit 0: alpha_scissor. Bit 1: unlit. Bit 2: backface_enabled.
		float alpha_scissor_threshold = 0.5f;
		float uv1_scale[2] = { 1, 1 };
		float uv1_offset[2] = { 0, 0 };
	};

	struct UploadResult {
		Range vertex_range; // Into the global vertex position/attribute buffers.
		Range meshlet_vertex_range; // Into the global meshlet-vertex (remap) buffer.
		Range meshlet_triangle_range; // Byte range into the global meshlet-triangle buffer.
		Range meshlet_range; // Into the global meshlet descriptor buffer.

		bool is_valid() const { return vertex_range.is_valid() && meshlet_range.is_valid(); }
	};

private:
	// First-fit allocator with free-list coalescing over a fixed-unit address space (vertices,
	// meshlets, or bytes depending on which GrowableBuffer it's paired with). Grow() extends the
	// space by appending a new free range at the end; it does not touch existing allocations.
	class RangeAllocator {
		struct FreeRange {
			uint32_t offset = 0;
			uint32_t count = 0;
		};
		LocalVector<FreeRange> free_ranges; // Kept sorted by offset, non-adjacent (coalesced).
		uint32_t capacity = 0;

	public:
		uint32_t get_capacity() const { return capacity; }
		void grow(uint32_t p_new_capacity);
		// Returns a range with count == 0 if there isn't enough free space (caller must grow()
		// the allocator - and the backing buffer - then retry).
		Range allocate(uint32_t p_count);
		void free(const Range &p_range);
	};

	// A storage buffer that can grow (doubling) by allocating a new RID and copying old contents
	// across, since RD storage buffers can't be resized in place.
	struct GrowableBuffer {
		RID rid;
		uint32_t capacity_bytes = 0;
		uint32_t element_size = 0;

		void init(uint32_t p_element_size, uint32_t p_initial_elements);
		void ensure_capacity(uint32_t p_required_elements);
		void upload(uint32_t p_element_offset, const void *p_data, uint32_t p_element_count);
		Vector<uint8_t> read_back(uint32_t p_element_offset, uint32_t p_element_count) const;
		void free();
	};

	static MeshletStorage *singleton;

	RangeAllocator vertex_allocator;
	RangeAllocator meshlet_vertex_allocator;
	RangeAllocator meshlet_triangle_allocator; // Byte-granular; allocations are rounded up to 4 bytes.
	RangeAllocator meshlet_allocator;

	GrowableBuffer vertex_position_buffer; // vec4 per vertex (xyz + pad).
	GrowableBuffer vertex_attribute_buffer; // vec4 per vertex (octahedral normal.xy + uv.xy).
	GrowableBuffer meshlet_vertex_buffer; // uint32 remap indices into the vertex buffers above.
	GrowableBuffer meshlet_triangle_buffer; // packed uint8 local triangle indices.
	GrowableBuffer meshlet_descriptor_buffer; // MeshletDescriptorGPU per meshlet.

	GrowableBuffer meshlet_material_buffer; // MeshletMaterialGPU per uploaded material.
	uint32_t meshlet_material_count = 0; // Simple append-only counter, not a RangeAllocator - one
			// entry per distinct material RID; materials accumulate but aren't freed for now
			// (this renderer's per-scene material count is expected to be small and bounded,
			// unlike vertices/meshlets which scale with geometry - revisit if that assumption
			// breaks in practice).
	HashMap<RID, uint32_t> material_rid_to_slot; // Dedup - many surfaces share one material RID.

	static Vector2 _oct_encode_normal(const Vector3 &p_normal);

public:
	static MeshletStorage *get_singleton();

	// Uploads one surface's worth of vertex + meshlet data (as produced by ImporterMesh's bake
	// step using SurfaceTool::build_meshlets(), converted to the layout-compatible
	// RenderingServerTypes::MeshletInfo/MeshletBoundsInfo mirrors) into the global buffers,
	// growing them as needed. p_normals/p_uvs may be empty (defaults to zero).
	UploadResult upload_mesh_meshlets(const PackedVector3Array &p_positions, const PackedVector3Array &p_normals, const PackedVector2Array &p_uvs, const Vector<RenderingServerTypes::MeshletInfo> &p_meshlets, const PackedInt32Array &p_meshlet_vertices, const PackedByteArray &p_meshlet_triangles, const Vector<RenderingServerTypes::MeshletBoundsInfo> &p_bounds);

	void free_mesh_meshlets(const UploadResult &p_result);

	// Split form of upload_mesh_meshlets(), for surfaces with multiple LODs that all index into
	// the same source vertex array: upload_vertices() is called once per surface, and
	// upload_meshlets() once per LOD (including the base/full-resolution one), reusing the same
	// vertex_range instead of re-uploading duplicate vertex data per LOD.
	Range upload_vertices(const PackedVector3Array &p_positions, const PackedVector3Array &p_normals, const PackedVector2Array &p_uvs);
	void free_vertices(const Range &p_vertex_range);
	UploadResult upload_meshlets(const Range &p_vertex_range, const Vector<RenderingServerTypes::MeshletInfo> &p_meshlets, const PackedInt32Array &p_meshlet_vertices, const PackedByteArray &p_meshlet_triangles, const Vector<RenderingServerTypes::MeshletBoundsInfo> &p_bounds);
	// Frees only the meshlet-side ranges of p_result (vertex_range is owned by upload_vertices()'s
	// caller and must be freed separately via free_vertices(), once all of its LODs are gone).
	void free_meshlets(const UploadResult &p_result);

	// Uploads (or returns the existing slot for, if p_material_rid was already uploaded) one
	// material's flattened GPU snapshot. Returns the slot index into meshlet_material_buffer.
	// p_material_rid is purely a dedup key (RID equality, no lifetime/ownership implications for
	// MeshletStorage) - callers are expected to re-call this whenever a material's parameters may
	// have changed (e.g. a ShaderMaterial uniform was edited) with the updated p_material_data;
	// the existing slot's contents get overwritten in place rather than allocating a new one.
	uint32_t upload_material(const RID &p_material_rid, const MeshletMaterialGPU &p_material_data);
	RID get_meshlet_material_buffer_rid() const { return meshlet_material_buffer.rid; }

	RID get_vertex_position_buffer_rid() const { return vertex_position_buffer.rid; }
	RID get_vertex_attribute_buffer_rid() const { return vertex_attribute_buffer.rid; }
	RID get_meshlet_vertex_buffer_rid() const { return meshlet_vertex_buffer.rid; }
	RID get_meshlet_triangle_buffer_rid() const { return meshlet_triangle_buffer.rid; }
	RID get_meshlet_descriptor_buffer_rid() const { return meshlet_descriptor_buffer.rid; }

	// Read-back helpers (stall the GPU - test/debug use only, never call these per-frame).
	MeshletDescriptorGPU debug_get_meshlet_descriptor(uint32_t p_global_meshlet_index) const;
	uint32_t debug_get_meshlet_vertex_remap(uint32_t p_global_remap_index) const;
	uint8_t debug_get_meshlet_triangle_index(uint32_t p_global_byte_index) const;
	Vector3 debug_get_vertex_position(uint32_t p_global_vertex_index) const;

	MeshletStorage();
	~MeshletStorage();
};

} // namespace RendererRD
