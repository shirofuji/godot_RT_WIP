/**************************************************************************/
/*  meshlet_software_rasterizer.h                                        */
/**************************************************************************/

#ifndef MESHLET_SOFTWARE_RASTERIZER_H
#define MESHLET_SOFTWARE_RASTERIZER_H

#include "servers/rendering/rendering_device.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_software_rasterize.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/meshlet_depth_blit.glsl.gen.h"
#include "servers/rendering/renderer_rd/meshlet_culler.h"

// Must match meshlet_software_rasterize.glsl's push_constant block exactly (std430): mat4 (64) +
// vec2 (8) = 72 bytes. No trailing padding - RenderingDevice validates the supplied push-constant
// size against the shader's declared size and rejects a mismatch (an earlier extra float[2] pad
// made this 80 and the dispatch's push constant silently never got set).
struct MeshletSoftwareRasterizePushConstant {
	float view_projection_matrix[16];
	float viewport_size[2];
};

// Must match meshlet_depth_blit.glsl's Params block exactly (std430): a single int = 4 bytes. No
// padding, same RD size-validation reason as above (an earlier uint32_t[3] pad made this 16).
struct MeshletDepthBlitPushConstant {
	uint32_t width;
};

class MeshletSoftwareRasterizer {
private:
	MeshletSoftwareRasterizeShaderRD rasterize_shader;
	RID rasterize_shader_version;
	RID rasterize_shader_rid; // The actual shader RID (NOT the version RID) - uniform_set_create() needs this.
	RID rasterize_pipeline;

	MeshletDepthBlitShaderRD blit_shader;
	RID blit_shader_version;
	RID blit_shader_rid; // The actual shader RID (NOT the version RID) - uniform_set_create() needs this.
	RID blit_pipeline;
	RD::FramebufferFormatID blit_framebuffer_format = RD::INVALID_FORMAT_ID;

	RID depth_buffer;
	uint32_t depth_buffer_size = 0;

	static MeshletSoftwareRasterizer *singleton;

public:
	static MeshletSoftwareRasterizer *get_singleton();

	MeshletSoftwareRasterizer();
	~MeshletSoftwareRasterizer();

	RID get_rasterize_pipeline() const { return rasterize_pipeline; }
	
	RID get_blit_pipeline(RD::FramebufferFormatID p_framebuffer_format);

	void dispatch(RendererRD::MeshletCuller::CullResult p_cull_result, RID p_transforms_buffer, RID p_depth_framebuffer, const Size2i &p_screen_size, const Projection &p_projection, const Transform3D &p_camera_transform);
};

#endif // MESHLET_SOFTWARE_RASTERIZER_H
