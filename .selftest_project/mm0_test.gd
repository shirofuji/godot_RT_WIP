extends Node3D

# MM0: render a sphere with a combined 45-degree Y-rotation + non-uniform scale (2 / 0.5 / 1,
# determinant +1) through the meshlet path, then screenshot it. This is the exact repro from the
# project memory's "rotation + non-uniform scale renders as a hollow ring" note - the cull shader
# now has an inverse-transpose cofactor cone-axis transform that claims to fix it, so this checks
# whether the bug is actually gone on this branch. Run with --meshlet-replace-default.
# A correct render = a fully-shaded squashed/rotated sphere. The bug = only a thin silhouette ring
# with the dark background showing through the middle.
func _ready() -> void:
	# A few frames so the temporal two-pass Hi-Z early pass engages (valid only after frame 1).
	for i in range(5):
		await get_tree().process_frame
	var img := get_viewport().get_texture().get_image()
	var path := "user://mm0_rot_nonuniform.png"
	var err := img.save_png(path)
	print("MM0_TEST: saved screenshot to ", ProjectSettings.globalize_path(path), " err=", err)
	get_tree().quit()
