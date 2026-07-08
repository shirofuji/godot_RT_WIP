#ifndef KILOS_ECS_SYSTEM_H
#define KILOS_ECS_SYSTEM_H

#include "modules/ecs/ecs_system.h"
#include "modules/ecs/ecs_world.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

class KilosECSSystem : public ECSSystem {
	GDCLASS(KilosECSSystem, ECSSystem);

private:
	int bulk_handle = -1;
	int max_slots = 0;
	HashMap<int64_t, int> entity_to_slot;
	Vector<int> free_slots;

protected:
	static void _bind_methods();

public:
	void initialize(int p_max_entities, RID p_multimesh);

	// Configure bulk collision so bodies don't free-fall through the world. These
	// forward to the (global) bulk physics state; call after initialize().
	void set_collision(bool p_enabled, real_t p_radius, real_t p_ground_y, int p_iterations);
	void set_terrain_sdf(RID p_sdf_texture, const AABB &p_bounds);

	virtual void update(ECSWorld *p_world, float p_delta) override;

	KilosECSSystem();
	~KilosECSSystem();
};

#endif // KILOS_ECS_SYSTEM_H
