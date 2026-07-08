#ifndef ECS_ENTITY_AUTHORING_H
#define ECS_ENTITY_AUTHORING_H

#include "scene/3d/node_3d.h"
#include "ecs_world.h"

class ECSEntityAuthoring : public Node3D {
	GDCLASS(ECSEntityAuthoring, Node3D);

private:
	Dictionary components;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_components(const Dictionary &p_components);
	Dictionary get_components() const;

	ECSEntityAuthoring();
	~ECSEntityAuthoring();
};

#endif // ECS_ENTITY_AUTHORING_H
