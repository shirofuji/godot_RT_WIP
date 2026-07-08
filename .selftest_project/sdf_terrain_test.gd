extends Node3D

# Validates the neesan approach: build a heightfield SDF by raycasting the real
# terrain colliders, bind it to the Kilos bulk solver, and confirm bulk bodies
# settle ON the terrain surface (not at ground_y=0, not sinking through).
#
# "Terrain" = flat ground at y~0 plus a raised platform at y~20 on the +X side.
# Bodies dropped over the ground must land near 0; bodies over the platform near 20.

const N := 400
const SDF_DIM := 48
const SDF_EXTENT := 96.0
const SDF_VEXTENT := 96.0
const RADIUS := 0.5

var _handle := -1
var _sdf_tex: RID
var _frame := 0

func _ready() -> void:
	_make_static_box(Vector3(0, -0.5, 0), Vector3(400, 1, 400))   # ground, top ~= 0
	_make_static_box(Vector3(40, 10, 0), Vector3(40, 20, 80))     # platform, top ~= 20
	# Let the static bodies register with the physics space before raycasting.
	await get_tree().physics_frame
	await get_tree().physics_frame

	_build_sdf(Vector3(0, 0, 0))

	PhysicsServer3D.bulk_set_collision(true, RADIUS, 0.0, 8)
	_handle = PhysicsServer3D.bulk_body_create(N)
	PhysicsServer3D.bulk_body_set_tracked(_handle, true)
	# Half the bodies over the ground (x<0), half over the platform (x~40), all dropped high.
	for i in N:
		var x := randf_range(-70, -10) if i % 2 == 0 else randf_range(25, 55)
		var p := Vector3(x, 45.0, randf_range(-30, 30))
		PhysicsServer3D.bulk_body_set_position(_handle, i, p)
	print("SDF-TERRAIN: dropped %d bodies (ground top~0, platform top~20)" % N)

func _make_static_box(pos: Vector3, size: Vector3) -> void:
	var body := StaticBody3D.new()
	var cs := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = size
	cs.shape = box
	body.add_child(cs)
	body.position = pos
	add_child(body)

func _build_sdf(center: Vector3) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	var space_state = get_world_3d().direct_space_state
	var mn := Vector3(center.x - SDF_EXTENT, center.y - SDF_VEXTENT, center.z - SDF_EXTENT)
	var sz := Vector3(SDF_EXTENT * 2.0, SDF_VEXTENT * 2.0, SDF_EXTENT * 2.0)

	var heights := PackedFloat32Array()
	heights.resize(SDF_DIM * SDF_DIM)
	for zi in SDF_DIM:
		for xi in SDF_DIM:
			var wx := mn.x + (xi + 0.5) / float(SDF_DIM) * sz.x
			var wz := mn.z + (zi + 0.5) / float(SDF_DIM) * sz.z
			var q := PhysicsRayQueryParameters3D.create(
				Vector3(wx, center.y + SDF_VEXTENT + 50.0, wz),
				Vector3(wx, center.y - SDF_VEXTENT - 50.0, wz))
			var r := space_state.intersect_ray(q)
			heights[zi * SDF_DIM + xi] = r.position.y if r else (mn.y - 1000.0)

	var data := PackedFloat32Array()
	data.resize(SDF_DIM * SDF_DIM * SDF_DIM)
	for zi in SDF_DIM:
		for yi in SDF_DIM:
			var wy := mn.y + (yi + 0.5) / float(SDF_DIM) * sz.y
			var row := (zi * SDF_DIM + yi) * SDF_DIM
			for xi in SDF_DIM:
				data[row + xi] = wy - heights[zi * SDF_DIM + xi]

	var tf := RDTextureFormat.new()
	tf.format = RenderingDevice.DATA_FORMAT_R32_SFLOAT
	tf.width = SDF_DIM
	tf.height = SDF_DIM
	tf.depth = SDF_DIM
	tf.texture_type = RenderingDevice.TEXTURE_TYPE_3D
	tf.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT
	_sdf_tex = rd.texture_create(tf, RDTextureView.new(), [data.to_byte_array()])
	PhysicsServer3D.bulk_set_sdf(_sdf_tex, AABB(mn, sz))
	print("SDF-TERRAIN: built %d^3 SDF from raycasts" % SDF_DIM)

func _process(_dt: float) -> void:
	if _handle < 0:
		return
	_frame += 1
	if _frame % 60 == 0:
		# Sample 30 bodies from each group (even idx = over ground, odd = over platform).
		var ground_y := 0.0
		var plat_y := 0.0
		for k in 30:
			ground_y += PhysicsServer3D.bulk_body_get_transform(_handle, k * 2).origin.y
			plat_y += PhysicsServer3D.bulk_body_get_transform(_handle, k * 2 + 1).origin.y
		print("frame %3d | over-ground avg Y = %.2f (want ~0.5) | over-platform avg Y = %.2f (want ~20.5)" % [
			_frame, ground_y / 30.0, plat_y / 30.0])
	if _frame >= 800:
		get_tree().quit()

func _exit_tree() -> void:
	if _handle >= 0:
		PhysicsServer3D.bulk_body_free(_handle)
	if _sdf_tex.is_valid():
		var rd := RenderingServer.get_rendering_device()
		if rd:
			rd.free_rid(_sdf_tex)
