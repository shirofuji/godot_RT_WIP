// Functions related to gi/svogi for our forward renderer

//standard voxel cone trace
vec4 voxel_cone_trace(texture3D probe, vec3 cell_size, vec3 pos, vec3 direction, float tan_half_angle, float max_distance, float p_bias) {
	float dist = p_bias;
	vec4 color = vec4(0.0);

	while (dist < max_distance && color.a < 0.95) {
		float diameter = max(1.0, 2.0 * tan_half_angle * dist);
		vec3 uvw_pos = (pos + dist * direction) * cell_size;
		float half_diameter = diameter * 0.5;
		//check if outside, then break
		if (any(greaterThan(abs(uvw_pos - 0.5), vec3(0.5f + half_diameter * cell_size)))) {
			break;
		}
		vec4 scolor = textureLod(sampler3D(probe, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uvw_pos, log2(diameter));
		float a = (1.0 - color.a);
		color += a * scolor;
		dist += half_diameter;
	}

	return color;
}

vec4 voxel_cone_trace_45_degrees(texture3D probe, vec3 cell_size, vec3 pos, vec3 direction, float tan_half_angle, float max_distance, float p_bias) {
	float dist = p_bias;
	vec4 color = vec4(0.0);
	float radius = max(0.5, tan_half_angle * dist);
	float lod_level = log2(radius * 2.0);

	while (dist < max_distance && color.a < 0.95) {
		vec3 uvw_pos = (pos + dist * direction) * cell_size;

		//check if outside, then break
		if (any(greaterThan(abs(uvw_pos - 0.5), vec3(0.5f + radius * cell_size)))) {
			break;
		}
		vec4 scolor = textureLod(sampler3D(probe, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uvw_pos, lod_level);
		lod_level += 1.0;

		float a = (1.0 - color.a);
		scolor *= a;
		color += scolor;
		dist += radius;
		radius = max(0.5, tan_half_angle * dist);
	}

	return color;
}

void voxel_gi_compute(uint index, vec3 position, vec3 normal, vec3 ref_vec, mat3 normal_xform, float roughness, vec3 ambient, vec3 environment, inout vec4 out_spec, inout vec4 out_diff) {
	position = (voxel_gi_instances.data[index].xform * vec4(position, 1.0)).xyz;
	ref_vec = normalize((voxel_gi_instances.data[index].xform * vec4(ref_vec, 0.0)).xyz);
	normal = normalize((voxel_gi_instances.data[index].xform * vec4(normal, 0.0)).xyz);

	position += normal * voxel_gi_instances.data[index].normal_bias;

	//this causes corrupted pixels, i have no idea why..
	if (any(bvec2(any(lessThan(position, vec3(0.0))), any(greaterThan(position, voxel_gi_instances.data[index].bounds))))) {
		return;
	}

	vec3 blendv = abs(position / voxel_gi_instances.data[index].bounds * 2.0 - 1.0);
	float blend = clamp(1.0 - max(blendv.x, max(blendv.y, blendv.z)), 0.0, 1.0);
	//float blend=1.0;

	float max_distance = length(voxel_gi_instances.data[index].bounds);
	vec3 cell_size = 1.0 / voxel_gi_instances.data[index].bounds;

	//radiance

#define MAX_CONE_DIRS 4

	vec3 cone_dirs[MAX_CONE_DIRS] = vec3[](
			vec3(0.707107, 0.0, 0.707107),
			vec3(0.0, 0.707107, 0.707107),
			vec3(-0.707107, 0.0, 0.707107),
			vec3(0.0, -0.707107, 0.707107));

	float cone_weights[MAX_CONE_DIRS] = float[](0.25, 0.25, 0.25, 0.25);
	float cone_angle_tan = 0.98269;

	vec3 light = vec3(0.0);

	for (int i = 0; i < MAX_CONE_DIRS; i++) {
		vec3 dir = normalize((voxel_gi_instances.data[index].xform * vec4(normal_xform * cone_dirs[i], 0.0)).xyz);

		vec4 cone_light = voxel_cone_trace_45_degrees(voxel_gi_textures[index], cell_size, position, dir, cone_angle_tan, max_distance, voxel_gi_instances.data[index].bias);

		if (voxel_gi_instances.data[index].blend_ambient) {
			cone_light.rgb = mix(ambient, cone_light.rgb, min(1.0, cone_light.a / 0.95));
		}

		light += cone_weights[i] * cone_light.rgb;
	}

	light *= voxel_gi_instances.data[index].dynamic_range * voxel_gi_instances.data[index].exposure_normalization;
	out_diff += vec4(light * blend, blend);

	//irradiance
	vec4 irr_light = voxel_cone_trace(voxel_gi_textures[index], cell_size, position, ref_vec, tan(roughness * 0.5 * M_PI * 0.99), max_distance, voxel_gi_instances.data[index].bias);
	if (voxel_gi_instances.data[index].blend_ambient) {
		irr_light.rgb = mix(environment, irr_light.rgb, min(1.0, irr_light.a / 0.95));
	}
	irr_light.rgb *= voxel_gi_instances.data[index].dynamic_range * voxel_gi_instances.data[index].exposure_normalization;
	//irr_light=vec3(0.0);

	out_spec += vec4(irr_light.rgb * blend, blend);
}

vec2 octahedron_wrap(vec2 v) {
	vec2 signVal;
	signVal.x = v.x >= 0.0 ? 1.0 : -1.0;
	signVal.y = v.y >= 0.0 ? 1.0 : -1.0;
	return (1.0 - abs(v.yx)) * signVal;
}

vec2 octahedron_encode(vec3 n) {
	// https://twitter.com/Stubbesaurus/status/937994790553227264
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	n.xy = n.z >= 0.0 ? n.xy : octahedron_wrap(n.xy);
	n.xy = n.xy * 0.5 + 0.5;
	return n.xy;
}

vec4 svogi_cone_trace(vec3 pos, vec3 dir, float tan_half_angle, float max_distance, float p_bias) {
	// Basic octree voxel cone tracing stub
	// Real implementation requires hierarchical traversal of svogi_nodes SSBO.
	vec4 color = vec4(0.0);
	float dist = p_bias;
	
	while (dist < max_distance && color.a < 0.95) {
		float diameter = max(1.0, 2.0 * tan_half_angle * dist);
		// Stub: return a default dark ambient color
		vec4 scolor = vec4(0.05, 0.05, 0.05, 0.1); 
		
		float a = (1.0 - color.a);
		color += a * scolor;
		dist += diameter * 0.5;
	}
	
	return color;
}

void svogi_process(uint cascade, vec3 cascade_pos, vec3 cam_pos, vec3 cam_normal, vec3 cam_specular_normal, bool use_specular, float roughness, out vec3 diffuse_light, out vec3 specular_light, out float blend) {
	// Replaced legacy SDFGI process with SVOGI cone trace
	
	// Diffuse cone (wide)
	vec4 diffuse_accum = svogi_cone_trace(cam_pos + cam_normal * svogi.normal_bias, cam_normal, 1.0, 100.0, 0.1);
	diffuse_light = diffuse_accum.rgb;
	
	// Specular cone (narrow based on roughness)
	if (use_specular) {
		vec4 specular_accum = svogi_cone_trace(cam_pos + cam_specular_normal * svogi.normal_bias, cam_specular_normal, tan(roughness * 0.5 * M_PI * 0.99), 100.0, 0.1);
		specular_light = specular_accum.rgb;
	} else {
		specular_light = vec3(0.0);
	}
	
	blend = 1.0; // Fully blend SVOGI
}
