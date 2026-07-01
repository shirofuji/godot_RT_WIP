extends Node3D

# MM1: verify alpha-scissor (cutout) discard in the meshlet path. A SPHERE (renders correctly through
# the meshlet path, unlike a one-sided plane which CULL_FRONT removes) with a checkerboard-alpha
# StandardMaterial3D in TRANSPARENCY_ALPHA_SCISSOR. Correct = the surface is punched full of holes
# (dark background visible through the transparent checker cells); broken = a solid orange sphere.
# Run with --meshlet-replace-default.
func _ready() -> void:
	var img := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	for y in 8:
		for x in 8:
			var on := (x + y) % 2 == 0
			img.set_pixel(x, y, Color(0.95, 0.55, 0.2, 1.0 if on else 0.0))
	var tex := ImageTexture.create_from_image(img)

	var mat := StandardMaterial3D.new()
	mat.albedo_texture = tex
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	mat.alpha_scissor_threshold = 0.5

	var sphere := SphereMesh.new()
	sphere.radius = 1.5
	sphere.height = 3.0
	sphere.radial_segments = 32
	sphere.rings = 16

	var mi := MeshInstance3D.new()
	mi.mesh = sphere
	mi.material_override = mat
	add_child(mi)

	for i in range(6):
		await get_tree().process_frame
	get_viewport().get_texture().get_image().save_png("user://mm1_cutout.png")
	print("MM1_TEST: done")
	get_tree().quit()
