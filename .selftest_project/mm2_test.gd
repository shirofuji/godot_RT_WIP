extends Node3D

# MM2: a MultiMesh of a high-res sphere with random Y-rotation + random non-uniform scale (the
# realistic flora case), pulled steadily away from the camera. Run with:
#   --meshlet-replace-default --meshlet-lod-diag
# Each frame the engine prints MESHLET_LOD_DIAG: instances=<N> visible_meshlets=<M>. N (the count of
# expanded MultiMesh instances scanned) stays fixed; M should DROP as the camera pulls back - that
# drop is per-instance continuous LOD working on MultiMesh, which is the whole deliverable.
const INSTANCE_COUNT := 800

func _ready() -> void:
	var mesh := SphereMesh.new()
	mesh.radius = 1.0
	mesh.height = 2.0
	mesh.radial_segments = 64
	mesh.rings = 32
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.3, 0.7, 0.35)
	mesh.material = mat

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = mesh
	mm.instance_count = INSTANCE_COUNT
	var rng := RandomNumberGenerator.new()
	rng.seed = 1234
	for i in INSTANCE_COUNT:
		var pos := Vector3(rng.randf_range(-18, 18), rng.randf_range(-7, 7), rng.randf_range(-18, 18))
		var b := Basis(Vector3.UP, rng.randf_range(0.0, TAU))
		b = b.scaled(Vector3(rng.randf_range(0.6, 1.6), rng.randf_range(0.6, 1.6), rng.randf_range(0.6, 1.6)))
		mm.set_instance_transform(i, Transform3D(b, pos))

	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = mm
	add_child(mmi)

	var cam := $Camera3D
	# Let the async DAG bake land (CLOD records arrive a few frames after mesh creation).
	for i in range(14):
		await get_tree().process_frame

	for step in range(6):
		cam.transform.origin = Vector3(0, 0, 28.0 + float(step) * 26.0)
		cam.look_at(Vector3.ZERO, Vector3.UP)
		await get_tree().process_frame
		await get_tree().process_frame
		print("MM2_TEST: step ", step, " camera_z=", cam.transform.origin.z)
		if step == 0:
			get_viewport().get_texture().get_image().save_png("user://mm2_near.png")
	get_viewport().get_texture().get_image().save_png("user://mm2_far.png")
	print("MM2_TEST: done")
	get_tree().quit()
