/**************************************************************************/
/*  meshlet_software_rasterizer.h                                        */
/**************************************************************************/

#ifndef MESHLET_SOFTWARE_RASTERIZER_H
#define MESHLET_SOFTWARE_RASTERIZER_H

#include "servers/rendering/rendering_device.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_software_rasterize.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_visbuffer_dispatch_args.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_visbuffer_hw_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/meshlet_culler.h"

// Visibility-buffer software rasterizer (P2b). Scan-converts the small/subpixel clusters that P1's
// classifier routed to the software worklist into a visibility buffer via atomicMax - the nearest
// surface's packed (depth, slot, triangle) wins per pixel. The material-resolve pass (P4) reads the
// visbuffer and shades. Two paths, selected from RD::SUPPORTS_BUFFER_ATOMIC_INT64: a primary uint64
// visbuffer (one atomicMax), and a 32-bit fallback (separate depth + payload buffers).
class MeshletSoftwareRasterizer {
public:
	// Must match meshlet_software_rasterize.glsl's Params block exactly (std430): mat4 (64) + 4 uints
	// (16) = 80 bytes.
	struct RasterizePushConstant {
		float view_projection_matrix[16];
		uint32_t viewport_width;
		uint32_t viewport_height;
		uint32_t max_visible;
		uint32_t pad;
	};

	// Must match meshlet_visbuffer_dispatch_args.glsl's Params block (std430): 4 uints (16 bytes).
	struct DispatchArgsPushConstant {
		uint32_t max_visible;
		uint32_t pad0;
		uint32_t pad1;
		uint32_t pad2;
	};

	// Must match meshlet_visbuffer_hw_raster.glsl's Params block (std430): mat4 (64) + 4 uints (16).
	struct HwRasterPushConstant {
		float view_projection_matrix[16];
		uint32_t viewport_width;
		uint32_t viewport_height;
		uint32_t pad0;
		uint32_t pad1;
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
	void rasterize(const RendererRD::MeshletCuller::CullResult &p_list, RID p_transforms_buffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform, bool p_force_fallback = false);

	// Hardware raster of p_list's meshlets into the SAME visbuffer (for the large clusters kept on
	// hardware). p_clear controls whether the visbuffer is cleared first (false to accumulate on top of
	// a prior software-raster pass into the same buffer). Uses the same visbuffer layout / mode as
	// rasterize(); pass p_force_fallback consistently across both if mixing.
	void rasterize_hardware(const RendererRD::MeshletCuller::CullResult &p_list, RID p_transforms_buffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform, bool p_clear = true, bool p_force_fallback = false);

	void _ensure_hw_framebuffer(const Size2i &p_screen_size);

	// Visbuffer accessors for the material-resolve pass (P4) and self-tests.
	bool visbuffer_is_int64_layout() const { return visbuffer_is_int64; }
	RID get_visbuffer_u64() const { return visbuffer_u64; }
	RID get_vis_depth() const { return vis_depth_u32; }
	RID get_vis_payload() const { return vis_payload_u32; }
	Size2i get_visbuffer_dims() const { return visbuffer_dims; }
};

#endif // MESHLET_SOFTWARE_RASTERIZER_H
