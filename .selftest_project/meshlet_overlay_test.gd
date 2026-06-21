extends Node3D

func _ready() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	var img := get_viewport().get_texture().get_image()
	var path := "user://meshlet_live_overlay_test.png"
	var err := img.save_png(path)
	if err == OK:
		print("MESHLET_LIVE_TEST: saved screenshot to ", ProjectSettings.globalize_path(path))
	else:
		print("MESHLET_LIVE_TEST: failed to save screenshot, error ", err)
	get_tree().quit()
