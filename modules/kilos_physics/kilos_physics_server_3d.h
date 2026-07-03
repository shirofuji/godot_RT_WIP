
#ifndef KILOS_PHYSICS_SERVER_3D_H
#define KILOS_PHYSICS_SERVER_3D_H

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "core/templates/local_vector.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "shaders/body_to_multimesh.glsl.gen.h"
#include "shaders/integration.glsl.gen.h"

class KilosPhysicsServer3D;

class KilosDirectSpaceState3D : public PhysicsDirectSpaceState3D {
	GDCLASS(KilosDirectSpaceState3D, PhysicsDirectSpaceState3D);

public:
	KilosPhysicsServer3D *server = nullptr;
	RID space;

	virtual bool intersect_ray(const RayParameters &p_parameters, RayResult &r_result) override;
	virtual int intersect_point(const PointParameters &p_parameters, ShapeResult *r_results, int p_result_max) override { return 0; }
	virtual int intersect_shape(const ShapeParameters &p_parameters, ShapeResult *r_results, int p_result_max) override { return 0; }
	virtual bool cast_motion(const ShapeParameters &p_parameters, real_t &p_closest_safe, real_t &p_closest_unsafe, ShapeRestInfo *r_info = nullptr) override { return false; }
	virtual bool collide_shape(const ShapeParameters &p_parameters, Vector3 *r_results, int p_result_max, int &r_result_count) override { return false; }
	virtual bool rest_info(const ShapeParameters &p_parameters, ShapeRestInfo *r_info) override { return false; }
	virtual Vector3 get_closest_point_to_object_volume(RID p_object, const Vector3 p_point) const override { return Vector3(); }
};

class KilosPhysicsServer3D;

class KilosDirectBodyState3D : public PhysicsDirectBodyState3D {
	GDCLASS(KilosDirectBodyState3D, PhysicsDirectBodyState3D);

public:
	KilosPhysicsServer3D *server = nullptr;
	RID body;

	virtual Vector3 get_total_gravity() const override;
	virtual real_t get_total_angular_damp() const override;
	virtual real_t get_total_linear_damp() const override;

	virtual Vector3 get_center_of_mass() const override;
	virtual Vector3 get_center_of_mass_local() const override;
	virtual Basis get_principal_inertia_axes() const override;
	virtual real_t get_inverse_mass() const override;
	virtual Vector3 get_inverse_inertia() const override;
	virtual Basis get_inverse_inertia_tensor() const override;

	virtual void set_linear_velocity(const Vector3 &p_velocity) override;
	virtual Vector3 get_linear_velocity() const override;

	virtual void set_angular_velocity(const Vector3 &p_velocity) override;
	virtual Vector3 get_angular_velocity() const override;

	virtual void set_transform(const Transform3D &p_transform) override;
	virtual Transform3D get_transform() const override;

	virtual Vector3 get_velocity_at_local_position(const Vector3 &p_position) const override;

	virtual void apply_central_impulse(const Vector3 &p_impulse) override;
	virtual void apply_impulse(const Vector3 &p_impulse, const Vector3 &p_position = Vector3()) override;
	virtual void apply_torque_impulse(const Vector3 &p_impulse) override;

	virtual void apply_central_force(const Vector3 &p_force) override;
	virtual void apply_force(const Vector3 &p_force, const Vector3 &p_position = Vector3()) override;
	virtual void apply_torque(const Vector3 &p_torque) override;

	virtual void add_constant_central_force(const Vector3 &p_force) override;
	virtual void add_constant_force(const Vector3 &p_force, const Vector3 &p_position = Vector3()) override;
	virtual void add_constant_torque(const Vector3 &p_torque) override;

	virtual void set_constant_force(const Vector3 &p_force) override;
	virtual Vector3 get_constant_force() const override;

	virtual void set_constant_torque(const Vector3 &p_torque) override;
	virtual Vector3 get_constant_torque() const override;

	virtual void set_sleep_state(bool p_sleep) override;
	virtual bool is_sleeping() const override;

	virtual void set_collision_layer(uint32_t p_layer) override;
	virtual uint32_t get_collision_layer() const override;

	virtual void set_collision_mask(uint32_t p_mask) override;
	virtual uint32_t get_collision_mask() const override;

	virtual int get_contact_count() const override;

	virtual Vector3 get_contact_local_position(int p_contact_idx) const override;
	virtual Vector3 get_contact_local_normal(int p_contact_idx) const override;
	virtual Vector3 get_contact_impulse(int p_contact_idx) const override;
	virtual int get_contact_local_shape(int p_contact_idx) const override;
	virtual Vector3 get_contact_local_velocity_at_position(int p_contact_idx) const override;

	virtual RID get_contact_collider(int p_contact_idx) const override;
	virtual Vector3 get_contact_collider_position(int p_contact_idx) const override;
	virtual ObjectID get_contact_collider_id(int p_contact_idx) const override;
	virtual Object *get_contact_collider_object(int p_contact_idx) const override;
	virtual int get_contact_collider_shape(int p_contact_idx) const override;
	virtual Vector3 get_contact_collider_velocity_at_position(int p_contact_idx) const override;

	virtual real_t get_step() const override;
	virtual void integrate_forces() override;

	virtual RequiredResult<PhysicsDirectSpaceState3D> get_space_state() override;
};

class KilosPhysicsServer3D : public PhysicsServer3D {
	GDCLASS(KilosPhysicsServer3D, PhysicsServer3D);

	KilosDirectSpaceState3D *space_state = nullptr;
	KilosDirectBodyState3D *direct_state = nullptr;

private:
	// GPU-side per-body record (std430-friendly: six vec4 = 96 bytes).
	// linear_velocity.w = mass (0 => static/kinematic, shader early-outs).
	// angular_velocity.w = inverse mass. Remaining fields reserved for P3 solver.
	struct RigidBodyData {
		float position[4];
		float rotation[4];
		float linear_velocity[4];
		float angular_velocity[4];
		float center_of_mass[4];
		float principal_inertia[4];
	};

	struct MeshletCollider {
		RID mesh;
		Transform3D transform;
	};
	mutable RID_Owner<MeshletCollider> meshlet_collider_owner;

	// A flattened binary BVH node (used both per-shape over triangles and
	// per-space over body AABBs). Leaves index a range in an order array.
	struct BVHNode {
		AABB bounds;
		int left = -1; // child node index, or -1 for a leaf
		int right = -1;
		int tri_start = 0; // into the order array (leaf only)
		int tri_count = 0;
	};

	struct KilosSpace {
		HashSet<RID> bodies; // bodies assigned to this space (for CPU queries)
		// Broadphase BVH over body world-AABBs (lazy; rebuilt when bodies change).
		Vector<RID> bvh_bodies;
		Vector<int> bvh_order;
		Vector<BVHNode> bvh;
		bool bvh_dirty = true;
		// Bodies added since the last build, not yet in the BVH. Queries scan these
		// linearly so incremental adds (e.g. GridMap tree colliders placed between
		// raycasts) don't force a full rebuild each time.
		Vector<RID> bvh_pending;
	};
	mutable RID_Owner<KilosSpace> space_owner;

	struct KilosShape {
		ShapeType type = SHAPE_CUSTOM;
		// Concave (trimesh) - triangle soup, 3 vertices per triangle.
		Vector<Vector3> faces;
		bool backface = false;
		Vector<BVHNode> bvh;
		Vector<int> tri_order; // triangle index per BVH leaf entry
		// Primitives.
		Vector3 box_half;
		Vector3 center; // local centre offset (non-zero for convex-as-box)
		real_t radius = 0.5;
		real_t height = 1.0;
		AABB local_aabb;
	};
	mutable RID_Owner<KilosShape> shape_owner;

	void _build_shape_bvh(KilosShape *p_shape) const;

	struct KilosBody {
		uint32_t slot = 0; // index into the GPU body pool
		bool has_slot = false;
		PhysicsServer3D::BodyMode mode = PhysicsServer3D::BODY_MODE_STATIC;
		Transform3D transform;
		Vector3 linear_velocity;
		Vector3 angular_velocity;
		real_t mass = 1.0;
		real_t gravity_scale = 1.0;
		ObjectID instance_id;
		Callable state_sync_callback;
		uint32_t collision_layer = 1;
		uint32_t collision_mask = 1;
		bool active = false;
		bool tracked = false; // read GPU state back to CPU each frame
		RID space;
		struct BodyShape {
			RID shape;
			Transform3D xform;
			bool disabled = false;
		};
		Vector<BodyShape> shapes;
	};
	mutable RID_Owner<KilosBody> body_owner;

	// CPU ray cast against all shapes of one body; updates the closest hit.
	bool _body_raycast(const KilosBody *p_body, RID p_body_rid, const Vector3 &p_from, const Vector3 &p_to, bool p_hit_back, real_t &r_closest_t, PhysicsDirectSpaceState3D::RayResult &r_result) const;
	AABB _body_world_aabb(const KilosBody *p_body) const;
	void _build_space_bvh(KilosSpace *p_space);
	bool _intersect_ray_unlocked(RID p_space, const PhysicsDirectSpaceState3D::RayParameters &p_parameters, PhysicsDirectSpaceState3D::RayResult &r_result);

	// Guards the CPU collision state (shapes, body shape lists, space body sets and
	// broadphase BVH) so main-thread queries don't race the physics thread creating
	// colliders. Required because the project runs physics on a separate thread.
	Mutex collision_mutex;

	struct KilosArea {};
	mutable RID_Owner<KilosArea> area_owner;

	struct KilosJoint {};
	mutable RID_Owner<KilosJoint> joint_owner;

	// === CPU-side body pool bookkeeping (physics thread) ===
	// P1 keeps a full CPU shadow as the upload source; the GPU-resident bulk
	// path in P2 replaces this for the visual majority.
	LocalVector<RigidBodyData> body_data;
	uint32_t body_high_water = 0; // slots [0, body_high_water) may be live => dispatch size
	Vector<uint32_t> free_slots;
	Vector<uint32_t> dirty_slots; // slots written since the last step
	Vector<uint32_t> tracked_slots; // slots whose GPU state we read back
	Vector<RID> active_bodies; // bodies with a state-sync callback, iterated in flush_queries

	// === Collision config (P3) ===
	bool collision_enabled = false;
	float body_radius = 0.5f;
	float ground_y = 0.0f;
	int solve_iterations = 4;
	float velocity_damping = 2.0f; // per-second linear damping, helps piles settle
	// Static-world SDF (P4). sdf_texture is an RD 3D texture RID; sdf_bounds is the
	// world-space region it covers.
	RID sdf_texture;
	AABB sdf_bounds;

	uint32_t _alloc_slot();
	void _free_slot(uint32_t p_slot);
	void _mark_dirty(uint32_t p_slot);

	// === Bulk (GPU-resident mass) bodies ===
	// A contiguous slot range that bypasses the per-RID/per-callback path and is
	// consumed on the GPU (rendered straight from body_buffer into a MultiMesh).
	struct KilosBulk {
		uint32_t base = 0;
		uint32_t count = 0;
		RID multimesh;
	};
	HashMap<int, KilosBulk> bulk_ranges; // keyed by base slot (the int handle)

	struct RangeDirty {
		uint32_t base = 0;
		uint32_t count = 0;
	};
	Vector<RangeDirty> dirty_ranges; // contiguous ranges to upload in one buffer_update

	// === Cross-thread step marshaling (physics thread -> render thread) ===
	struct BodyUpload {
		uint32_t index = 0;
		RigidBodyData data;
	};
	struct RangeUpload {
		uint32_t base = 0;
		LocalVector<RigidBodyData> data;
	};
	struct BulkBind {
		uint32_t base = 0;
		uint32_t count = 0;
		RID multimesh;
	};
	struct StepJob {
		double dt = 0.0;
		uint32_t body_count = 0;
		uint32_t required_capacity = 0;
		LocalVector<BodyUpload> uploads;
		LocalVector<uint32_t> tracked;
		LocalVector<RangeUpload> range_uploads; // one-shot contiguous uploads (bulk spawn)
		LocalVector<BulkBind> bulk_binds; // ranges to convert into their MultiMesh this frame
		// collision snapshot (P3)
		bool collision_enabled = false;
		float radius = 0.5f;
		float ground_y = 0.0f;
		uint32_t solve_iterations = 4;
		// SDF snapshot (P4)
		RID sdf_texture;
		AABB sdf_bounds;
	};
	Mutex job_mutex;
	List<StepJob> job_queue;

	// Readback results: render thread writes, physics thread reads.
	// Mutable so const accessors (body_get_state) can lock it.
	mutable Mutex readback_mutex;
	HashMap<uint32_t, RigidBodyData> readback_results;

	// === Render-thread-only state (valid only with RD in hand) ===
	RenderingDevice *rd = nullptr;
	bool rt_pipeline_ready = false;
	RID integrate_shader;
	RID integrate_pipeline;
	RID body_buffer;
	RID body_uniform_set;
	uint32_t gpu_capacity = 0;
	LocalVector<RID> buffers_to_free; // old pool buffers, freed one step after growth

	// PBD collision solve (render thread)
	bool rt_solve_ready = false;
	RID solve_shader;
	RID solve_pipeline;
	RID finalize_shader;
	RID finalize_pipeline;
	// Spatial-hash broad phase (P3b)
	RID grid_build_shader;
	RID grid_build_pipeline;
	RID grid_counts_buffer; // table_size uints
	RID grid_bodies_buffer; // table_size * max_per_cell uints
	RID grid_uniform_set; // set 1: binding 0 = counts, binding 1 = bodies
	uint32_t grid_table_size = 1u << 20; // power of two
	uint32_t grid_max_per_cell = 16;
	// SDF sampling (set 2 of the solve shader)
	RID sdf_sampler;
	RID sdf_dummy_texture; // 1^3, bound when no SDF is set (sampling gated by flag)
	RID sdf_uniform_set;
	RID sdf_uniform_set_texture; // texture the current sdf_uniform_set was built against

	// body->multimesh conversion (render thread)
	bool rt_convert_ready = false;
	RID convert_shader;
	RID convert_pipeline;
	RID convert_body_uniform_set; // set 0 = body_buffer, bound against convert_shader
	RID convert_body_uniform_set_buffer; // body_buffer the above was built against
	// set 1 = multimesh buffer, cached per multimesh (rebuilt if its buffer changes)
	struct ConvertMMSet {
		RID buffer;
		RID uniform_set;
	};
	HashMap<RID, ConvertMMSet> mm_uniform_sets;

	static const uint32_t INITIAL_CAPACITY = 4096;

	void _render_process_step();
	void _rt_ensure_pipeline();
	void _rt_ensure_capacity(uint32_t p_capacity);
	void _rt_dispatch_integrate(const StepJob &p_job);
	void _rt_readback(const LocalVector<uint32_t> &p_tracked);
	void _rt_on_readback(const PackedByteArray &p_data, uint32_t p_slot);
	void _rt_ensure_convert_pipeline();
	void _rt_convert_to_multimesh(const BulkBind &p_bind);
	void _rt_ensure_solve_pipeline();
	void _rt_dispatch_collision(const StepJob &p_job);
	RID _rt_get_sdf_uniform_set(RID p_texture);

public:
	virtual RID world_boundary_shape_create() override;
	virtual RID separation_ray_shape_create() override;
	virtual RID sphere_shape_create() override;
	virtual RID box_shape_create() override;
	virtual RID capsule_shape_create() override;
	virtual RID cylinder_shape_create() override;
	virtual RID convex_polygon_shape_create() override;
	virtual RID concave_polygon_shape_create() override;
	virtual RID heightmap_shape_create() override;
	virtual RID custom_shape_create() override;
	virtual void shape_set_data(RID p_shape, const Variant &p_data) override;
	virtual void shape_set_custom_solver_bias(RID p_shape, real_t p_bias) override;
	virtual ShapeType shape_get_type(RID p_shape) const override;
	virtual Variant shape_get_data(RID p_shape) const override;
	virtual void shape_set_margin(RID p_shape, real_t p_margin) override;
	virtual real_t shape_get_margin(RID p_shape) const override;
	virtual real_t shape_get_custom_solver_bias(RID p_shape) const override;
	virtual RID space_create() override;
	virtual void space_set_active(RID p_space, bool p_active) override;
	virtual bool space_is_active(RID p_space) const override;
	virtual void space_set_param(RID p_space, SpaceParameter p_param, real_t p_value) override;
	virtual real_t space_get_param(RID p_space, SpaceParameter p_param) const override;
	virtual PhysicsDirectSpaceState3D *space_get_direct_state(RID p_space) override;
	virtual void space_set_debug_contacts(RID p_space, int p_max_contacts) override;
	virtual Vector<Vector3> space_get_contacts(RID p_space) const override;
	virtual int space_get_contact_count(RID p_space) const override;
	virtual RID area_create() override;
	virtual void area_set_space(RID p_area, RID p_space) override;
	virtual RID area_get_space(RID p_area) const override;
	virtual void area_add_shape(RID p_area, RID p_shape, const Transform3D &p_transform, bool p_disabled) override;
	virtual void area_set_shape(RID p_area, int p_shape_idx, RID p_shape) override;
	virtual void area_set_shape_transform(RID p_area, int p_shape_idx, const Transform3D &p_transform) override;
	virtual int area_get_shape_count(RID p_area) const override;
	virtual RID area_get_shape(RID p_area, int p_shape_idx) const override;
	virtual Transform3D area_get_shape_transform(RID p_area, int p_shape_idx) const override;
	virtual void area_remove_shape(RID p_area, int p_shape_idx) override;
	virtual void area_clear_shapes(RID p_area) override;
	virtual void area_set_shape_disabled(RID p_area, int p_shape_idx, bool p_disabled) override;
	virtual void area_attach_object_instance_id(RID p_area, ObjectID p_id) override;
	virtual ObjectID area_get_object_instance_id(RID p_area) const override;
	virtual void area_set_param(RID p_area, AreaParameter p_param, const Variant &p_value) override;
	virtual void area_set_transform(RID p_area, const Transform3D &p_transform) override;
	virtual Variant area_get_param(RID p_area, AreaParameter p_param) const override;
	virtual Transform3D area_get_transform(RID p_area) const override;
	virtual void area_set_ray_pickable(RID p_area, bool p_enable) override;
	virtual void area_set_collision_layer(RID p_area, uint32_t p_layer) override;
	virtual uint32_t area_get_collision_layer(RID p_area) const override;
	virtual void area_set_collision_mask(RID p_area, uint32_t p_mask) override;
	virtual uint32_t area_get_collision_mask(RID p_area) const override;
	virtual void area_set_monitorable(RID p_area, bool p_monitorable) override;
	virtual void area_set_monitor_callback(RID p_area, const Callable &p_callback) override;
	virtual void area_set_area_monitor_callback(RID p_area, const Callable &p_callback) override;
	virtual RID body_create() override;
	virtual void body_set_space(RID p_body, RID p_space) override;
	virtual RID body_get_space(RID p_body) const override;
	virtual void body_set_mode(RID p_body, BodyMode p_mode) override;
	virtual BodyMode body_get_mode(RID p_body) const override;
	virtual void body_add_shape(RID p_body, RID p_shape, const Transform3D &p_transform, bool p_disabled) override;
	virtual void body_set_shape(RID p_body, int p_shape_idx, RID p_shape) override;
	virtual void body_set_shape_transform(RID p_body, int p_shape_idx, const Transform3D &p_transform) override;
	virtual int body_get_shape_count(RID p_body) const override;
	virtual RID body_get_shape(RID p_body, int p_shape_idx) const override;
	virtual Transform3D body_get_shape_transform(RID p_body, int p_shape_idx) const override;
	virtual void body_set_shape_disabled(RID p_body, int p_shape_idx, bool p_disabled) override;
	virtual void body_remove_shape(RID p_body, int p_shape_idx) override;
	virtual void body_clear_shapes(RID p_body) override;
	virtual void body_attach_object_instance_id(RID p_body, ObjectID p_id) override;
	virtual ObjectID body_get_object_instance_id(RID p_body) const override;
	virtual void body_set_enable_continuous_collision_detection(RID p_body, bool p_enable) override;
	virtual bool body_is_continuous_collision_detection_enabled(RID p_body) const override;
	virtual void body_set_collision_layer(RID p_body, uint32_t p_layer) override;
	virtual uint32_t body_get_collision_layer(RID p_body) const override;
	virtual void body_set_collision_mask(RID p_body, uint32_t p_mask) override;
	virtual uint32_t body_get_collision_mask(RID p_body) const override;
	virtual void body_set_collision_priority(RID p_body, real_t p_priority) override;
	virtual real_t body_get_collision_priority(RID p_body) const override;
	virtual void body_set_user_flags(RID p_body, uint32_t p_flags) override;
	virtual uint32_t body_get_user_flags(RID p_body) const override;
	virtual void body_set_param(RID p_body, BodyParameter p_param, const Variant &p_value) override;
	virtual Variant body_get_param(RID p_body, BodyParameter p_param) const override;
	virtual void body_reset_mass_properties(RID p_body) override;
	virtual void body_set_state(RID p_body, BodyState p_state, const Variant &p_variant) override;
	virtual Variant body_get_state(RID p_body, BodyState p_state) const override;
	virtual void body_apply_central_impulse(RID p_body, const Vector3 &p_impulse) override;
	virtual void body_apply_impulse(RID p_body, const Vector3 &p_impulse, const Vector3 &p_position) override;
	virtual void body_apply_torque_impulse(RID p_body, const Vector3 &p_impulse) override;
	virtual void soft_body_apply_point_impulse(RID p_body, int p_point_index, const Vector3 &p_impulse) override;
	virtual void soft_body_apply_point_force(RID p_body, int p_point_index, const Vector3 &p_force) override;
	virtual void soft_body_apply_central_impulse(RID p_body, const Vector3 &p_impulse) override;
	virtual void soft_body_apply_central_force(RID p_body, const Vector3 &p_force) override;
	virtual void body_apply_central_force(RID p_body, const Vector3 &p_force) override;
	virtual void body_apply_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position) override;
	virtual void body_apply_torque(RID p_body, const Vector3 &p_torque) override;
	virtual void body_add_constant_central_force(RID p_body, const Vector3 &p_force) override;
	virtual void body_add_constant_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position) override;
	virtual void body_add_constant_torque(RID p_body, const Vector3 &p_torque) override;
	virtual void body_set_constant_force(RID p_body, const Vector3 &p_force) override;
	virtual Vector3 body_get_constant_force(RID p_body) const override;
	virtual void body_set_constant_torque(RID p_body, const Vector3 &p_torque) override;
	virtual Vector3 body_get_constant_torque(RID p_body) const override;
	virtual void body_set_axis_velocity(RID p_body, const Vector3 &p_axis_velocity) override;
	virtual void body_set_axis_lock(RID p_body, BodyAxis p_axis, bool p_lock) override;
	virtual bool body_is_axis_locked(RID p_body, BodyAxis p_axis) const override;
	virtual void body_add_collision_exception(RID p_body, RID p_body_b) override;
	virtual void body_remove_collision_exception(RID p_body, RID p_body_b) override;
	virtual void body_get_collision_exceptions(RID p_body, List<RID> *p_exceptions) override;
	virtual void body_set_contacts_reported_depth_threshold(RID p_body, real_t p_threshold) override;
	virtual real_t body_get_contacts_reported_depth_threshold(RID p_body) const override;
	virtual void body_set_omit_force_integration(RID p_body, bool p_omit) override;
	virtual bool body_is_omitting_force_integration(RID p_body) const override;
	virtual void body_set_max_contacts_reported(RID p_body, int p_contacts) override;
	virtual int body_get_max_contacts_reported(RID p_body) const override;
	virtual void body_set_state_sync_callback(RID p_body, const Callable &p_callable) override;
	virtual void body_set_force_integration_callback(RID p_body, const Callable &p_callable, const Variant &p_udata) override;
	virtual void body_set_ray_pickable(RID p_body, bool p_enable) override;
	virtual bool body_test_motion(RID p_body, const MotionParameters &p_parameters, MotionResult *r_result) override;
	virtual PhysicsDirectBodyState3D *body_get_direct_state(RID p_body) override;
	virtual RID soft_body_create() override;
	virtual void soft_body_update_rendering_server(RID p_body, RequiredParam<PhysicsServer3DRenderingServerHandler> rp_rendering_server_handler) override;
	virtual void soft_body_set_space(RID p_body, RID p_space) override;
	virtual RID soft_body_get_space(RID p_body) const override;
	virtual void soft_body_set_collision_layer(RID p_body, uint32_t p_layer) override;
	virtual uint32_t soft_body_get_collision_layer(RID p_body) const override;
	virtual void soft_body_set_collision_mask(RID p_body, uint32_t p_mask) override;
	virtual uint32_t soft_body_get_collision_mask(RID p_body) const override;
	virtual void soft_body_add_collision_exception(RID p_body, RID p_body_b) override;
	virtual void soft_body_remove_collision_exception(RID p_body, RID p_body_b) override;
	virtual void soft_body_get_collision_exceptions(RID p_body, List<RID> *p_exceptions) override;
	virtual void soft_body_set_state(RID p_body, BodyState p_state, const Variant &p_variant) override;
	virtual Variant soft_body_get_state(RID p_body, BodyState p_state) const override;
	virtual void soft_body_set_transform(RID p_body, const Transform3D &p_transform) override;
	virtual void soft_body_set_ray_pickable(RID p_body, bool p_enable) override;
	virtual void soft_body_set_simulation_precision(RID p_body, int p_simulation_precision) override;
	virtual int soft_body_get_simulation_precision(RID p_body) const override;
	virtual void soft_body_set_total_mass(RID p_body, real_t p_total_mass) override;
	virtual real_t soft_body_get_total_mass(RID p_body) const override;
	virtual void soft_body_set_linear_stiffness(RID p_body, real_t p_stiffness) override;
	virtual real_t soft_body_get_linear_stiffness(RID p_body) const override;
	virtual void soft_body_set_shrinking_factor(RID p_body, real_t p_shrinking_factor) override;
	virtual real_t soft_body_get_shrinking_factor(RID p_body) const override;
	virtual void soft_body_set_pressure_coefficient(RID p_body, real_t p_pressure_coefficient) override;
	virtual real_t soft_body_get_pressure_coefficient(RID p_body) const override;
	virtual void soft_body_set_damping_coefficient(RID p_body, real_t p_damping_coefficient) override;
	virtual real_t soft_body_get_damping_coefficient(RID p_body) const override;
	virtual void soft_body_set_drag_coefficient(RID p_body, real_t p_drag_coefficient) override;
	virtual real_t soft_body_get_drag_coefficient(RID p_body) const override;
	virtual void soft_body_set_mesh(RID p_body, RID p_mesh) override;
	virtual AABB soft_body_get_bounds(RID p_body) const override;
	virtual void soft_body_move_point(RID p_body, int p_point_index, const Vector3 &p_global_position) override;
	virtual Vector3 soft_body_get_point_global_position(RID p_body, int p_point_index) const override;
	virtual void soft_body_remove_all_pinned_points(RID p_body) override;
	virtual void soft_body_pin_point(RID p_body, int p_point_index, bool p_pin) override;
	virtual bool soft_body_is_point_pinned(RID p_body, int p_point_index) const override;
	/* MESHLET COLLIDER API */
	virtual RID meshlet_collider_create() override;
	virtual void meshlet_collider_set_data(RID p_collider, RID p_mesh, const Transform3D &p_transform) override;
	virtual void meshlet_collider_set_transform(RID p_collider, const Transform3D &p_transform) override;
	virtual void meshlet_collider_clear(RID p_collider) override;
	/* BULK BODY API */
	virtual int bulk_body_create(int p_count) override;
	virtual void bulk_body_scatter(int p_handle, const AABB &p_region) override;
	virtual void bulk_body_set_multimesh(int p_handle, RID p_multimesh) override;
	virtual void bulk_body_free(int p_handle) override;
	/* COLLISION CONFIG (P3/P4) */
	virtual void bulk_set_collision(bool p_enabled, real_t p_radius, real_t p_ground_y, int p_iterations) override;
	virtual void bulk_set_sdf(RID p_sdf_texture, const AABB &p_bounds) override;

	virtual RID joint_create() override;
	virtual void joint_clear(RID p_joint) override;
	virtual void joint_make_pin(RID p_joint, RID p_body_A, const Vector3 &p_local_A, RID p_body_B, const Vector3 &p_local_B) override;
	virtual void pin_joint_set_param(RID p_joint, PinJointParam p_param, real_t p_value) override;
	virtual real_t pin_joint_get_param(RID p_joint, PinJointParam p_param) const override;
	virtual void pin_joint_set_local_a(RID p_joint, const Vector3 &p_A) override;
	virtual Vector3 pin_joint_get_local_a(RID p_joint) const override;
	virtual void pin_joint_set_local_b(RID p_joint, const Vector3 &p_B) override;
	virtual Vector3 pin_joint_get_local_b(RID p_joint) const override;
	virtual void joint_make_hinge(RID p_joint, RID p_body_A, const Transform3D &p_frame_A, RID p_body_B, const Transform3D &p_frame_B) override;
	virtual void joint_make_hinge_simple(RID p_joint, RID p_body_A, const Vector3 &p_pivot_A, const Vector3 &p_axis_A, RID p_body_B, const Vector3 &p_pivot_B, const Vector3 &p_axis_B) override;
	virtual void hinge_joint_set_param(RID p_joint, HingeJointParam p_param, real_t p_value) override;
	virtual real_t hinge_joint_get_param(RID p_joint, HingeJointParam p_param) const override;
	virtual void hinge_joint_set_flag(RID p_joint, HingeJointFlag p_flag, bool p_value) override;
	virtual bool hinge_joint_get_flag(RID p_joint, HingeJointFlag p_flag) const override;
	virtual void joint_make_slider(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) override;
	virtual void slider_joint_set_param(RID p_joint, SliderJointParam p_param, real_t p_value) override;
	virtual real_t slider_joint_get_param(RID p_joint, SliderJointParam p_param) const override;
	virtual void joint_make_cone_twist(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) override;
	virtual void cone_twist_joint_set_param(RID p_joint, ConeTwistJointParam p_param, real_t p_value) override;
	virtual real_t cone_twist_joint_get_param(RID p_joint, ConeTwistJointParam p_param) const override;
	virtual void joint_make_generic_6dof(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) override;
	virtual void generic_6dof_joint_set_param(RID p_joint, Vector3::Axis, G6DOFJointAxisParam p_param, real_t p_value) override;
	virtual real_t generic_6dof_joint_get_param(RID p_joint, Vector3::Axis, G6DOFJointAxisParam p_param) const override;
	virtual void generic_6dof_joint_set_flag(RID p_joint, Vector3::Axis, G6DOFJointAxisFlag p_flag, bool p_enable) override;
	virtual bool generic_6dof_joint_get_flag(RID p_joint, Vector3::Axis, G6DOFJointAxisFlag p_flag) const override;
	virtual JointType joint_get_type(RID p_joint) const override;
	virtual void joint_set_solver_priority(RID p_joint, int p_priority) override;
	virtual int joint_get_solver_priority(RID p_joint) const override;
	virtual void joint_disable_collisions_between_bodies(RID p_joint, bool p_disable) override;
	virtual bool joint_is_disabled_collisions_between_bodies(RID p_joint) const override;
	virtual void free_rid(RID p_rid) override;
	virtual void set_active(bool p_active) override;
	virtual void init() override;
	virtual void step(real_t p_step) override;
	virtual void sync() override;
	virtual void flush_queries() override;
	virtual void end_sync() override;
	virtual void finish() override;
	virtual bool is_flushing_queries() const override;
	virtual int get_process_info(ProcessInfo p_info) override;

	// Selftest-only: force a synchronous GPU->CPU readback of the whole pool
	// into the CPU shadow so body_get_state() reflects the GPU result. This
	// stalls (submit+sync) and must not be used on the hot path.
	void _debug_sync_readback();

	// Query entry point used by KilosDirectSpaceState3D (takes the collision lock).
	bool _intersect_ray(RID p_space, const PhysicsDirectSpaceState3D::RayParameters &p_parameters, PhysicsDirectSpaceState3D::RayResult &r_result);

	// Selftest-only: read a slot's position from the CPU shadow (call after
	// _debug_sync_readback()).
	Vector3 _debug_slot_position(int p_slot) const;

	KilosPhysicsServer3D(bool p_using_threads = false);
	~KilosPhysicsServer3D();
};

#endif // KILOS_PHYSICS_SERVER_3D_H
