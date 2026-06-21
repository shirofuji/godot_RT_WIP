#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(r32f, set = 0, binding = 1) uniform restrict writeonly image2D dest;

layout(push_constant, std430) uniform Params {
	ivec2 dest_size;
	ivec2 pad;
}
params;

// Min-reduces a 2x2 (source-resolution) footprint into 1 dest texel. Godot's RD/Vulkan backend
// uses reversed-Z by default (RenderSceneDataRD::get_cam_projection()'s depth correction defaults
// to p_reverse_z=true) - larger raw device depth = nearer, smaller = farther. The conservative
// occluder value to keep per footprint is the *farthest* sample (if even the farthest-occluder
// sample in a footprint is still nearer than some object, the whole footprint occludes it) - under
// reversed-Z that's the minimum value, not the maximum. (A prior version of this shader assumed
// non-reversed depth and used max() - confirmed wrong against the real engine: it caused
// near/foreground objects to be wrongly culled instead of distant ones.) Edge reads are clamped to
// the source's valid range instead of using shader variants for odd dimensions - harmless for a
// min reduction (just resamples the edge texel, never introduces incorrect data).
void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel_pos, params.dest_size))) {
		return;
	}

	ivec2 src_size = textureSize(source, 0);
	ivec2 src_max = max(src_size - ivec2(1), ivec2(0));

	float depth = texelFetch(source, min(pixel_pos * 2 + ivec2(0, 0), src_max), 0).x;
	depth = min(depth, texelFetch(source, min(pixel_pos * 2 + ivec2(1, 0), src_max), 0).x);
	depth = min(depth, texelFetch(source, min(pixel_pos * 2 + ivec2(0, 1), src_max), 0).x);
	depth = min(depth, texelFetch(source, min(pixel_pos * 2 + ivec2(1, 1), src_max), 0).x);

	imageStore(dest, pixel_pos, vec4(depth, 0.0, 0.0, 0.0));
}
