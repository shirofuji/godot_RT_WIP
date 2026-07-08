/* register_types.cpp */

#include "register_types.h"
#include "core/object/class_db.h"

#include "ecs_world.h"
#include "ecs_system.h"
#include "ecs_render_system.h"
#include "ecs_entity_authoring.h"

void initialize_ecs_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		ClassDB::register_class<ECSWorld>();
		ClassDB::register_class<ECSSystem>();
		ClassDB::register_class<ECSRenderSystem>();
		ClassDB::register_class<ECSEntityAuthoring>();
	}
}

void uninitialize_ecs_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		// Cleanup if necessary
	}
}
