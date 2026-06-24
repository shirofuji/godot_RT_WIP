extends Node3D

# Pulls the camera steadily away from a high-res sphere so the engine's LODCUT_DIAG print can show
# the post-cut visible meshlet count dropping as the sphere's screen size (and thus acceptable LOD
# error) shrinks with distance. Two process frames per step so the temporal Hi-Z / cut settles.
func _ready() -> void:
	var cam := $Camera3D
	for i in range(10):
		cam.transform.origin = Vector3(0, 0, 4.0 + float(i) * 6.0)
		cam.look_at(Vector3.ZERO, Vector3.UP)
		await get_tree().process_frame
		await get_tree().process_frame
	get_tree().quit()
