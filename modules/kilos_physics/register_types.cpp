#include "register_types.h"

#include "kilos_physics_server_3d.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/physics_3d/physics_server_3d_wrap_mt.h"

static PhysicsServer3D *_createKilosPhysics3DCallback() {
#ifdef THREADS_ENABLED
	bool using_threads = GLOBAL_GET("physics/3d/run_on_separate_thread");
#else
	bool using_threads = false;
#endif

	PhysicsServer3D *physics_server_3d = memnew(KilosPhysicsServer3D(using_threads));

	return memnew(PhysicsServer3DWrapMT(physics_server_3d, using_threads));
}

#include "core/object/class_db.h"

void initialize_kilos_physics_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	GDREGISTER_CLASS(KilosPhysicsServer3D);
	GDREGISTER_CLASS(KilosDirectSpaceState3D);
	PhysicsServer3DManager::get_singleton()->register_server("KilosPhysics3D", callable_mp_static(_createKilosPhysics3DCallback));
	PhysicsServer3DManager::get_singleton()->set_default_server("KilosPhysics3D");
}

void uninitialize_kilos_physics_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
}
