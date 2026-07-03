// Meshlet geometry helpers shared across the meshlet pipeline stages (vertex-pulling render, and -
// in the visibility-buffer software rasterizer / material-resolve passes to come). Pure fetch/decode
// utilities with no lighting. The includer MUST declare the `meshlet_triangles` storage buffer
// (packed 4x uint8 triangle-local-vertex-indices per uint32 word, matching MeshletStorage's
// meshlet_triangle_buffer) BEFORE including this file - fetch_triangle_local_vertex() references it
// by name.

vec3 oct_decode_normal(vec2 e) {
	vec3 v = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
	float t = clamp(-v.z, 0.0, 1.0);
	v.x += v.x >= 0.0 ? -t : t;
	v.y += v.y >= 0.0 ? -t : t;
	return normalize(v);
}

uint fetch_triangle_local_vertex(uint p_byte_index) {
	uint word = meshlet_triangles.data[p_byte_index / 4];
	return (word >> ((p_byte_index % 4) * 8)) & 0xFFu;
}
