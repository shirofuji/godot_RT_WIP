#include "ecs_world.h"
#include "ecs_system.h"
#include "core/config/engine.h"
#include "core/object/class_db.h"

void ECSWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create_entity"), &ECSWorld::create_entity);
	ClassDB::bind_method(D_METHOD("destroy_entity", "entity"), &ECSWorld::destroy_entity);
	ClassDB::bind_method(D_METHOD("is_alive", "entity"), &ECSWorld::is_alive);

	ClassDB::bind_method(D_METHOD("set_dynamic_component", "entity", "name", "value"), &ECSWorld::set_dynamic_component);
	ClassDB::bind_method(D_METHOD("get_dynamic_component", "entity", "name"), &ECSWorld::get_dynamic_component);
	ClassDB::bind_method(D_METHOD("has_dynamic_component", "entity", "name"), &ECSWorld::has_dynamic_component);
	ClassDB::bind_method(D_METHOD("remove_dynamic_component", "entity", "name"), &ECSWorld::remove_dynamic_component);
	ClassDB::bind_method(D_METHOD("query_dynamic_components", "component_names"), &ECSWorld::query_dynamic_components);

	ClassDB::bind_method(D_METHOD("add_system", "system"), &ECSWorld::add_system);
	ClassDB::bind_method(D_METHOD("remove_system", "system"), &ECSWorld::remove_system);
}

void ECSWorld::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		if (Engine::get_singleton()->is_editor_hint()) {
			return; // Don't tick systems in editor
		}
		
		float delta = get_process_delta_time();
		for (int i = 0; i < active_systems.size(); i++) {
			ECSSystem *sys = Object::cast_to<ECSSystem>(active_systems[i]);
			if (sys) {
				sys->update(this, delta);
			}
		}
	}
}

ECSWorld::ECSWorld() {
	set_process(true);
}

ECSWorld::~ECSWorld() {
	registry.clear();
}

int64_t ECSWorld::create_entity() {
	return (int64_t)registry.create();
}

void ECSWorld::destroy_entity(int64_t p_entity) {
	entt::entity e = (entt::entity)p_entity;
	if (registry.valid(e)) {
		registry.destroy(e);
	}
}

bool ECSWorld::is_alive(int64_t p_entity) const {
	return registry.valid((entt::entity)p_entity);
}

void ECSWorld::set_dynamic_component(int64_t p_entity, const StringName &p_name, const Variant &p_value) {
	entt::entity e = (entt::entity)p_entity;
	ERR_FAIL_COND(!registry.valid(e));

	if (!registry.all_of<GDDynamicComponent>(e)) {
		registry.emplace<GDDynamicComponent>(e);
	}
	
	GDDynamicComponent &dyn = registry.get<GDDynamicComponent>(e);
	dyn.properties[p_name] = p_value;
}

Variant ECSWorld::get_dynamic_component(int64_t p_entity, const StringName &p_name) const {
	entt::entity e = (entt::entity)p_entity;
	ERR_FAIL_COND_V(!registry.valid(e), Variant());

	if (registry.all_of<GDDynamicComponent>(e)) {
		const GDDynamicComponent &dyn = registry.get<GDDynamicComponent>(e);
		if (dyn.properties.has(p_name)) {
			return dyn.properties[p_name];
		}
	}
	return Variant();
}

bool ECSWorld::has_dynamic_component(int64_t p_entity, const StringName &p_name) const {
	entt::entity e = (entt::entity)p_entity;
	if (!registry.valid(e)) return false;

	if (registry.all_of<GDDynamicComponent>(e)) {
		const GDDynamicComponent &dyn = registry.get<GDDynamicComponent>(e);
		return dyn.properties.has(p_name);
	}
	return false;
}

void ECSWorld::remove_dynamic_component(int64_t p_entity, const StringName &p_name) {
	entt::entity e = (entt::entity)p_entity;
	ERR_FAIL_COND(!registry.valid(e));

	if (registry.all_of<GDDynamicComponent>(e)) {
		GDDynamicComponent &dyn = registry.get<GDDynamicComponent>(e);
		dyn.properties.erase(p_name);
	}
}

TypedArray<int64_t> ECSWorld::query_dynamic_components(const TypedArray<StringName> &p_names) {
	TypedArray<int64_t> results;
	auto view = registry.view<GDDynamicComponent>();
	for (auto entity : view) {
		const GDDynamicComponent &dyn = view.get<GDDynamicComponent>(entity);
		bool has_all = true;
		for (int i = 0; i < p_names.size(); i++) {
			if (!dyn.properties.has(p_names[i])) {
				has_all = false;
				break;
			}
		}
		if (has_all) {
			results.push_back((int64_t)entity);
		}
	}
	return results;
}

void ECSWorld::add_system(Object *p_system) {
	ERR_FAIL_NULL(p_system);
	ECSSystem *sys = Object::cast_to<ECSSystem>(p_system);
	ERR_FAIL_COND_MSG(!sys, "Object must inherit from ECSSystem.");
	
	if (!active_systems.has(p_system)) {
		active_systems.push_back(p_system);
	}
}

void ECSWorld::remove_system(Object *p_system) {
	int idx = active_systems.find(p_system);
	if (idx != -1) {
		active_systems.remove_at(idx);
	}
}
