#ifndef ECS_WORLD_H
#define ECS_WORLD_H

#include "scene/main/node.h"
#include "core/variant/typed_array.h"
#include "entt.hpp"

// We use a single struct to store dynamic GDScript properties for an entity.
struct GDDynamicComponent {
	Dictionary properties;
};

class ECSWorld : public Node {
	GDCLASS(ECSWorld, Node);

private:
	entt::registry registry;
	TypedArray<Object> active_systems;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	// Entity management
	int64_t create_entity();
	void destroy_entity(int64_t p_entity);
	bool is_alive(int64_t p_entity) const;

	// Dynamic GDScript component support
	void set_dynamic_component(int64_t p_entity, const StringName &p_name, const Variant &p_value);
	Variant get_dynamic_component(int64_t p_entity, const StringName &p_name) const;
	bool has_dynamic_component(int64_t p_entity, const StringName &p_name) const;
	void remove_dynamic_component(int64_t p_entity, const StringName &p_name);
	
	TypedArray<int64_t> query_dynamic_components(const TypedArray<StringName> &p_names);

	// System management
	void add_system(Object *p_system);
	void remove_system(Object *p_system);

	// Direct access to EnTT for C++ systems
	entt::registry &get_registry() { return registry; }

	ECSWorld();
	~ECSWorld();
};

#endif // ECS_WORLD_H
