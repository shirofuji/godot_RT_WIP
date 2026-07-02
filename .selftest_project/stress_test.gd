extends Node3D

# P2 proof: spawn N GPU-resident physics bodies through the kilos bulk API and render
# them straight from the physics buffer via a single MultiMesh - no per-body node, no
# per-body CPU cost. Compare against the old path (350k individual MeshInstance3D nodes).
# Bodies free-fall (collision lands in P3); we periodically re-scatter so it loops.

const N := 300000
const RESPAWN_EVERY := 12.0
const RADIUS := 0.5
const GROUND_Y := 0.0

var _handle := -1
var _accum := 0.0

@onready var mmi: MultiMeshInstance3D = $Bodies
@onready var label: Label = $UI/Label

func _ready() -> void:
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
	# We never set instance transforms on the CPU, so the renderer's data_cache stays
	# empty and it won't clobber our GPU compute writes. That also means the auto AABB
	# is zero, so set a large custom AABB or the whole batch gets frustum-culled away.
	mmi.custom_aabb = AABB(Vector3(-500, -2000, -500), Vector3(1000, 3000, 1000))

	# Enable GPU body-body + ground collision (spatial-hash broad phase + PBD solve).
	PhysicsServer3D.bulk_set_collision(true, RADIUS, GROUND_Y, 4)

	_handle = PhysicsServer3D.bulk_body_create(N)
	_scatter()
	PhysicsServer3D.bulk_body_set_multimesh(_handle, mm.get_rid())

	$Camera3D.look_at(Vector3(0, 10, 0), Vector3.UP)

	print("STRESS: spawned ", N, " GPU bodies (collision on), handle=", _handle)

func _scatter() -> void:
	# Rain down over a wide floor area so they pile a few bodies deep.
	PhysicsServer3D.bulk_body_scatter(_handle, AABB(Vector3(-160, 30, -160), Vector3(320, 120, 320)))

func _process(dt: float) -> void:
	_accum += dt
	if _accum >= RESPAWN_EVERY:
		_accum = 0.0
		_scatter()
	label.text = "GPU bodies: %d\nFPS: %d" % [N, Engine.get_frames_per_second()]

func _exit_tree() -> void:
	if _handle >= 0:
		PhysicsServer3D.bulk_body_free(_handle)
