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

} // namespace

void run_kilos_selftest_if_requested() {
	if (!OS::get_singleton()->get_cmdline_args().find("--kilos-selftest")) {
		return;
	}

	print_line("KILOS_SELFTEST: starting");
	g_failures = 0;
	test_gpu_integration_free_fall();
	test_bulk_render_from_buffer();
	if (g_failures == 0) {
		print_line("KILOS_SELFTEST: all checks passed");
	} else {
		print_line(vformat("KILOS_SELFTEST: %d check(s) FAILED", g_failures));
	}
}
