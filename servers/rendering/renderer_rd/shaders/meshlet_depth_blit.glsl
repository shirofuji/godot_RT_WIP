#[vertex]
#version 450
#VERSION_DEFINES

layout(location = 0) out vec2 uv_interp;

void main() {
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
	uv_interp = uv;
}

#[fragment]
#version 450
#VERSION_DEFINES

layout(location = 0) in vec2 uv_interp;

layout(set = 0, binding = 0, std430) restrict readonly buffer DepthBuffer {
	uint data[];
} depth_buffer;

layout(push_constant, std430) uniform Params {
	int width;
} params;

void main() {
	ivec2 pixel_coord = ivec2(gl_FragCoord.xy);
	int pixel_index = pixel_coord.y * params.width + pixel_coord.x;
	
	uint z_uint = depth_buffer.data[pixel_index];
	
	if (z_uint == 0u) {
		discard;
	}
	
	gl_FragDepth = uintBitsToFloat(z_uint);
}
