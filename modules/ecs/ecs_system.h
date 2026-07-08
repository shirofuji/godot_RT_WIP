#ifndef ECS_SYSTEM_H
#define ECS_SYSTEM_H

#include "core/object/ref_counted.h"
#include "core/object/gdvirtual.gen.h"

class ECSWorld;

class ECSSystem : public RefCounted {
	GDCLASS(ECSSystem, RefCounted);

protected:
	static void _bind_methods();

public:
	// C++ override
	virtual void update(ECSWorld *p_world, float p_delta);

	// GDScript override
	GDVIRTUAL2(_update, Object *, float)

	ECSSystem();
	~ECSSystem();
};

#endif // ECS_SYSTEM_H
