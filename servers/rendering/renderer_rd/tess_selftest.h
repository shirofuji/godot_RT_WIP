/**************************************************************************/
/*  tess_selftest.h                                                       */
/**************************************************************************/
/* Adaptive-tessellation P0 de-risk: proves the RD tessellation stack     */
/* (glslang tesc/tese -> SPIR-V -> patch pipeline -> Vulkan driver) works  */
/* end-to-end on this hardware before the gdshader-language integration.   */
/* Run headless with a real RenderingDevice via `--tess-selftest`.         */
/**************************************************************************/

#ifndef TESS_SELFTEST_H
#define TESS_SELFTEST_H

// Runs only when `--tess-selftest` is on the command line; otherwise returns immediately.
void run_tess_selftest_if_requested();

// P1 language-hook check (parser-level, no RenderingDevice needed): verifies the gdshader front-end
// accepts `render_mode tessellation_adaptive;` and the `displacement()` processor with its built-ins,
// and rejects the expected error cases. Runs only when `--tess-shader-selftest` is on the command line.
void run_tess_shader_selftest_if_requested();

#endif // TESS_SELFTEST_H
