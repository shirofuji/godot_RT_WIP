/**************************************************************************/
/*  meshlet_software_rasterizer.h                                        */
/**************************************************************************/

#ifndef MESHLET_SOFTWARE_RASTERIZER_H
#define MESHLET_SOFTWARE_RASTERIZER_H

#include "servers/rendering/rendering_device.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_software_rasterize.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_visbuffer_dispatch_args.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_visbuffer_hw_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_visbuffer_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_visbuffer_resolve_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/meshlet_culler.h"

#include "core/math/color.h"

// Visibility-buffer software rasterizer (P2b). Scan-converts the small/subpixel clusters that P1's
// classifier routed to the software worklist into a visibility buffer via atomicMax - the nearest
// surface's packed (depth, slot, triangle) wins per pixel. The material-resolve pass (P4) reads the
// visbuffer and shades. Two paths, selected from RD::SUPPORTS_BUFFER_ATOMIC_INT64: a primary uint64
// visbuffer (one atomicMax), and a 32-bit fallback (separate depth + payload buffers).
class MeshletSoftwareRasterizer {
public:
	// Must match meshlet_software_rasterize.glsl's Params block exactly (std430, 96 bytes): mat4 (64) +
	// vec3 camera_position (12) + viewport_width (fills the vec3's 16-byte slot) + 4 uints.
	struct RasterizePushConstant {
		float view_projection_matrix[16]; // Camera-RELATIVE (projection * rotation-only inverse camera).
		float camera_position[3];
		uint32_t viewport_width;
		uint32_t viewport_height;
		uint32_t max_visible;
		uint32_t pad0;
		uint32_t pad1;
	};

	// Must match meshlet_visbuffer_dispatch_args.glsl's Params block (std430): 4 uints (16 bytes).
	struct DispatchArgsPushConstant {
		uint32_t max_count;
		uint32_t divisor; // Consuming shader's local_size_x (1 = one workgroup per item).
		uint32_t pad0;
		uint32_t pad1;
	};

	// Must match meshlet_visbuffer_hw_raster.glsl's Params block (std430, 92 bytes): mat4 (64) +
	// vec3 camera_position (12) + viewport_width (fills the slot) + viewport_height + 2 pads.
	struct HwRasterPushConstant {
		float view_projection_matrix[16]; // Camera-RELATIVE.
		float camera_position[3];
		uint32_t viewport_width;
		uint32_t viewport_height;
		uint32_t pad0;
		uint32_t pad1;
	};

	// Must match meshlet_visbuffer_resolve.glsl's Params block (std430, 128 bytes): mat4 (64) +
	// vec3+uint (16) + 3x vec4 (48).
	struct ResolvePushConstant {
		float view_projection_matrix[16];
		float camera_position[3];
		uint32_t light_count;
		float ambient_color[4]; // .a = sky-radiance mix.
		float svogi_bounds[4]; // xyz = octree center, w = half-size (0 = SVOGI off).
		float svogi_params[4]; // x = energy, y = radiance exposure, z = max-roughness LOD layer.
	};

private:
	// Rasterize shader variants: 0 = int64 visbuffer, 1 = MESHLET_VISBUFFER_FALLBACK (32-bit pair).
	MeshletSoftwareRasterizeShaderRD rasterize_shader;
	RID rasterize_shader_version;
	RID rasterize_pipeline_int64; // Valid only when int64_supported (the int64 variant needs the device feature).
	RID rasterize_pipeline_fallback; // Always valid.

	MeshletVisbufferDispatchArgsShaderRD dispatch_args_shader;
	RID dispatch_args_shader_version;
	RID dispatch_args_shader_rid;
	RID dispatch_args_pipeline;
	RID dispatch_args_buffer; // VkDispatchIndirectCommand {x,y,z}, DISPATCH_INDIRECT usage.

	// Hardware raster into the same visbuffer (large clusters): vertex-pulling + side-effect fragment.
	// Variants mirror the compute rasterizer (0 = int64, 1 = fallback).
	MeshletVisbufferHwRasterShaderRD hw_raster_shader;
	RID hw_raster_shader_version;
	RID hw_raster_pipeline_int64;
	RID hw_raster_pipeline_fallback;
	RD::VertexFormatID hw_vertex_format = RD::INVALID_FORMAT_ID; // Empty format (vertex-pulling, no attributes).
	RID hw_empty_vertex_array;
	RID hw_synthetic_index_buffer; // 0,1,2,... - drives gl_VertexIndex for the vertex-pull.
	RID hw_synthetic_index_array;
	RD::FramebufferFormatID hw_framebuffer_format = RD::INVALID_FORMAT_ID; // Attachment-less.
	RID hw_framebuffer; // Attachment-less, screen-sized; grow-and-reuse.
	Size2i hw_framebuffer_dims;

	// Material-resolve pass (P4): reads the visbuffer, shades, writes out_color.
	MeshletVisbufferResolveShaderRD resolve_shader;
	RID resolve_shader_version;
	RID resolve_pipeline_int64;
	RID resolve_pipeline_fallback;
	RID out_color; // rgba32f storage image, screen-sized; grow-and-reuse.
	Size2i out_color_dims;
	RID resolve_radiance_sampler; // Linear, clamp, mipmapped - for the sky octmap ambient.
	RID resolve_material_sampler; // Linear, repeat, anisotropic - for material textures.

	// Fragment resolve (P5): writes shaded color + gl_FragDepth into a real framebuffer for live
	// compositing. Pipelines are cached per framebuffer format (recreated on change).
	MeshletVisbufferResolveRasterShaderRD resolve_raster_shader;
	RID resolve_raster_shader_version;
	RID resolve_raster_pipeline_int64;
	RID resolve_raster_pipeline_fallback;
	RD::FramebufferFormatID resolve_raster_pipeline_format = RD::INVALID_FORMAT_ID;

	bool int64_supported = false;

	// Visbuffer, grow-and-reuse, sized to the screen. Only the layout matching the last rasterize()
	// mode is allocated (the other layout's RIDs are freed). Single shared buffer - one viewport for
	// now; RenderSceneBuffers-managed per-view allocation is a P5 concern.
	RID visbuffer_u64; // int64 layout.
	RID vis_depth_u32; // fallback layout.
	RID vis_payload_u32; // fallback layout.
	Size2i visbuffer_dims;
	bool visbuffer_is_int64 = false;

	void _ensure_visbuffer(const Size2i &p_screen_size, bool p_int64);

	static MeshletSoftwareRasterizer *singleton;

public:
	static MeshletSoftwareRasterizer *get_singleton();

	MeshletSoftwareRasterizer();
	~MeshletSoftwareRasterizer();

	bool is_int64_supported() const { return int64_supported; }

	// Rasterizes the meshlet list in p_list (its visible_buffer / max_visible) into the visbuffer.
	// p_force_fallback forces the 32-bit path even when int64 is supported (for testing that path on
	// int64 hardware); otherwise int64 is used whenever supported.
	void rasterize(const RendererRD::MeshletCuller::CullResult &p_list, RID p_transforms_buffer, RID p_material_ids_buffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform, bool p_force_fallback = false);

	// Hardware raster of p_list's meshlets into the SAME visbuffer (for the large clusters kept on
	// hardware). p_clear controls whether the visbuffer is cleared first (false to accumulate on top of
	// a prior software-raster pass into the same buffer). Uses the same visbuffer layout / mode as
	// rasterize(); pass p_force_fallback consistently across both if mixing.
	void rasterize_hardware(const RendererRD::MeshletCuller::CullResult &p_list, RID p_transforms_buffer, RID p_material_ids_buffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform, bool p_clear = true, bool p_force_fallback = false);

	void _ensure_hw_framebuffer(const Size2i &p_screen_size);
	void _ensure_out_color(const Size2i &p_screen_size);
	// Appends the alpha-scissor bindings (8: vertex attributes, 9: material ids, 10: materials,
	// 11: material textures, 12: sampler) that both raster shaders declare - so a cutout material can be
	// tested at raster time.
	void _append_alpha_scissor_uniforms(LocalVector<RD::Uniform> &r_uniforms, RID p_material_ids_buffer);

	// Material-resolve pass: reads the current visbuffer (whichever layout the last rasterize()/
	// rasterize_hardware() wrote), shades each covered pixel via meshlet_shade(), writes the lit color
	// to get_out_color(). p_sw_list / p_hw_list map a payload's slot back to (instance, meshlet) - pass
	// whichever list(s) the visbuffer was rastered from (an invalid CullResult binds a harmless stand-in
	// that is never indexed). The shading params mirror MeshletRenderer::render().
	void resolve(const RendererRD::MeshletCuller::CullResult &p_sw_list, const RendererRD::MeshletCuller::CullResult &p_hw_list, RID p_transforms_buffer, RID p_material_ids_buffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform, RID p_lights_buffer, uint32_t p_light_count, const Color &p_ambient_color, float p_sky_mix, RID p_svogi_octree, const Vector3 &p_svogi_center, float p_svogi_half, float p_svogi_energy, RID p_radiance_texture, float p_radiance_exposure, float p_max_roughness_lod);

	RID get_out_color() const { return out_color; }
	Size2i get_out_color_dims() const { return out_color_dims; }

	// Debug: read back the current visbuffer and count non-zero (covered) pixels. GPU stall - debug only.
	uint32_t debug_visbuffer_coverage(const Size2i &p_screen_size);

	void _ensure_resolve_raster_pipeline(RD::FramebufferFormatID p_fb_format);

	// Fragment resolve into an existing color+depth framebuffer (live path): shades the visbuffer,
	// writing color to attachment 0 and the winning reverse-Z depth to gl_FragDepth (depth test
	// GREATER_OR_EQUAL, depth write on), so meshlet geometry lands in the real depth buffer. p_clear
	// clears the framebuffer's color/depth first (test use); false composites onto existing contents.
	void resolve_raster(RID p_target_framebuffer, const RendererRD::MeshletCuller::CullResult &p_sw_list, const RendererRD::MeshletCuller::CullResult &p_hw_list, RID p_transforms_buffer, RID p_material_ids_buffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform, RID p_lights_buffer, uint32_t p_light_count, const Color &p_ambient_color, float p_sky_mix, RID p_svogi_octree, const Vector3 &p_svogi_center, float p_svogi_half, float p_svogi_energy, RID p_radiance_texture, float p_radiance_exposure, float p_max_roughness_lod, bool p_clear);

	// Visbuffer accessors for the material-resolve pass (P4) and self-tests.
	bool visbuffer_is_int64_layout() const { return visbuffer_is_int64; }
	RID get_visbuffer_u64() const { return visbuffer_u64; }
	RID get_vis_depth() const { return vis_depth_u32; }
	RID get_vis_payload() const { return vis_payload_u32; }
	Size2i get_visbuffer_dims() const { return visbuffer_dims; }
};

#endif // MESHLET_SOFTWARE_RASTERIZER_H
