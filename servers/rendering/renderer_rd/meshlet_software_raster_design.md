# Meshlet software rasterizer — visibility-buffer design (Option C)

Status: **P0 complete & verified** (2026-07-03). Architecture confirmed: **full visbuffer**.
Int64-atomic **fallback required** (see Risks). P0 (shared-shading-include extraction) verified
behavior-neutral (real-scene A/B clean per user). **P1 (size classifier + split worklist) DONE &
VERIFIED** — see below. **P2 IN PROGRESS.**

Decision (user, 2026-07-03): Godot does NOT enable int64 buffer atomics (no
`VK_KHR_shader_atomic_int64` anywhere in `drivers/`, no `Features` query) → chose to **add int64
backend support first** rather than ship 32-bit-only. Backend wiring done (see P2 below); the
visbuffer will use int64 primary + 32-bit fallback selected from `SUPPORTS_BUFFER_ATOMIC_INT64`.

### P2a — int64-atomic backend enablement (RenderingDevice) — DONE & VERIFIED (2026-07-03)
Added `RDD::Features::SUPPORTS_BUFFER_ATOMIC_INT64` (rendering_device_commons.h). Vulkan driver
(rendering_device_driver_vulkan.{h,cpp}): register `VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME`
(optional), query `shaderBufferInt64Atomics` (from `VkPhysicalDeviceVulkan12Features` on 1.2+, else
the `VkPhysicalDeviceShaderAtomicInt64Features` KHR struct), store `shader_atomic_int64_support`,
enable it in `_initialize_device`'s create-info pNext chain, and return it from `has_feature()`.
`RenderingDevice::has_feature` already delegates unknown features to the driver; Metal/D3D12
`has_feature` `default: return false`, so they report unsupported (correct). Selftest prints
`SUPPORTS_BUFFER_ATOMIC_INT64 = true/false` to confirm it turns on for the GTX 1660 Super.
**Verified**: GTX 1660 Super reports `= true`, device init clean (no validation errors), no meshlet
regression. Build gotcha: a `timeout`-killed selftest can leave a `godot` process holding
`bin/…exe`, so the next link fails with "Access is denied" (+ a secondary scons atexit
`AttributeError` in methods.py purge_flaky_files) - kill stray godot processes before rebuilding.

### P2b — software raster into the int64 visbuffer — DONE & VERIFIED (2026-07-03)
Rewrote meshlet_software_rasterize.glsl: one workgroup per software-visible meshlet, one thread per
triangle, edge-function scan-convert, `atomicMax` a `uint64` visbuffer packed `depth<<32 |
(slot&0x1FFFFFF)<<7 | (tri&0x7F)`. Two variants: default int64 (`GL_EXT_shader_atomic_int64`),
`MESHLET_VISBUFFER_FALLBACK` = two uint32 buffers (depth atomicMax + payload with a tie re-check).
No backface cull (atomicMax picks nearest → Z-fight-free for closed meshes; matching hardware
CULL_BACK is a P5 concern). Hard bbox-area cap (4096 px) as the large-triangle safety net. New
meshlet_visbuffer_dispatch_args.glsl (1 invocation) reads the GPU-side software count → writes
`{count,1,1}` for `compute_list_dispatch_indirect` (no 4M over-dispatch). Reuses
meshlet_geometry_inc.glsl (fetch_triangle_local_vertex).

MeshletSoftwareRasterizer fully rewritten (was the dead depth-only prototype): picks int64 vs
fallback from `RD::SUPPORTS_BUFFER_ATOMIC_INT64` (builds the int64 pipeline only when supported),
owns the grow-and-reuse visbuffer (int64: one uint64 buffer; fallback: two uint32), `rasterize(list,
transforms, screen, proj, cam, force_fallback)` = clear → build args → indirect dispatch, uniform
sets cached via UniformSetCacheRD. Accessors expose the visbuffer for P4. Old blit path removed
(meshlet_depth_blit.glsl now unused-but-harmless).

**Verified** by `test_meshlet_visbuffer_rasterize` (--meshlet-selftest): sphere forced entirely to
the software list, rasterized both ways; int64 + fallback each cover a meaningful pixel count with
valid (slot,tri) payloads and an empty corner, AND the fallback covers EXACTLY the same pixels as
int64 (coverage cross-check). Int64 shader compiles clean at runtime (device feature enabled). Only
the pre-existing center-pixel known-fail remains.

### P3 — hardware raster into the same visbuffer — DONE & VERIFIED (2026-07-04)
New meshlet_visbuffer_hw_raster.glsl: vertex-pulling vertex stage (same buffers 0-5 as the compute
rasterizer, absolute `view_projection` matching it) + a **side-effect fragment** that atomicMax-es
the same packed (depth, slot, tri) into the visbuffer (bindings 6 int64 / 6+7 fallback). Depth test
OFF, no cull, zero color attachments (`SUPPORTS_FRAGMENT_SHADER_WITH_ONLY_SIDE_EFFECTS`, true on
Vulkan). MeshletSoftwareRasterizer gained `rasterize_hardware(list, transforms, screen, proj, cam,
clear, force_fallback)`: attachment-less framebuffer (`framebuffer_create_empty`, grow-and-reuse,
mirrors cluster_builder_rd), its own vertex-pull empty-vertex-array + synthetic index array + int64/
fallback render pipelines, reuses MeshletCuller::emit_indirect_draws + draw_indirect_count. `p_clear`
= false lets it accumulate on top of a prior software pass into the same buffer (the P5 mixed frame).
**Verified** by test_meshlet_visbuffer_hardware_raster: hw side-effect fragment covers meaningful
pixels with valid payloads + empty corner, AND hw coverage matches the software rasterizer within
fill-rule tolerance (0.85-1.18x). NB: the hw visbuffer path uses NO cull, so it covers the sphere
center that the hardware COLOR path's CULL_FRONT/winding bug misses (the known center-pixel selftest
fail) - a hint the visbuffer path sidesteps that bug.

OPEN payload-disambiguation (P4): both hw and sw fragments store `slot` into THEIR OWN list, so a
shared visbuffer pixel's payload is ambiguous about which list. In P2b/P3 each is tested alone. P4
resolve must disambiguate - e.g. a 1-bit hw/sw flag in the payload (slot to 24 bits) + read the
matching list, OR unify into one visible-meshlet buffer.

### P4 — material-resolve compute pass — NEXT
One thread/pixel: unpack (slot, tri) [+ hw/sw], refetch the triangle's 3 verts, recompute
barycentrics + ANALYTIC gradients (no dFdx in compute - see P0 note), interpolate, shade via
meshlet_shade() (P0), write color + depth to the real targets. Then P5 live integration (+ route
sw/hw through occlude(), retire the color render() path and its CULL_BACK/depth-bias hacks).

## P0 result (files added)
- `shaders/meshlet_geometry_inc.glsl` — oct_decode_normal + fetch_triangle_local_vertex.
- `shaders/meshlet_shade_types_inc.glsl` — MeshletMaterial/MeshletLight/SVOGINode structs + M_PI,
  MESHLET_TEXTURE_NONE.
- `shaders/meshlet_shade_inc.glsl` — all BRDF/light/SVOGI/ambient functions + stage-agnostic
  `meshlet_shade(...)` (takes former varyings/push-constant fields as params; reports alpha-scissor
  via `out bool r_discard` since compute can't discard). perturb_normal still uses dFdx/dFdy →
  FRAGMENT-ONLY; P4 must feed analytic triangle gradients.
- `shaders/meshlet_render.glsl` — vertex includes geometry; fragment declares bindings 8-14 then
  includes shading and calls meshlet_shade(). Pure extraction, math unchanged.
Run to verify: `godot --rendering-driver vulkan --rendering-method forward_plus --meshlet-selftest`
(the meshlet singletons only exist under the Vulkan/RD backend; a plain run falls back to GL
Compatibility and every singleton check trivially fails).

## Problem

The hardware meshlet path has no small-triangle handling. Hardware rasterizers shade in fixed 2×2
quads, so a subpixel triangle wastes ~4× (or worse) of its shading throughput. There is **no other
subpixel/small-triangle path in the fork** — the existing `MeshletSoftwareRasterizer`
(`forward_clustered/meshlet_software_rasterizer.cpp`) is a **dead, depth-only prototype** with no
call site: it `atomicMax`es a uint32 depth buffer and produces no color/material, so wiring it up
as-is would only duplicate the hardware depth pass, not shade anything.

Goal: route small (subpixel–few-pixel) clusters to a compute rasterizer, keep large triangles on
hardware, and shade both through one pass so output matches today's color exactly.

Non-goals: transparency, MSAA, skeletal/particle meshlets, SVOGI voxelize changes.

## Architecture decision (pending user confirmation)

- **Full visbuffer (recommended):** both HW and SW raster write one visibility buffer (packed
  depth+ID); a single **material-resolve compute pass** shades the whole buffer, replacing the
  current color `MeshletRenderer::render()`. Cleanest, and it retires the per-instance cull-mode /
  winding / manual-depth-bias workarounds in `meshlet_renderer.cpp::_ensure_pipeline` and
  `meshlet_render.glsl` (visbuffer edge-function raster is exact and per-triangle).
- **Hybrid (alternative):** keep the HW color pass for large tris, add SW only for small ones. Less
  invasive but maintains two shading paths that must stay pixel-identical + depth-consistent.

This document assumes **full visbuffer**.

## Visbuffer format (stays within the GTX 1660 Super floor)

Use a **storage-buffer** visbuffer (`width*height` × `uint64`), NOT an image — this lets us use
`VK_KHR_shader_atomic_int64` **buffer** atomics (supported on Turing) and avoids
`VK_EXT_shader_image_atomic_int64` (not reliable on the floor). Continuous with the existing
storage-buffer depth prototype.

Reverse-Z packing (larger = nearer, `atomicMax` keeps nearest):

```
uint64 entry = (uint64(floatBitsToUint(ndc_z)) << 32) | (sw_slot << 7) | tri_id;
//              [63:32] depth (orders the atomic)      [31:7] slot   [6:0] tri
```

`MAX_TRIANGLES_PER_MESHLET = 124` → 7 bits exactly (0..123); 25 bits left for the visible-meshlet
slot (~33M, over `MESHLET_LIVE_CAPACITY` = 4M).

## Phases

### P0 — Extract shared shading (behavior-neutral; de-risks everything)
Pull the fragment body of `meshlet_render.glsl` (`light_compute` / `light_process_*` / `svogi_basis`
/ ambient, ~lines 233–660) into `meshlet_shade_inc.glsl`, parameterized on interpolated inputs
(world pos/normal/uv, material id) instead of `in` varyings. Fragment shader `#include`s it and
renders identically. Also factor the vertex-attribute fetch+interpolate (vertex shader ~100–231)
into a shared helper the resolve pass reuses. **Checkpoint: build + visual A/B must be
pixel-identical before continuing.** This phase is load-bearing — every later phase inherits any
drift here.

### P1 — Size classifier + split worklist (`meshlet_cull.glsl`) — DONE & VERIFIED (2026-07-03)
After the frustum test, compute `screen_px = world_radius * projection_scale / dist` and route:
below `sw_cluster_px` → software worklist (binding 5), else → hardware worklist. Both bounded by
`max_visible` (software list shares hw capacity, keeping the push constant at the 128-byte Vulkan
floor - reused the old `pad2` slot for `sw_cluster_px`). Plumbed through `MeshletCuller::cull()`
(new trailing `p_sw_cluster_px` arg, default 0 = split off = today's behavior; `CullResult` gained
`sw_visible_buffer`/`sw_max_visible` + `has_software()`; grow-and-reuse `sw_visible_buffer`, sized 1
when off). Diagnostic: `--meshlet-swraster-diag` (optional `--meshlet-swraster-px=N`, default 8)
prints `MESHLET_SWRASTER_DIAG: hw=… sw=…` from the frustum-cull output each late pass - watch entries
migrate hw→sw as the camera pulls back. **Verified** by 5 new `--meshlet-selftest` checks: with a
huge threshold every survivor routes to software (hw list empty) and the software list equals the
split-off baseline set exactly (no loss/dup/false-positive) - isolates the classifier from the LOD
cut by using identical LOD params in both culls. Split-off path unchanged (only the known center
pixel still fails).
NOTE: the split currently happens at the *frustum* cull; `occlude()` still runs on the hardware list
only, so the software list bypasses occlusion. Fine while the sw path isn't rendered (default off);
P2/P3 must route the sw list through occlusion (or move the split post-occlusion) before enabling.

### P2 — Software raster into the visbuffer (rewrite `meshlet_software_rasterize.glsl`)
Change the depth-only `atomicMax` to the 64-bit packed `atomicMax`, reading the software worklist.
Keep one-workgroup-per-meshlet / one-thread-per-triangle (correct as-is). Hard pixel-area cap on the
inner bbox loop as a safety net for a stray large tri. While here: cache uniform sets, dispatch the
real (indirect) count not the 4M capacity. **Verify `VK_KHR_shader_atomic_int64` at init before
this phase.**

### P3 — Hardware raster into the same visbuffer
Render the hardware worklist with a trivial fragment shader doing the same packed `atomicMax` into
the visbuffer (depth-test off; the atomic is the depth test). Reuses the existing
indirect-draw/vertex-pulling `render()` plumbing — swap color output for visbuffer write. Both paths
now populate one coherent visbuffer.

### P4 — Material-resolve compute pass (new shader + `MeshletResolver`)
One thread per pixel: unpack `(slot, tri_id)`, refetch the triangle's 3 vertices, recompute
barycentrics, interpolate (P0 helper), shade (P0 include), write **color** to the color target and
**depth** to the real depth buffer (so sky/transparents composite correctly). Binds the same
material/lights/SVOGI/radiance resources as `MeshletRenderer::render()` today.

### P5 — Integrate, gate, retire hacks
In `_render_meshlet_late_pass`, replace the `render()` color call with sw-raster → hw-raster →
resolve. Gate behind `rendering/meshlet/software_raster` (default off) for A/B. Once validated,
delete the dead `MeshletSoftwareRasterizer` prototype and remove the `CULL_BACK` / depth-bias
workarounds.

### P6 — Perf + correctness
Indirect dispatch everywhere (kills the 4M over-dispatch, worse now with more passes), visbuffer
VRAM sizing, and a `--meshlet-selftest` subpixel case (known geometry → known visbuffer).

## Risks
- **64-bit buffer atomics on the 1660 Super** — research indicates `VK_KHR_shader_atomic_int64`
  (`shaderBufferInt64Atomics`) is available on the target hardware, so it's the primary path. A
  **fallback is required** (per direction), selected at init from the device capability:
  - *Primary:* single `uint64` visbuffer, packed `depth<<32 | payload`, one `atomicMax`.
  - *Fallback (no int64 buffer atomics):* two parallel 32-bit buffers — a `uint` depth buffer
    (`atomicMax` on `floatBitsToUint(ndc_z)`) and a `uint` payload buffer. Race resolution on exact
    depth ties: after the depth `atomicMax`, re-read depth and only write payload if it still equals
    our value (benign flicker on true ties; acceptable). Both raster shaders (`#define`-select the
    path) and the resolve pass read whichever layout is active. Gate via a shader variant +
    a `MeshletVisbuffer::use_int64` flag set from `RD::get_device_capabilities()`.
- **Resolve-pass divergence** — neighboring pixels on different materials/meshlets → incoherent
  fetches. Acceptable first cut; per-material tile/bin pass is a later optimization.
- **P0 correctness** — if the shared-include refactor isn't provably identical, all later phases
  inherit the drift. Don't skip the A/B.

## Build gotcha (learned in P0)
The RD shader header builder scans for `#include "..."` with a naive regex that does **not** skip
comments. Do NOT write the literal token `#include "` inside a `//` comment in any `.glsl` /
`_inc.glsl` file — it's parsed as a real include directive and fails the build with "Invalid
argument". Reword (e.g. "include foo_inc.glsl") in prose.

## Effort shape
P0+P1 = de-risking half (small, verifiable). P2–P4 = core build. P5–P6 = integration/cleanup. Own
branch, multi-session.
