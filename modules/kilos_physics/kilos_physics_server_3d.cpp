#include "kilos_physics_server_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/object.h"
#include "core/object/object_id.h"
#include "core/templates/sort_array.h"

#include "shaders/body_to_multimesh.glsl.gen.h"
#include "shaders/grid_build.glsl.gen.h"
#include "shaders/integration.glsl.gen.h"
#include "shaders/pbd_finalize.glsl.gen.h"
#include "shaders/pbd_solve.glsl.gen.h"

#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/rendering_device_binds.h"

RID KilosPhysicsServer3D::world_boundary_shape_create() {
	KilosShape s;
	s.type = SHAPE_WORLD_BOUNDARY;
	return shape_owner.make_rid(s);
}

RID KilosPhysicsServer3D::separation_ray_shape_create() {
	KilosShape s;
	s.type = SHAPE_SEPARATION_RAY;
	return shape_owner.make_rid(s);
}

RID KilosPhysicsServer3D::sphere_shape_create() {
	KilosShape s;
	s.type = SHAPE_SPHERE;
	return shape_owner.make_rid(s);
}

RID KilosPhysicsServer3D::box_shape_create() {
	KilosShape s;
	s.type = SHAPE_BOX;
	return shape_owner.make_rid(s);
}

RID KilosPhysicsServer3D::capsule_shape_create() {
	KilosShape s;
	s.type = SHAPE_CAPSULE;
	return shape_owner.make_rid(s);
}

RID KilosPhysicsServer3D::cylinder_shape_create() {
	KilosShape s;
	s.type = SHAPE_CYLINDER;
	return shape_owner.make_rid(s);
}

RID KilosPhysicsServer3D::convex_polygon_shape_create() {
	KilosShape s;
	s.type = SHAPE_CONVEX_POLYGON;
	return shape_owner.make_rid(s);
}

RID KilosPhysicsServer3D::concave_polygon_shape_create() {
	KilosShape s;
	s.type = SHAPE_CONCAVE_POLYGON;
	return shape_owner.make_rid(s);
}

RID KilosPhysicsServer3D::heightmap_shape_create() {
	KilosShape s;
	s.type = SHAPE_HEIGHTMAP;
	return shape_owner.make_rid(s);
}

RID KilosPhysicsServer3D::custom_shape_create() {
	KilosShape s;
	s.type = SHAPE_CUSTOM;
	return shape_owner.make_rid(s);
}

void KilosPhysicsServer3D::shape_set_data(RID p_shape, const Variant &p_data) {
	MutexLock lock(collision_mutex);
	KilosShape *s = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(s);
	switch (s->type) {
		case SHAPE_CONCAVE_POLYGON: {
			Dictionary d = p_data;
			if (d.has("faces")) {
				PackedVector3Array pf = d["faces"];
				const int n = pf.size();
				const Vector3 *r = pf.ptr();
				AABB bounds;
				for (int i = 0; i < n; i++) {
					if (i == 0) {
						bounds.position = r[0];
					} else {
						bounds.expand_to(r[i]);
					}
				}
				s->local_aabb = bounds;
				// Quantize the soup to 16-bit per axis (half the memory of floats).
				const Vector3 inv_sz(
						bounds.size.x > 0 ? 1.0 / bounds.size.x : 0.0,
						bounds.size.y > 0 ? 1.0 / bounds.size.y : 0.0,
						bounds.size.z > 0 ? 1.0 / bounds.size.z : 0.0);
				s->qfaces.resize(n * 3);
				uint16_t *q = s->qfaces.ptrw();
				for (int i = 0; i < n; i++) {
					const Vector3 rel = r[i] - bounds.position;
					q[i * 3 + 0] = (uint16_t)CLAMP(Math::round(rel.x * inv_sz.x * 65535.0), 0.0, 65535.0);
					q[i * 3 + 1] = (uint16_t)CLAMP(Math::round(rel.y * inv_sz.y * 65535.0), 0.0, 65535.0);
					q[i * 3 + 2] = (uint16_t)CLAMP(Math::round(rel.z * inv_sz.z * 65535.0), 0.0, 65535.0);
				}
				s->backface = d.has("backface_collision") ? (bool)d["backface_collision"] : false;
				// The per-triangle BVH is built lazily on the first raycast, so
				// chunks that are never queried (underground/air) cost nothing.
				s->bvh.clear();
				s->tri_order.clear();
			}
		} break;
		case SHAPE_SPHERE: {
			s->radius = p_data;
			s->local_aabb = AABB(Vector3(-s->radius, -s->radius, -s->radius), Vector3(2, 2, 2) * s->radius);
		} break;
		case SHAPE_BOX: {
			Vector3 he = p_data; // half-extents
			s->box_half = he;
			s->local_aabb = AABB(-he, he * 2.0);
		} break;
		case SHAPE_CAPSULE: {
			Dictionary d = p_data;
			s->radius = d.has("radius") ? (real_t)d["radius"] : 0.5;
			s->height = d.has("height") ? (real_t)d["height"] : 1.0; // total height incl. caps
			real_t r = s->radius, hh = MAX(s->height * 0.5, r);
			s->local_aabb = AABB(Vector3(-r, -hh, -r), Vector3(2 * r, 2 * hh, 2 * r));
		} break;
		case SHAPE_CYLINDER: {
			Dictionary d = p_data;
			s->radius = d.has("radius") ? (real_t)d["radius"] : 0.5;
			s->height = d.has("height") ? (real_t)d["height"] : 1.0;
			real_t r = s->radius, hh = s->height * 0.5;
			s->local_aabb = AABB(Vector3(-r, -hh, -r), Vector3(2 * r, 2 * hh, 2 * r));
		} break;
		case SHAPE_CONVEX_POLYGON: {
			// Approximated by its AABB (treated as a box) for queries.
			PackedVector3Array pts = p_data;
			AABB bounds;
			const Vector3 *r = pts.ptr();
			for (int i = 0; i < pts.size(); i++) {
				if (i == 0) {
					bounds.position = r[0];
				} else {
					bounds.expand_to(r[i]);
				}
			}
			s->local_aabb = bounds;
			s->box_half = bounds.size * 0.5;
			s->center = bounds.position + bounds.size * 0.5;
		} break;
		default:
			break;
	}
}

void KilosPhysicsServer3D::shape_set_custom_solver_bias(RID p_shape, real_t p_bias) {
}

PhysicsServer3D::ShapeType KilosPhysicsServer3D::shape_get_type(RID p_shape) const {
	KilosShape *s = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL_V(s, SHAPE_CUSTOM);
	return s->type;
}

Variant KilosPhysicsServer3D::shape_get_data(RID p_shape) const {
	return Variant();
}

void KilosPhysicsServer3D::shape_set_margin(RID p_shape, real_t p_margin) {
}

real_t KilosPhysicsServer3D::shape_get_margin(RID p_shape) const {
	return 0;
}

real_t KilosPhysicsServer3D::shape_get_custom_solver_bias(RID p_shape) const {
	return 0;
}

RID KilosPhysicsServer3D::space_create() {
	MutexLock lock(collision_mutex);
	return space_owner.make_rid(KilosSpace());
}

void KilosPhysicsServer3D::space_set_active(RID p_space, bool p_active) {
}

bool KilosPhysicsServer3D::space_is_active(RID p_space) const {
	return false;
}

void KilosPhysicsServer3D::space_set_param(RID p_space, SpaceParameter p_param, real_t p_value) {
}

real_t KilosPhysicsServer3D::space_get_param(RID p_space, SpaceParameter p_param) const {
	return 0;
}

PhysicsDirectSpaceState3D * KilosPhysicsServer3D::space_get_direct_state(RID p_space) {
	if (!space_state) {
		space_state = memnew(KilosDirectSpaceState3D);
	}
	space_state->server = this;
	space_state->space = p_space;
	return space_state;
}

void KilosPhysicsServer3D::space_set_debug_contacts(RID p_space, int p_max_contacts) {
}

Vector<Vector3> KilosPhysicsServer3D::space_get_contacts(RID p_space) const {
	return Vector<Vector3>();
}

int KilosPhysicsServer3D::space_get_contact_count(RID p_space) const {
	return 0;
}

RID KilosPhysicsServer3D::area_create() {
	return area_owner.make_rid(KilosArea());
}

void KilosPhysicsServer3D::area_set_space(RID p_area, RID p_space) {
}

RID KilosPhysicsServer3D::area_get_space(RID p_area) const {
	return RID();
}

void KilosPhysicsServer3D::area_add_shape(RID p_area, RID p_shape, const Transform3D &p_transform, bool p_disabled) {
}

void KilosPhysicsServer3D::area_set_shape(RID p_area, int p_shape_idx, RID p_shape) {
}

void KilosPhysicsServer3D::area_set_shape_transform(RID p_area, int p_shape_idx, const Transform3D &p_transform) {
}

int KilosPhysicsServer3D::area_get_shape_count(RID p_area) const {
	return 0;
}

RID KilosPhysicsServer3D::area_get_shape(RID p_area, int p_shape_idx) const {
	return RID();
}

Transform3D KilosPhysicsServer3D::area_get_shape_transform(RID p_area, int p_shape_idx) const {
	return Transform3D();
}

void KilosPhysicsServer3D::area_remove_shape(RID p_area, int p_shape_idx) {
}

void KilosPhysicsServer3D::area_clear_shapes(RID p_area) {
}

void KilosPhysicsServer3D::area_set_shape_disabled(RID p_area, int p_shape_idx, bool p_disabled) {
}

void KilosPhysicsServer3D::area_attach_object_instance_id(RID p_area, ObjectID p_id) {
}

RID KilosPhysicsServer3D::meshlet_collider_create() {
	MeshletCollider mc;
	return meshlet_collider_owner.make_rid(mc);
}

void KilosPhysicsServer3D::meshlet_collider_set_data(RID p_collider, RID p_mesh, const Transform3D &p_transform) {
	MeshletCollider *mc = meshlet_collider_owner.get_or_null(p_collider);
	ERR_FAIL_NULL(mc);
	mc->mesh = p_mesh;
	mc->transform = p_transform;
}

void KilosPhysicsServer3D::meshlet_collider_set_transform(RID p_collider, const Transform3D &p_transform) {
	MeshletCollider *mc = meshlet_collider_owner.get_or_null(p_collider);
	ERR_FAIL_NULL(mc);
	mc->transform = p_transform;
}

void KilosPhysicsServer3D::meshlet_collider_clear(RID p_collider) {
	meshlet_collider_owner.free(p_collider);
}

/* BULK BODY API */

int KilosPhysicsServer3D::bulk_body_create(int p_count) {
	if (p_count <= 0) {
		return -1;
	}
	const uint32_t base = body_high_water;
	body_high_water += (uint32_t)p_count;
	if ((uint32_t)body_data.size() < body_high_water) {
		body_data.resize(body_high_water);
	}
	for (uint32_t i = 0; i < (uint32_t)p_count; i++) {
		RigidBodyData &d = body_data[base + i];
		memset(&d, 0, sizeof(RigidBodyData));
		d.rotation[3] = 1.0f; // identity quaternion
		d.linear_velocity[3] = 1.0f; // mass 1 => dynamic
		d.angular_velocity[3] = 1.0f; // inverse mass
	}

	KilosBulk bulk;
	bulk.base = base;
	bulk.count = (uint32_t)p_count;
	bulk_ranges[(int)base] = bulk;

	RangeDirty r;
	r.base = base;
	r.count = (uint32_t)p_count;
	dirty_ranges.push_back(r);
	return (int)base;
}

void KilosPhysicsServer3D::bulk_body_scatter(int p_handle, const AABB &p_region) {
	KilosBulk *bulk = bulk_ranges.getptr(p_handle);
	ERR_FAIL_NULL(bulk);

	const Vector3 pos0 = p_region.position;
	const Vector3 size = p_region.size;
	const uint32_t seed = 0x9e3779b9u ^ (uint32_t)p_handle;
	for (uint32_t i = 0; i < bulk->count; i++) {
		// Cheap integer hash -> 3 independent [0,1) values per body.
		auto hash = [](uint32_t x) -> uint32_t {
			x ^= x >> 16;
			x *= 0x7feb352du;
			x ^= x >> 15;
			x *= 0x846ca68bu;
			x ^= x >> 16;
			return x;
		};
		const uint32_t s = (seed + i) * 3u;
		const float rx = (hash(s + 0u) & 0xffffffu) / float(0x1000000);
		const float ry = (hash(s + 1u) & 0xffffffu) / float(0x1000000);
		const float rz = (hash(s + 2u) & 0xffffffu) / float(0x1000000);
		RigidBodyData &d = body_data[bulk->base + i];
		d.position[0] = pos0.x + rx * size.x;
		d.position[1] = pos0.y + ry * size.y;
		d.position[2] = pos0.z + rz * size.z;
		// Reset velocity too, so scatter also works as a clean respawn.
		d.linear_velocity[0] = 0.0f;
		d.linear_velocity[1] = 0.0f;
		d.linear_velocity[2] = 0.0f;
	}

	RangeDirty r;
	r.base = bulk->base;
	r.count = bulk->count;
	dirty_ranges.push_back(r);
}

void KilosPhysicsServer3D::bulk_body_set_multimesh(int p_handle, RID p_multimesh) {
	KilosBulk *bulk = bulk_ranges.getptr(p_handle);
	ERR_FAIL_NULL(bulk);
	bulk->multimesh = p_multimesh;
}

void KilosPhysicsServer3D::bulk_body_free(int p_handle) {
	KilosBulk *bulk = bulk_ranges.getptr(p_handle);
	if (!bulk) {
		return;
	}
	// Mark the range inert (mass 0) so the integrator skips it. Slots are not
	// reclaimed in P2 (no fragmentation handling yet).
	for (uint32_t i = 0; i < bulk->count; i++) {
		body_data[bulk->base + i].linear_velocity[3] = 0.0f;
	}
	RangeDirty r;
	r.base = bulk->base;
	r.count = bulk->count;
	dirty_ranges.push_back(r);
	bulk_ranges.erase(p_handle);
}

void KilosPhysicsServer3D::bulk_set_collision(bool p_enabled, real_t p_radius, real_t p_ground_y, int p_iterations) {
	collision_enabled = p_enabled;
	body_radius = MAX(0.001f, (float)p_radius);
	ground_y = (float)p_ground_y;
	solve_iterations = MAX(1, p_iterations);
}

void KilosPhysicsServer3D::bulk_set_sdf(RID p_sdf_texture, const AABB &p_bounds) {
	sdf_texture = p_sdf_texture;
	sdf_bounds = p_bounds;
}

Vector3 KilosPhysicsServer3D::_debug_slot_position(int p_slot) const {
	if (p_slot < 0 || (uint32_t)p_slot >= (uint32_t)body_data.size()) {
		return Vector3();
	}
	const RigidBodyData &d = body_data[p_slot];
	return Vector3(d.position[0], d.position[1], d.position[2]);
}

ObjectID KilosPhysicsServer3D::area_get_object_instance_id(RID p_area) const {
	return ObjectID();
}

void KilosPhysicsServer3D::area_set_param(RID p_area, AreaParameter p_param, const Variant &p_value) {
}

void KilosPhysicsServer3D::area_set_transform(RID p_area, const Transform3D &p_transform) {
}

Variant KilosPhysicsServer3D::area_get_param(RID p_area, AreaParameter p_param) const {
	return Variant();
}

Transform3D KilosPhysicsServer3D::area_get_transform(RID p_area) const {
	return Transform3D();
}

void KilosPhysicsServer3D::area_set_ray_pickable(RID p_area, bool p_enable) {
}

void KilosPhysicsServer3D::area_set_collision_layer(RID p_area, uint32_t p_layer) {
}

uint32_t KilosPhysicsServer3D::area_get_collision_layer(RID p_area) const {
	return 0;
}

void KilosPhysicsServer3D::area_set_collision_mask(RID p_area, uint32_t p_mask) {
}

uint32_t KilosPhysicsServer3D::area_get_collision_mask(RID p_area) const {
	return 0;
}

void KilosPhysicsServer3D::area_set_monitorable(RID p_area, bool p_monitorable) {
}

void KilosPhysicsServer3D::area_set_monitor_callback(RID p_area, const Callable &p_callback) {
}

void KilosPhysicsServer3D::area_set_area_monitor_callback(RID p_area, const Callable &p_callback) {
}

uint32_t KilosPhysicsServer3D::_alloc_slot() {
	uint32_t slot;
	if (free_slots.size() > 0) {
		slot = free_slots[free_slots.size() - 1];
		free_slots.resize(free_slots.size() - 1);
	} else {
		slot = body_high_water++;
		if ((uint32_t)body_data.size() <= slot) {
			body_data.resize(slot + 1);
		}
	}
	// Fresh slot starts as a zeroed, static (mass 0) body so it is inert until configured.
	RigidBodyData &d = body_data[slot];
	memset(&d, 0, sizeof(RigidBodyData));
	d.rotation[3] = 1.0f; // identity quaternion
	_mark_dirty(slot);
	return slot;
}

void KilosPhysicsServer3D::_free_slot(uint32_t p_slot) {
	// Zero the slot's mass so the integrator treats it as inert until reused.
	if (p_slot < (uint32_t)body_data.size()) {
		memset(&body_data[p_slot], 0, sizeof(RigidBodyData));
		body_data[p_slot].rotation[3] = 1.0f;
		_mark_dirty(p_slot);
	}
	free_slots.push_back(p_slot);
}

void KilosPhysicsServer3D::_mark_dirty(uint32_t p_slot) {
	dirty_slots.push_back(p_slot);
}

RID KilosPhysicsServer3D::body_create() {
	MutexLock lock(collision_mutex);
	KilosBody body;
	body.slot = _alloc_slot();
	body.has_slot = true;
	return body_owner.make_rid(body);
}

void KilosPhysicsServer3D::body_set_space(RID p_body, RID p_space) {
	MutexLock lock(collision_mutex);
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	if (body->space == p_space) {
		return;
	}
	if (body->space.is_valid()) {
		KilosSpace *old = space_owner.get_or_null(body->space);
		if (old) {
			old->bodies.erase(p_body);
			old->bvh_dirty = true;
		}
	}
	body->space = p_space;
	if (p_space.is_valid()) {
		KilosSpace *sp = space_owner.get_or_null(p_space);
		if (sp) {
			sp->bodies.insert(p_body);
			if (!sp->bvh_dirty) {
				// BVH already built: defer via the pending list, fold in periodically.
				sp->bvh_pending.push_back(p_body);
				if (sp->bvh_pending.size() > 64) {
					sp->bvh_dirty = true;
				}
			}
		}
	}
	body->active = p_space.is_valid();
}

RID KilosPhysicsServer3D::body_get_space(RID p_body) const {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, RID());
	return body->space;
}

void KilosPhysicsServer3D::body_set_mode(RID p_body, BodyMode p_mode) {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->mode = p_mode;
	if (body->has_slot) {
		const bool dynamic = (p_mode != BODY_MODE_STATIC && p_mode != BODY_MODE_KINEMATIC);
		body_data[body->slot].linear_velocity[3] = dynamic ? (float)body->mass : 0.0f;
		body_data[body->slot].angular_velocity[3] = (dynamic && body->mass > 0.0) ? (float)(1.0 / body->mass) : 0.0f;
		_mark_dirty(body->slot);
	}
}

PhysicsServer3D::BodyMode KilosPhysicsServer3D::body_get_mode(RID p_body) const {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, BODY_MODE_STATIC);
	return body->mode;
}

void KilosPhysicsServer3D::_mark_space_dirty(RID p_space) {
	if (p_space.is_valid()) {
		KilosSpace *sp = space_owner.get_or_null(p_space);
		if (sp) {
			sp->bvh_dirty = true;
		}
	}
}

void KilosPhysicsServer3D::body_add_shape(RID p_body, RID p_shape, const Transform3D &p_transform, bool p_disabled) {
	MutexLock lock(collision_mutex);
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	KilosBody::BodyShape bs;
	bs.shape = p_shape;
	bs.xform = p_transform;
	bs.disabled = p_disabled;
	body->shapes.push_back(bs);
	// Shapes often added AFTER body_set_space (e.g. GridMap), so the broadphase
	// must be rebuilt or the body (empty when added) stays absent from it.
	_mark_space_dirty(body->space);
}

void KilosPhysicsServer3D::body_set_shape(RID p_body, int p_shape_idx, RID p_shape) {
	MutexLock lock(collision_mutex);
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	ERR_FAIL_INDEX(p_shape_idx, body->shapes.size());
	body->shapes.write[p_shape_idx].shape = p_shape;
	_mark_space_dirty(body->space);
}

void KilosPhysicsServer3D::body_set_shape_transform(RID p_body, int p_shape_idx, const Transform3D &p_transform) {
	MutexLock lock(collision_mutex);
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	ERR_FAIL_INDEX(p_shape_idx, body->shapes.size());
	body->shapes.write[p_shape_idx].xform = p_transform;
	_mark_space_dirty(body->space);
}

int KilosPhysicsServer3D::body_get_shape_count(RID p_body) const {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);
	return body->shapes.size();
}

RID KilosPhysicsServer3D::body_get_shape(RID p_body, int p_shape_idx) const {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, RID());
	ERR_FAIL_INDEX_V(p_shape_idx, body->shapes.size(), RID());
	return body->shapes[p_shape_idx].shape;
}

Transform3D KilosPhysicsServer3D::body_get_shape_transform(RID p_body, int p_shape_idx) const {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Transform3D());
	ERR_FAIL_INDEX_V(p_shape_idx, body->shapes.size(), Transform3D());
	return body->shapes[p_shape_idx].xform;
}

void KilosPhysicsServer3D::body_set_shape_disabled(RID p_body, int p_shape_idx, bool p_disabled) {
	MutexLock lock(collision_mutex);
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	ERR_FAIL_INDEX(p_shape_idx, body->shapes.size());
	body->shapes.write[p_shape_idx].disabled = p_disabled;
}

void KilosPhysicsServer3D::body_remove_shape(RID p_body, int p_shape_idx) {
	MutexLock lock(collision_mutex);
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	ERR_FAIL_INDEX(p_shape_idx, body->shapes.size());
	body->shapes.remove_at(p_shape_idx);
	_mark_space_dirty(body->space);
}

void KilosPhysicsServer3D::body_clear_shapes(RID p_body) {
	MutexLock lock(collision_mutex);
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->shapes.clear();
	_mark_space_dirty(body->space);
}

void KilosPhysicsServer3D::body_attach_object_instance_id(RID p_body, ObjectID p_id) {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->instance_id = p_id;
}

ObjectID KilosPhysicsServer3D::body_get_object_instance_id(RID p_body) const {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, ObjectID());
	return body->instance_id;
}

void KilosPhysicsServer3D::body_set_enable_continuous_collision_detection(RID p_body, bool p_enable) {
}

bool KilosPhysicsServer3D::body_is_continuous_collision_detection_enabled(RID p_body) const {
	return false;
}

void KilosPhysicsServer3D::body_set_collision_layer(RID p_body, uint32_t p_layer) {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->collision_layer = p_layer;
}

uint32_t KilosPhysicsServer3D::body_get_collision_layer(RID p_body) const {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);
	return body->collision_layer;
}

void KilosPhysicsServer3D::body_set_collision_mask(RID p_body, uint32_t p_mask) {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->collision_mask = p_mask;
}

uint32_t KilosPhysicsServer3D::body_get_collision_mask(RID p_body) const {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);
	return body->collision_mask;
}

void KilosPhysicsServer3D::body_set_collision_priority(RID p_body, real_t p_priority) {
}

real_t KilosPhysicsServer3D::body_get_collision_priority(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::body_set_user_flags(RID p_body, uint32_t p_flags) {
}

uint32_t KilosPhysicsServer3D::body_get_user_flags(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::body_set_param(RID p_body, BodyParameter p_param, const Variant &p_value) {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	if (p_param == BODY_PARAM_MASS) {
		body->mass = p_value;
		if (body->has_slot) {
			const bool dynamic = (body->mode != BODY_MODE_STATIC && body->mode != BODY_MODE_KINEMATIC);
			body_data[body->slot].linear_velocity[3] = dynamic ? (float)body->mass : 0.0f;
			body_data[body->slot].angular_velocity[3] = (dynamic && body->mass > 0.0) ? (float)(1.0 / body->mass) : 0.0f;
			_mark_dirty(body->slot);
		}
	} else if (p_param == BODY_PARAM_GRAVITY_SCALE) {
		body->gravity_scale = p_value;
	}
}

Variant KilosPhysicsServer3D::body_get_param(RID p_body, BodyParameter p_param) const {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Variant());
	if (p_param == BODY_PARAM_MASS) {
		return body->mass;
	} else if (p_param == BODY_PARAM_GRAVITY_SCALE) {
		return body->gravity_scale;
	}
	return Variant();
}

void KilosPhysicsServer3D::body_reset_mass_properties(RID p_body) {
}

void KilosPhysicsServer3D::body_set_state(RID p_body, BodyState p_state, const Variant &p_variant) {
	MutexLock lock(collision_mutex);
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	if (p_state == BODY_STATE_TRANSFORM) {
		Transform3D t = p_variant;
		body->transform = t;
		if (body->has_slot) {
			RigidBodyData &d = body_data[body->slot];
			d.position[0] = t.origin.x;
			d.position[1] = t.origin.y;
			d.position[2] = t.origin.z;

			Quaternion q(t.basis.get_rotation_quaternion());
			d.rotation[0] = q.x;
			d.rotation[1] = q.y;
			d.rotation[2] = q.z;
			d.rotation[3] = q.w;
			_mark_dirty(body->slot);
		}
		// Static colliders (e.g. GridMap octants) can be repositioned once; refresh
		// their broadphase entry. Skip kinematic/dynamic bodies (the player) so
		// per-frame moves don't thrash the BVH.
		if (body->mode == BODY_MODE_STATIC) {
			_mark_space_dirty(body->space);
		}
	} else if (p_state == BODY_STATE_LINEAR_VELOCITY) {
		Vector3 v = p_variant;
		body->linear_velocity = v;
		if (body->has_slot) {
			RigidBodyData &d = body_data[body->slot];
			d.linear_velocity[0] = v.x;
			d.linear_velocity[1] = v.y;
			d.linear_velocity[2] = v.z;
			_mark_dirty(body->slot);
		}
	} else if (p_state == BODY_STATE_ANGULAR_VELOCITY) {
		Vector3 v = p_variant;
		body->angular_velocity = v;
		if (body->has_slot) {
			RigidBodyData &d = body_data[body->slot];
			d.angular_velocity[0] = v.x;
			d.angular_velocity[1] = v.y;
			d.angular_velocity[2] = v.z;
			_mark_dirty(body->slot);
		}
	}
}

Variant KilosPhysicsServer3D::body_get_state(RID p_body, BodyState p_state) const {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Variant());

	if (!body->has_slot) {
		return Variant();
	}

	// Prefer the most recent GPU readback for tracked bodies; otherwise fall
	// back to the CPU shadow (the last value we uploaded).
	RigidBodyData d = body_data[body->slot];
	if (body->tracked) {
		MutexLock lock(readback_mutex);
		const RigidBodyData *r = readback_results.getptr(body->slot);
		if (r) {
			d = *r;
		}
	}

	if (p_state == BODY_STATE_TRANSFORM) {
		Vector3 pos(d.position[0], d.position[1], d.position[2]);
		Quaternion q(d.rotation[0], d.rotation[1], d.rotation[2], d.rotation[3]);
		if (!q.is_normalized()) {
			q = Quaternion(); // guard against an uninitialized/degenerate readback
		}
		return Transform3D(Basis(q), pos);
	} else if (p_state == BODY_STATE_LINEAR_VELOCITY) {
		return Vector3(d.linear_velocity[0], d.linear_velocity[1], d.linear_velocity[2]);
	} else if (p_state == BODY_STATE_ANGULAR_VELOCITY) {
		return Vector3(d.angular_velocity[0], d.angular_velocity[1], d.angular_velocity[2]);
	}
	return Variant();
}

void KilosPhysicsServer3D::body_apply_central_impulse(RID p_body, const Vector3 &p_impulse) {
}

void KilosPhysicsServer3D::body_apply_impulse(RID p_body, const Vector3 &p_impulse, const Vector3 &p_position) {
}

void KilosPhysicsServer3D::body_apply_torque_impulse(RID p_body, const Vector3 &p_impulse) {
}

void KilosPhysicsServer3D::soft_body_apply_point_impulse(RID p_body, int p_point_index, const Vector3 &p_impulse) {
}

void KilosPhysicsServer3D::soft_body_apply_point_force(RID p_body, int p_point_index, const Vector3 &p_force) {
}

void KilosPhysicsServer3D::soft_body_apply_central_impulse(RID p_body, const Vector3 &p_impulse) {
}

void KilosPhysicsServer3D::soft_body_apply_central_force(RID p_body, const Vector3 &p_force) {
}

void KilosPhysicsServer3D::body_apply_central_force(RID p_body, const Vector3 &p_force) {
}

void KilosPhysicsServer3D::body_apply_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position) {
}

void KilosPhysicsServer3D::body_apply_torque(RID p_body, const Vector3 &p_torque) {
}

void KilosPhysicsServer3D::body_add_constant_central_force(RID p_body, const Vector3 &p_force) {
}

void KilosPhysicsServer3D::body_add_constant_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position) {
}

void KilosPhysicsServer3D::body_add_constant_torque(RID p_body, const Vector3 &p_torque) {
}

void KilosPhysicsServer3D::body_set_constant_force(RID p_body, const Vector3 &p_force) {
}

Vector3 KilosPhysicsServer3D::body_get_constant_force(RID p_body) const {
	return Vector3();
}

void KilosPhysicsServer3D::body_set_constant_torque(RID p_body, const Vector3 &p_torque) {
}

Vector3 KilosPhysicsServer3D::body_get_constant_torque(RID p_body) const {
	return Vector3();
}

void KilosPhysicsServer3D::body_set_axis_velocity(RID p_body, const Vector3 &p_axis_velocity) {
}

void KilosPhysicsServer3D::body_set_axis_lock(RID p_body, BodyAxis p_axis, bool p_lock) {
}

bool KilosPhysicsServer3D::body_is_axis_locked(RID p_body, BodyAxis p_axis) const {
	return false;
}

void KilosPhysicsServer3D::body_add_collision_exception(RID p_body, RID p_body_b) {
}

void KilosPhysicsServer3D::body_remove_collision_exception(RID p_body, RID p_body_b) {
}

void KilosPhysicsServer3D::body_get_collision_exceptions(RID p_body, List<RID> *p_exceptions) {
}

void KilosPhysicsServer3D::body_set_contacts_reported_depth_threshold(RID p_body, real_t p_threshold) {
}

real_t KilosPhysicsServer3D::body_get_contacts_reported_depth_threshold(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::body_set_omit_force_integration(RID p_body, bool p_omit) {
}

bool KilosPhysicsServer3D::body_is_omitting_force_integration(RID p_body) const {
	return false;
}

void KilosPhysicsServer3D::body_set_max_contacts_reported(RID p_body, int p_contacts) {
}

int KilosPhysicsServer3D::body_get_max_contacts_reported(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::body_set_state_sync_callback(RID p_body, const Callable &p_callable) {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	if (body->state_sync_callback.is_null() && !p_callable.is_null()) {
		active_bodies.push_back(p_body);
		if (body->has_slot && !body->tracked) {
			body->tracked = true;
			tracked_slots.push_back(body->slot);
		}
	} else if (!body->state_sync_callback.is_null() && p_callable.is_null()) {
		active_bodies.erase(p_body);
		if (body->tracked) {
			body->tracked = false;
			tracked_slots.erase(body->slot);
		}
	}
	body->state_sync_callback = p_callable;
}

void KilosPhysicsServer3D::body_set_force_integration_callback(RID p_body, const Callable &p_callable, const Variant &p_udata) {
}

void KilosPhysicsServer3D::body_set_ray_pickable(RID p_body, bool p_enable) {
}

bool KilosPhysicsServer3D::body_test_motion(RID p_body, const MotionParameters &p_parameters, MotionResult *r_result) {
	// Approximate swept motion of a capsule/sphere body: cast a ray from each of
	// the capsule's two sphere centres along the motion and stop at the earliest
	// contact, accounting for the radius. Approximate but enough for a character
	// to stand and walk on static trimesh terrain.
	auto no_collision = [&]() -> bool {
		if (r_result) {
			r_result->travel = p_parameters.motion;
			r_result->remainder = Vector3();
			r_result->collision_safe_fraction = 1.0;
			r_result->collision_unsafe_fraction = 1.0;
			r_result->collision_count = 0;
		}
		return false;
	};

	MutexLock lock(collision_mutex);
	KilosBody *body = body_owner.get_or_null(p_body);
	if (!body) {
		return no_collision();
	}
	KilosSpace *space = space_owner.get_or_null(body->space);
	const Vector3 motion = p_parameters.motion;
	const real_t motion_len = motion.length();
	const real_t margin = p_parameters.margin;
	if (!space || motion_len < 1e-6) {
		return no_collision();
	}

	// First capsule/sphere shape on the body.
	real_t radius = 0.0;
	real_t cap_h = 0.0;
	Transform3D shape_xform;
	bool found = false;
	for (int i = 0; i < body->shapes.size(); i++) {
		KilosShape *sh = shape_owner.get_or_null(body->shapes[i].shape);
		if (sh && (sh->type == SHAPE_CAPSULE || sh->type == SHAPE_SPHERE)) {
			radius = sh->radius;
			cap_h = (sh->type == SHAPE_CAPSULE) ? sh->height : (2.0 * sh->radius);
			shape_xform = body->shapes[i].xform;
			found = true;
			break;
		}
	}
	if (!found) {
		return no_collision();
	}

	const Transform3D world = p_parameters.from * shape_xform;
	const real_t half = MAX((real_t)0.0, cap_h * 0.5 - radius);
	const Vector3 centers_local[2] = { Vector3(0, half, 0), Vector3(0, -half, 0) };
	const Vector3 dir = motion / motion_len;

	real_t best_safe = 1.0;
	bool any_hit = false;
	PhysicsServer3D::MotionCollision best_col;

	for (int i = 0; i < 2; i++) {
		const Vector3 c = world.xform(centers_local[i]);
		PhysicsDirectSpaceState3D::RayParameters rp;
		rp.from = c;
		rp.to = c + dir * (motion_len + radius + margin);
		rp.exclude.insert(p_body);
		rp.collision_mask = body->collision_mask;
		rp.hit_back_faces = true;
		PhysicsDirectSpaceState3D::RayResult rr;
		if (_intersect_ray_unlocked(body->space, rp, rr)) {
			const real_t hit_dist = c.distance_to(rr.position);
			// A sphere of radius r moving along dir touches a surface with normal n
			// when its centre is r/|dir.n| from the hit point (not r). On flat ground
			// |dir.n|=1; on slopes it's < 1, so using plain r let the capsule sink.
			const real_t ndot = MAX((real_t)0.25, Math::abs(dir.dot(rr.normal)));
			real_t allowed = hit_dist - radius / ndot - margin;
			if (allowed < 0.0) {
				allowed = 0.0;
			}
			const real_t safe = allowed / motion_len;
			if (safe < best_safe) {
				best_safe = safe;
				any_hit = true;
				best_col.normal = rr.normal;
				best_col.position = rr.position;
				best_col.collider = rr.rid;
				best_col.collider_id = rr.collider_id;
				best_col.collider_shape = rr.shape;
				best_col.local_shape = 0;
				best_col.depth = margin;
			}
		}
	}

	best_safe = CLAMP(best_safe, (real_t)0.0, (real_t)1.0);
	if (r_result) {
		r_result->travel = motion * best_safe;
		r_result->remainder = motion - r_result->travel;
		r_result->collision_safe_fraction = best_safe;
		r_result->collision_unsafe_fraction = best_safe;
		r_result->collision_depth = margin;
		if (any_hit) {
			r_result->collisions[0] = best_col;
			r_result->collision_count = 1;
		} else {
			r_result->collision_count = 0;
		}
	}
	return any_hit;
}

PhysicsDirectBodyState3D * KilosPhysicsServer3D::body_get_direct_state(RID p_body) {
	if (!direct_state) {
		direct_state = memnew(KilosDirectBodyState3D);
		direct_state->server = this;
	}
	direct_state->body = p_body;
	return direct_state;
}

RID KilosPhysicsServer3D::soft_body_create() {
	return body_owner.make_rid(KilosBody());
}

void KilosPhysicsServer3D::soft_body_update_rendering_server(RID p_body, RequiredParam<PhysicsServer3DRenderingServerHandler> rp_rendering_server_handler) {
}

void KilosPhysicsServer3D::soft_body_set_space(RID p_body, RID p_space) {
}

RID KilosPhysicsServer3D::soft_body_get_space(RID p_body) const {
	return RID();
}

void KilosPhysicsServer3D::soft_body_set_collision_layer(RID p_body, uint32_t p_layer) {
}

uint32_t KilosPhysicsServer3D::soft_body_get_collision_layer(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::soft_body_set_collision_mask(RID p_body, uint32_t p_mask) {
}

uint32_t KilosPhysicsServer3D::soft_body_get_collision_mask(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::soft_body_add_collision_exception(RID p_body, RID p_body_b) {
}

void KilosPhysicsServer3D::soft_body_remove_collision_exception(RID p_body, RID p_body_b) {
}

void KilosPhysicsServer3D::soft_body_get_collision_exceptions(RID p_body, List<RID> *p_exceptions) {
}

void KilosPhysicsServer3D::soft_body_set_state(RID p_body, BodyState p_state, const Variant &p_variant) {
}

Variant KilosPhysicsServer3D::soft_body_get_state(RID p_body, BodyState p_state) const {
	return Variant();
}

void KilosPhysicsServer3D::soft_body_set_transform(RID p_body, const Transform3D &p_transform) {
}

void KilosPhysicsServer3D::soft_body_set_ray_pickable(RID p_body, bool p_enable) {
}

void KilosPhysicsServer3D::soft_body_set_simulation_precision(RID p_body, int p_simulation_precision) {
}

int KilosPhysicsServer3D::soft_body_get_simulation_precision(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::soft_body_set_total_mass(RID p_body, real_t p_total_mass) {
}

real_t KilosPhysicsServer3D::soft_body_get_total_mass(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::soft_body_set_linear_stiffness(RID p_body, real_t p_stiffness) {
}

real_t KilosPhysicsServer3D::soft_body_get_linear_stiffness(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::soft_body_set_shrinking_factor(RID p_body, real_t p_shrinking_factor) {
}

real_t KilosPhysicsServer3D::soft_body_get_shrinking_factor(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::soft_body_set_pressure_coefficient(RID p_body, real_t p_pressure_coefficient) {
}

real_t KilosPhysicsServer3D::soft_body_get_pressure_coefficient(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::soft_body_set_damping_coefficient(RID p_body, real_t p_damping_coefficient) {
}

real_t KilosPhysicsServer3D::soft_body_get_damping_coefficient(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::soft_body_set_drag_coefficient(RID p_body, real_t p_drag_coefficient) {
}

real_t KilosPhysicsServer3D::soft_body_get_drag_coefficient(RID p_body) const {
	return 0;
}

void KilosPhysicsServer3D::soft_body_set_mesh(RID p_body, RID p_mesh) {
}

Vector3 KilosPhysicsServer3D::pin_joint_get_local_a(RID p_joint) const {
	return Vector3();
}

void KilosPhysicsServer3D::pin_joint_set_local_b(RID p_joint, const Vector3 &p_B) {
}

Vector3 KilosPhysicsServer3D::pin_joint_get_local_b(RID p_joint) const {
	return Vector3();
}

void KilosPhysicsServer3D::joint_make_pin(RID p_joint, RID p_body_A, const Vector3 &p_local_A, RID p_body_B, const Vector3 &p_local_B) {
}
void KilosPhysicsServer3D::joint_make_hinge_simple(RID p_joint, RID p_body_A, const Vector3 &p_pivot_A, const Vector3 &p_axis_A, RID p_body_B, const Vector3 &p_pivot_B, const Vector3 &p_axis_B) {
}

void KilosPhysicsServer3D::hinge_joint_set_param(RID p_joint, HingeJointParam p_param, real_t p_value) {
}

real_t KilosPhysicsServer3D::hinge_joint_get_param(RID p_joint, HingeJointParam p_param) const {
	return 0;
}

void KilosPhysicsServer3D::hinge_joint_set_flag(RID p_joint, HingeJointFlag p_flag, bool p_value) {
}

bool KilosPhysicsServer3D::hinge_joint_get_flag(RID p_joint, HingeJointFlag p_flag) const {
	return false;
}

void KilosPhysicsServer3D::joint_make_slider(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) {
}

void KilosPhysicsServer3D::slider_joint_set_param(RID p_joint, SliderJointParam p_param, real_t p_value) {
}

real_t KilosPhysicsServer3D::slider_joint_get_param(RID p_joint, SliderJointParam p_param) const {
	return 0;
}

void KilosPhysicsServer3D::joint_make_cone_twist(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) {
}

void KilosPhysicsServer3D::cone_twist_joint_set_param(RID p_joint, ConeTwistJointParam p_param, real_t p_value) {
}

real_t KilosPhysicsServer3D::cone_twist_joint_get_param(RID p_joint, ConeTwistJointParam p_param) const {
	return 0;
}

void KilosPhysicsServer3D::joint_make_generic_6dof(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) {
}

void KilosPhysicsServer3D::generic_6dof_joint_set_param(RID p_joint, Vector3::Axis, G6DOFJointAxisParam p_param, real_t p_value) {
}

real_t KilosPhysicsServer3D::generic_6dof_joint_get_param(RID p_joint, Vector3::Axis, G6DOFJointAxisParam p_param) const {
	return 0;
}

void KilosPhysicsServer3D::generic_6dof_joint_set_flag(RID p_joint, Vector3::Axis, G6DOFJointAxisFlag p_flag, bool p_enable) {
}

bool KilosPhysicsServer3D::generic_6dof_joint_get_flag(RID p_joint, Vector3::Axis, G6DOFJointAxisFlag p_flag) const {
	return false;
}

PhysicsServer3D::JointType KilosPhysicsServer3D::joint_get_type(RID p_joint) const {
	return PhysicsServer3D::JOINT_TYPE_PIN;
}

void KilosPhysicsServer3D::joint_set_solver_priority(RID p_joint, int p_priority) {
}

int KilosPhysicsServer3D::joint_get_solver_priority(RID p_joint) const {
	return 0;
}

void KilosPhysicsServer3D::joint_disable_collisions_between_bodies(RID p_joint, bool p_disable) {
}

bool KilosPhysicsServer3D::joint_is_disabled_collisions_between_bodies(RID p_joint) const {
	return false;
}

void KilosPhysicsServer3D::free_rid(RID p_rid) {
	MutexLock lock(collision_mutex);
	if (space_owner.owns(p_rid)) {
		space_owner.free(p_rid);
	} else if (body_owner.owns(p_rid)) {
		KilosBody *body = body_owner.get_or_null(p_rid);
		if (body) {
			if (body->space.is_valid()) {
				KilosSpace *sp = space_owner.get_or_null(body->space);
				if (sp) {
					sp->bodies.erase(p_rid);
					sp->bvh_dirty = true;
				}
			}
			if (body->tracked) {
				tracked_slots.erase(body->slot);
			}
			if (body->has_slot) {
				_free_slot(body->slot);
			}
		}
		active_bodies.erase(p_rid);
		body_owner.free(p_rid);
	} else if (shape_owner.owns(p_rid)) {
		shape_owner.free(p_rid);
	} else if (area_owner.owns(p_rid)) {
		area_owner.free(p_rid);
	} else if (joint_owner.owns(p_rid)) {
		joint_owner.free(p_rid);
	}
}

void KilosPhysicsServer3D::set_active(bool p_active) {
}

void KilosPhysicsServer3D::init() {
}

void KilosPhysicsServer3D::step(real_t p_step) {
	// Runs on the physics thread (or main thread if physics is single-threaded).
	// Must NOT touch the RenderingDevice here: we only assemble a StepJob and
	// hand it to the render thread, which owns all GPU state.
	StepJob job;
	job.dt = p_step;
	job.body_count = body_high_water;
	job.required_capacity = body_high_water;
	job.collision_enabled = collision_enabled;
	job.radius = body_radius;
	job.ground_y = ground_y;
	job.solve_iterations = (uint32_t)solve_iterations;
	job.sdf_texture = sdf_texture;
	job.sdf_bounds = sdf_bounds;

	// Snapshot the slots written since the last step. A slot may appear more
	// than once; the render thread applies them in order, last write wins.
	job.uploads.reserve(dirty_slots.size());
	for (int i = 0; i < dirty_slots.size(); i++) {
		const uint32_t slot = dirty_slots[i];
		if (slot < (uint32_t)body_data.size()) {
			BodyUpload u;
			u.index = slot;
			u.data = body_data[slot];
			job.uploads.push_back(u);
		}
	}
	dirty_slots.clear();

	// Slots to read back for CPU-side (tracked) bodies.
	job.tracked.reserve(tracked_slots.size());
	for (int i = 0; i < tracked_slots.size(); i++) {
		job.tracked.push_back(tracked_slots[i]);
	}

	// Contiguous range uploads (bulk spawn/scatter/free) - each becomes ONE
	// buffer_update instead of per-slot writes. Only carries data on the frame
	// the range changed, so there's no per-frame cost for a resident bulk.
	for (int i = 0; i < dirty_ranges.size(); i++) {
		const RangeDirty &r = dirty_ranges[i];
		RangeUpload ru;
		ru.base = r.base;
		ru.data.resize(r.count);
		for (uint32_t k = 0; k < r.count; k++) {
			ru.data[k] = body_data[r.base + k];
		}
		job.range_uploads.push_back(ru);
	}
	dirty_ranges.clear();

	// Every frame: convert each bound bulk range into its MultiMesh on the GPU.
	for (const KeyValue<int, KilosBulk> &E : bulk_ranges) {
		if (E.value.multimesh.is_valid()) {
			BulkBind bind;
			bind.base = E.value.base;
			bind.count = E.value.count;
			bind.multimesh = E.value.multimesh;
			job.bulk_binds.push_back(bind);
		}
	}

	{
		MutexLock lock(job_mutex);
		job_queue.push_back(job);
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs) {
		rs->call_on_render_thread(callable_mp(this, &KilosPhysicsServer3D::_render_process_step));
	}
}

void KilosPhysicsServer3D::sync() {
}

void KilosPhysicsServer3D::flush_queries() {
	for (int i = 0; i < active_bodies.size(); i++) {
		RID body_rid = active_bodies[i];
		KilosBody *body = body_owner.get_or_null(body_rid);
		if (body && !body->state_sync_callback.is_null() && body->active) {
			Variant direct_state_variant = body_get_direct_state(body_rid);
			const Variant *vp[1] = { &direct_state_variant };
			Callable::CallError ce;
			Variant rv;
			body->state_sync_callback.callp(vp, 1, rv, ce);
		}
	}
}

void KilosPhysicsServer3D::end_sync() {
}

void KilosPhysicsServer3D::finish() {
}

bool KilosPhysicsServer3D::is_flushing_queries() const {
	return true; // We are always ready to flush queries or we just pretend so that SceneTree will call flush_queries on us
}

int KilosPhysicsServer3D::get_process_info(ProcessInfo p_info) {
	return 0;
}


void KilosPhysicsServer3D::_rt_ensure_pipeline() {
	if (rt_pipeline_ready) {
		return;
	}
	rd = RenderingServer::get_singleton()->get_rendering_device();
	if (!rd) {
		return; // No global RD (e.g. dummy renderer) - stay inert.
	}

	Ref<RDShaderFile> shader_file;
	shader_file.instantiate();
	Error err = shader_file->parse_versions_from_text(String(integration_shader_glsl));
	if (err != OK) {
		shader_file->print_errors("KilosPhysics integration shader");
		return;
	}
	Vector<RD::ShaderStageSPIRVData> stages = shader_file->get_spirv_stages("");
	integrate_shader = rd->shader_create_from_spirv(stages);
	if (!integrate_shader.is_valid()) {
		return;
	}
	integrate_pipeline = rd->compute_pipeline_create(integrate_shader);
	rt_pipeline_ready = integrate_pipeline.is_valid();
}

void KilosPhysicsServer3D::_rt_ensure_capacity(uint32_t p_capacity) {
	if (p_capacity <= gpu_capacity && body_buffer.is_valid()) {
		return;
	}

	uint32_t new_capacity = MAX(p_capacity, INITIAL_CAPACITY);
	if (gpu_capacity > 0) {
		new_capacity = MAX(new_capacity, gpu_capacity * 2);
	}

	const uint32_t stride = sizeof(RigidBodyData);
	// Zero-initialize the whole new buffer so unused/tail slots are inert
	// (mass 0 => static => the integrator skips them).
	PackedByteArray zero_bytes;
	zero_bytes.resize((int64_t)new_capacity * stride);
	zero_bytes.fill(0);
	RID new_buffer = rd->storage_buffer_create(zero_bytes.size(), zero_bytes);

	if (body_buffer.is_valid() && gpu_capacity > 0) {
		// Preserve existing body state, then defer freeing the old buffer by one
		// step so any in-flight GPU work referencing it has drained.
		rd->buffer_copy(body_buffer, new_buffer, 0, 0, (uint32_t)gpu_capacity * stride);
		buffers_to_free.push_back(body_buffer);
	}

	body_buffer = new_buffer;
	gpu_capacity = new_capacity;

	// (Re)create the uniform set bound to the new buffer.
	Vector<RD::Uniform> uniforms;
	RD::Uniform u;
	u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u.binding = 0;
	u.append_id(body_buffer);
	uniforms.push_back(u);
	body_uniform_set = rd->uniform_set_create(uniforms, integrate_shader, 0);
}

void KilosPhysicsServer3D::_rt_dispatch_integrate(const StepJob &p_job) {
	struct Params {
		float gravity[4];
		float delta_time;
		float linear_damp;
		float angular_damp;
		uint32_t body_count;
	} pc;

	pc.gravity[0] = 0.0f;
	pc.gravity[1] = -9.8f;
	pc.gravity[2] = 0.0f;
	pc.gravity[3] = 0.0f;
	pc.delta_time = (float)p_job.dt;
	pc.linear_damp = 0.0f;
	pc.angular_damp = 0.0f;
	pc.body_count = p_job.body_count;

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, integrate_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, body_uniform_set, 0);
	rd->compute_list_set_push_constant(compute_list, &pc, sizeof(Params));
	rd->compute_list_dispatch(compute_list, (p_job.body_count + 63) / 64, 1, 1);
	rd->compute_list_end();
}

void KilosPhysicsServer3D::_rt_readback(const LocalVector<uint32_t> &p_tracked) {
	// Async, non-stalling: each callback lands after the GPU finishes this
	// frame's work, so tracked bodies read one frame late (acceptable).
	const uint32_t stride = sizeof(RigidBodyData);
	for (uint32_t i = 0; i < p_tracked.size(); i++) {
		const uint32_t slot = p_tracked[i];
		if (slot >= gpu_capacity) {
			continue;
		}
		rd->buffer_get_data_async(
				body_buffer,
				callable_mp(this, &KilosPhysicsServer3D::_rt_on_readback).bind(slot),
				slot * stride,
				stride);
	}
}

void KilosPhysicsServer3D::_rt_on_readback(const PackedByteArray &p_data, uint32_t p_slot) {
	if (p_data.size() < (int)sizeof(RigidBodyData)) {
		return;
	}
	RigidBodyData d;
	memcpy(&d, p_data.ptr(), sizeof(RigidBodyData));
	MutexLock lock(readback_mutex);
	readback_results[p_slot] = d;
}

void KilosPhysicsServer3D::_rt_ensure_convert_pipeline() {
	if (rt_convert_ready || !rd) {
		return;
	}
	Ref<RDShaderFile> sf;
	sf.instantiate();
	Error err = sf->parse_versions_from_text(String(body_to_multimesh_shader_glsl));
	if (err != OK) {
		sf->print_errors("KilosPhysics body_to_multimesh shader");
		return;
	}
	Vector<RD::ShaderStageSPIRVData> stages = sf->get_spirv_stages("");
	convert_shader = rd->shader_create_from_spirv(stages);
	if (!convert_shader.is_valid()) {
		return;
	}
	convert_pipeline = rd->compute_pipeline_create(convert_shader);
	rt_convert_ready = convert_pipeline.is_valid();
}

void KilosPhysicsServer3D::_rt_convert_to_multimesh(const BulkBind &p_bind) {
	RendererRD::MeshStorage *ms = RendererRD::MeshStorage::get_singleton();
	if (!ms) {
		return;
	}
	RID mm_buffer = ms->_multimesh_get_buffer_rd_rid(p_bind.multimesh);
	if (!mm_buffer.is_valid()) {
		return;
	}

	// set 0 = body buffer. Rebuild if the pool grew (body_buffer changed); the old
	// uniform set is auto-invalidated when the old buffer is freed.
	if (!convert_body_uniform_set.is_valid() || convert_body_uniform_set_buffer != body_buffer || !rd->uniform_set_is_valid(convert_body_uniform_set)) {
		Vector<RD::Uniform> u0;
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 0;
		u.append_id(body_buffer);
		u0.push_back(u);
		convert_body_uniform_set = rd->uniform_set_create(u0, convert_shader, 0);
		convert_body_uniform_set_buffer = body_buffer;
	}

	// set 1 = multimesh buffer, cached per multimesh (rebuilt if its buffer changed).
	ConvertMMSet *entry = mm_uniform_sets.getptr(p_bind.multimesh);
	if (!entry || entry->buffer != mm_buffer || !rd->uniform_set_is_valid(entry->uniform_set)) {
		Vector<RD::Uniform> u1;
		RD::Uniform um;
		um.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		um.binding = 0;
		um.append_id(mm_buffer);
		u1.push_back(um);
		ConvertMMSet set;
		set.buffer = mm_buffer;
		set.uniform_set = rd->uniform_set_create(u1, convert_shader, 1);
		mm_uniform_sets[p_bind.multimesh] = set;
		entry = mm_uniform_sets.getptr(p_bind.multimesh);
	}
	if (!entry->uniform_set.is_valid()) {
		return;
	}

	struct CParams {
		uint32_t base;
		uint32_t count;
		uint32_t stride;
		uint32_t pad;
	} pc;
	pc.base = p_bind.base;
	pc.count = p_bind.count;
	pc.stride = 12; // transform-only MultiMesh (no per-instance color/custom data)
	pc.pad = 0;

	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, convert_pipeline);
	rd->compute_list_bind_uniform_set(cl, convert_body_uniform_set, 0);
	rd->compute_list_bind_uniform_set(cl, entry->uniform_set, 1);
	rd->compute_list_set_push_constant(cl, &pc, sizeof(CParams));
	rd->compute_list_dispatch(cl, (p_bind.count + 63) / 64, 1, 1);
	rd->compute_list_end();
}

void KilosPhysicsServer3D::_rt_ensure_solve_pipeline() {
	if (rt_solve_ready || !rd) {
		return;
	}
	auto build = [&](const String &p_src, RID &r_shader, RID &r_pipeline, const char *p_label) -> bool {
		Ref<RDShaderFile> sf;
		sf.instantiate();
		Error err = sf->parse_versions_from_text(p_src);
		if (err != OK) {
			sf->print_errors(p_label);
			return false;
		}
		Vector<RD::ShaderStageSPIRVData> stages = sf->get_spirv_stages("");
		r_shader = rd->shader_create_from_spirv(stages);
		if (!r_shader.is_valid()) {
			return false;
		}
		r_pipeline = rd->compute_pipeline_create(r_shader);
		return r_pipeline.is_valid();
	};
	bool ok = build(String(pbd_solve_shader_glsl), solve_shader, solve_pipeline, "KilosPhysics pbd_solve shader");
	ok = ok && build(String(pbd_finalize_shader_glsl), finalize_shader, finalize_pipeline, "KilosPhysics pbd_finalize shader");
	ok = ok && build(String(grid_build_shader_glsl), grid_build_shader, grid_build_pipeline, "KilosPhysics grid_build shader");
	if (!ok) {
		return;
	}

	// Spatial-hash grid buffers (fixed size, allocated once).
	grid_counts_buffer = rd->storage_buffer_create(grid_table_size * sizeof(uint32_t));
	grid_bodies_buffer = rd->storage_buffer_create((uint64_t)grid_table_size * grid_max_per_cell * sizeof(uint32_t));

	// set 1 = { counts, bodies }, built against solve_shader (shared with grid_build).
	Vector<RD::Uniform> gu;
	RD::Uniform uc;
	uc.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	uc.binding = 0;
	uc.append_id(grid_counts_buffer);
	gu.push_back(uc);
	RD::Uniform ub;
	ub.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	ub.binding = 1;
	ub.append_id(grid_bodies_buffer);
	gu.push_back(ub);
	grid_uniform_set = rd->uniform_set_create(gu, solve_shader, 1);

	// SDF sampler + 1^3 dummy texture (bound to set 2 when no real SDF is set).
	RD::SamplerState ss;
	ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
	ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	ss.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sdf_sampler = rd->sampler_create(ss);

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	tf.width = 1;
	tf.height = 1;
	tf.depth = 1;
	tf.texture_type = RD::TEXTURE_TYPE_3D;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	float far_val = 1.0e9f; // large positive distance => "empty space"
	Vector<uint8_t> px;
	px.resize(sizeof(float));
	memcpy(px.ptrw(), &far_val, sizeof(float));
	Vector<Vector<uint8_t>> tdata;
	tdata.push_back(px);
	sdf_dummy_texture = rd->texture_create(tf, RD::TextureView(), tdata);

	rt_solve_ready = grid_uniform_set.is_valid() && sdf_sampler.is_valid() && sdf_dummy_texture.is_valid();
}

RID KilosPhysicsServer3D::_rt_get_sdf_uniform_set(RID p_texture) {
	RID tex = p_texture.is_valid() ? p_texture : sdf_dummy_texture;
	if (sdf_uniform_set.is_valid() && sdf_uniform_set_texture == tex && rd->uniform_set_is_valid(sdf_uniform_set)) {
		return sdf_uniform_set;
	}
	Vector<RD::Uniform> u;
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sdf_sampler, tex })));
	sdf_uniform_set = rd->uniform_set_create(u, solve_shader, 2);
	sdf_uniform_set_texture = tex;
	return sdf_uniform_set;
}

void KilosPhysicsServer3D::_rt_dispatch_collision(const StepJob &p_job) {
	// body_uniform_set (built against integrate_shader) and grid_uniform_set (set 1)
	// are layout-compatible with all of these pipelines: identical set formats.
	const uint32_t groups = (p_job.body_count + 63) / 64;
	const float cell_size = 2.0f * p_job.radius;
	const uint32_t table_mask = grid_table_size - 1u;

	// 1) Clear per-cell counts (GPU-side memset).
	rd->buffer_clear(grid_counts_buffer, 0, grid_table_size * sizeof(uint32_t));

	// 2) Build the spatial hash from the predicted positions.
	struct GridParams {
		float cell_size;
		uint32_t table_mask;
		uint32_t max_per_cell;
		uint32_t body_count;
	} gp;
	gp.cell_size = cell_size;
	gp.table_mask = table_mask;
	gp.max_per_cell = grid_max_per_cell;
	gp.body_count = p_job.body_count;
	{
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, grid_build_pipeline);
		rd->compute_list_bind_uniform_set(cl, body_uniform_set, 0);
		rd->compute_list_bind_uniform_set(cl, grid_uniform_set, 1);
		rd->compute_list_set_push_constant(cl, &gp, sizeof(GridParams));
		rd->compute_list_dispatch(cl, groups, 1, 1);
		rd->compute_list_end();
	}

	// 3) PBD relaxation iterations (body-body + optional SDF world + ground).
	RID sdf_set = _rt_get_sdf_uniform_set(p_job.sdf_texture);
	const bool sdf_on = p_job.sdf_texture.is_valid();
	struct SolveParams {
		float radius;
		float ground_y;
		float cell_size;
		uint32_t body_count;
		uint32_t table_mask;
		uint32_t max_per_cell;
		uint32_t sdf_enabled;
		uint32_t pad0;
		float sdf_min[4];
		float sdf_size[4];
	} sp;
	sp.radius = p_job.radius;
	sp.ground_y = p_job.ground_y;
	sp.cell_size = cell_size;
	sp.body_count = p_job.body_count;
	sp.table_mask = table_mask;
	sp.max_per_cell = grid_max_per_cell;
	sp.sdf_enabled = sdf_on ? 1u : 0u;
	sp.pad0 = 0;
	sp.sdf_min[0] = p_job.sdf_bounds.position.x;
	sp.sdf_min[1] = p_job.sdf_bounds.position.y;
	sp.sdf_min[2] = p_job.sdf_bounds.position.z;
	sp.sdf_min[3] = 0.0f;
	sp.sdf_size[0] = p_job.sdf_bounds.size.x;
	sp.sdf_size[1] = p_job.sdf_bounds.size.y;
	sp.sdf_size[2] = p_job.sdf_bounds.size.z;
	sp.sdf_size[3] = 0.0f;
	for (uint32_t it = 0; it < p_job.solve_iterations; it++) {
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, solve_pipeline);
		rd->compute_list_bind_uniform_set(cl, body_uniform_set, 0);
		rd->compute_list_bind_uniform_set(cl, grid_uniform_set, 1);
		rd->compute_list_bind_uniform_set(cl, sdf_set, 2);
		rd->compute_list_set_push_constant(cl, &sp, sizeof(SolveParams));
		rd->compute_list_dispatch(cl, groups, 1, 1);
		rd->compute_list_end();
	}

	// 4) Recover velocity from the corrected position (with light damping).
	struct FinParams {
		float inv_dt;
		float damping_factor;
		uint32_t body_count;
		uint32_t pad1;
	} fp;
	fp.inv_dt = (p_job.dt > 0.0) ? (float)(1.0 / p_job.dt) : 0.0f;
	fp.damping_factor = MAX(0.0f, 1.0f - velocity_damping * (float)p_job.dt);
	fp.body_count = p_job.body_count;
	fp.pad1 = 0;

	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, finalize_pipeline);
	rd->compute_list_bind_uniform_set(cl, body_uniform_set, 0);
	rd->compute_list_set_push_constant(cl, &fp, sizeof(FinParams));
	rd->compute_list_dispatch(cl, groups, 1, 1);
	rd->compute_list_end();
}

void KilosPhysicsServer3D::_render_process_step() {
	// Runs on the render thread with the global RD in hand.
	_rt_ensure_pipeline();
	if (!rt_pipeline_ready) {
		// Drop the job so the queue can't grow unbounded when there's no RD.
		MutexLock lock(job_mutex);
		if (!job_queue.is_empty()) {
			job_queue.pop_front();
		}
		return;
	}

	// Free old pool buffers retired on a previous step (GPU has drained them).
	for (uint32_t i = 0; i < buffers_to_free.size(); i++) {
		if (buffers_to_free[i].is_valid()) {
			rd->free_rid(buffers_to_free[i]);
		}
	}
	buffers_to_free.clear();

	StepJob job;
	{
		MutexLock lock(job_mutex);
		if (job_queue.is_empty()) {
			return;
		}
		job = job_queue.front()->get();
		job_queue.pop_front();
	}

	_rt_ensure_capacity(MAX(job.required_capacity, 1u));

	// Apply per-slot uploads (creations / state changes) in order.
	const uint32_t stride = sizeof(RigidBodyData);
	for (uint32_t i = 0; i < job.uploads.size(); i++) {
		const BodyUpload &u = job.uploads[i];
		if (u.index < gpu_capacity) {
			rd->buffer_update(body_buffer, u.index * stride, stride, &u.data);
		}
	}

	// Apply contiguous range uploads (bulk) as single buffer_updates.
	for (uint32_t i = 0; i < job.range_uploads.size(); i++) {
		const RangeUpload &ru = job.range_uploads[i];
		const uint32_t count = ru.data.size();
		if (count > 0 && ru.base + count <= gpu_capacity) {
			rd->buffer_update(body_buffer, ru.base * stride, count * stride, ru.data.ptr());
		}
	}

	if (job.body_count == 0) {
		return;
	}

	_rt_dispatch_integrate(job);

	// PBD collision: correct the predicted positions in place, then recover velocity.
	if (job.collision_enabled) {
		_rt_ensure_solve_pipeline();
		if (rt_solve_ready) {
			_rt_dispatch_collision(job);
		}
	}

	// GPU->GPU: write integrated transforms straight into each bound MultiMesh.
	if (!job.bulk_binds.is_empty()) {
		_rt_ensure_convert_pipeline();
		if (rt_convert_ready) {
			for (uint32_t i = 0; i < job.bulk_binds.size(); i++) {
				_rt_convert_to_multimesh(job.bulk_binds[i]);
			}
		}
	}

	if (!job.tracked.is_empty()) {
		_rt_readback(job.tracked);
	}
}

void KilosPhysicsServer3D::_debug_sync_readback() {
	// Selftest helper: force pending compute to submit, stall, and pull the
	// whole pool back into the CPU shadow. Not for hot-path use.
	if (!rd || !body_buffer.is_valid() || gpu_capacity == 0) {
		return;
	}
	PackedByteArray bytes = rd->buffer_get_data(body_buffer, 0, gpu_capacity * sizeof(RigidBodyData));
	const uint32_t count = MIN((uint32_t)body_data.size(), gpu_capacity);
	if ((uint32_t)bytes.size() >= count * sizeof(RigidBodyData)) {
		memcpy(body_data.ptr(), bytes.ptr(), count * sizeof(RigidBodyData));
	}
}

KilosPhysicsServer3D::KilosPhysicsServer3D(bool p_using_threads) {
	// GPU resources are created lazily on the render thread in _render_process_step().
	// The body pool grows on demand from empty.
}

KilosPhysicsServer3D::~KilosPhysicsServer3D() {
	if (space_state) {
		memdelete(space_state);
	}
	if (direct_state) {
		memdelete(direct_state);
	}
	if (rd) {
		if (body_uniform_set.is_valid()) {
			rd->free_rid(body_uniform_set);
		}
		if (body_buffer.is_valid()) {
			rd->free_rid(body_buffer);
		}
		for (uint32_t i = 0; i < buffers_to_free.size(); i++) {
			if (buffers_to_free[i].is_valid()) {
				rd->free_rid(buffers_to_free[i]);
			}
		}
		if (integrate_pipeline.is_valid()) {
			rd->free_rid(integrate_pipeline);
		}
		if (integrate_shader.is_valid()) {
			rd->free_rid(integrate_shader);
		}
		if (convert_pipeline.is_valid()) {
			rd->free_rid(convert_pipeline);
		}
		if (convert_shader.is_valid()) {
			rd->free_rid(convert_shader);
		}
		if (solve_pipeline.is_valid()) {
			rd->free_rid(solve_pipeline);
		}
		if (solve_shader.is_valid()) {
			rd->free_rid(solve_shader);
		}
		if (finalize_pipeline.is_valid()) {
			rd->free_rid(finalize_pipeline);
		}
		if (finalize_shader.is_valid()) {
			rd->free_rid(finalize_shader);
		}
		if (grid_build_pipeline.is_valid()) {
			rd->free_rid(grid_build_pipeline);
		}
		if (grid_build_shader.is_valid()) {
			rd->free_rid(grid_build_shader);
		}
		if (grid_counts_buffer.is_valid()) {
			rd->free_rid(grid_counts_buffer);
		}
		if (grid_bodies_buffer.is_valid()) {
			rd->free_rid(grid_bodies_buffer);
		}
		if (sdf_dummy_texture.is_valid()) {
			rd->free_rid(sdf_dummy_texture);
		}
		if (sdf_sampler.is_valid()) {
			rd->free_rid(sdf_sampler);
		}
	}
}

AABB KilosPhysicsServer3D::soft_body_get_bounds(RID p_body) const { return AABB(); }
void KilosPhysicsServer3D::soft_body_move_point(RID p_body, int p_point_index, const Vector3 &p_global_position) {}
Vector3 KilosPhysicsServer3D::soft_body_get_point_global_position(RID p_body, int p_point_index) const { return Vector3(); }
void KilosPhysicsServer3D::soft_body_remove_all_pinned_points(RID p_body) {}
void KilosPhysicsServer3D::soft_body_pin_point(RID p_body, int p_point_index, bool p_pin) {}
bool KilosPhysicsServer3D::soft_body_is_point_pinned(RID p_body, int p_point_index) const { return false; }
RID KilosPhysicsServer3D::joint_create() { return joint_owner.make_rid(KilosJoint()); }
void KilosPhysicsServer3D::joint_clear(RID p_joint) {}
void KilosPhysicsServer3D::pin_joint_set_param(RID p_joint, PhysicsServer3D::PinJointParam p_param, real_t p_value) {}
real_t KilosPhysicsServer3D::pin_joint_get_param(RID p_joint, PhysicsServer3D::PinJointParam p_param) const { return 0.0f; }
void KilosPhysicsServer3D::pin_joint_set_local_a(RID p_joint, const Vector3 &p_A) {}
void KilosPhysicsServer3D::joint_make_hinge(RID p_joint, RID p_body_A, const Transform3D &p_frame_A, RID p_body_B, const Transform3D &p_frame_B) {}

// ===== P5a: CPU ray queries against static trimesh bodies =====

namespace {

struct CentroidSorter {
	const Vector3 *centroids = nullptr;
	int axis = 0;
	bool operator()(int a, int b) const {
		return centroids[a][axis] < centroids[b][axis];
	}
};

// Dequantize a 16-bit vertex (3 uint16) back to world/local space via the AABB.
_FORCE_INLINE_ Vector3 dequant(const uint16_t *q, const AABB &box) {
	const real_t k = 1.0 / 65535.0;
	return box.position + Vector3(
									box.size.x * (real_t)q[0] * k,
									box.size.y * (real_t)q[1] * k,
									box.size.z * (real_t)q[2] * k);
}

// Segment (from -> from + dir*seg_len, dir unit) vs triangle. r_t is world distance.
bool segment_triangle(const Vector3 &from, const Vector3 &dir, real_t seg_len,
		const Vector3 &v0, const Vector3 &v1, const Vector3 &v2,
		bool hit_back, real_t &r_t, Vector3 &r_normal) {
	const real_t EPS = 1e-9;
	Vector3 e1 = v1 - v0;
	Vector3 e2 = v2 - v0;
	Vector3 pvec = dir.cross(e2);
	real_t det = e1.dot(pvec);
	if (det < 0.0 && !hit_back) {
		return false;
	}
	if (Math::abs(det) < EPS) {
		return false;
	}
	real_t inv_det = 1.0 / det;
	Vector3 tvec = from - v0;
	real_t u = tvec.dot(pvec) * inv_det;
	if (u < 0.0 || u > 1.0) {
		return false;
	}
	Vector3 qvec = tvec.cross(e1);
	real_t v = dir.dot(qvec) * inv_det;
	if (v < 0.0 || u + v > 1.0) {
		return false;
	}
	real_t t = e2.dot(qvec) * inv_det;
	if (t < 0.0 || t > seg_len) {
		return false;
	}
	r_t = t;
	Vector3 n = e1.cross(e2).normalized();
	if (n.dot(dir) > 0.0) {
		n = -n;
	}
	r_normal = n;
	return true;
}

bool ray_sphere(const Vector3 &from, const Vector3 &dir, real_t seg_len, const Vector3 &center, real_t r, real_t &r_t, Vector3 &r_n) {
	Vector3 oc = from - center;
	real_t b = oc.dot(dir);
	real_t c = oc.dot(oc) - r * r;
	real_t disc = b * b - c;
	if (disc < 0.0) {
		return false;
	}
	real_t sq = Math::sqrt(disc);
	real_t t = -b - sq;
	if (t < 0.0) {
		t = -b + sq; // ray starts inside
	}
	if (t < 0.0 || t > seg_len) {
		return false;
	}
	r_t = t;
	r_n = ((from + dir * t) - center).normalized();
	return true;
}

bool ray_box(const Vector3 &from0, const Vector3 &dir, real_t seg_len, const Vector3 &center, const Vector3 &he, real_t &r_t, Vector3 &r_n) {
	Vector3 from = from0 - center;
	real_t tmin = 0.0;
	real_t tmax = seg_len;
	Vector3 n;
	for (int a = 0; a < 3; a++) {
		if (Math::abs(dir[a]) < 1e-9) {
			if (from[a] < -he[a] || from[a] > he[a]) {
				return false;
			}
		} else {
			real_t inv = 1.0 / dir[a];
			real_t t1 = (-he[a] - from[a]) * inv;
			real_t t2 = (he[a] - from[a]) * inv;
			real_t sign = -1.0;
			if (t1 > t2) {
				SWAP(t1, t2);
				sign = 1.0;
			}
			if (t1 > tmin) {
				tmin = t1;
				n = Vector3();
				n[a] = sign;
			}
			if (t2 < tmax) {
				tmax = t2;
			}
			if (tmin > tmax) {
				return false;
			}
		}
	}
	if (tmin > seg_len) {
		return false;
	}
	r_t = tmin;
	r_n = n;
	return true;
}

// Finite cylinder along local Y, radius r, half-height hh, centred at origin.
bool ray_cylinder(const Vector3 &from, const Vector3 &dir, real_t seg_len, real_t r, real_t hh, real_t &r_t, Vector3 &r_n) {
	real_t best_t = seg_len + 1.0;
	Vector3 best_n;
	bool got = false;
	const real_t a = dir.x * dir.x + dir.z * dir.z;
	if (a > 1e-12) {
		const real_t b = 2.0 * (from.x * dir.x + from.z * dir.z);
		const real_t c = from.x * from.x + from.z * from.z - r * r;
		const real_t disc = b * b - 4.0 * a * c;
		if (disc >= 0.0) {
			const real_t sq = Math::sqrt(disc);
			for (int s = 0; s < 2; s++) {
				const real_t t = (s == 0) ? (-b - sq) / (2.0 * a) : (-b + sq) / (2.0 * a);
				if (t >= 0.0 && t <= seg_len) {
					const real_t y = from.y + dir.y * t;
					if (y >= -hh && y <= hh) {
						Vector3 h = from + dir * t;
						best_t = t;
						best_n = Vector3(h.x, 0, h.z).normalized();
						got = true;
						break;
					}
				}
			}
		}
	}
	for (int s = 0; s < 2; s++) {
		const real_t cy = (s == 0) ? hh : -hh;
		if (Math::abs(dir.y) > 1e-9) {
			const real_t t = (cy - from.y) / dir.y;
			if (t >= 0.0 && t <= seg_len && t < best_t) {
				Vector3 h = from + dir * t;
				if (h.x * h.x + h.z * h.z <= r * r) {
					best_t = t;
					best_n = Vector3(0, (s == 0) ? 1 : -1, 0);
					got = true;
				}
			}
		}
	}
	if (!got) {
		return false;
	}
	r_t = best_t;
	r_n = best_n;
	return true;
}

// Capsule along local Y: radius r, cylinder half-height chh, spheres at y=±chh.
bool ray_capsule(const Vector3 &from, const Vector3 &dir, real_t seg_len, real_t r, real_t chh, real_t &r_t, Vector3 &r_n) {
	real_t best_t = seg_len + 1.0;
	Vector3 best_n;
	bool got = false;
	const real_t a = dir.x * dir.x + dir.z * dir.z;
	if (a > 1e-12) {
		const real_t b = 2.0 * (from.x * dir.x + from.z * dir.z);
		const real_t c = from.x * from.x + from.z * from.z - r * r;
		const real_t disc = b * b - 4.0 * a * c;
		if (disc >= 0.0) {
			const real_t sq = Math::sqrt(disc);
			for (int s = 0; s < 2; s++) {
				const real_t t = (s == 0) ? (-b - sq) / (2.0 * a) : (-b + sq) / (2.0 * a);
				if (t >= 0.0 && t <= seg_len) {
					const real_t y = from.y + dir.y * t;
					if (y >= -chh && y <= chh) {
						Vector3 h = from + dir * t;
						best_t = t;
						best_n = Vector3(h.x, 0, h.z).normalized();
						got = true;
						break;
					}
				}
			}
		}
	}
	for (int s = 0; s < 2; s++) {
		const real_t cy = (s == 0) ? chh : -chh;
		real_t t;
		Vector3 n;
		if (ray_sphere(from, dir, seg_len, Vector3(0, cy, 0), r, t, n)) {
			// Only accept the hemispherical caps (beyond the cylinder section).
			const real_t hy = from.y + dir.y * t;
			if (((s == 0 && hy >= chh) || (s == 1 && hy <= -chh)) && t < best_t) {
				best_t = t;
				best_n = n;
				got = true;
			}
		}
	}
	if (!got) {
		return false;
	}
	r_t = best_t;
	r_n = best_n;
	return true;
}

// Slab test for segment [0, seg_len] along unit dir (inv_dir = 1/dir per axis).
bool ray_aabb(const Vector3 &from, const Vector3 &inv_dir, real_t seg_len, const AABB &box, real_t &r_tmin) {
	Vector3 bmax = box.position + box.size;
	real_t tmin = 0.0;
	real_t tmax = seg_len;
	for (int a = 0; a < 3; a++) {
		real_t t1 = (box.position[a] - from[a]) * inv_dir[a];
		real_t t2 = (bmax[a] - from[a]) * inv_dir[a];
		if (t1 > t2) {
			SWAP(t1, t2);
		}
		tmin = MAX(tmin, t1);
		tmax = MIN(tmax, t2);
		if (tmin > tmax) {
			return false;
		}
	}
	r_tmin = tmin;
	return true;
}

} // namespace

void KilosPhysicsServer3D::_build_shape_bvh(KilosShape *s) const {
	s->bvh.clear();
	s->tri_order.clear();
	const int tri_count = s->qfaces.size() / 9; // 9 uint16 per triangle
	if (tri_count == 0) {
		return;
	}

	const uint16_t *qf = s->qfaces.ptr();
	const AABB &qbox = s->local_aabb; // set at shape_set_data; needed for dequant
	Vector<AABB> tri_aabb;
	tri_aabb.resize(tri_count);
	Vector<Vector3> centroids;
	centroids.resize(tri_count);
	s->tri_order.resize(tri_count);
	for (int i = 0; i < tri_count; i++) {
		const Vector3 a = dequant(qf + i * 9 + 0, qbox);
		const Vector3 b = dequant(qf + i * 9 + 3, qbox);
		const Vector3 c = dequant(qf + i * 9 + 6, qbox);
		AABB ta(a, Vector3());
		ta.expand_to(b);
		ta.expand_to(c);
		tri_aabb.write[i] = ta;
		centroids.write[i] = (a + b + c) / 3.0;
		s->tri_order.write[i] = i;
	}

	struct Work {
		int start;
		int end;
		int node;
	};
	Vector<Work> stack;
	s->bvh.push_back(BVHNode());
	stack.push_back({ 0, tri_count, 0 });
	const int LEAF = 4;

	while (!stack.is_empty()) {
		Work wk = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);

		AABB bounds = tri_aabb[s->tri_order[wk.start]];
		for (int i = wk.start + 1; i < wk.end; i++) {
			bounds.merge_with(tri_aabb[s->tri_order[i]]);
		}
		const int count = wk.end - wk.start;
		BVHNode node;
		node.bounds = bounds;

		if (count <= LEAF) {
			node.left = -1;
			node.right = -1;
			node.tri_start = wk.start;
			node.tri_count = count;
			s->bvh.write[wk.node] = node;
			continue;
		}

		Vector3 ext = bounds.size;
		int axis = 0;
		if (ext.y > ext.x) {
			axis = 1;
		}
		if (ext.z > ext[axis]) {
			axis = 2;
		}
		CentroidSorter cs;
		cs.centroids = centroids.ptr();
		cs.axis = axis;
		SortArray<int, CentroidSorter> sorter;
		sorter.compare = cs;
		sorter.sort(s->tri_order.ptrw() + wk.start, count);

		const int mid = (wk.start + wk.end) / 2;
		int left_idx = s->bvh.size();
		s->bvh.push_back(BVHNode());
		int right_idx = s->bvh.size();
		s->bvh.push_back(BVHNode());
		node.left = left_idx;
		node.right = right_idx;
		node.tri_count = 0;
		s->bvh.write[wk.node] = node;
		stack.push_back({ wk.start, mid, left_idx });
		stack.push_back({ mid, wk.end, right_idx });
	}
}

bool KilosPhysicsServer3D::_body_raycast(const KilosBody *p_body, RID p_body_rid, const Vector3 &p_from, const Vector3 &p_to, bool p_hit_back, real_t &r_closest_t, PhysicsDirectSpaceState3D::RayResult &r_result) const {
	bool hit_any = false;
	const Vector3 world_seg = p_to - p_from;

	for (int si = 0; si < p_body->shapes.size(); si++) {
		const KilosBody::BodyShape &bs = p_body->shapes[si];
		if (bs.disabled) {
			continue;
		}
		KilosShape *shape = shape_owner.get_or_null(bs.shape);
		if (!shape) {
			continue;
		}

		const Transform3D world_xform = p_body->transform * bs.xform;
		const Transform3D inv = world_xform.affine_inverse();
		const Vector3 lfrom = inv.xform(p_from);
		const Vector3 lto = inv.xform(p_to);
		const Vector3 lseg = lto - lfrom;
		const real_t lseg_len = lseg.length();
		if (lseg_len < 1e-9) {
			continue;
		}
		const Vector3 ldir = lseg / lseg_len;

		// Records a local-space hit (t along ldir, local normal) if it's the closest so far.
		auto record = [&](real_t p_t_local, const Vector3 &p_n_local, int p_face) {
			const real_t frac = p_t_local / lseg_len;
			if (frac >= r_closest_t) {
				return;
			}
			r_closest_t = frac;
			hit_any = true;
			r_result.position = p_from + world_seg * frac;
			Vector3 wn = world_xform.basis.xform(p_n_local).normalized();
			if (wn.dot(world_seg) > 0.0) {
				wn = -wn;
			}
			r_result.normal = wn;
			r_result.rid = p_body_rid;
			r_result.collider_id = p_body->instance_id;
			r_result.collider = ObjectDB::get_instance(p_body->instance_id);
			r_result.shape = si;
			r_result.face_index = p_face;
		};

		real_t t_local;
		Vector3 n_local;
		switch (shape->type) {
			case SHAPE_CONCAVE_POLYGON: {
				if (shape->bvh.is_empty()) {
					if (shape->qfaces.is_empty()) {
						break;
					}
					_build_shape_bvh(shape); // lazy: first raycast against this chunk
				}
				const Vector3 inv_dir(
						Math::abs(ldir.x) < 1e-9 ? 1e30 : 1.0 / ldir.x,
						Math::abs(ldir.y) < 1e-9 ? 1e30 : 1.0 / ldir.y,
						Math::abs(ldir.z) < 1e-9 ? 1e30 : 1.0 / ldir.z);
				const bool hit_back = p_hit_back || shape->backface;
				const uint16_t *qf = shape->qfaces.ptr();
				const AABB &qbox = shape->local_aabb;
				const int *tri_order = shape->tri_order.ptr();
				int node_stack[64];
				int sp = 0;
				node_stack[sp++] = 0;
				while (sp > 0) {
					const BVHNode &node = shape->bvh[node_stack[--sp]];
					real_t node_tmin;
					if (!ray_aabb(lfrom, inv_dir, r_closest_t * lseg_len, node.bounds, node_tmin)) {
						continue;
					}
					if (node.left < 0) {
						for (int k = 0; k < node.tri_count; k++) {
							const int tri = tri_order[node.tri_start + k];
							const Vector3 a = dequant(qf + tri * 9 + 0, qbox);
							const Vector3 b = dequant(qf + tri * 9 + 3, qbox);
							const Vector3 c = dequant(qf + tri * 9 + 6, qbox);
							if (segment_triangle(lfrom, ldir, lseg_len, a, b, c, hit_back, t_local, n_local)) {
								record(t_local, n_local, tri);
							}
						}
					} else if (sp < 62) {
						node_stack[sp++] = node.left;
						node_stack[sp++] = node.right;
					}
				}
			} break;
			case SHAPE_SPHERE: {
				if (ray_sphere(lfrom, ldir, lseg_len, Vector3(), shape->radius, t_local, n_local)) {
					record(t_local, n_local, -1);
				}
			} break;
			case SHAPE_BOX:
			case SHAPE_CONVEX_POLYGON: {
				if (ray_box(lfrom, ldir, lseg_len, shape->center, shape->box_half, t_local, n_local)) {
					record(t_local, n_local, -1);
				}
			} break;
			case SHAPE_CYLINDER: {
				if (ray_cylinder(lfrom, ldir, lseg_len, shape->radius, shape->height * 0.5, t_local, n_local)) {
					record(t_local, n_local, -1);
				}
			} break;
			case SHAPE_CAPSULE: {
				const real_t chh = MAX((real_t)0.0, shape->height * 0.5 - shape->radius);
				if (ray_capsule(lfrom, ldir, lseg_len, shape->radius, chh, t_local, n_local)) {
					record(t_local, n_local, -1);
				}
			} break;
			default:
				break;
		}
	}
	return hit_any;
}

AABB KilosPhysicsServer3D::_body_world_aabb(const KilosBody *p_body) const {
	AABB result;
	bool first = true;
	for (int i = 0; i < p_body->shapes.size(); i++) {
		const KilosBody::BodyShape &bs = p_body->shapes[i];
		if (bs.disabled) {
			continue;
		}
		KilosShape *sh = shape_owner.get_or_null(bs.shape);
		if (!sh) {
			continue;
		}
		AABB la = sh->local_aabb;
		if (la.size == Vector3() && sh->type != SHAPE_CONCAVE_POLYGON) {
			// Approximate a primitive's local bounds.
			real_t r = MAX(sh->radius, (real_t)0.01);
			real_t h = MAX(sh->height, 2.0 * r);
			la = AABB(Vector3(-r, -h * 0.5, -r), Vector3(2.0 * r, h, 2.0 * r));
		}
		AABB wa = (p_body->transform * bs.xform).xform(la);
		if (first) {
			result = wa;
			first = false;
		} else {
			result.merge_with(wa);
		}
	}
	return result;
}

void KilosPhysicsServer3D::_build_space_bvh(KilosSpace *space) {
	space->bvh.clear();
	space->bvh_bodies.clear();
	space->bvh_order.clear();
	space->bvh_pending.clear();
	space->bvh_dirty = false;

	Vector<AABB> aabbs;
	Vector<Vector3> centroids;
	for (const RID &rid : space->bodies) {
		KilosBody *b = body_owner.get_or_null(rid);
		if (!b || b->shapes.is_empty()) {
			continue;
		}
		AABB wa = _body_world_aabb(b);
		space->bvh_bodies.push_back(rid);
		aabbs.push_back(wa);
		centroids.push_back(wa.position + wa.size * 0.5);
	}
	const int n = space->bvh_bodies.size();
	if (n == 0) {
		return;
	}
	space->bvh_order.resize(n);
	for (int i = 0; i < n; i++) {
		space->bvh_order.write[i] = i;
	}

	struct Work {
		int start;
		int end;
		int node;
	};
	Vector<Work> stack;
	space->bvh.push_back(BVHNode());
	stack.push_back({ 0, n, 0 });
	const int LEAF = 2;
	while (!stack.is_empty()) {
		Work wk = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);
		AABB bounds = aabbs[space->bvh_order[wk.start]];
		for (int i = wk.start + 1; i < wk.end; i++) {
			bounds.merge_with(aabbs[space->bvh_order[i]]);
		}
		const int count = wk.end - wk.start;
		BVHNode node;
		node.bounds = bounds;
		if (count <= LEAF) {
			node.left = -1;
			node.right = -1;
			node.tri_start = wk.start;
			node.tri_count = count;
			space->bvh.write[wk.node] = node;
			continue;
		}
		Vector3 ext = bounds.size;
		int axis = 0;
		if (ext.y > ext.x) {
			axis = 1;
		}
		if (ext.z > ext[axis]) {
			axis = 2;
		}
		CentroidSorter cs;
		cs.centroids = centroids.ptr();
		cs.axis = axis;
		SortArray<int, CentroidSorter> sorter;
		sorter.compare = cs;
		sorter.sort(space->bvh_order.ptrw() + wk.start, count);
		const int mid = (wk.start + wk.end) / 2;
		int l = space->bvh.size();
		space->bvh.push_back(BVHNode());
		int r = space->bvh.size();
		space->bvh.push_back(BVHNode());
		node.left = l;
		node.right = r;
		node.tri_count = 0;
		space->bvh.write[wk.node] = node;
		stack.push_back({ wk.start, mid, l });
		stack.push_back({ mid, wk.end, r });
	}
}

bool KilosPhysicsServer3D::_intersect_ray(RID p_space, const PhysicsDirectSpaceState3D::RayParameters &p_parameters, PhysicsDirectSpaceState3D::RayResult &r_result) {
	MutexLock lock(collision_mutex);
	return _intersect_ray_unlocked(p_space, p_parameters, r_result);
}

bool KilosPhysicsServer3D::_intersect_ray_unlocked(RID p_space, const PhysicsDirectSpaceState3D::RayParameters &p_parameters, PhysicsDirectSpaceState3D::RayResult &r_result) {
	KilosSpace *space = space_owner.get_or_null(p_space);
	if (!space) {
		return false;
	}
	if (space->bvh_dirty) {
		_build_space_bvh(space);
	}
	if (space->bvh.is_empty()) {
		return false;
	}

	const Vector3 from = p_parameters.from;
	const Vector3 seg = p_parameters.to - from;
	const real_t seg_len = seg.length();
	if (seg_len < 1e-9) {
		return false;
	}
	const Vector3 dir = seg / seg_len;
	const Vector3 inv_dir(
			Math::abs(dir.x) < 1e-9 ? 1e30 : 1.0 / dir.x,
			Math::abs(dir.y) < 1e-9 ? 1e30 : 1.0 / dir.y,
			Math::abs(dir.z) < 1e-9 ? 1e30 : 1.0 / dir.z);

	real_t closest_t = 1.0;
	bool hit = false;

	int node_stack[128];
	int sp = 0;
	node_stack[sp++] = 0;
	while (sp > 0) {
		const BVHNode &node = space->bvh[node_stack[--sp]];
		real_t tmin;
		if (!ray_aabb(from, inv_dir, closest_t * seg_len, node.bounds, tmin)) {
			continue;
		}
		if (node.left < 0) {
			for (int k = 0; k < node.tri_count; k++) {
				RID body_rid = space->bvh_bodies[space->bvh_order[node.tri_start + k]];
				if (p_parameters.exclude.has(body_rid)) {
					continue;
				}
				KilosBody *body = body_owner.get_or_null(body_rid);
				if (!body || body->shapes.is_empty()) {
					continue;
				}
				if (!(body->collision_layer & p_parameters.collision_mask)) {
					continue;
				}
				if (_body_raycast(body, body_rid, from, p_parameters.to, p_parameters.hit_back_faces, closest_t, r_result)) {
					hit = true;
				}
			}
		} else {
			if (sp < 126) {
				node_stack[sp++] = node.left;
				node_stack[sp++] = node.right;
			}
		}
	}

	// Bodies added since the last BVH build (not yet folded in).
	for (int i = 0; i < space->bvh_pending.size(); i++) {
		RID body_rid = space->bvh_pending[i];
		if (p_parameters.exclude.has(body_rid)) {
			continue;
		}
		KilosBody *body = body_owner.get_or_null(body_rid);
		if (!body || body->shapes.is_empty()) {
			continue;
		}
		if (!(body->collision_layer & p_parameters.collision_mask)) {
			continue;
		}
		if (_body_raycast(body, body_rid, from, p_parameters.to, p_parameters.hit_back_faces, closest_t, r_result)) {
			hit = true;
		}
	}
	return hit;
}

bool KilosDirectSpaceState3D::intersect_ray(const RayParameters &p_parameters, RayResult &r_result) {
	if (!server) {
		return false;
	}
	return server->_intersect_ray(space, p_parameters, r_result);
}

// --- KilosDirectBodyState3D implementation ---

Vector3 KilosDirectBodyState3D::get_total_gravity() const {
	return Vector3(0, -9.8, 0); // Hardcoded for now
}
real_t KilosDirectBodyState3D::get_total_angular_damp() const { return 0.0; }
real_t KilosDirectBodyState3D::get_total_linear_damp() const { return 0.0; }

Vector3 KilosDirectBodyState3D::get_center_of_mass() const { return Vector3(); }
Vector3 KilosDirectBodyState3D::get_center_of_mass_local() const { return Vector3(); }
Basis KilosDirectBodyState3D::get_principal_inertia_axes() const { return Basis(); }
real_t KilosDirectBodyState3D::get_inverse_mass() const {
	real_t mass = server->body_get_param(body, PhysicsServer3D::BODY_PARAM_MASS);
	return mass > 0 ? 1.0 / mass : 0;
}
Vector3 KilosDirectBodyState3D::get_inverse_inertia() const { return Vector3(); }
Basis KilosDirectBodyState3D::get_inverse_inertia_tensor() const { return Basis(); }

void KilosDirectBodyState3D::set_linear_velocity(const Vector3 &p_velocity) {
	server->body_set_state(body, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY, p_velocity);
}
Vector3 KilosDirectBodyState3D::get_linear_velocity() const {
	return server->body_get_state(body, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY);
}

void KilosDirectBodyState3D::set_angular_velocity(const Vector3 &p_velocity) {
	server->body_set_state(body, PhysicsServer3D::BODY_STATE_ANGULAR_VELOCITY, p_velocity);
}
Vector3 KilosDirectBodyState3D::get_angular_velocity() const {
	return server->body_get_state(body, PhysicsServer3D::BODY_STATE_ANGULAR_VELOCITY);
}

void KilosDirectBodyState3D::set_transform(const Transform3D &p_transform) {
	server->body_set_state(body, PhysicsServer3D::BODY_STATE_TRANSFORM, p_transform);
}
Transform3D KilosDirectBodyState3D::get_transform() const {
	return server->body_get_state(body, PhysicsServer3D::BODY_STATE_TRANSFORM);
}

Vector3 KilosDirectBodyState3D::get_velocity_at_local_position(const Vector3 &p_position) const { return get_linear_velocity(); }

void KilosDirectBodyState3D::apply_central_impulse(const Vector3 &p_impulse) {}
void KilosDirectBodyState3D::apply_impulse(const Vector3 &p_impulse, const Vector3 &p_position) {}
void KilosDirectBodyState3D::apply_torque_impulse(const Vector3 &p_impulse) {}
void KilosDirectBodyState3D::apply_central_force(const Vector3 &p_force) {}
void KilosDirectBodyState3D::apply_force(const Vector3 &p_force, const Vector3 &p_position) {}
void KilosDirectBodyState3D::apply_torque(const Vector3 &p_torque) {}
void KilosDirectBodyState3D::add_constant_central_force(const Vector3 &p_force) {}
void KilosDirectBodyState3D::add_constant_force(const Vector3 &p_force, const Vector3 &p_position) {}
void KilosDirectBodyState3D::add_constant_torque(const Vector3 &p_torque) {}
void KilosDirectBodyState3D::set_constant_force(const Vector3 &p_force) {}
Vector3 KilosDirectBodyState3D::get_constant_force() const { return Vector3(); }
void KilosDirectBodyState3D::set_constant_torque(const Vector3 &p_torque) {}
Vector3 KilosDirectBodyState3D::get_constant_torque() const { return Vector3(); }

void KilosDirectBodyState3D::set_sleep_state(bool p_sleep) {}
bool KilosDirectBodyState3D::is_sleeping() const { return false; }

void KilosDirectBodyState3D::set_collision_layer(uint32_t p_layer) {}
uint32_t KilosDirectBodyState3D::get_collision_layer() const { return 0; }
void KilosDirectBodyState3D::set_collision_mask(uint32_t p_mask) {}
uint32_t KilosDirectBodyState3D::get_collision_mask() const { return 0; }

int KilosDirectBodyState3D::get_contact_count() const { return 0; }
Vector3 KilosDirectBodyState3D::get_contact_local_position(int p_contact_idx) const { return Vector3(); }
Vector3 KilosDirectBodyState3D::get_contact_local_normal(int p_contact_idx) const { return Vector3(); }
Vector3 KilosDirectBodyState3D::get_contact_impulse(int p_contact_idx) const { return Vector3(); }
int KilosDirectBodyState3D::get_contact_local_shape(int p_contact_idx) const { return 0; }
Vector3 KilosDirectBodyState3D::get_contact_local_velocity_at_position(int p_contact_idx) const { return Vector3(); }
RID KilosDirectBodyState3D::get_contact_collider(int p_contact_idx) const { return RID(); }
Vector3 KilosDirectBodyState3D::get_contact_collider_position(int p_contact_idx) const { return Vector3(); }
ObjectID KilosDirectBodyState3D::get_contact_collider_id(int p_contact_idx) const { return ObjectID(); }
Object *KilosDirectBodyState3D::get_contact_collider_object(int p_contact_idx) const { return nullptr; }
int KilosDirectBodyState3D::get_contact_collider_shape(int p_contact_idx) const { return 0; }
Vector3 KilosDirectBodyState3D::get_contact_collider_velocity_at_position(int p_contact_idx) const { return Vector3(); }

real_t KilosDirectBodyState3D::get_step() const { return 1.0 / 60.0; }
void KilosDirectBodyState3D::integrate_forces() {}
RequiredResult<PhysicsDirectSpaceState3D> KilosDirectBodyState3D::get_space_state() { return RequiredResult<PhysicsDirectSpaceState3D>(nullptr); }
