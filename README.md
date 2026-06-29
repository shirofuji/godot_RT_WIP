# Godot Likha Engine

**Likha** is an experimental fork of [Godot Engine](https://godotengine.org) **4.8**
(`4.8.likha`) built around a **GPU-driven, Nanite-style meshlet renderer** for the
Forward+ backend. It tracks upstream Godot and stays a drop-in editor — everything below
about stock Godot still applies — but adds a parallel high-density geometry path.

> ⚠️ **Experimental / work in progress.** The meshlet path is opt-in/gated and has so far
> been validated against the developer's own scenes, not arbitrary projects. Hardware
> floor for the meshlet pipeline is roughly a **GTX 1660 Super**.

## What Likha adds

- **GPU-driven meshlet renderer** — geometry is split into meshlet clusters and drawn with
  GPU frustum culling, backface-**cone** culling, **two-pass temporal Hi-Z occlusion**
  culling, and indirect multi-draw. Gated by `rendering/meshlet/enabled`.
- **Continuous LOD (CLOD)** — a Nanite-style per-surface cluster **DAG** with per-cluster
  screen-space-error LOD-cut data, so the GPU selects a crack-free continuous-LOD subset
  per view in the cull shader. Tuned via `rendering/meshlet/lod_error_threshold_px`.
- **Asynchronous DAG bake** *(new)* — the heavy cluster-DAG bake now runs on a background
  `WorkerThreadPool` thread instead of stalling surface creation. Meshes appear immediately
  at full detail and CLOD engages a beat later when each bake lands, so continuous LOD can
  stay on (`rendering/meshlet/bake_lod_dag`) without the multi-second boot stall.
- **Meshlet PBR materials** — albedo / normal / ORM sampled through a capped texture array,
  with in-shader derived (cotangent-frame) tangents and sky-radiance ambient — no stored
  vertex tangents required.
- **SVOGI ambient fixes** — sparse-voxel GI now *adds* to ambient with a 6-cone
  cosine-weighted gather, fixing pure-black shadows on both the Forward+ and meshlet paths.
- **Large-world precision** — the meshlet render path is camera-relative, matching Forward+
  precision for distant/large-coordinate geometry.

Most meshlet behavior is controlled under **Project Settings → Rendering → Meshlet**.

---

## Upstream: Godot Engine

<p align="center">
  <a href="https://godotengine.org">
    <img src="misc/logo/logo_outlined.svg" width="400" alt="Godot Engine logo">
  </a>
</p>

## 2D and 3D cross-platform game engine

**[Godot Engine](https://godotengine.org) is a feature-packed, cross-platform
game engine to create 2D and 3D games from a unified interface.** It provides a
comprehensive set of [common tools](https://godotengine.org/features), so that
users can focus on making games without having to reinvent the wheel. Games can
be exported with one click to a number of platforms, including the major desktop
platforms (Linux, macOS, Windows), mobile platforms (Android, iOS), as well as
Web-based platforms and [consoles](https://godotengine.org/consoles).

## Free, open source and community-driven

Godot is completely free and open source under the very permissive [MIT license](https://godotengine.org/license).
No strings attached, no royalties, nothing. The users' games are theirs, down
to the last line of engine code. Godot's development is fully independent and
community-driven, empowering users to help shape their engine to match their
expectations. It is supported by the [Godot Foundation](https://godot.foundation/)
not-for-profit.

Before being open sourced in [February 2014](https://github.com/godotengine/godot/commit/0b806ee0fc9097fa7bda7ac0109191c9c5e0a1ac),
Godot had been developed by [Juan Linietsky](https://github.com/reduz) and
[Ariel Manzur](https://github.com/punto-) for several years as an in-house
engine, used to publish several work-for-hire titles.

![Screenshot of a 3D scene in the Godot Engine editor](https://raw.githubusercontent.com/godotengine/godot-design/master/screenshots/editor_tps_demo_1920x1080.jpg)

## Getting the engine

### Binary downloads

Official binaries for the Godot editor and the export templates can be found
[on the Godot website](https://godotengine.org/download).

### Compiling from source

[See the official docs](https://docs.godotengine.org/en/latest/engine_details/development/compiling)
for compilation instructions for every supported platform.

## Community and contributing

Godot is not only an engine but an ever-growing community of users and engine
developers. The main community channels are listed [on the homepage](https://godotengine.org/community).

The best way to get in touch with the core engine developers is to join the
[Godot Contributors Chat](https://chat.godotengine.org).

To get started contributing to the project, see the [contributing guide](CONTRIBUTING.md).
This document also includes guidelines for reporting bugs.

## Documentation and demos

The official documentation is hosted on [Read the Docs](https://docs.godotengine.org).
It is maintained by the Godot community in its own [GitHub repository](https://github.com/godotengine/godot-docs).

The [class reference](https://docs.godotengine.org/en/latest/classes/)
is also accessible from the Godot editor.

We also maintain official demos in their own [GitHub repository](https://github.com/godotengine/godot-demo-projects)
as well as a list of [awesome Godot community resources](https://github.com/godotengine/awesome-godot).

There are also a number of other
[learning resources](https://docs.godotengine.org/en/latest/community/tutorials.html)
provided by the community, such as text and video tutorials, demos, etc.
Consult the [community channels](https://godotengine.org/community)
for more information.

[![Code Triagers Badge](https://www.codetriage.com/godotengine/godot/badges/users.svg)](https://www.codetriage.com/godotengine/godot)
[![Translate on Weblate](https://hosted.weblate.org/widgets/godot-engine/-/godot/svg-badge.svg)](https://hosted.weblate.org/engage/godot-engine/?utm_source=widget)
