#[compute]

#version 450

#VERSION_DEFINES

// Builds indirect dispatch arguments from a GPU-side atomic count, so a pass runs exactly the real
// number of items instead of a fixed multi-million capacity. Reads the leading uint (the count) of any
// { uint count; ... } list buffer (clamped to its capacity, since the atomic can overrun it) and writes
// {groupCountX = ceil(min(count,cap)/divisor), 1, 1}. divisor = the consuming shader's local_size_x
// (use 1 for a one-workgroup-per-item pass, or e.g. 64 to reproduce dispatch_threads(count)). One
// invocation total.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer CountSource {
	uint count; // Any list buffer: only the leading count word is read.
}
count_source;

layout(set = 0, binding = 1, std430) restrict writeonly buffer DispatchArgs {
	uint x;
	uint y;
	uint z;
}
dispatch_args;

layout(push_constant, std430) uniform Params {
	uint max_count; // Buffer capacity - clamp the (possibly overrun) atomic count to it.
	uint divisor; // Consuming shader's local_size_x.
	uint pad0;
	uint pad1;
}
params;

void main() {
	uint c = min(count_source.count, params.max_count);
	uint div = max(params.divisor, 1u);
	dispatch_args.x = (c + div - 1u) / div; // ceil(c / divisor).
	dispatch_args.y = 1u;
	dispatch_args.z = 1u;
}
