#include "kilos_ecs_system.h"
#include "core/object/class_db.h"

void KilosECSSystem::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize", "max_entities", "multimesh"), &KilosECSSystem::initialize);
	ClassDB::bind_method(D_METHOD("set_collision", "enabled", "radius", "ground_y", "iterations"), &KilosECSSystem::set_collision);
	ClassDB::bind_method(D_METHOD("set_terrain_sdf", "sdf_texture", "bounds"), &KilosECSSystem::set_terrain_sdf);
}

void KilosECSSystem::set_collision(bool p_enabled, real_t p_radius, real_t p_ground_y, int p_iterations) {
	PhysicsServer3D::get_singleton()->bulk_set_collision(p_enabled, p_radius, p_ground_y, p_iterations);
}

void KilosECSSystem::set_terrain_sdf(RID p_sdf_texture, const AABB &p_bounds) {
	PhysicsServer3D::get_singleton()->bulk_set_sdf(p_sdf_texture, p_bounds);
}

KilosECSSystem::KilosECSSystem() {
}

KilosECSSystem::~KilosECSSystem() {
	if (bulk_handle != -1) {
		PhysicsServer3D::get_singleton()->bulk_body_free(bulk_handle);
	}
}

void KilosECSSystem::initialize(int p_max_entities, RID p_multimesh) {
	if (bulk_handle != -1) {
		PhysicsServer3D::get_singleton()->bulk_body_free(bulk_handle);
	}

	max_slots = p_max_entities;
	bulk_handle = PhysicsServer3D::get_singleton()->bulk_body_create(max_slots);
	
	// Track all slots so we can read back their transform for the AI
	PhysicsServer3D::get_singleton()->bulk_body_set_tracked(bulk_handle, true);
	PhysicsServer3D::get_singleton()->bulk_body_set_multimesh(bulk_handle, p_multimesh);

	free_slots.clear();
	entity_to_slot.clear();

	for (int i = max_slots - 1; i >= 0; i--) {
		free_slots.push_back(i);
		// Hide initially
		PhysicsServer3D::get_singleton()->bulk_body_set_position(bulk_handle, i, Vector3(0, -99999, 0));
	}
}

void KilosECSSystem::update(ECSWorld *p_world, float p_delta) {
	if (bulk_handle == -1) return;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();

	// 1. Cleanup dead entities and free their slots
	List<int64_t> to_remove;
	for (KeyValue<int64_t, int> &E : entity_to_slot) {
		if (!p_world->is_alive(E.key)) {
			to_remove.push_back(E.key);
			free_slots.push_back(E.value);
			ps->bulk_body_set_position(bulk_handle, E.value, Vector3(0, -99999, 0));
		}
	}
	for (int64_t ent : to_remove) {
		entity_to_slot.erase(ent);
	}

	// 2. Query active dynamic animals. Only a transform is required; velocity is an
	//    optional AI steering input (a spawned-but-idle entity still gets a body).
	Array names;
	names.push_back(StringName("transform"));
	TypedArray<int64_t> active = p_world->query_dynamic_components(names);

	// 3. Pull every body's transform back in ONE cross-thread sync (instead of one
	//    blocking sync per entity, which was the dominant per-frame cost).
	Vector<Transform3D> transforms = ps->bulk_body_get_transforms(bulk_handle);

	for (int i = 0; i < active.size(); i++) {
		int64_t ent = active[i];
		int slot = -1;
		bool newly_assigned = false;

		if (!entity_to_slot.has(ent)) {
			if (free_slots.size() > 0) {
				slot = free_slots[free_slots.size() - 1];
				free_slots.resize(free_slots.size() - 1);
				entity_to_slot[ent] = slot;
				newly_assigned = true;

				// Initialize position to the ECS transform
				Variant var_t = p_world->get_dynamic_component(ent, "transform");
				if (var_t.get_type() == Variant::TRANSFORM3D) {
					Transform3D t = var_t;
					ps->bulk_body_set_position(bulk_handle, slot, t.origin);
				}
			}
		} else {
			slot = entity_to_slot[ent];
		}

		if (slot != -1) {
			// Sync AI Steering Velocity -> Kilos Physics
			Variant var_vel = p_world->get_dynamic_component(ent, "velocity");
			if (var_vel.get_type() == Variant::VECTOR3) {
				Vector3 vel = var_vel;
				ps->bulk_body_set_velocity(bulk_handle, slot, vel);
			}

			// Sync Kilos Physics -> ECS Transform (so AI knows where it is). Skip on
			// the assignment frame: the readback still holds the old/hidden position
			// and would clobber the authored spawn transform.
			if (!newly_assigned && slot < transforms.size()) {
				p_world->set_dynamic_component(ent, "transform", transforms[slot]);
			}
		}
	}
}
