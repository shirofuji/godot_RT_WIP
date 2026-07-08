extends Node3D

# Isolated repro of the neesan ECS<->physics failure, OUTSIDE neesan.
#
# It routes bodies through the exact path the animal-AI uses:
#   ECSWorld  ->  KilosECSSystem.initialize(N, multimesh)  ->  bulk_body_* API
# and instruments the three reported symptoms:
#   (1) heavy / low FPS      -> we print FPS every 30 frames
#   (2) not spawned/rendered -> we count how many entities got a physics slot
#   (3) fall through terrain -> we sample body Y over time (should NOT free-fall)
#
# Toggle CONFIGURE_COLLISION to test the hypothesis that the bridge simply never
# calls bulk_set_collision()/bulk_set_sdf(), so bodies free-fall forever.

const N := 2000
const STEER_VELOCITY := Vector3(2, 0, 0)  # neesan-like horizontal wander: bodies should slide +X
const CONFIGURE_COLLISION := true    # driven through the bridge's set_collision API
const BIND_MULTIMESH := true         # full path: render bodies from the physics buffer
const SET_VELOCITY := true            # full path: AI steering velocity -> physics
const RADIUS := 0.5

var _world: ECSWorld
var _sys                              # KilosECSSystem (RefCounted) - keep a ref alive
var _entities: Array = []
var _frame := 0
var _sdf_tex: RID

@onready var mmi: MultiMeshInstance3D = $Bodies

func _ready() -> void:
	# --- MultiMesh set up exactly like the WORKING stress_test (best-case render) ---
	var mesh := BoxMesh.new()
	mesh.size = Vector3.ONE
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.4, 0.7, 1.0)
	mesh.material = mat

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = mesh
	mm.instance_count = N
	mmi.multimesh = mm
	mmi.custom_aabb = AABB(Vector3(-500, -2000, -500), Vector3(1000, 3000, 1000))

	# --- The ECS bridge, driven exactly as ECSEntityAuthoring/neesan drives it ---
	_world = ECSWorld.new()
	add_child(_world)

	_sys = KilosECSSystem.new()
	_world.add_system(_sys)
	_sys.initialize(N, mm.get_rid() if BIND_MULTIMESH else RID())

	if CONFIGURE_COLLISION:
		# Collision now configured THROUGH the bridge (the fix), not the raw server.
		_sys.set_collision(true, RADIUS, 0.0, 4)
		_build_sdf()   # sets the terrain SDF via _sys.set_terrain_sdf()

	# Author N entities with BOTH transform+velocity (KilosECSSystem query needs both).
	for i in N:
		var e := _world.create_entity()
		var t := Transform3D()
		t.origin = Vector3(
			randf_range(-90, 90),
			randf_range(40, 90),
			randf_range(-90, 90))
		_world.set_dynamic_component(e, "transform", t)
		if SET_VELOCITY:
			_world.set_dynamic_component(e, "velocity", STEER_VELOCITY)
		_entities.append(e)

	$Camera3D.look_at(Vector3(0, 15, 0), Vector3.UP)
	print("ECS-BRIDGE: authored %d entities, collision=%s" % [N, CONFIGURE_COLLISION])

func _build_sdf() -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	const DIM := 64
	var mn := Vector3(-100, -20, -100)
	var sz := Vector3(200, 120, 200)
	var data := PackedFloat32Array()
	data.resize(DIM * DIM * DIM)
	for z in DIM:
		for y in DIM:
			var row := (z * DIM + y) * DIM
			for x in DIM:
				var uvw := Vector3((x + 0.5) / DIM, (y + 0.5) / DIM, (z + 0.5) / DIM)
				var p := mn + uvw * sz
				data[row + x] = p.y   # solid ground half-space y<0
	var tf := RDTextureFormat.new()
	tf.format = RenderingDevice.DATA_FORMAT_R32_SFLOAT
	tf.width = DIM; tf.height = DIM; tf.depth = DIM
	tf.texture_type = RenderingDevice.TEXTURE_TYPE_3D
	tf.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT
	_sdf_tex = rd.texture_create(tf, RDTextureView.new(), [data.to_byte_array()])
	_sys.set_terrain_sdf(_sdf_tex, AABB(mn, sz))

func _process(_dt: float) -> void:
	_frame += 1
	if _frame % 30 == 0:
		# Sample body 0's X and Y over time. With STEER_VELOCITY=(2,0,0), X must climb.
		var tt = _world.get_dynamic_component(_entities[0], "transform")
		var px := 0.0
		var py := 0.0
		if tt is Transform3D:
			px = tt.origin.x
			py = tt.origin.y
		print("frame %4d | FPS %3d | body0 X = %.2f  Y = %.2f" % [
			_frame, Engine.get_frames_per_second(), px, py])
	if _frame >= 240:
		print("ECS-BRIDGE: done")
		get_tree().quit()

func _exit_tree() -> void:
	if _sdf_tex.is_valid():
		var rd := RenderingServer.get_rendering_device()
		if rd:
			rd.free_rid(_sdf_tex)
