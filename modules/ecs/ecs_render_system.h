#ifndef ECS_RENDER_SYSTEM_H
#define ECS_RENDER_SYSTEM_H

#include "ecs_system.h"

class ECSRenderSystem : public ECSSystem {
	GDCLASS(ECSRenderSystem, ECSSystem);

protected:
	static void _bind_methods();

public:
	virtual void update(ECSWorld *p_world, float p_delta) override;

	ECSRenderSystem();
	~ECSRenderSystem();
};

#endif // ECS_RENDER_SYSTEM_H
