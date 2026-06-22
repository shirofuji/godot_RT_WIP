extends Node3D

# Runs several frames with the camera moving slightly between them, screenshotting each one -
# this is what actually exercises the temporal two-pass Hi-Z early pass under
# --meshlet-replace-default: meshlet_hiz_history_valid only becomes true at the end of frame 1's
# late pass, so the early pass never engages on frame 1 (a single-frame run, like this script used
# to be, only ever tests the "history invalid, fall back to single-pass" path). Frames 2+ engage
# the early pass; the camera move between frames is what can expose disocclusion-recovery bugs
# (the late pass should still fill in anything the early pass wrongly culled against stale Hi-Z).
const FRAME_COUNT := 6

func _ready() -> void:
	var camera := $Camera3D
	for i in range(FRAME_COUNT):
		await get_tree().process_frame
		var img := get_viewport().get_texture().get_image()
		var path := "user://meshlet_live_overlay_test_frame%d.png" % i
		var err := img.save_png(path)
		if err == OK:
			print("MESHLET_LIVE_TEST: saved frame ", i, " screenshot to ", ProjectSettings.globalize_path(path))
		else:
			print("MESHLET_LIVE_TEST: failed to save frame ", i, " screenshot, error ", err)
		# Small lateral move each frame - enough to shift which meshlets are near silhouette
		# edges/each other's occlusion boundaries without ever moving the sphere fully out of frame.
		camera.transform.origin.x += 0.15
		camera.look_at(Vector3.ZERO, Vector3.UP)
	# Keep the legacy single fixed filename pointing at the last frame, so any external tooling
	# that still looks for meshlet_live_overlay_test.png (no suffix) keeps working.
	var last_img := get_viewport().get_texture().get_image()
	last_img.save_png("user://meshlet_live_overlay_test.png")
	get_tree().quit()
