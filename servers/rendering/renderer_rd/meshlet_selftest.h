/**************************************************************************/
/*  meshlet_selftest.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

// Godot's own --test doctest harness (tests/test_main.cpp) always brings up RasterizerDummy
// (see Main::test_setup() in main.cpp) and never a real RenderingDevice - so RD-dependent code
// (MeshletStorage, MeshletCuller) is structurally untestable there; every "GPU round-trip" check
// written against that harness silently no-ops via its own early-return guard instead of failing.
//
// This is a real, separate verification path that runs inside the actual engine startup (where a
// real RD-backed RendererCompositorRD does exist), gated behind a command-line flag so it's a
// no-op in normal use: `godot.exe --headless --quit --meshlet-selftest`. Called once from
// RendererCompositorRD's constructor, after MeshletStorage/MeshletCuller both exist.
//
// Prints "MESHLET_SELFTEST: PASS" or "MESHLET_SELFTEST: FAIL: <reason>" lines to stdout for each
// check, so results are greppable from a captured run.
void run_meshlet_selftest_if_requested();
