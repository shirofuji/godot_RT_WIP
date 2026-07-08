#include "ecs_entity_authoring.h"
#include "core/config/engine.h"
#include "core/object/class_db.h"

void ECSEntityAuthoring::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_components", "components"), &ECSEntityAuthoring::set_components);
	ClassDB::bind_method(D_METHOD("get_components"), &ECSEntityAuthoring::get_components);

	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "components"), "set_components", "get_components");
}

void ECSEntityAuthoring::_notification(int p_what) {
	// We bake during ENTER_TREE, which fires before _ready()
	if (p_what == NOTIFICATION_ENTER_TREE) {
		if (Engine::get_singleton()->is_editor_hint()) {
			return; // Behave as a standard 3D node in the editor.
		}

		Node *parent = get_parent();
		while (parent) {
			ECSWorld *world = Object::cast_to<ECSWorld>(parent);
			if (world) {
				int64_t entity = world->create_entity();
				
				// Transfer the dictionary into dynamic components
				Array keys = components.keys();
				for (int i = 0; i < keys.size(); i++) {
					world->set_dynamic_component(entity, keys[i], components[keys[i]]);
				}
				
				// Implicitly attach its global 3D transform so rendering systems know where it is
				world->set_dynamic_component(entity, "transform", get_global_transform());

				// Self-destruct immediately to save runtime node overhead
				queue_free();
				break;
			}
			parent = parent->get_parent();
		}
	}
}

void ECSEntityAuthoring::set_components(const Dictionary &p_components) {
	components = p_components;
}

Dictionary ECSEntityAuthoring::get_components() const {
	return components;
}

ECSEntityAuthoring::ECSEntityAuthoring() {
}

ECSEntityAuthoring::~ECSEntityAuthoring() {
}
