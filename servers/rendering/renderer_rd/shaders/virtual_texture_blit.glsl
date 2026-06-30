#[compute]

#version 450

#VERSION_DEFINES

// Copies one source-texture page into one physical pool tile for the software virtual-texturing
// path (see virtual_texture_storage.h). One thread per STORED page texel (content + replicated
// borders). The border texels are filled from the source texture's TRUE neighbouring texels
// (clamped at the source mip edge), not by replicating the page's own edge - so bilinear/anisotropic
// filtering inside a resident page is seamless across the page boundary, matching what sampling the
// original full-resolution texture would have produced.
//
// PAGE_BORDER / STORED_PAGE_SIZE here MUST match VirtualTextureStorage::PAGE_BORDER /
// STORED_PAGE_SIZE in virtual_texture_storage.h.

#define PAGE_BORDER 4
#define STORED_PAGE_SIZE 136

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(rgba8, set = 0, binding = 1) uniform restrict writeonly image2D pool;

layout(push_constant, std430) uniform Params {
	ivec2 src_page_origin; // Texel origin of this page within the source mip (page_xy * PAGE_SIZE).
	ivec2 pool_tile_origin; // Texel origin of the destination tile within the pool atlas.
	int src_mip; // Source mip level this page is taken from.
	int pad0;
	int pad1;
	int pad2;
}
params;

void main() {
	ivec2 local_pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(local_pos, ivec2(STORED_PAGE_SIZE)))) {
		return;
	}

	ivec2 src_mip_size = textureSize(source, params.src_mip);
	ivec2 src_max = max(src_mip_size - ivec2(1), ivec2(0));

	// local_pos covers [0, STORED_PAGE_SIZE); the content starts at PAGE_BORDER, so the source texel
	// for this output texel is page_origin + (local - border). Border texels resolve to the true
	// source neighbour, clamped at the mip edge (replicated only where the source itself ends).
	ivec2 src_texel = clamp(params.src_page_origin + (local_pos - ivec2(PAGE_BORDER)), ivec2(0), src_max);
	vec4 color = texelFetch(source, src_texel, params.src_mip);

	imageStore(pool, params.pool_tile_origin + local_pos, color);
}
