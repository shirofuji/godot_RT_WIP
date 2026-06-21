/**************************************************************************/
/*  meshlet_storage.cpp                                                  */
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

#include "meshlet_storage.h"

using namespace RendererRD;

MeshletStorage *MeshletStorage::singleton = nullptr;

MeshletStorage *MeshletStorage::get_singleton() {
	return singleton;
}

// ---------------------------------------------------------------------------
// RangeAllocator
// ---------------------------------------------------------------------------

void MeshletStorage::RangeAllocator::grow(uint32_t p_new_capacity) {
	if (p_new_capacity <= capacity) {
		return;
	}
	uint32_t added = p_new_capacity - capacity;
	if (!free_ranges.is_empty() && free_ranges[free_ranges.size() - 1].offset + free_ranges[free_ranges.size() - 1].count == capacity) {
		free_ranges[free_ranges.size() - 1].count += added;
	} else {
		free_ranges.push_back({ capacity, added });
	}
	capacity = p_new_capacity;
}

MeshletStorage::Range MeshletStorage::RangeAllocator::allocate(uint32_t p_count) {
	for (uint32_t i = 0; i < free_ranges.size(); i++) {
		if (free_ranges[i].count >= p_count) {
			Range result{ free_ranges[i].offset, p_count };
			if (free_ranges[i].count == p_count) {
				free_ranges.remove_at(i);
			} else {
				free_ranges[i].offset += p_count;
				free_ranges[i].count -= p_count;
			}
			return result;
		}
	}
	return Range{ 0, 0 };
}

void MeshletStorage::RangeAllocator::free(const Range &p_range) {
	if (!p_range.is_valid()) {
		return;
	}
	uint32_t insert_at = 0;
	while (insert_at < free_ranges.size() && free_ranges[insert_at].offset < p_range.offset) {
		insert_at++;
	}
	free_ranges.insert(insert_at, { p_range.offset, p_range.count });

	// Coalesce with the next neighbor, then the previous one.
	if (insert_at + 1 < free_ranges.size() && free_ranges[insert_at].offset + free_ranges[insert_at].count == free_ranges[insert_at + 1].offset) {
		free_ranges[insert_at].count += free_ranges[insert_at + 1].count;
		free_ranges.remove_at(insert_at + 1);
	}
	if (insert_at > 0 && free_ranges[insert_at - 1].offset + free_ranges[insert_at - 1].count == free_ranges[insert_at].offset) {
		free_ranges[insert_at - 1].count += free_ranges[insert_at].count;
		free_ranges.remove_at(insert_at);
	}
}

// ---------------------------------------------------------------------------
// GrowableBuffer
// ---------------------------------------------------------------------------

void MeshletStorage::GrowableBuffer::init(uint32_t p_element_size, uint32_t p_initial_elements) {
	element_size = p_element_size;
	capacity_bytes = MAX((uint32_t)4, p_element_size * p_initial_elements);
	rid = RD::get_singleton()->storage_buffer_create(capacity_bytes);
}

void MeshletStorage::GrowableBuffer::ensure_capacity(uint32_t p_required_elements) {
	uint32_t required_bytes = p_required_elements * element_size;
	if (required_bytes <= capacity_bytes) {
		return;
	}
	uint32_t new_capacity_bytes = MAX(capacity_bytes * 2, required_bytes);
	RID new_rid = RD::get_singleton()->storage_buffer_create(new_capacity_bytes);
	if (rid.is_valid()) {
		RD::get_singleton()->buffer_copy(rid, new_rid, 0, 0, capacity_bytes);
		RD::get_singleton()->free_rid(rid);
	}
	rid = new_rid;
	capacity_bytes = new_capacity_bytes;
}

void MeshletStorage::GrowableBuffer::upload(uint32_t p_element_offset, const void *p_data, uint32_t p_element_count) {
	if (p_element_count == 0) {
		return;
	}
	RD::get_singleton()->buffer_update(rid, p_element_offset * element_size, p_element_count * element_size, p_data);
}

Vector<uint8_t> MeshletStorage::GrowableBuffer::read_back(uint32_t p_element_offset, uint32_t p_element_count) const {
	return RD::get_singleton()->buffer_get_data(rid, p_element_offset * element_size, p_element_count * element_size);
}

void MeshletStorage::GrowableBuffer::free() {
	if (rid.is_valid()) {
		RD::get_singleton()->free_rid(rid);
		rid = RID();
	}
	capacity_bytes = 0;
}

// ---------------------------------------------------------------------------
// MeshletStorage
// ---------------------------------------------------------------------------

MeshletStorage::MeshletStorage() {
	singleton = this;

	const uint32_t initial_vertices = 1024;
	const uint32_t initial_meshlet_vertices = 1024;
	const uint32_t initial_meshlet_triangle_bytes = 4096; // Must stay a multiple of 4.
	const uint32_t initial_meshlets = 256;

	vertex_position_buffer.init(sizeof(float) * 4, initial_vertices);
	vertex_attribute_buffer.init(sizeof(float) * 4, initial_vertices);
	meshlet_vertex_buffer.init(sizeof(uint32_t), initial_meshlet_vertices);
	meshlet_triangle_buffer.init(sizeof(uint8_t), initial_meshlet_triangle_bytes);
	meshlet_descriptor_buffer.init(sizeof(MeshletDescriptorGPU), initial_meshlets);

	vertex_allocator.grow(initial_vertices);
	meshlet_vertex_allocator.grow(initial_meshlet_vertices);
	meshlet_triangle_allocator.grow(initial_meshlet_triangle_bytes);
	meshlet_allocator.grow(initial_meshlets);
}

MeshletStorage::~MeshletStorage() {
	vertex_position_buffer.free();
	vertex_attribute_buffer.free();
	meshlet_vertex_buffer.free();
	meshlet_triangle_buffer.free();
	meshlet_descriptor_buffer.free();
	singleton = nullptr;
}

Vector2 MeshletStorage::_oct_encode_normal(const Vector3 &p_normal) {
	Vector3 n = p_normal;
	real_t l1_norm = Math::abs(n.x) + Math::abs(n.y) + Math::abs(n.z);
	if (l1_norm < (real_t)CMP_EPSILON) {
		return Vector2(0, 0);
	}
	n /= l1_norm;
	if (n.z >= 0) {
		return Vector2(n.x, n.y);
	}
	return Vector2(
			(1.0f - Math::abs(n.y)) * (n.x >= 0 ? 1.0f : -1.0f),
			(1.0f - Math::abs(n.x)) * (n.y >= 0 ? 1.0f : -1.0f));
}

MeshletStorage::UploadResult MeshletStorage::upload_mesh_meshlets(const PackedVector3Array &p_positions, const PackedVector3Array &p_normals, const PackedVector2Array &p_uvs, const Vector<RenderingServerTypes::MeshletInfo> &p_meshlets, const PackedInt32Array &p_meshlet_vertices, const PackedByteArray &p_meshlet_triangles, const Vector<RenderingServerTypes::MeshletBoundsInfo> &p_bounds) {
	UploadResult result;

	ERR_FAIL_COND_V(p_positions.is_empty(), result);
	ERR_FAIL_COND_V(p_meshlets.is_empty(), result);
	ERR_FAIL_COND_V(p_meshlets.size() != p_bounds.size(), result);
	ERR_FAIL_COND_V(!p_normals.is_empty() && p_normals.size() != p_positions.size(), result);
	ERR_FAIL_COND_V(!p_uvs.is_empty() && p_uvs.size() != p_positions.size(), result);

	result.vertex_range = upload_vertices(p_positions, p_normals, p_uvs);
	UploadResult meshlet_result = upload_meshlets(result.vertex_range, p_meshlets, p_meshlet_vertices, p_meshlet_triangles, p_bounds);
	result.meshlet_vertex_range = meshlet_result.meshlet_vertex_range;
	result.meshlet_triangle_range = meshlet_result.meshlet_triangle_range;
	result.meshlet_range = meshlet_result.meshlet_range;
	return result;
}

void MeshletStorage::free_mesh_meshlets(const UploadResult &p_result) {
	free_vertices(p_result.vertex_range);
	free_meshlets(p_result);
}

MeshletStorage::Range MeshletStorage::upload_vertices(const PackedVector3Array &p_positions, const PackedVector3Array &p_normals, const PackedVector2Array &p_uvs) {
	ERR_FAIL_COND_V(p_positions.is_empty(), Range());
	ERR_FAIL_COND_V(!p_normals.is_empty() && p_normals.size() != p_positions.size(), Range());
	ERR_FAIL_COND_V(!p_uvs.is_empty() && p_uvs.size() != p_positions.size(), Range());

	const uint32_t vertex_count = (uint32_t)p_positions.size();

	Range vertex_range = vertex_allocator.allocate(vertex_count);
	while (!vertex_range.is_valid()) {
		vertex_allocator.grow(MAX(vertex_allocator.get_capacity() * 2, vertex_allocator.get_capacity() + vertex_count));
		vertex_range = vertex_allocator.allocate(vertex_count);
	}
	vertex_position_buffer.ensure_capacity(vertex_allocator.get_capacity());
	vertex_attribute_buffer.ensure_capacity(vertex_allocator.get_capacity());

	LocalVector<float> positions;
	positions.resize(vertex_count * 4);
	LocalVector<float> attributes;
	attributes.resize(vertex_count * 4);
	for (uint32_t i = 0; i < vertex_count; i++) {
		positions[i * 4 + 0] = p_positions[i].x;
		positions[i * 4 + 1] = p_positions[i].y;
		positions[i * 4 + 2] = p_positions[i].z;
		positions[i * 4 + 3] = 0.0f;

		Vector2 oct = p_normals.is_empty() ? Vector2() : _oct_encode_normal(p_normals[i]);
		Vector2 uv = p_uvs.is_empty() ? Vector2() : p_uvs[i];
		attributes[i * 4 + 0] = oct.x;
		attributes[i * 4 + 1] = oct.y;
		attributes[i * 4 + 2] = uv.x;
		attributes[i * 4 + 3] = uv.y;
	}
	vertex_position_buffer.upload(vertex_range.offset, positions.ptr(), vertex_count);
	vertex_attribute_buffer.upload(vertex_range.offset, attributes.ptr(), vertex_count);

	return vertex_range;
}

void MeshletStorage::free_vertices(const Range &p_vertex_range) {
	vertex_allocator.free(p_vertex_range);
}

MeshletStorage::UploadResult MeshletStorage::upload_meshlets(const Range &p_vertex_range, const Vector<RenderingServerTypes::MeshletInfo> &p_meshlets, const PackedInt32Array &p_meshlet_vertices, const PackedByteArray &p_meshlet_triangles, const Vector<RenderingServerTypes::MeshletBoundsInfo> &p_bounds) {
	UploadResult result;
	result.vertex_range = p_vertex_range;

	ERR_FAIL_COND_V(!p_vertex_range.is_valid(), result);
	ERR_FAIL_COND_V(p_meshlets.is_empty(), result);
	ERR_FAIL_COND_V(p_meshlets.size() != p_bounds.size(), result);

	const uint32_t meshlet_vertex_count = (uint32_t)p_meshlet_vertices.size();
	const uint32_t meshlet_triangle_bytes = ((uint32_t)p_meshlet_triangles.size() + 3) & ~3u; // Round up to a multiple of 4.
	const uint32_t meshlet_count = (uint32_t)p_meshlets.size();

	result.meshlet_vertex_range = meshlet_vertex_allocator.allocate(meshlet_vertex_count);
	while (!result.meshlet_vertex_range.is_valid()) {
		meshlet_vertex_allocator.grow(MAX(meshlet_vertex_allocator.get_capacity() * 2, meshlet_vertex_allocator.get_capacity() + meshlet_vertex_count));
		result.meshlet_vertex_range = meshlet_vertex_allocator.allocate(meshlet_vertex_count);
	}
	meshlet_vertex_buffer.ensure_capacity(meshlet_vertex_allocator.get_capacity());

	if (meshlet_triangle_bytes > 0) {
		result.meshlet_triangle_range = meshlet_triangle_allocator.allocate(meshlet_triangle_bytes);
		while (!result.meshlet_triangle_range.is_valid()) {
			meshlet_triangle_allocator.grow(MAX(meshlet_triangle_allocator.get_capacity() * 2, meshlet_triangle_allocator.get_capacity() + meshlet_triangle_bytes));
			result.meshlet_triangle_range = meshlet_triangle_allocator.allocate(meshlet_triangle_bytes);
		}
		meshlet_triangle_buffer.ensure_capacity(meshlet_triangle_allocator.get_capacity());
	}

	result.meshlet_range = meshlet_allocator.allocate(meshlet_count);
	while (!result.meshlet_range.is_valid()) {
		meshlet_allocator.grow(MAX(meshlet_allocator.get_capacity() * 2, meshlet_allocator.get_capacity() + meshlet_count));
		result.meshlet_range = meshlet_allocator.allocate(meshlet_count);
	}
	meshlet_descriptor_buffer.ensure_capacity(meshlet_allocator.get_capacity());

	// Upload meshlet vertex remap, rebased onto the (shared) global vertex range.
	{
		LocalVector<uint32_t> remap;
		remap.resize(meshlet_vertex_count);
		for (uint32_t i = 0; i < meshlet_vertex_count; i++) {
			remap[i] = (uint32_t)p_meshlet_vertices[i] + p_vertex_range.offset;
		}
		meshlet_vertex_buffer.upload(result.meshlet_vertex_range.offset, remap.ptr(), meshlet_vertex_count);
	}

	// Upload meshlet triangle indices verbatim (they're meshlet-local, unaffected by global offsets).
	if (meshlet_triangle_bytes > 0) {
		meshlet_triangle_buffer.upload(result.meshlet_triangle_range.offset, p_meshlet_triangles.ptr(), (uint32_t)p_meshlet_triangles.size());
	}

	// Upload meshlet descriptors, with offsets rebased onto the global meshlet-vertex/triangle ranges.
	{
		LocalVector<MeshletDescriptorGPU> descriptors;
		descriptors.resize(meshlet_count);
		for (uint32_t i = 0; i < meshlet_count; i++) {
			const RenderingServerTypes::MeshletInfo &m = p_meshlets[i];
			const RenderingServerTypes::MeshletBoundsInfo &b = p_bounds[i];
			MeshletDescriptorGPU &d = descriptors[i];
			d.bounds_center[0] = b.center[0];
			d.bounds_center[1] = b.center[1];
			d.bounds_center[2] = b.center[2];
			d.bounds_radius = b.radius;
			d.cone_axis[0] = b.cone_axis[0];
			d.cone_axis[1] = b.cone_axis[1];
			d.cone_axis[2] = b.cone_axis[2];
			d.cone_cutoff = b.cone_cutoff;
			d.vertex_remap_offset = result.meshlet_vertex_range.offset + m.vertex_offset;
			d.triangle_offset = result.meshlet_triangle_range.offset + m.triangle_offset;
			d.vertex_count = m.vertex_count;
			d.triangle_count = m.triangle_count;
		}
		meshlet_descriptor_buffer.upload(result.meshlet_range.offset, descriptors.ptr(), meshlet_count);
	}

	return result;
}

void MeshletStorage::free_meshlets(const UploadResult &p_result) {
	meshlet_vertex_allocator.free(p_result.meshlet_vertex_range);
	meshlet_triangle_allocator.free(p_result.meshlet_triangle_range);
	meshlet_allocator.free(p_result.meshlet_range);
}

MeshletStorage::MeshletDescriptorGPU MeshletStorage::debug_get_meshlet_descriptor(uint32_t p_global_meshlet_index) const {
	MeshletDescriptorGPU descriptor;
	Vector<uint8_t> data = meshlet_descriptor_buffer.read_back(p_global_meshlet_index, 1);
	if ((uint32_t)data.size() >= sizeof(MeshletDescriptorGPU)) {
		memcpy(&descriptor, data.ptr(), sizeof(MeshletDescriptorGPU));
	}
	return descriptor;
}

uint32_t MeshletStorage::debug_get_meshlet_vertex_remap(uint32_t p_global_remap_index) const {
	Vector<uint8_t> data = meshlet_vertex_buffer.read_back(p_global_remap_index, 1);
	uint32_t result = 0;
	if ((uint32_t)data.size() >= sizeof(uint32_t)) {
		memcpy(&result, data.ptr(), sizeof(uint32_t));
	}
	return result;
}

uint8_t MeshletStorage::debug_get_meshlet_triangle_index(uint32_t p_global_byte_index) const {
	Vector<uint8_t> data = meshlet_triangle_buffer.read_back(p_global_byte_index, 1);
	return data.is_empty() ? 0 : data[0];
}

Vector3 MeshletStorage::debug_get_vertex_position(uint32_t p_global_vertex_index) const {
	Vector<uint8_t> data = vertex_position_buffer.read_back(p_global_vertex_index, 1);
	Vector3 result;
	if ((uint32_t)data.size() >= sizeof(float) * 3) {
		float values[3];
		memcpy(values, data.ptr(), sizeof(float) * 3);
		result = Vector3(values[0], values[1], values[2]);
	}
	return result;
}
