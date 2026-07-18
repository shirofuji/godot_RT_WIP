#[compute]

#version 450

#VERSION_DEFINES

// Edge-aware a-trous spatial filter for the screen-space SVOGI resolve (denoiser FC). One thread per
// HALF-res pixel. Reads the temporally-accumulated GI (denoiser FB output) and blurs it with a 5x5
// a-trous kernel whose taps are spread by `step` (run repeatedly with step = 1,2,4,... for a wide blur
// at low cost). Edge-stopping weights reject taps that cross a geometry boundary so GI does not bleed
// across silhouettes: a view-space normal similarity term plus an SVGF-style plane-distance term
// (how far the tap's surface point sits off the center point's tangent plane).
//
// This is what lets the temporal pass (FB) stay light - the spatial blur removes the per-frame cone-
// trace noise that would otherwise show through while the camera moves, without the heavy temporal
// accumulation that causes ghosting/trailing.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform texture2D gi_in;
layout(set = 0, binding = 1) uniform texture2D depth_buffer;
layout(set = 0, binding = 2) uniform texture2D normal_roughness_buffer;
layout(set = 0, binding = 3) uniform sampler nearest_sampler;
layout(set = 0, binding = 4, rgba16f) uniform restrict writeonly image2D gi_out;

layout(push_constant, std430) uniform Params {
	mat4 clip_to_view; // inv(corrected projection): clip -> view space (for the plane-distance weight).
	vec4 misc; // x = step (a-trous hole size), y = full_width, z = full_height, w = normal power.
	vec4 misc2; // x = plane-distance sigma (view-space metres), y/z/w = pad.
}
params;

// Reconstruct the view-space position of a full-res gbuffer pixel from its hardware depth.
vec3 reconstruct_view(ivec2 fp, ivec2 full, float depth) {
	vec2 ndc = (2.0 * vec2(fp) / vec2(full)) - 1.0;
	vec4 vh = params.clip_to_view * vec4(ndc, depth, 1.0);
	return vh.xyz / vh.w;
}

void main() {
	ivec2 half_dims = imageSize(gi_out);
	ivec2 hp = ivec2(gl_GlobalInvocationID.xy);
	if (hp.x >= half_dims.x || hp.y >= half_dims.y) {
		return;
	}

	ivec2 full = ivec2(int(params.misc.y), int(params.misc.z));
	// gbuffer stride = full-res / GI-buffer size (2 half-res, 4 quarter-res); derived so this follows
	// whatever downscale gi.cpp picks.
	ivec2 gi_stride = max(full / half_dims, ivec2(1));
	ivec2 fp = min(hp * gi_stride, full - ivec2(1));

	vec4 c = texelFetch(sampler2D(gi_in, nearest_sampler), hp, 0);

	// Center gbuffer: skip filtering where there is no valid static geometry (sky / dynamic / far) - just
	// pass the value through so the FB validity (alpha) and the forward upsample stay consistent.
	vec4 nr_c = texelFetch(sampler2D(normal_roughness_buffer, nearest_sampler), fp, 0);
	vec3 n_c = nr_c.xyz * 2.0 - 1.0;
	float d_c = texelFetch(sampler2D(depth_buffer, nearest_sampler), fp, 0).r;
	if (d_c <= 0.0 || dot(n_c, n_c) < 0.25 || nr_c.w > 0.5 || c.a <= 0.0) {
		imageStore(gi_out, hp, c);
		return;
	}
	n_c = normalize(n_c);
	vec3 p_c = reconstruct_view(fp, full, d_c);

	int step = int(params.misc.x);
	float n_pow = params.misc.w;
	float plane_sigma = max(params.misc2.x, 1e-4);

	// 5x5 separable-ish a-trous kernel weights (B3 spline: 1, 2/3, 1/6 along each axis). Center is 1.
	float kernel[3] = float[](1.0, 2.0 / 3.0, 1.0 / 6.0);
	const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

	vec3 sum = vec3(0.0);
	float wsum = 0.0;
	float max_nb_luma = 0.0; // brightest valid neighbour, for the outlier clamp below.
	float min_nb_luma = 1e30; // darkest valid neighbour.
	int nb_count = 0;

	for (int dy = -2; dy <= 2; dy++) {
		for (int dx = -2; dx <= 2; dx++) {
			if (dx == 0 && dy == 0) {
				continue;
			}
			ivec2 thp = hp + ivec2(dx, dy) * step;
			if (thp.x < 0 || thp.y < 0 || thp.x >= half_dims.x || thp.y >= half_dims.y) {
				continue;
			}
			vec4 t = texelFetch(sampler2D(gi_in, nearest_sampler), thp, 0);
			if (t.a <= 0.0) {
				continue; // invalid GI sample (sky / dynamic / disoccluded) - do not average it in.
			}
			ivec2 tfp = min(thp * gi_stride, full - ivec2(1));
			vec4 nr_t = texelFetch(sampler2D(normal_roughness_buffer, nearest_sampler), tfp, 0);
			vec3 n_t = nr_t.xyz * 2.0 - 1.0;
			float d_t = texelFetch(sampler2D(depth_buffer, nearest_sampler), tfp, 0).r;
			if (d_t <= 0.0 || dot(n_t, n_t) < 0.25) {
				continue;
			}
			n_t = normalize(n_t);
			vec3 p_t = reconstruct_view(tfp, full, d_t);

			float w_n = pow(max(0.0, dot(n_c, n_t)), n_pow);
			// Plane distance: how far the tap point is off the center's tangent plane. Coplanar (same
			// flat ground) -> ~0 -> weight ~1; a step/edge -> large -> weight ~0.
			float w_p = exp(-abs(dot(n_c, p_t - p_c)) / plane_sigma);
			float k = kernel[abs(dx)] * kernel[abs(dy)];
			float w = w_n * w_p * k;

			sum += t.rgb * w;
			wsum += w;
			float tl = dot(t.rgb, LUMA);
			max_nb_luma = max(max_nb_luma, tl);
			min_nb_luma = min(min_nb_luma, tl);
			nb_count++;
		}
	}

	// Outlier rejection (first iteration only): a pixel far outside its ENTIRE neighbourhood's brightness
	// range is GI sampling noise, not a real feature. The visible specks are DARK holes - pixels whose 6
	// cones happened to miss the lit voxels (or self-occluded) while their neighbours gathered light - so a
	// linear blur can't fix them (a hole surrounded by lit pixels just becomes a dimmer, wider hole, which
	// is why widening the a-trous did nothing). Replace a too-dark (or too-bright) center with the
	// neighbourhood mean so the hole fills in. Guarded by enough valid neighbours to trust the estimate
	// (avoid nuking a genuine isolated crevice/edge). params.misc2.y = 1 on the first (step-1) pass only.
	vec3 cc = c.rgb;
	if (params.misc2.y > 0.5 && nb_count >= 4 && wsum > 1e-4) {
		vec3 nb_mean = sum / wsum;
		float cl = dot(cc, LUMA);
		if (cl < min_nb_luma * 0.5 || cl > max_nb_luma * 2.0 + 1e-4) {
			cc = nb_mean;
		}
	}
	sum += cc; // center weight = kernel[0]*kernel[0]*1*1 = 1.
	wsum += 1.0;

	imageStore(gi_out, hp, vec4(sum / wsum, c.a));
}
