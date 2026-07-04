// Alpha-scissor (cutout) test for the visibility-buffer rasterizers. A visbuffer can only store one
// surface per pixel, so a cutout material MUST be tested at RASTER time (skip the atomicMax where the
// texture is transparent) - otherwise the cutout quad wins the depth and the resolve, discarding it,
// reveals the background instead of the geometry layered behind the hole. The includer must include
// meshlet_shade_types_inc.glsl (for MeshletMaterial + MESHLET_TEXTURE_NONE) and declare the
// meshlet_materials storage buffer + material_textures[] array + material_sampler before this file.
//
// Uses textureLod (explicit LOD 0) so it works in a compute stage too (no implicit derivatives). This
// is a coverage test, not shading - LOD 0 alpha is fine.

// Returns true if the fragment at raw mesh UV p_uv should be discarded (cutout hole). Non-scissor
// materials (flags bit 0 clear) never discard.
bool meshlet_alpha_scissor_discard(uint p_material_id, vec2 p_uv) {
	MeshletMaterial mat = meshlet_materials.data[p_material_id];
	if ((mat.flags & 1u) == 0u) {
		return false;
	}
	float alpha = mat.albedo.a;
	if (mat.albedo_texture_index != MESHLET_TEXTURE_NONE) {
		vec2 muv = p_uv * mat.uv1_scale + mat.uv1_offset;
		alpha *= textureLod(sampler2D(material_textures[mat.albedo_texture_index], material_sampler), muv, 0.0).a;
	}
	return alpha < mat.alpha_scissor_threshold;
}
