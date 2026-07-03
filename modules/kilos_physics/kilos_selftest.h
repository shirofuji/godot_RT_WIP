#ifndef KILOS_SELFTEST_H
#define KILOS_SELFTEST_H

// Verification path for the GPU physics substrate that runs inside real engine
// startup (where a real RD-backed RenderingDevice exists), mirroring the
// meshlet selftest. Godot's --test doctest harness only ever brings up the
// dummy rasterizer, so RD-dependent code cannot be exercised there.
//
// Gated behind a command-line flag so it is a no-op in normal use:
//   godot.exe --path .selftest_project --quit --kilos-selftest
//
// NOTE: a REAL RenderingDevice is required. In this fork --headless brings up
// the dummy rasterizer (RD::get_singleton() == nullptr), and launching with no
// project doesn't initialize the RD renderer either, so the test must run
// against an actual project (which brings up the Vulkan RD).
//
// Prints "KILOS_SELFTEST: PASS/FAIL: <reason>" lines so results are greppable.
void run_kilos_selftest_if_requested();

#endif // KILOS_SELFTEST_H
