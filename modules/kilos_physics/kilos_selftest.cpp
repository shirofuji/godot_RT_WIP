#include "kilos_selftest.h"

#include "kilos_physics_server_3d.h"

#include "core/os/os.h"
#include "core/string/print_string.h"

namespace {

int g_failures = 0;

void check(bool p_condition, const String &p_message) {
	if (p_condition) {
		print_line("KILOS_SELFTEST: PASS: " + p_message);
	} else {
		print_line("KILOS_SELFTEST: FAIL: " + p_message);
		g_failures++;
	}
}

// Symplectic (semi-implicit) Euler reference matching integration.glsl:
//   for each step: v += g*dt; pos += v*dt
// After n steps from rest: dy = g * dt^2 * n(n+1)/2.
double expected_free_fall_dy(double g, double dt, int n) {
	return g * dt * dt * (double)n * (double)(n + 1) * 0.5;
}

void test_gpu_integration_free_fall() {
	KilosPhysicsServer3D *server = memnew(KilosPhysicsServer3D(false));

	const int DYNAMIC_COUNT = 256;
	const double START_Y = 100.0;
	const double STATIC_Y = 50.0;
	const double DT = 1.0 / 60.0;
	const int STEPS = 60;

	LocalVector<RID> dynamic_bodies;
	for (int i = 0; i < DYNAMIC_COUNT; i++) {
		RID b = server->body_create();
		server->body_set_mode(b, PhysicsServer3D::BODY_MODE_RIGID);
		server->body_set_param(b, PhysicsServer3D::BODY_PARAM_MASS, 1.0);
		server->body_set_state(b, PhysicsServer3D::BODY_STATE_TRANSFORM,
				Transform3D(Basis(), Vector3(i, START_Y, 0)));
		dynamic_bodies.push_back(b);
	}

	// A static body must be ignored by the integrator (mass 0 => early-out).
	RID static_body = server->body_create();
	server->body_set_mode(static_body, PhysicsServer3D::BODY_MODE_STATIC);
	server->body_set_state(static_body, PhysicsServer3D::BODY_STATE_TRANSFORM,
			Transform3D(Basis(), Vector3(0, STATIC_Y, 0)));

	for (int s = 0; s < STEPS; s++) {
		server->step(DT);
	}
	server->_debug_sync_readback();

	const double expected_dy = expected_free_fall_dy(-9.8, DT, STEPS);
	const double expected_y = START_Y + expected_dy;
	const double expected_vy = -9.8 * DT * STEPS;

	// Sample a body in the middle of the pool.
	RID sample = dynamic_bodies[DYNAMIC_COUNT / 2];
	Transform3D t = server->body_get_state(sample, PhysicsServer3D::BODY_STATE_TRANSFORM);
	Vector3 v = server->body_get_state(sample, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY);

	check(Math::abs(t.origin.y - expected_y) < 0.01,
			vformat("Dynamic body free-fall y=%.4f matches symplectic-Euler reference %.4f", t.origin.y, expected_y));
	check(Math::abs(v.y - expected_vy) < 0.01,
			vformat("Dynamic body velocity vy=%.4f matches reference %.4f", v.y, expected_vy));
	check(Math::abs(t.origin.x - (double)(DYNAMIC_COUNT / 2)) < 0.001 && Math::abs(t.origin.z) < 0.001,
			"Dynamic body did not drift on X/Z (gravity is Y-only)");

	// Verify every dynamic body integrated identically (uniform pool dispatch).
	bool all_match = true;
	for (uint32_t i = 0; i < dynamic_bodies.size(); i++) {
		Transform3D ti = server->body_get_state(dynamic_bodies[i], PhysicsServer3D::BODY_STATE_TRANSFORM);
		if (Math::abs(ti.origin.y - expected_y) > 0.01) {
			all_match = false;
			break;
		}
	}
	check(all_match, vformat("All %d dynamic bodies fell to the same height", DYNAMIC_COUNT));

	Transform3D st = server->body_get_state(static_body, PhysicsServer3D::BODY_STATE_TRANSFORM);
	check(Math::abs(st.origin.y - STATIC_Y) < 0.0001,
			vformat("Static body stayed at rest (y=%.4f, expected %.4f)", st.origin.y, STATIC_Y));

	// Free everything; also exercises slot recycling.
	for (uint32_t i = 0; i < dynamic_bodies.size(); i++) {
		server->free_rid(dynamic_bodies[i]);
	}
	server->free_rid(static_body);

	memdelete(server);
}

void test_bulk_render_from_buffer() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		check(false, "RenderingServer singleton exists");
		return;
	}

	KilosPhysicsServer3D *server = memnew(KilosPhysicsServer3D(false));

	const int N = 5000; // > INITIAL_CAPACITY (4096) so this also exercises pool growth
	const double PLANE_Y = 100.0;
	const double DT = 1.0 / 60.0;
	const int STEPS = 30;

	// A transform-only MultiMesh (no per-instance color/custom data), sized to N.
	RID mm = rs->multimesh_create();
	rs->multimesh_allocate_data(mm, N, RSE::MULTIMESH_TRANSFORM_3D, false, false);

	// Bulk-create N bodies, scatter them across a flat plane at y=PLANE_Y (size.y=0
	// so every body starts at exactly the same height -> deterministic free-fall).
	int handle = server->bulk_body_create(N);
	check(handle >= 0, "bulk_body_create returned a valid handle");
	server->bulk_body_scatter(handle, AABB(Vector3(0, PLANE_Y, 0), Vector3(200, 0, 200)));
	server->bulk_body_set_multimesh(handle, mm);

	for (int s = 0; s < STEPS; s++) {
		server->step(DT);
	}
	server->_debug_sync_readback();

	const double expected_y = PLANE_Y + expected_free_fall_dy(-9.8, DT, STEPS);

	Vector<float> buf = rs->multimesh_get_buffer(mm);
	check(buf.size() == N * 12, vformat("MultiMesh buffer has N*12 floats (got %d, expected %d)", buf.size(), N * 12));

	if (buf.size() == N * 12) {
		bool convert_matches = true;
		bool integrated = true;
		int sample_stride = MAX(1, N / 512);
		for (int i = 0; i < N; i += sample_stride) {
			// Instance origin lives at floats [3],[7],[11] of the row-major 3x4.
			Vector3 mm_origin(buf[i * 12 + 3], buf[i * 12 + 7], buf[i * 12 + 11]);
			Vector3 body_pos = server->_debug_slot_position(handle + i);
			if (mm_origin.distance_to(body_pos) > 0.001) {
				convert_matches = false;
			}
			if (Math::abs(mm_origin.y - expected_y) > 0.01) {
				integrated = false;
			}
		}
		check(convert_matches, "Every sampled MultiMesh instance origin matches its GPU body position (GPU->GPU convert)");
		check(integrated, vformat("Every sampled instance fell to the free-fall height y=%.4f", expected_y));

		// Rotation block of instance 0 should be identity (no angular velocity).
		bool identity_rot = Math::abs(buf[0] - 1.0f) < 0.001 && Math::abs(buf[5] - 1.0f) < 0.001 && Math::abs(buf[10] - 1.0f) < 0.001;
		check(identity_rot, "Instance rotation is identity for non-spinning bodies");
	}

	server->bulk_body_free(handle);
	rs->free_rid(mm);
	memdelete(server);
}

void test_ground_rest() {
	KilosPhysicsServer3D *server = memnew(KilosPhysicsServer3D(false));
	const float RADIUS = 0.5f;
	const float GROUND_Y = 0.0f;
	server->bulk_set_collision(true, RADIUS, GROUND_Y, 4);

	const int N = 100;
	const double DT = 1.0 / 60.0;
	const double START_Y = 10.0;

	int handle = server->bulk_body_create(N);
	// Flat plane at START_Y (size.y = 0) so every body starts at the same height.
	server->bulk_body_scatter(handle, AABB(Vector3(-20, START_Y, -20), Vector3(40, 0, 40)));

	// ~3s: plenty to fall 9.5m and settle.
	for (int s = 0; s < 180; s++) {
		server->step(DT);
	}
	server->_debug_sync_readback();

	const double rest_y = GROUND_Y + RADIUS;
	bool all_rest = true;
	double worst = 0.0;
	for (int i = 0; i < N; i += 7) {
		Vector3 p = server->_debug_slot_position(handle + i);
		worst = MAX(worst, Math::abs(p.y - rest_y));
		if (Math::abs(p.y - rest_y) > 0.01) {
			all_rest = false;
		}
	}
	check(all_rest, vformat("Bodies fall and rest on the ground plane at y=%.2f (worst dy=%.4f)", rest_y, worst));

	server->bulk_body_free(handle);
	memdelete(server);
}

void test_pile_no_interpenetration() {
	KilosPhysicsServer3D *server = memnew(KilosPhysicsServer3D(false));
	const float RADIUS = 0.5f;
	const float GROUND_Y = 0.0f;
	server->bulk_set_collision(true, RADIUS, GROUND_Y, 8);
	const double DT = 1.0 / 60.0;

	// Drop a cluster into a modest volume above the ground; they collide, spread,
	// and pile on the floor. This is the realistic scenario (small per-frame
	// overlaps), unlike an artificial deep initial penetration.
	const int N = 64;
	int handle = server->bulk_body_create(N);
	server->bulk_body_scatter(handle, AABB(Vector3(-2, 3, -2), Vector3(4, 6, 4)));

	for (int s = 0; s < 480; s++) { // ~8s to settle
		server->step(DT);
	}
	server->_debug_sync_readback();

	// (1) Everything is on or above the ground.
	bool above_ground = true;
	double lowest = 1e9;
	for (int i = 0; i < N; i++) {
		double y = server->_debug_slot_position(handle + i).y;
		lowest = MIN(lowest, y);
		if (y < GROUND_Y + RADIUS - 0.05) {
			above_ground = false;
		}
	}
	check(above_ground, vformat("All %d bodies rest on/above the ground (lowest y=%.3f, floor=%.3f)", N, lowest, GROUND_Y + RADIUS));

	// (2) No significant interpenetration: min pairwise centre distance stays near 2*R.
	double min_dist = 1e9;
	for (int i = 0; i < N; i++) {
		Vector3 pi = server->_debug_slot_position(handle + i);
		for (int j = i + 1; j < N; j++) {
			double d = pi.distance_to(server->_debug_slot_position(handle + j));
			min_dist = MIN(min_dist, d);
		}
	}
	// Approximate solver: allow up to ~20% penetration.
	check(min_dist >= 0.8 * (2.0 * RADIUS), vformat("Settled pile has no significant interpenetration (min pair dist=%.4f, 2*R=%.2f)", min_dist, 2.0 * RADIUS));

	server->bulk_body_free(handle);
	memdelete(server);
}

void test_sdf_dome() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		check(false, "RenderingDevice available for SDF test");
		return;
	}

	KilosPhysicsServer3D *server = memnew(KilosPhysicsServer3D(false));
	const float RADIUS = 0.5f;
	// Ground far below so the body only interacts with the SDF dome.
	server->bulk_set_collision(true, RADIUS, -1000.0f, 4);
	const double DT = 1.0 / 60.0;

	// Bake a solid-sphere SDF (world distance) into a 128^3 R32F 3D texture.
	const int DIM = 128;
	const Vector3 CENTER(0, 0, 0);
	const float SPHERE_R = 20.0f;
	const AABB bounds(Vector3(-32, -32, -32), Vector3(64, 64, 64));

	Vector<float> sdf;
	sdf.resize(DIM * DIM * DIM);
	float *w = sdf.ptrw();
	for (int z = 0; z < DIM; z++) {
		for (int y = 0; y < DIM; y++) {
			for (int x = 0; x < DIM; x++) {
				Vector3 uvw((x + 0.5f) / DIM, (y + 0.5f) / DIM, (z + 0.5f) / DIM);
				Vector3 p = bounds.position + uvw * bounds.size;
				w[(z * DIM + y) * DIM + x] = (float)(p.distance_to(CENTER) - SPHERE_R);
			}
		}
	}
	Vector<uint8_t> bytes;
	bytes.resize((int64_t)sdf.size() * sizeof(float));
	memcpy(bytes.ptrw(), sdf.ptr(), bytes.size());

	RenderingDevice::TextureFormat tf;
	tf.format = RenderingDevice::DATA_FORMAT_R32_SFLOAT;
	tf.width = DIM;
	tf.height = DIM;
	tf.depth = DIM;
	tf.texture_type = RenderingDevice::TEXTURE_TYPE_3D;
	tf.usage_bits = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<Vector<uint8_t>> tdata;
	tdata.push_back(bytes);
	RID sdf_tex = rd->texture_create(tf, RenderingDevice::TextureView(), tdata);

	server->bulk_set_sdf(sdf_tex, bounds);

	// Drop a body just above the top of the dome (top is at y = SPHERE_R = 20).
	RID b = server->body_create();
	server->body_set_mode(b, PhysicsServer3D::BODY_MODE_RIGID);
	server->body_set_param(b, PhysicsServer3D::BODY_PARAM_MASS, 1.0);
	server->body_set_state(b, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(Basis(), Vector3(0, 22, 0)));

	for (int s = 0; s < 200; s++) {
		server->step(DT);
	}
	server->_debug_sync_readback();

	Transform3D t = server->body_get_state(b, PhysicsServer3D::BODY_STATE_TRANSFORM);
	double dist = t.origin.distance_to(CENTER);
	double expected = SPHERE_R + RADIUS; // 20.5

	check(dist > expected - 0.5 && dist < expected + 0.5,
			vformat("Body rests on the SDF dome surface (dist from centre=%.3f, expected ~%.2f)", dist, expected));
	check(t.origin.y > SPHERE_R - 0.5,
			vformat("Body stayed near the top of the dome (y=%.3f, dome top=%.1f)", t.origin.y, SPHERE_R));

	server->free_rid(b);
	rd->free_rid(sdf_tex);
	memdelete(server);
}

void test_raycast_trimesh() {
	KilosPhysicsServer3D *server = memnew(KilosPhysicsServer3D(false));
	RID space = server->space_create();

	// A flat quad at y=5 spanning x,z in [-10,10] (two triangles).
	PackedVector3Array faces;
	Vector3 a(-10, 5, -10), b(10, 5, -10), c(10, 5, 10), d(-10, 5, 10);
	faces.push_back(a);
	faces.push_back(b);
	faces.push_back(c);
	faces.push_back(a);
	faces.push_back(c);
	faces.push_back(d);

	RID shape = server->concave_polygon_shape_create();
	Dictionary sd;
	sd["faces"] = faces;
	sd["backface_collision"] = false;
	server->shape_set_data(shape, sd);

	RID body = server->body_create();
	server->body_set_mode(body, PhysicsServer3D::BODY_MODE_STATIC);
	server->body_add_shape(body, shape, Transform3D(), false);
	server->body_set_space(body, space);

	PhysicsDirectSpaceState3D *ss = server->space_get_direct_state(space);

	PhysicsDirectSpaceState3D::RayParameters rp;
	rp.from = Vector3(1, 20, 2);
	rp.to = Vector3(1, -20, 2);
	PhysicsDirectSpaceState3D::RayResult rr;
	bool hit = ss->intersect_ray(rp, rr);
	check(hit, "intersect_ray hits the trimesh terrain quad");
	if (hit) {
		check(Math::abs(rr.position.y - 5.0) < 0.01, vformat("Ray hit at y=%.4f (expected 5.0)", rr.position.y));
		check(rr.normal.y > 0.99, vformat("Hit normal points up (n.y=%.3f)", rr.normal.y));
		check(rr.rid == body, "Hit reports the correct body RID");
	}

	// A ray outside the quad must miss.
	rp.from = Vector3(50, 20, 50);
	rp.to = Vector3(50, -20, 50);
	PhysicsDirectSpaceState3D::RayResult rr2;
	check(!ss->intersect_ray(rp, rr2), "Ray outside the mesh misses");

	server->free_rid(body);
	server->free_rid(shape);
	server->free_rid(space);
	memdelete(server);
}

} // namespace

void run_kilos_selftest_if_requested() {
	if (!OS::get_singleton()->get_cmdline_args().find("--kilos-selftest")) {
		return;
	}

	print_line("KILOS_SELFTEST: starting");
	g_failures = 0;
	test_gpu_integration_free_fall();
	test_bulk_render_from_buffer();
	test_ground_rest();
	test_pile_no_interpenetration();
	test_sdf_dome();
	test_raycast_trimesh();
	if (g_failures == 0) {
		print_line("KILOS_SELFTEST: all checks passed");
	} else {
		print_line(vformat("KILOS_SELFTEST: %d check(s) FAILED", g_failures));
	}
}
