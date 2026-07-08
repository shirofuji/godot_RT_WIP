extends SceneTree

func _init():
	print("=======================================")
	print("      ECS MODULE BASIC API TEST        ")
	print("=======================================")
	
	# 1. World Creation
	var world = ECSWorld.new()
	print("[OK] ECSWorld instantiated successfully.")
	
	# 2. Entity Management
	var ent1 = world.create_entity()
	var ent2 = world.create_entity()
	print("[OK] Created entities. IDs: ", ent1, ", ", ent2)
	
	# 3. Dynamic Components
	world.set_dynamic_component(ent1, "health", 100)
	world.set_dynamic_component(ent1, "name", "Wolf_1")
	world.set_dynamic_component(ent2, "health", 80)
	
	var h1 = world.get_dynamic_component(ent1, "health")
	var h2 = world.get_dynamic_component(ent2, "health")
	var has_name = world.has_dynamic_component(ent2, "name")
	
	print("[OK] Entity 1 Health: ", h1)
	print("[OK] Entity 2 Health: ", h2)
	print("[OK] Entity 2 Has Name Component: ", has_name)
	
	# 4. System Test
	var sys = ECSSystem.new()
	world.add_system(sys)
	print("[OK] ECSSystem instantiated and added to world.")
	
	# 4.5. Query Test
	var query_results = world.query_dynamic_components(["health"])
	print("[OK] Queried entities with 'health': ", query_results)
	var query_results2 = world.query_dynamic_components(["health", "name"])
	print("[OK] Queried entities with 'health' AND 'name': ", query_results2)
	
	# 5. Entity Destruction
	world.destroy_entity(ent1)
	var is_alive = world.is_alive(ent1)
	print("[OK] Entity 1 destroyed. Is Alive? ", is_alive)
	
	print("=======================================")
	print("        TEST COMPLETED SECURELY        ")
	print("=======================================")
	quit()
