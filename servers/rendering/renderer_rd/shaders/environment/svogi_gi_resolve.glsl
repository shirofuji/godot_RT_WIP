#[compute]

#version 450

#VERSION_DEFINES

// Screen-space SVOGI indirect-diffuse resolve for the Forward+ path (denoiser FA-1). One thread per
// HALF-res pixel: read the Forward+ depth + normal-roughness gbuffer, reconstruct the absolute-world
// position + world normal, cone-trace the octree, and write the raw (un-filtered) GI radiance into a
// half-res buffer. The Forward+ opaque shader then samples this (bilaterally upsampled) instead of
// cone-tracing inline per fragment - which is what lets FB/FC temporally + spatially filter it.
//
// Reconstruction mirrors gi.glsl exactly: depth -> view space via inv_projection, then the FULL
// cam_transform (rotation + translation) to absolute world (the octree is built in absolute world
// space); the gbuffer normal is view-space, rotated to world via mat3(cam_transform).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// SVOGINode struct (+ M_PI). Shader lives in environment/, the include is one dir up.
#include "../meshlet_shade_types_inc.glsl"

layout(set = 0, binding = 0) uniform texture2D depth_buffer;
layout(set = 0, binding = 1) uniform texture2D normal_roughness_buffer;
layout(set = 0, binding = 2) uniform sampler nearest_sampler;

layout(set = 0, binding = 3, std430) restrict readonly buffer SVOGINodes {
	SVOGINode data[];
}
svogi_nodes;

layout(set = 0, binding = 4, rgba16f) uniform restrict writeonly image2D gi_out;

// Temporal accumulation (FB): previous frame's blended GI + the matrix to reproject this frame's world
// position into it. history_valid gates the first frame / resizes; new_weight is the exponential-moving-
// average blend factor (fraction of the current cone-trace mixed in each frame).
layout(set = 0, binding = 5) uniform texture2D history_buffer;
layout(set = 0, binding = 6) uniform sampler linear_sampler;
layout(set = 0, binding = 7, std140) uniform Temporal {
	mat4 prev_world_to_clip;
	vec4 params; // x = history_valid (0/1), y = new_weight, z/w = pad.
}
temporal;

// Packed into 128 bytes (RD's MAX_PUSH_CONSTANT_SIZE). clip_to_world = cam_transform * inv_projection
// folds clip->view->world into one matrix (cam_transform is affine so it preserves w, letting the
// perspective divide happen after). The camera basis columns (for the view->world normal rotation) and
// the octree bounds/energy/screen-size ride in the spare vec4 lanes.
layout(push_constant, std430) uniform Params {
	mat4 clip_to_world;
	vec4 cam_basis_0; // xyz = camera x-axis, w = octree center x.
	vec4 cam_basis_1; // xyz = camera y-axis, w = octree center y.
	vec4 cam_basis_2; // xyz = camera z-axis, w = octree center z.
	vec4 misc; // x = octree half-size (0 = off), y = energy, z = full_width, w = full_height.
}
params;

// svogi_cone_trace / svogi_hemisphere_gather (reference svogi_nodes declared above).
#include "../meshlet_svogi_inc.glsl"

void main() {
	ivec2 half_dims = imageSize(gi_out);
	ivec2 hp = ivec2(gl_GlobalInvocationID.xy);
	if (hp.x >= half_dims.x || hp.y >= half_dims.y) {
		return;
	}


	ivec2 full_size = ivec2(int(params.misc.z), int(params.misc.w));
	ivec2 fp = min(hp * 2, full_size - ivec2(1));

	// Normal-roughness gbuffer: xyz = view-space normal (0.5+0.5 packed), w = roughness (dynamic-object
	// flag encoded as roughness > 0.5, per gi.glsl). Length ~0 => no geometry (sky / uncovered) - skip.
	vec4 nr = texelFetch(sampler2D(normal_roughness_buffer, nearest_sampler), fp, 0);
	vec3 view_normal = nr.xyz * 2.0 - 1.0;
	if (dot(view_normal, view_normal) < 0.25 || nr.w > 0.5) {
		// No valid static geometry here (empty, or a dynamic object - SVOGI is static-only, those get
		// their indirect light from SSIL instead). Write zero; the upsample/temporal pass rejects it.
		imageStore(gi_out, hp, vec4(0.0));
		return;
	}

	float depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), fp, 0).r;
	if (depth <= 0.0) { // Reverse-Z: far plane = 0.
		imageStore(gi_out, hp, vec4(0.0));
		return;
	}

	// Reconstruct absolute-world position via the folded clip->world matrix (perspective divide after).
	vec2 ndc_xy = (2.0 * vec2(fp) / vec2(full_size)) - 1.0;
	vec4 wh = params.clip_to_world * vec4(ndc_xy, depth, 1.0);
	vec3 world_pos = wh.xyz / wh.w;

	// View-space normal -> world via the camera basis columns.
	vec3 vn = normalize(view_normal);
	vec3 world_normal = normalize(params.cam_basis_0.xyz * vn.x + params.cam_basis_1.xyz * vn.y + params.cam_basis_2.xyz * vn.z);

	vec3 bounds_center = vec3(params.cam_basis_0.w, params.cam_basis_1.w, params.cam_basis_2.w);
	float bounds_half = params.misc.x;
	float energy = params.misc.y;


	vec3 gi = vec3(0.0);
	if (bounds_half > 0.0) {
		float voxel_size = bounds_half / 32.0; // root half-size / 32 ~= leaf diameter.
		gi = svogi_hemisphere_gather(world_pos, world_normal, voxel_size * 3.0, bounds_half * 2.0, bounds_center, bounds_half, energy);
	}

	// Temporal accumulation: reproject this pixel's absolute-world position into the previous frame's clip
	// space, sample the accumulated history there, and exponentially blend the noisy current cone-trace
	// into it. This suppresses the ray-jitter/undersampling shimmer. History is rejected when it lands
	// off-screen (disocclusion / camera turned away) or was never written (alpha 0), falling back to the
	// raw current sample - which reconverges over ~1/new_weight frames. A world-position reprojection (no
	// motion vectors) is enough here: the SVOGI-lit geometry is static terrain.
	vec3 result = gi;
	if (temporal.params.x > 0.5) {
		vec4 pc = temporal.prev_world_to_clip * vec4(world_pos, 1.0);
		if (pc.w > 0.0) {
			vec2 prev_uv = (pc.xy / pc.w) * 0.5 + 0.5;
			if (all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
				vec4 hist = texture(sampler2D(history_buffer, linear_sampler), prev_uv);
				if (hist.a > 0.0) {
					result = mix(hist.rgb, gi, clamp(temporal.params.y, 0.0, 1.0));
				}
			}
		}
	}

	imageStore(gi_out, hp, vec4(result, 1.0));
}
