// Meshlet PBR shading, factored out of meshlet_render.glsl's fragment stage so the material-resolve
// compute pass (visibility-buffer path) can reuse the exact same lighting/GI/ambient math and stay
// pixel-identical to the hardware color path. Behavior is identical to the previous inline fragment
// code - this is a pure extraction.
//
// The includer MUST, BEFORE including this file:
//   1. include meshlet_shade_types_inc.glsl (MeshletMaterial/MeshletLight/SVOGINode + constants),
//   2. declare these globals (any set/binding; names must match):
//        buffer  meshlet_materials  { MeshletMaterial data[]; }
//        buffer  lights             { MeshletLight    data[]; }
//        buffer  svogi_nodes        { SVOGINode       data[]; }
//        texture2DArray radiance_octmap;   sampler radiance_sampler;
//        texture2D      material_textures[MeshletStorage::MAX_MATERIAL_TEXTURES];
//        sampler        material_sampler;
//
// NOTE: perturb_normal() below uses dFdx/dFdy (screen-space derivatives), so meshlet_shade() is
// currently FRAGMENT-STAGE ONLY. The material-resolve compute pass (P4) must supply analytic
// triangle gradients instead - see the design doc (meshlet_software_raster_design.md).

// --- Trimmed extraction of scene_forward_lights_inc.glsl's BRDF helpers (no buffer access; Lambert
// diffuse + Schlick-GGX specular, the non-fp16 path's exact behavior). ---

float D_GGX(float NoH, float roughness) {
	float a = NoH * roughness;
	float k = roughness / (1.0 - NoH * NoH + a * a);
	return k * k * (1.0 / M_PI);
}

float V_GGX(float NdotL, float NdotV, float alpha) {
	return 0.5 / mix(2.0 * NdotL * NdotV, NdotL + NdotV, alpha);
}

float SchlickFresnel(float u) {
	float m = 1.0 - u;
	float m2 = m * m;
	return m2 * m2 * m; // pow(m, 5).
}

vec3 F0(float metallic, float specular, vec3 albedo) {
	float dielectric = 0.16 * specular * specular;
	return mix(vec3(dielectric), albedo, metallic);
}

float get_omni_attenuation(float distance, float inv_range, float decay) {
	float nd = distance * inv_range;
	nd *= nd;
	nd *= nd; // nd^4.
	nd = max(1.0 - nd, 0.0);
	nd *= nd; // nd^2.
	return nd * pow(max(distance, 0.0001), -decay);
}

// Extended light_compute using MeshletMaterial to evaluate Principled BSDF features.
void light_compute(vec3 N, vec3 L, vec3 V, vec3 light_color, bool is_directional, float attenuation, MeshletMaterial mat, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	float NdotL = min(dot(N, L), 1.0);
	float cNdotV = max(dot(N, V), 1e-4);

	if (is_directional || attenuation > 1.175494351e-38) {
		float cNdotL = max(NdotL, 0.0);

		vec3 H = normalize(V + L);
		float cNdotH = clamp(dot(N, H), 0.0, 1.0);
		float cLdotH = clamp(dot(L, H), 0.0, 1.0);
		float cLdotH5 = SchlickFresnel(cLdotH);

		// F0 calculation from IOR and Metallic
		float f0_ior = pow((mat.ior - 1.0) / (mat.ior + 1.0), 2.0);
		vec3 f0 = mix(vec3(f0_ior), albedo, mat.metallic);

		float clearcoat_attenuation = 1.0;

		// Clearcoat (fixed IOR of 1.5 -> F0 = 0.04)
		if (mat.clearcoat > 0.0) {
			float cc_alpha = mix(0.001, 0.1, mat.clearcoat_roughness);
			float cc_D = D_GGX(cNdotH, cc_alpha);
			float cc_G = V_GGX(cNdotL, cNdotV, cc_alpha);
			float cc_F = mix(0.04, 1.0, cLdotH5) * mat.clearcoat;

			clearcoat_attenuation = 1.0 - cc_F;
			vec3 cc_specular = vec3(cNdotL * cc_D * cc_G * cc_F);
			specular_light += cc_specular * light_color * attenuation;
		}

		if (mat.metallic < 1.0) {
			// Basic diffuse (Lambert)
			float diffuse_brdf_NL = cNdotL * (1.0 / M_PI);

			// Simple Subsurface approximation (Wrap lighting)
			if (mat.subsurface_weight > 0.0) {
				float wrap = 0.5;
				float wrap_NdotL = max(0.0, (dot(N, L) + wrap) / (1.0 + wrap));
				vec3 sss_color = mat.subsurface_color * mat.subsurface_weight;
				diffuse_brdf_NL = mix(diffuse_brdf_NL, wrap_NdotL * (1.0 / M_PI), mat.subsurface_weight);
				diffuse_light += light_color * diffuse_brdf_NL * attenuation * clearcoat_attenuation * sss_color;
			} else {
				diffuse_light += light_color * diffuse_brdf_NL * attenuation * clearcoat_attenuation;
			}
		}

		if (mat.roughness > 0.0) {
			float alpha_ggx = mat.roughness * mat.roughness;
			float D = D_GGX(cNdotH, alpha_ggx);
			float G = V_GGX(cNdotL, cNdotV, alpha_ggx);

			float f90 = clamp(dot(f0, vec3(50.0 * 0.33)), mat.metallic, 1.0);
			vec3 F = f0 + (f90 - f0) * cLdotH5;
			vec3 specular_brdf_NL = vec3(cNdotL * D * G) * F;
			specular_light += specular_brdf_NL * light_color * attenuation * clearcoat_attenuation * mat.specular;
		}
	}
}

void light_process_directional(uint idx, vec3 N, vec3 V, MeshletMaterial mat, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	// L is the direction TO the light; lights.data[].direction already points toward the source for
	// this path, so it's used directly - NOT negated.
	vec3 L = normalize(lights.data[idx].direction);
	light_compute(N, L, V, lights.data[idx].color, true, 1.0, mat, albedo, diffuse_light, specular_light);
}

void light_process_omni(uint idx, vec3 vertex, vec3 N, vec3 V, MeshletMaterial mat, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	vec3 light_rel_vec = lights.data[idx].position - vertex;
	float light_length = length(light_rel_vec);
	float attenuation = get_omni_attenuation(light_length, lights.data[idx].inv_radius, lights.data[idx].attenuation);
	vec3 L = normalize(light_rel_vec);
	light_compute(N, L, V, lights.data[idx].color, false, attenuation, mat, albedo, diffuse_light, specular_light);
}

void light_process_spot(uint idx, vec3 vertex, vec3 N, vec3 V, MeshletMaterial mat, vec3 albedo, inout vec3 diffuse_light, inout vec3 specular_light) {
	vec3 light_rel_vec = lights.data[idx].position - vertex;
	float light_length = length(light_rel_vec);
	vec3 light_rel_vec_norm = light_rel_vec / light_length;
	float attenuation = get_omni_attenuation(light_length, lights.data[idx].inv_radius, lights.data[idx].attenuation);
	float cone_angle = lights.data[idx].cone_angle;
	float scos = max(dot(-light_rel_vec_norm, lights.data[idx].direction), cone_angle);
	float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - cone_angle));
	attenuation *= 1.0 - pow(spot_rim, lights.data[idx].cone_attenuation);
	vec3 L = light_rel_vec_norm;
	light_compute(N, L, V, lights.data[idx].color, false, attenuation, mat, albedo, diffuse_light, specular_light);
}

// Diffuse cone-march of the SVOGI octree, in absolute world space. Returns rgb = accumulated
// incident radiance, a = coverage.
vec4 svogi_cone_trace(vec3 pos, vec3 dir, float tan_half_angle, float max_distance, float bias, vec3 bounds_center, float bounds_half, float energy) {
	vec4 color = vec4(0.0);
	float dist = bias;

	while (dist < max_distance && color.a < 0.95) {
		float diameter = max(1.0, 2.0 * tan_half_angle * dist);
		vec3 sample_pos = pos + dir * dist;

		vec3 d = abs(sample_pos - bounds_center);
		if (d.x > bounds_half || d.y > bounds_half || d.z > bounds_half) {
			break; // Left the octree's bounds.
		}

		uint node_idx = 0u;
		vec3 current_center = bounds_center;
		float current_half = bounds_half;
		vec4 voxel_color = vec4(0.0);
		float target_size = max(diameter, current_half / 64.0); // 6 levels: leaf = bounds/64.

		for (uint depth = 0u; depth < 6u; depth++) {
			bvec3 is_pos = greaterThan(sample_pos, current_center);
			uint child_idx = (is_pos.x ? 1u : 0u) | ((is_pos.y ? 1u : 0u) << 1) | ((is_pos.z ? 1u : 0u) << 2);

			if ((svogi_nodes.data[node_idx].child_mask & (1u << child_idx)) == 0u) {
				break; // Empty space.
			}
			uint base_idx = svogi_nodes.data[node_idx].children_base_index;
			if (base_idx == 0u) {
				break; // No children allocated.
			}
			node_idx = base_idx + child_idx;

			vec3 offset = vec3(is_pos.x ? 1.0 : -1.0, is_pos.y ? 1.0 : -1.0, is_pos.z ? 1.0 : -1.0);
			current_half *= 0.5;
			current_center += offset * current_half;

			// Internal nodes hold mipmap-aggregated data (see svogi_mipmap.glsl), so sampling a
			// coarser level for a wide cone is correct, not just empty.
			if (current_half * 2.0 <= target_size || depth == 5u) {
				// The node's "albedo" field actually holds LIT RADIANCE (surface albedo * direct
				// light + emission), written by svogi_voxelize.glsl's direct-light-injection step.
				uint radiance_packed = svogi_nodes.data[node_idx].albedo;
				if (radiance_packed != 0u) {
					vec3 vox_radiance = vec3(
							float((radiance_packed >> 24u) & 0xFFu),
							float((radiance_packed >> 16u) & 0xFFu),
							float((radiance_packed >> 8u) & 0xFFu)) /
							255.0;
					voxel_color = vec4(vox_radiance * energy, 1.0);
				}
				break;
			}
		}

		if (voxel_color.a > 0.0) {
			float a = (1.0 - color.a);
			color += a * voxel_color;
		}
		dist += max(0.5, diameter * 0.5);
	}

	return color;
}

// Branchless orthonormal basis from a unit normal (Duff et al. 2017).
void svogi_basis(vec3 n, out vec3 t, out vec3 b) {
	float s = n.z >= 0.0 ? 1.0 : -1.0;
	float a = -1.0 / (s + n.z);
	float bb = n.x * n.y * a;
	t = vec3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
	b = vec3(bb, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted 6-cone hemisphere gather of indirect diffuse from the octree.
vec3 svogi_hemisphere_gather(vec3 pos, vec3 normal, float surface_offset, float max_distance, vec3 bounds_center, float bounds_half, float energy) {
	vec3 t, b;
	svogi_basis(normal, t, b);

	const vec3 CONE_DIRS[6] = vec3[](
			vec3(0.0, 0.0, 1.0),
			vec3(0.0, 0.866025, 0.5),
			vec3(0.823639, 0.267617, 0.5),
			vec3(0.509037, -0.700629, 0.5),
			vec3(-0.509037, -0.700629, 0.5),
			vec3(-0.823639, 0.267617, 0.5));
	const float CONE_WEIGHTS[6] = float[](0.25, 0.15, 0.15, 0.15, 0.15, 0.15);

	// Push the shared cone origin off the surface along the normal before tracing (clears local
	// curvature uniformly, avoiding the dark-petal/pinwheel artifact).
	vec3 start = pos + normal * surface_offset;

	vec3 acc = vec3(0.0);
	for (int i = 0; i < 6; i++) {
		vec3 dir = normalize(t * CONE_DIRS[i].x + b * CONE_DIRS[i].y + normal * CONE_DIRS[i].z);
		acc += CONE_WEIGHTS[i] * svogi_cone_trace(start, dir, 0.577, max_distance, surface_offset * 0.5, bounds_center, bounds_half, energy).rgb;
	}
	return acc;
}

// Cotangent-frame normal mapping without precomputed vertex tangents (Schueler 2011). Takes the
// screen-space world-position and UV gradients as parameters (dp1/dp2 = d(world_pos)/dx,dy; duv1/duv2
// = d(uv)/dx,dy) rather than calling dFdx/dFdy, so it works in BOTH the fragment stage (which passes
// dFdx/dFdy) and the compute resolve pass (which passes analytic triangle gradients).
vec3 perturb_normal_grad(vec3 N, vec3 dp1, vec3 dp2, vec2 duv1, vec2 duv2, vec3 map_normal) {
	vec3 dp2perp = cross(dp2, N);
	vec3 dp1perp = cross(N, dp1);
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

	// Degenerate UV derivatives -> keep the geometric normal.
	float det = max(dot(T, T), dot(B, B));
	if (det <= 0.0) {
		return N;
	}
	float invmax = inversesqrt(det);
	mat3 TBN = mat3(T * invmax, B * invmax, N);
	return normalize(TBN * map_normal);
}

// Full opaque PBR shade for one surface point. Inputs are what were previously interpolated varyings
// + push-constant fields; returns the final lit color (.rgb) and alpha (.a). Sets r_discard when
// alpha-scissor rejects the fragment (the caller does the actual discard / visbuffer skip). Identical
// math to meshlet_render.glsl's former inline fragment body.
// dpdx/dpdy = screen-space gradients of world_pos; duvdx/duvdy = screen-space gradients of the raw
// (pre-transform) UV. Fragment callers pass dFdx/dFdy of the interpolated varyings; the compute
// resolve pass passes analytic triangle gradients. Only used for normal mapping (perturb_normal_grad).
vec4 meshlet_shade(uint material_id, vec3 world_normal_in, vec3 world_pos, vec2 uv, vec3 camera_position, vec4 ambient_color, vec4 svogi_bounds, vec4 svogi_params, uint light_count, vec3 dpdx, vec3 dpdy, vec2 duvdx, vec2 duvdy, out bool r_discard) {
	r_discard = false;
	MeshletMaterial mat = meshlet_materials.data[material_id];
	vec3 N = normalize(world_normal_in);
	vec3 V = normalize(camera_position - world_pos);

	// StandardMaterial3D uv1 transform applied to the interpolated mesh UV.
	vec2 muv = uv * mat.uv1_scale + mat.uv1_offset;

	// Albedo texture modulates the base color factor; its alpha feeds alpha-scissor below.
	vec3 albedo = mat.albedo.rgb;
	float alpha = mat.albedo.a;
	if (mat.albedo_texture_index != MESHLET_TEXTURE_NONE) {
		vec4 albedo_tex = texture(sampler2D(material_textures[mat.albedo_texture_index], material_sampler), muv);
		albedo *= albedo_tex.rgb;
		alpha *= albedo_tex.a;
	}

	// Alpha-scissor (cutout) discard: flags bit 0 = alpha_scissor. Hard discard (opaque-only path).
	if ((mat.flags & 1u) != 0u && alpha < mat.alpha_scissor_threshold) {
		r_discard = true;
		return vec4(0.0);
	}

	// Normal map -> world normal via the derivative-built TBN (no stored vertex tangents).
	if (mat.normal_texture_index != MESHLET_TEXTURE_NONE) {
		vec3 nm = texture(sampler2D(material_textures[mat.normal_texture_index], material_sampler), muv).xyz * 2.0 - 1.0;
		nm.xy *= mat.normal_scale;
		// TBN from the transformed-UV gradients: d(muv) = d(uv) * uv1_scale (uv1_scale is uniform per
		// primitive), so scale the raw-UV gradients here to match muv.
		N = perturb_normal_grad(N, dpdx, dpdy, duvdx * mat.uv1_scale, duvdy * mat.uv1_scale, normalize(nm));
	}

	// ORM map: r = occlusion, g = roughness, b = metallic (Godot's ORM packing).
	float ao = 1.0;
	if (mat.orm_texture_index != MESHLET_TEXTURE_NONE) {
		vec3 orm = texture(sampler2D(material_textures[mat.orm_texture_index], material_sampler), muv).rgb;
		ao = orm.r;
		mat.roughness *= orm.g;
		mat.metallic *= orm.b;
	}

	vec3 diffuse_light = vec3(0.0);
	vec3 specular_light = vec3(0.0);
	for (uint i = 0; i < light_count; i++) {
		if (lights.data[i].is_directional != 0u) {
			light_process_directional(i, N, V, mat, albedo, diffuse_light, specular_light);
		} else if (lights.data[i].cone_angle < 1.0) {
			light_process_spot(i, world_pos, N, V, mat, albedo, diffuse_light, specular_light);
		} else {
			light_process_omni(i, world_pos, N, V, mat, albedo, diffuse_light, specular_light);
		}
	}

	// SVOGI indirect-diffuse bounce via the cosine-weighted multi-cone hemisphere gather.
	vec3 gi_diffuse = vec3(0.0);
	if (svogi_bounds.w > 0.0) {
		float voxel_size = svogi_bounds.w / 32.0; // root half-size / 32 ~= leaf diameter.
		gi_diffuse = svogi_hemisphere_gather(world_pos, N, voxel_size * 3.0, svogi_bounds.w * 2.0, svogi_bounds.xyz, svogi_bounds.w, svogi_params.x);
	}

	// Environment ambient (flat color, plus optional sky-radiance blend).
	vec3 ambient = ambient_color.rgb;
	if (ambient_color.a > 0.0) {
		vec3 sky_ambient = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(0.5, 0.5, svogi_params.z), 0.0).rgb * svogi_params.y;
		ambient = mix(ambient, sky_ambient, ambient_color.a);
	}

	vec3 color = albedo * (1.0 - mat.metallic) * (diffuse_light + (ambient + gi_diffuse) * ao) + specular_light + mat.emission;
	return vec4(color, alpha);
}
