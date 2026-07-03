// Shared per-pixel visibility-buffer resolve, used by both the compute resolve
// (meshlet_visbuffer_resolve.glsl -> imageStore, for tests) and the fragment resolve
// (meshlet_visbuffer_resolve_raster.glsl -> frag_color + gl_FragDepth, for the live frame), so both
// stay identical. The includer MUST, before including this file:
//   - include meshlet_shade_types_inc.glsl, then declare all the resolve buffers (visbuffer/vis_depth
//     +vis_payload, sw_visible, hw_visible, transforms, meshlet_descriptors, meshlet_vertex_remap,
//     meshlet_triangles, vertex_positions, vertex_attributes, instance_material_ids, meshlet_materials,
//     lights, svogi_nodes, radiance_octmap/sampler, material_textures/sampler) + the VisibleMeshlet and
//     MeshletDescriptor structs + a `params` push constant with view_projection/camera_position/
//     light_count/ambient_color/svogi_bounds/svogi_params,
//   - include meshlet_geometry_inc.glsl and meshlet_shade_inc.glsl.

// Signed area * 2 of triangle (a, b, c) - the edge function.
float edge(vec2 a, vec2 b, vec2 c) {
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Perspective-correct, normalized barycentric weights of screen point p in triangle (s0,s1,s2) with
// per-vertex 1/w (iw0..2). area = edge(s0,s1,s2) (nonzero, caller-checked).
vec3 pc_bary(vec2 s0, vec2 s1, vec2 s2, float iw0, float iw1, float iw2, vec2 p, float area) {
	float b0 = edge(s1, s2, p) / area;
	float b1 = edge(s2, s0, p) / area;
	float b2 = edge(s0, s1, p) / area;
	b0 *= iw0;
	b1 *= iw1;
	b2 *= iw2;
	float s = b0 + b1 + b2;
	if (abs(s) < 1e-20) {
		return vec3(b0, b1, b2);
	}
	return vec3(b0, b1, b2) / s;
}

// Resolve pixel `pix` (in a `dims`-sized target). Returns true and fills r_color (lit rgb) + r_depth
// (the winning reverse-Z NDC depth, for gl_FragDepth) when the visbuffer has a fragment there;
// returns false for empty pixels and alpha-scissor discards.
bool resolve_visbuffer_pixel(ivec2 pix, ivec2 dims, out vec3 r_color, out float r_depth) {
	int idx = pix.y * dims.x + pix.x;

#ifndef MESHLET_VISBUFFER_FALLBACK
	uint64_t packed = visbuffer.data[idx];
	if (packed == 0ul) {
		return false;
	}
	uint z_bits = uint(packed >> 32);
	uint payload = uint(packed & 0xFFFFFFFFul);
#else
	uint z_bits = vis_depth.data[idx];
	if (z_bits == 0u) {
		return false;
	}
	uint payload = vis_payload.data[idx];
#endif

	uint is_sw = (payload >> 31) & 0x1u;
	uint slot = (payload >> 7) & 0xFFFFFFu;
	uint tri = payload & 0x7Fu;

	VisibleMeshlet vm = (is_sw == 1u) ? sw_visible.data[slot] : hw_visible.data[slot];
	MeshletDescriptor d = meshlet_descriptors.data[vm.meshlet_index];
	mat4 model = transforms.data[vm.instance_index];

	// Inverse-transpose (cofactor) of the model matrix for normals (see meshlet_render.glsl).
	mat3 m3 = mat3(model);
	vec3 cof0 = cross(m3[1], m3[2]);
	mat3 normal_matrix = mat3(cof0, cross(m3[2], m3[0]), cross(m3[0], m3[1]));
	float det_sign = sign(dot(m3[0], cof0));

	// Fetch the triangle's three vertices: world position, world normal, UV, and screen position.
	vec3 wpos[3];
	vec3 wnrm[3];
	vec2 uv[3];
	vec2 screen[3];
	float invw[3];
	uint base = d.triangle_offset + tri * 3u;
	for (uint c = 0u; c < 3u; c++) {
		uint local_v = fetch_triangle_local_vertex(base + c);
		uint global_v = meshlet_vertex_remap.data[d.vertex_remap_offset + local_v];
		vec4 lpos = vertex_positions.data[global_v];
		vec4 attrib = vertex_attributes.data[global_v];
		wpos[c] = (model * vec4(lpos.xyz, 1.0)).xyz;
		wnrm[c] = normalize(normal_matrix * oct_decode_normal(attrib.xy)) * det_sign;
		uv[c] = attrib.zw;
		vec4 clip = params.view_projection * vec4(wpos[c], 1.0);
		invw[c] = 1.0 / clip.w;
		vec3 ndc = clip.xyz * invw[c];
		screen[c] = (ndc.xy * 0.5 + 0.5) * vec2(dims);
	}

	float area = edge(screen[0], screen[1], screen[2]);
	if (abs(area) < 1e-9) {
		return false; // Degenerate on screen.
	}

	vec2 p = vec2(pix) + 0.5;
	vec3 bc = pc_bary(screen[0], screen[1], screen[2], invw[0], invw[1], invw[2], p, area);
	vec3 world_pos = bc.x * wpos[0] + bc.y * wpos[1] + bc.z * wpos[2];
	vec3 world_normal = normalize(bc.x * wnrm[0] + bc.y * wnrm[1] + bc.z * wnrm[2]);
	vec2 tex_uv = bc.x * uv[0] + bc.y * uv[1] + bc.z * uv[2];

	// Analytic screen-space gradients: perspective-correct interpolate one pixel over in x and y.
	vec3 bcx = pc_bary(screen[0], screen[1], screen[2], invw[0], invw[1], invw[2], p + vec2(1.0, 0.0), area);
	vec3 bcy = pc_bary(screen[0], screen[1], screen[2], invw[0], invw[1], invw[2], p + vec2(0.0, 1.0), area);
	vec3 dpdx = (bcx.x * wpos[0] + bcx.y * wpos[1] + bcx.z * wpos[2]) - world_pos;
	vec3 dpdy = (bcy.x * wpos[0] + bcy.y * wpos[1] + bcy.z * wpos[2]) - world_pos;
	vec2 duvdx = (bcx.x * uv[0] + bcx.y * uv[1] + bcx.z * uv[2]) - tex_uv;
	vec2 duvdy = (bcy.x * uv[0] + bcy.y * uv[1] + bcy.z * uv[2]) - tex_uv;

	uint material_id = instance_material_ids.data[vm.instance_index] & 0x7FFFFFFFu; // Strip owns-own-depth flag.

	bool do_discard = false;
	vec4 shaded = meshlet_shade(material_id, world_normal, world_pos, tex_uv, params.camera_position, params.ambient_color, params.svogi_bounds, params.svogi_params, params.light_count, dpdx, dpdy, duvdx, duvdy, do_discard);
	if (do_discard) {
		return false; // Alpha-scissor cutout.
	}
	r_color = shaded.rgb;
	r_depth = uintBitsToFloat(z_bits);
	return true;
}
