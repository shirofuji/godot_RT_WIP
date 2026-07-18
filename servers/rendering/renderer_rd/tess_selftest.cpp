/**************************************************************************/
/*  tess_selftest.cpp                                                     */
/**************************************************************************/
/* Adaptive-tessellation P0 de-risk. See tess_selftest.h.                 */
/**************************************************************************/

#include "tess_selftest.h"

#include "core/os/os.h"
#include "core/string/print_string.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

typedef RenderingDevice RD;

namespace {

int g_failures = 0;

void check(bool p_cond, const String &p_what) {
	if (p_cond) {
		print_line("TESS_SELFTEST:   PASS  " + p_what);
	} else {
		print_line("TESS_SELFTEST:   FAIL  " + p_what);
		g_failures++;
	}
}

// A patch of 3 control points (one triangle) covering most of NDC. The TCS sets a uniform tess level
// from a push constant; the TES bulges edge/interior vertices OUTWARD from the centroid by an amount
// that is exactly zero at the 3 base corners. So level=1 (corners only) renders the flat base triangle,
// while level>1 renders a convex "inflated" triangle that covers strictly MORE pixels - a coverage gain
// only possible if real subdivision produced new, displaced vertices.

const char *VERT_SRC = R"(#version 450
out gl_PerVertex { vec4 gl_Position; };
const vec2 CP[3] = vec2[](vec2(-0.8, -0.8), vec2(0.8, -0.8), vec2(0.0, 0.8));
void main() {
	gl_Position = vec4(CP[gl_VertexIndex], 0.0, 1.0);
}
)";

const char *FRAG_SRC = R"(#version 450
layout(location = 0) out vec4 frag_color;
void main() {
	frag_color = vec4(1.0);
}
)";

const char *TESC_SRC = R"(#version 450
layout(vertices = 3) out;
in gl_PerVertex { vec4 gl_Position; } gl_in[gl_MaxPatchVertices];
out gl_PerVertex { vec4 gl_Position; } gl_out[];
layout(push_constant, std430) uniform Params { float level; } pc;
void main() {
	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
	if (gl_InvocationID == 0) {
		gl_TessLevelOuter[0] = pc.level;
		gl_TessLevelOuter[1] = pc.level;
		gl_TessLevelOuter[2] = pc.level;
		gl_TessLevelInner[0] = pc.level;
	}
}
)";

const char *TESE_SRC = R"(#version 450
layout(triangles, equal_spacing, cw) in;
in gl_PerVertex { vec4 gl_Position; } gl_in[gl_MaxPatchVertices];
out gl_PerVertex { vec4 gl_Position; };
void main() {
	vec3 bc = gl_TessCoord;
	vec2 p0 = gl_in[0].gl_Position.xy;
	vec2 p1 = gl_in[1].gl_Position.xy;
	vec2 p2 = gl_in[2].gl_Position.xy;
	vec2 p = bc.x * p0 + bc.y * p1 + bc.z * p2;
	vec2 tri_center = (p0 + p1 + p2) / 3.0;
	// (1 - max(bc)) is 0 at the corners (one bc==1) and grows toward edges/interior.
	float bulge = (1.0 - max(bc.x, max(bc.y, bc.z))) * 0.15;
	vec2 dir = p - tri_center;
	float len = length(dir);
	if (len > 1e-4) {
		p += (dir / len) * bulge;
	}
	gl_Position = vec4(p, 0.0, 1.0);
}
)";

bool add_stage(RD *rd, Vector<RD::ShaderStageSPIRVData> &r_stages, RD::ShaderStage p_stage, const char *p_src, const char *p_name) {
	String err;
	Vector<uint8_t> spirv = rd->shader_compile_spirv_from_source(p_stage, String::utf8(p_src), RD::SHADER_LANGUAGE_GLSL, &err);
	if (spirv.is_empty()) {
		print_line(vformat("TESS_SELFTEST:   FAIL  %s stage compiled to SPIR-V (%s)", p_name, err));
		g_failures++;
		return false;
	}
	RD::ShaderStageSPIRVData d;
	d.shader_stage = p_stage;
	d.spirv = spirv;
	r_stages.push_back(d);
	return true;
}

// Renders the patch at the given tess level and returns the number of covered (white) pixels.
int render_coverage(RD *rd, RID p_fb, RID p_pipeline, RID p_color_tex, int p_size, float p_level) {
	// The TCS declares a single float push constant, which reflects as exactly 4 bytes - supply that
	// (no padding), or draw_list_set_push_constant rejects the size mismatch and the draw is skipped.
	float level = p_level;

	Vector<Color> clear;
	clear.push_back(Color(0, 0, 0, 1));
	RD::DrawListID dl = rd->draw_list_begin(p_fb, RD::DRAW_CLEAR_ALL, clear);
	rd->draw_list_bind_render_pipeline(dl, p_pipeline);
	rd->draw_list_set_push_constant(dl, &level, sizeof(level));
	rd->draw_list_draw(dl, false, 1, 3); // 3 procedural vertices = one 3-point patch
	rd->draw_list_end();

	Vector<uint8_t> pixels = rd->texture_get_data(p_color_tex, 0);
	int covered = 0;
	if ((int)pixels.size() == p_size * p_size * 4) {
		for (int i = 0; i < p_size * p_size; i++) {
			if (pixels[i * 4] > 127) {
				covered++;
			}
		}
	}
	return covered;
}

} // namespace

void run_tess_selftest_if_requested() {
	if (!OS::get_singleton()->get_cmdline_args().find("--tess-selftest")) {
		return;
	}

	print_line("TESS_SELFTEST: starting (P0: RD tessellation stack de-risk)");
	g_failures = 0;

	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		print_line("TESS_SELFTEST: no RenderingDevice - run with a real driver (e.g. --rendering-driver vulkan --rendering-method forward_plus), not --test");
		return;
	}

	// NOTE: stages MUST be supplied in ShaderStage enum order (vertex, fragment, TCS, TES). The shader
	// container pairs the code array (built in input order) with the stage-label array (rebuilt in enum
	// order) by index, so any other input order mislabels the stages and the driver rejects the pipeline.
	Vector<RD::ShaderStageSPIRVData> stages;
	bool ok = true;
	ok = add_stage(rd, stages, RD::SHADER_STAGE_VERTEX, VERT_SRC, "vertex") && ok;
	ok = add_stage(rd, stages, RD::SHADER_STAGE_FRAGMENT, FRAG_SRC, "fragment") && ok;
	ok = add_stage(rd, stages, RD::SHADER_STAGE_TESSELATION_CONTROL, TESC_SRC, "tesselation_control") && ok;
	ok = add_stage(rd, stages, RD::SHADER_STAGE_TESSELATION_EVALUATION, TESE_SRC, "tesselation_evaluation") && ok;
	check(ok, "all four stages (vertex + TCS + TES + fragment) compiled to SPIR-V via glslang");
	if (!ok) {
		print_line(vformat("TESS_SELFTEST: %d check(s) FAILED (compile)", g_failures));
		return;
	}

	RID shader = rd->shader_create_from_spirv(stages, "tess_selftest");
	check(shader.is_valid(), "shader object created from the tessellation SPIR-V stages");
	if (!shader.is_valid()) {
		print_line(vformat("TESS_SELFTEST: %d check(s) FAILED (shader)", g_failures));
		return;
	}

	const int SIZE = 256;
	RD::TextureFormat ctf;
	ctf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	ctf.width = SIZE;
	ctf.height = SIZE;
	ctf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	RID color_tex = rd->texture_create(ctf, RD::TextureView());
	Vector<RID> att;
	att.push_back(color_tex);
	RID fb = rd->framebuffer_create(att);
	RD::FramebufferFormatID fb_format = rd->framebuffer_get_format(fb);

	RD::PipelineRasterizationState raster;
	raster.patch_control_points = 3;
	raster.cull_mode = RD::POLYGON_CULL_DISABLED; // bulged interior tris can flip winding; coverage-only test
	RD::PipelineMultisampleState ms;
	RD::PipelineDepthStencilState ds; // depth test off by default
	RD::PipelineColorBlendState blend = RD::PipelineColorBlendState::create_disabled(1);

	RID pipeline = rd->render_pipeline_create(shader, fb_format, RD::INVALID_ID, RD::RENDER_PRIMITIVE_TESSELATION_PATCH, raster, ms, ds, blend);
	check(pipeline.is_valid(), "render pipeline created with RENDER_PRIMITIVE_TESSELATION_PATCH + patch_control_points=3");
	if (!pipeline.is_valid()) {
		rd->free_rid(fb);
		rd->free_rid(color_tex);
		rd->free_rid(shader);
		print_line(vformat("TESS_SELFTEST: %d check(s) FAILED (pipeline)", g_failures));
		return;
	}

	int cov_1 = render_coverage(rd, fb, pipeline, color_tex, SIZE, 1.0f);
	int cov_16 = render_coverage(rd, fb, pipeline, color_tex, SIZE, 16.0f);
	print_line(vformat("TESS_SELFTEST: coverage level=1 -> %d px, level=16 -> %d px (of %d)", cov_1, cov_16, SIZE * SIZE));

	check(cov_1 > 0, "level=1 patch draws the flat base triangle (non-empty coverage)");
	check(cov_16 > 0, "level=16 patch draws (non-empty coverage)");
	// The clincher: only real subdivision creates the edge/interior vertices that bulge the silhouette
	// convex, so level=16 must cover meaningfully MORE than the level=1 flat triangle.
	check(cov_16 > cov_1 * 105 / 100, "tessellation subdivided: level=16 convex coverage exceeds level=1 flat triangle (proves TCS/TES ran)");
	// Sanity ceiling: a 0.15-NDC bulge on a ~0.8-scale triangle shouldn't more than ~double the area.
	check(cov_16 < cov_1 * 3, "level=16 coverage is a bounded convex bulge, not a degenerate full-screen fill");

	rd->free_rid(pipeline);
	rd->free_rid(fb);
	rd->free_rid(color_tex);
	rd->free_rid(shader);

	if (g_failures == 0) {
		print_line("TESS_SELFTEST: all checks passed - RD tessellation stack works end-to-end");
	} else {
		print_line(vformat("TESS_SELFTEST: %d check(s) FAILED", g_failures));
	}
}

// --- P1 language-hook parser check ------------------------------------------------------------------

namespace {

// Compiles a spatial shader through the ShaderLanguage parser using the real ShaderTypes tables (the
// same info ShaderCompiler feeds it), with no RenderingDevice. Returns OK / error just like production.
Error compile_spatial(const String &p_code, String &r_error) {
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(RSE::SHADER_SPATIAL);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	info.global_shader_uniform_type_func = nullptr; // test shaders use no global uniforms.

	ShaderLanguage parser;
	Error err = parser.compile(p_code, info);
	if (err != OK) {
		r_error = vformat("line %d: %s", parser.get_error_line(), parser.get_error_text());
	}
	return err;
}

void check_compiles(const char *p_what, const String &p_code) {
	String err;
	Error e = compile_spatial(p_code, err);
	check(e == OK, vformat("%s (%s)", p_what, e == OK ? String("compiled") : err));
}

void check_rejects(const char *p_what, const String &p_code) {
	String err;
	Error e = compile_spatial(p_code, err);
	check(e != OK, vformat("%s (%s)", p_what, e != OK ? String("rejected as expected") : String("UNEXPECTEDLY COMPILED")));
}

} // namespace

void run_tess_shader_selftest_if_requested() {
	if (!OS::get_singleton()->get_cmdline_args().find("--tess-shader-selftest")) {
		return;
	}

	print_line("TESS_SELFTEST: starting (P1: gdshader language-hook parser check)");
	g_failures = 0;

	if (ShaderTypes::get_singleton() == nullptr) {
		print_line("TESS_SELFTEST: ShaderTypes singleton unavailable - run after server init");
		return;
	}

	// A regression guard: an ordinary spatial shader with no tessellation still compiles.
	check_compiles("baseline spatial shader unaffected",
			"shader_type spatial;\n"
			"void fragment() { ALBEDO = vec3(0.5); }\n");

	// The opt-in render_mode is recognized.
	check_compiles("render_mode tessellation_adaptive accepted",
			"shader_type spatial;\n"
			"render_mode tessellation_adaptive;\n"
			"void fragment() { ALBEDO = vec3(0.5); }\n");

	// The displacement() processor and its built-ins parse: read const NORMAL/UV/VIEW_DEPTH, write
	// VERTEX/DISPLACEMENT.
	check_compiles("displacement() with built-ins compiles",
			"shader_type spatial;\n"
			"render_mode tessellation_adaptive;\n"
			"uniform float height_scale = 1.0;\n"
			"uniform float fade_distance = 200.0;\n"
			"void displacement() {\n"
			"	if (VIEW_DEPTH <= fade_distance) {\n"
			"		DISPLACEMENT = UV.x * 10.0 * height_scale;\n"
			"		VERTEX += NORMAL * DISPLACEMENT;\n"
			"	}\n"
			"}\n"
			"void fragment() { ALBEDO = vec3(0.5); }\n");

	check_rejects("VIEW_DEPTH is not visible outside displacement()",
			"shader_type spatial;\n"
			"render_mode tessellation_adaptive;\n"
			"void displacement() { DISPLACEMENT = 0.0; }\n"
			"void fragment() { ALBEDO = vec3(VIEW_DEPTH); }\n");

	// Negative controls prove the built-in metadata is real, not permissive.
	check_rejects("writing to const built-in NORMAL is rejected",
			"shader_type spatial;\n"
			"render_mode tessellation_adaptive;\n"
			"void displacement() { NORMAL = vec3(1.0); }\n");

	// The evaluation stage cannot read the shared scene descriptor sets, so built-ins sourced from scene data
	// must be rejected by name rather than silently resolving to a constant (TIME) or failing to compile deep
	// in glslang (CAMERA_POSITION_WORLD -> inv_view_matrix[3].xyz, undeclared in that stage).
	check_rejects("TIME is not available in displacement()",
			"shader_type spatial;\n"
			"render_mode tessellation_adaptive;\n"
			"void displacement() { DISPLACEMENT = sin(TIME); }\n");

	check_rejects("CAMERA_POSITION_WORLD is not available in displacement()",
			"shader_type spatial;\n"
			"render_mode tessellation_adaptive;\n"
			"void displacement() { DISPLACEMENT = CAMERA_POSITION_WORLD.y; }\n");

	check_rejects("EXPOSURE is not available in displacement()",
			"shader_type spatial;\n"
			"render_mode tessellation_adaptive;\n"
			"void displacement() { DISPLACEMENT = EXPOSURE; }\n");

	check_rejects("IN_SHADOW_PASS is not available in displacement()",
			"shader_type spatial;\n"
			"render_mode tessellation_adaptive;\n"
			"void displacement() { DISPLACEMENT = IN_SHADOW_PASS ? 1.0 : 0.0; }\n");

	// The excluded built-ins must stay available everywhere else - the opt-out is per function, not global.
	check_compiles("TIME still works in vertex() and fragment()",
			"shader_type spatial;\n"
			"render_mode tessellation_adaptive;\n"
			"void displacement() { DISPLACEMENT = 1.0; }\n"
			"void vertex() { VERTEX.y += sin(TIME); }\n"
			"void fragment() { ALBEDO = vec3(EXPOSURE * sin(TIME)); NORMAL = NORMAL; }\n");

	check_rejects("unknown render_mode is still rejected",
			"shader_type spatial;\n"
			"render_mode tessellation_bogus;\n"
			"void fragment() { ALBEDO = vec3(0.5); }\n");

	check_rejects("DISPLACEMENT is not visible outside displacement()",
			"shader_type spatial;\n"
			"render_mode tessellation_adaptive;\n"
			"void fragment() { ALBEDO = vec3(DISPLACEMENT); }\n");

	if (g_failures == 0) {
		print_line("TESS_SELFTEST: all checks passed - gdshader tessellation language hook works");
	} else {
		print_line(vformat("TESS_SELFTEST: %d check(s) FAILED", g_failures));
	}
}
