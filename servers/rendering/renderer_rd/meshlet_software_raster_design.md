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

### P4 — material-resolve compute pass — DONE & VERIFIED (2026-07-04)
New meshlet_visbuffer_resolve.glsl (8x8, int64 + fallback variants): per pixel reads the visbuffer,
unpacks payload (bit31 hw/sw, 24-bit slot, 7-bit tri), reads the matching visible list ->
(instance, meshlet), refetches the triangle's 3 verts, computes perspective-correct barycentrics +
ANALYTIC screen-space gradients (perspective-correct interp at p, p+x, p+y - no dFdx), interpolates
world pos/normal/uv, and shades via the shared meshlet_shade() -> imageStore into an rgba32f
out_color (uncovered pixels left untouched for compositing). Full shading bindings (materials,
lights, svogi, radiance, material textures) so it's P5-ready; push constant is 128 bytes.
Prereqs done first (behavior-neutral, separately built/verified): payload gained a bit31 hw/sw source
flag (slot 25->24 bit) in BOTH raster shaders; perturb_normal -> perturb_normal_grad (takes gradients
as params, no dFdx) so meshlet_shade() is now stage-agnostic - the fragment passes dFdx/dFdy, the
resolve passes analytic gradients (meshlet_render.glsl updated, still pixel-identical).
MeshletSoftwareRasterizer gained resolve() (mirrors MeshletRenderer::render's shading params) +
out_color (grow-reuse) + 2 samplers. **Verified** by test_meshlet_visbuffer_resolve: rasterize sw ->
resolve -> shaded pixel count EQUALS visbuffer coverage exactly, colors in [0,1], corner unshaded.

### P5 — live integration — IN PROGRESS
- **P5a fragment resolve — DONE & VERIFIED (2026-07-04).** Extracted the per-pixel resolve into
  meshlet_visbuffer_resolve_inc.glsl (resolve_visbuffer_pixel -> out color + reverse-Z depth);
  compute resolve (P4) now uses it (re-verified). New meshlet_visbuffer_resolve_raster.glsl: fullscreen
  triangle vertex + fragment that calls it and writes frag_color + gl_FragDepth (depth test
  GREATER_OR_EQUAL, depth write on) - so meshlet geometry lands in the REAL depth buffer for compositing
  (compute+imageStore can't write a depth attachment). Viewport dims packed into svogi_params.w to keep
  the push constant at 128 bytes. MeshletSoftwareRasterizer::resolve_raster(target_fb, ...) draws it into
  a caller framebuffer (pipeline cached per fb format; INVALID_FORMAT_ID vertex format = procedural, no
  vertex array). Verified by test_meshlet_visbuffer_resolve (raster block): shades a real color+depth
  framebuffer, coverage == visbuffer coverage exactly. GOTCHA: a procedural fullscreen draw needs the
  pipeline's vertex format = INVALID_FORMAT_ID, NOT an empty vertex format (else "No vertex array bound").

- **P5c late-pass wiring — DONE (gated, smoke-tested live; visual A/B pending user) 2026-07-04.**
  Gate: `--meshlet-software-raster` (default OFF). When on, _render_meshlet_late_pass branches at the
  render() call: rasterize frustum_result's sw list (compute, clears visbuffer) + hw list (draw,
  accumulate) -> resolve_raster composites color+depth into p_color_only_framebuffer, skipping the
  color render(). Reuses the render() path's exact shading params. `--meshlet-swraster-px=N` tunes the
  split (1e9 = all software). Occlusion SKIPPED for now (still correct - atomicMax; occlusion = P5b
  perf). Smoke-tested on neesan/meshlet_pbr_test.tscn: both paths run 30-40 frames, no crashes, no
  Vulkan validation errors (8px -> hw=37/sw=0; 1e5px -> hw=0/sw=37). Not yet visually A/B'd (needs the
  user to look). Default-off path unregressed (--meshlet-selftest still only the known center fail).
  NB: resolve_raster assumes p_color_only_framebuffer has exactly 1 color attachment + depth (same as
  render()'s pipeline) - holds for the common Forward+ opaque target.

- **P5c HOLES — REAL FIX = backface cull (2026-07-04, confirmed clean by user).** The holes were
  BACK FACES winning the visbuffer depth in patches. The rasterizers ran with NO cull ("atomicMax
  keeps nearest") - but with both front+back faces written, a back face won in blocky patches and
  shaded as the dark mesh interior -> looked like holes. Debug proof: `--meshlet-visbuffer-debug=1`
  (coverage) = solid disc (coverage 100%), `=2` (normals) = smooth gradient with blocky WRONG-facing
  patches. Fix: cull back faces like render() does - `POLYGON_CULL_BACK` on the hardware raster
  pipeline + a signed-area test in the software shader (front = positive area in y-down screen space;
  cull <= 0). Coverage unchanged after (49,676, so it culled back not front). Debug viz modes kept
  (packed into light_count top 4 bits; --meshlet-visbuffer-debug=N: 1 coverage, 2 normal, 3 slot-hash,
  4 depth-gray, 5 front/back-facing). `--meshlet-swraster-diag` also prints MESHLET_VISBUFFER_COV
  (covered px). SOFTWARE-path cull sign not yet visually confirmed (coverage can't tell front-cull
  from back-cull for a sphere - both project to the disc); if px=1e9 looks inverted, flip the sw
  `<= 0` to `>= 0`.
- **P5c depth-conflict fix (2026-07-04, also needed):** first live A/B showed the mesh full of holes (sw=0 = all hardware,
  so it was the hw path). Cause: the meshlet EARLY depth pass writes meshlet depth with camera-relative
  projection; the visbuffer resolve writes gl_FragDepth with absolute projection; resolve_raster's
  GREATER_OR_EQUAL test against that early depth fails on ~half the covered pixels (float precision
  gap) -> they keep the background -> holes. (The P5a selftest passed because it used p_clear=true =
  test against far; the live path uses p_clear=false = composite.) Fix: `_meshlet_early_pass_should_engage`
  returns false when --meshlet-software-raster is on, so plain INSTANCE_MESH meshlets (already skipped
  from the Forward+ depth pre-pass by the per-surface filter) have NO pre-existing depth -> resolve
  always wins against the far-cleared background, exactly like the normal render() path. NB: MULTIMESH
  meshlets still stay in the depth pre-pass (bit-31 clear + depth-bias band-aid), so flora could still
  show holes under software raster - handle by also skipping multimesh from the pre-pass when the gate
  is on (follow-up; test scene is plain meshes). Real fix for both = P5d camera-relative projection.

- **P5d camera-relative projection — DONE & VERIFIED (2026-07-04).** Switched all three visbuffer
  rasterizers + the resolve from absolute (`view_projection * world_pos`) to camera-relative
  (`(projection * rotation-only-inverse-camera) * (world_pos - camera_position)`), matching
  meshlet_render.glsl/Forward+. Same NDC, small operands -> no float32 cancellation, so no depth
  flicker for geometry far from the world origin, and the resolve's gl_FragDepth now matches Forward+'s
  depth convention -> MultiMesh flora (which stays in the depth pre-pass with Forward+ camera-relative
  depth) composites correctly under software raster too. C++ helper `_meshlet_camera_relative_vp()`
  used by rasterize/rasterize_hardware/resolve/resolve_raster; camera_position added to the sw (96B)
  and hw (92B) push constants (resolve already had it). Selftest unchanged (NDC-identical, resolve
  coverage still == visbuffer coverage exactly); neesan smoke stable. NOTE: with camera-relative the
  early-pass depth conflict would no longer occur, but the early pass stays disabled under software
  raster anyway (occlusion is skipped) - harmless.

## Perf findings (2026-07-04, user A/B in real scene) — parked, revisit later
Correctness DONE: renders correctly incl. MultiMesh flora, no holes. Perf: NO FPS change (1-2 fps).
Why: (1) the test scene is CPU-bound - `process` (animal AI) ~163ms/frame dominates; the GPU/render
(cpu-render ~66ms + GPU) runs on the render thread IN PARALLEL and is fully hidden under the AI wall,
so no GPU change can move FPS. (2) At the default view sw=0 (no subpixel clusters), so the visbuffer
path is strictly MORE work than render() (raster-into-visbuffer + fullscreen resolve) for zero
benefit - it can only help where sw>0 (distant/dense geometry). So the feature is correct but can't
show a win under these conditions (GPU isn't the bottleneck; no subpixel tris).
OPEN perf items (parked, user said keep in mind):
- **sw≈0 fallback gate**: when the subpixel fraction is ~0, skip the whole visbuffer path and use the
  normal render(), skipping the resolve - makes --meshlet-software-raster free when it can't help
  (removes the 1-2 fps overhead). The right "kicks in only when it helps" productionization.
- **gpu(render) counter corruption**: with --meshlet-software-raster ON, the perf overlay's gpu-render
  timestamp reads garbage (~1.78e12 ns; sane ~300 with it off). Measurement artifact only (rendering
  is correct). My extra GPU passes disrupt the frame's timestamp span read; NOT a RENDER_TIMESTAMP
  count mismatch (the late pass adds none). Needs a debug iteration to pin; use Nsight/RenderDoc to
  profile sw>0 meanwhile. Blocks trustworthy in-engine GPU-time profiling of the sw path.
- Note: the real FPS lever for this scene is the AI (process=163ms), unrelated to this meshlet work.

STILL TODO (needs the live scene, user A/B):
- **P5b occlusion routing**: occlude BOTH lists before raster (currently skipped in the gated path -
  correct but not perf-optimal). occlude() uses one shared occluded_buffer - needs a 2nd, or occlude
  both in one call.
- **P5d**: camera-relative projection (P2b/P3/P4/P5a use absolute - fine standalone; revisit for
  large-world precision, matching meshlet_render.glsl).
- **P5e**: once validated, retire the color render() path + its CULL_BACK/depth-bias hacks; consider the
  visbuffer moving into RenderSceneBuffers (per-view, resize-managed) vs the current single shared buffer.

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
