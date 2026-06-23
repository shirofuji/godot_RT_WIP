extends Node3D

# SVOGI builds its octree incrementally over several frames (cascade region updates), similar to
# SDFGI's own update cadence - run enough frames for it to converge before screenshotting.
const FRAME_COUNT := 30

func _ready() -> void:
	for i in range(FRAME_COUNT):
		await get_tree().process_frame
		if i == FRAME_COUNT - 1:
			var img := get_viewport().get_texture().get_image()
			var path := "user://svogi_test_result.png"
			var err := img.save_png(path)
			if err == OK:
				print("SVOGI_TEST: saved screenshot to ", ProjectSettings.globalize_path(path))
			else:
				print("SVOGI_TEST: failed to save screenshot, error ", err)
	get_tree().quit()
