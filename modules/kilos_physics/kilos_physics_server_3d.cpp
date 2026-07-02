#include "kilos_physics_server_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/object_id.h"

#include "shaders/body_to_multimesh.glsl.gen.h"
#include "shaders/integration.glsl.gen.h"

#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/rendering_device_binds.h"

RID KilosPhysicsServer3D::world_boundary_shape_create() {
	return shape_owner.make_rid(KilosShape());
}

RID KilosPhysicsServer3D::separation_ray_shape_create() {
	return shape_owner.make_rid(KilosShape());
}

RID KilosPhysicsServer3D::sphere_shape_create() {
	return shape_owner.make_rid(KilosShape());
}

RID KilosPhysicsServer3D::box_shape_create() {
	return shape_owner.make_rid(KilosShape());
}

RID KilosPhysicsServer3D::capsule_shape_create() {
	return shape_owner.make_rid(KilosShape());
}

RID KilosPhysicsServer3D::cylinder_shape_create() {
	return shape_owner.make_rid(KilosShape());
}

RID KilosPhysicsServer3D::convex_polygon_shape_create() {
	return shape_owner.make_rid(KilosShape());
}

RID KilosPhysicsServer3D::concave_polygon_shape_create() {
	return shape_owner.make_rid(KilosShape());
}

RID KilosPhysicsServer3D::heightmap_shape_create() {
	return shape_owner.make_rid(KilosShape());
}

RID KilosPhysicsServer3D::custom_shape_create() {
	return shape_owner.make_rid(KilosShape());
}

void KilosPhysicsServer3D::shape_set_data(RID p_shape, const Variant &p_data) {
}

void KilosPhysicsServer3D::shape_set_custom_solver_bias(RID p_shape, real_t p_bias) {
}

PhysicsServer3D::ShapeType KilosPhysicsServer3D::shape_get_type(RID p_shape) const {
	return SHAPE_CONCAVE_POLYGON;
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
	KilosBody body;
	body.slot = _alloc_slot();
	body.has_slot = true;
	return body_owner.make_rid(body);
}

void KilosPhysicsServer3D::body_set_space(RID p_body, RID p_space) {
	KilosBody *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->active = p_space.is_valid();
}

RID KilosPhysicsServer3D::body_get_space(RID p_body) const {
	return RID();
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

void KilosPhysicsServer3D::body_add_shape(RID p_body, RID p_shape, const Transform3D &p_transform, bool p_disabled) {
}

void KilosPhysicsServer3D::body_set_shape(RID p_body, int p_shape_idx, RID p_shape) {
}

void KilosPhysicsServer3D::body_set_shape_transform(RID p_body, int p_shape_idx, const Transform3D &p_transform) {
}

int KilosPhysicsServer3D::body_get_shape_count(RID p_body) const {
	return 0;
}

RID KilosPhysicsServer3D::body_get_shape(RID p_body, int p_shape_idx) const {
	return RID();
}

Transform3D KilosPhysicsServer3D::body_get_shape_transform(RID p_body, int p_shape_idx) const {
	return Transform3D();
}

void KilosPhysicsServer3D::body_set_shape_disabled(RID p_body, int p_shape_idx, bool p_disabled) {
}

void KilosPhysicsServer3D::body_remove_shape(RID p_body, int p_shape_idx) {
}

void KilosPhysicsServer3D::body_clear_shapes(RID p_body) {
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
	if (r_result) {
		r_result->travel = p_parameters.motion;
		r_result->collision_safe_fraction = 1.0;
		r_result->collision_unsafe_fraction = 1.0;
	}
	return false;
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
	if (space_owner.owns(p_rid)) {
		space_owner.free(p_rid);
	} else if (body_owner.owns(p_rid)) {
		KilosBody *body = body_owner.get_or_null(p_rid);
		if (body) {
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
