extends Node3D

# P2/P3/P4 proof: spawn N GPU-resident physics bodies through the kilos bulk API and
# render them straight from the physics buffer via a single MultiMesh - no per-body
# node, no per-body CPU cost. They collide with each other and with a static-world SDF
# (a mountain on a floor), all on the GPU. Compare against the old path (350k
# individual MeshInstance3D nodes). We periodically re-scatter so it loops.

const N := 250000
const RESPAWN_EVERY := 12.0
const RADIUS := 0.5

# SDF volume (world-space region the baked field covers).
const SDF_DIM := 64
const SDF_MIN := Vector3(-100, -20, -100)
const SDF_SIZE := Vector3(200, 120, 200)
const DOME_CENTER := Vector3(0, -8, 0)
const DOME_R := 40.0

var _handle := -1
var _accum := 0.0
var _sdf_tex: RID

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

	# Enable GPU collision (spatial-hash broad phase + PBD solve) and bind an SDF world.
	PhysicsServer3D.bulk_set_collision(true, RADIUS, 0.0, 4)
	_build_sdf()

	_handle = PhysicsServer3D.bulk_body_create(N)
	_scatter()
	PhysicsServer3D.bulk_body_set_multimesh(_handle, mm.get_rid())

	$Camera3D.look_at(Vector3(0, 15, 0), Vector3.UP)

	print("STRESS: spawned ", N, " GPU bodies (collision + SDF world), handle=", _handle)

func _build_sdf() -> void:
	# Bake a static-world SDF: union of a solid ground (y < 0) and a central dome.
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	var data := PackedFloat32Array()
	data.resize(SDF_DIM * SDF_DIM * SDF_DIM)
	for z in SDF_DIM:
		for y in SDF_DIM:
			var row := (z * SDF_DIM + y) * SDF_DIM
			for x in SDF_DIM:
				var uvw := Vector3((x + 0.5) / SDF_DIM, (y + 0.5) / SDF_DIM, (z + 0.5) / SDF_DIM)
				var p := SDF_MIN + uvw * SDF_SIZE
				var d_ground := p.y # SDF of the solid half-space y < 0
				var d_dome := p.distance_to(DOME_CENTER) - DOME_R
				data[row + x] = minf(d_ground, d_dome)

	var tf := RDTextureFormat.new()
	tf.format = RenderingDevice.DATA_FORMAT_R32_SFLOAT
	tf.width = SDF_DIM
	tf.height = SDF_DIM
	tf.depth = SDF_DIM
	tf.texture_type = RenderingDevice.TEXTURE_TYPE_3D
	tf.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT
	_sdf_tex = rd.texture_create(tf, RDTextureView.new(), [data.to_byte_array()])
	PhysicsServer3D.bulk_set_sdf(_sdf_tex, AABB(SDF_MIN, SDF_SIZE))

func _scatter() -> void:
	# Rain down over the SDF-covered area so they land on the mountain and floor.
	PhysicsServer3D.bulk_body_scatter(_handle, AABB(Vector3(-90, 45, -90), Vector3(180, 90, 180)))

func _process(dt: float) -> void:
	_accum += dt
	if _accum >= RESPAWN_EVERY:
		_accum = 0.0
		_scatter()
	label.text = "GPU bodies: %d\nFPS: %d" % [N, Engine.get_frames_per_second()]

func _exit_tree() -> void:
	if _handle >= 0:
		PhysicsServer3D.bulk_body_free(_handle)
	if _sdf_tex.is_valid():
		RenderingServer.get_rendering_device().free_rid(_sdf_tex)
