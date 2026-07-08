#include "ecs_system.h"
#include "ecs_world.h"
#include "core/object/class_db.h"

void ECSSystem::_bind_methods() {
	GDVIRTUAL_BIND(_update, "world", "delta");
}

void ECSSystem::update(ECSWorld *p_world, float p_delta) {
	// Call GDScript implementation if it exists
	GDVIRTUAL_CALL(_update, p_world, p_delta);
}

ECSSystem::ECSSystem() {
}

ECSSystem::~ECSSystem() {
}
