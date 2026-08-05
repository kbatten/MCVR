# CLAUDE.md — MCVR (native C++ Vulkan ray-tracing renderer)

## What this repo is

MCVR (Minecraft Vulkan Renderer) is the **native half** of Radiance: a C++ Vulkan
**hardware ray-tracing** render framework. It builds a shared library (`core.dll` on
Windows) that the Radiance Fabric mod loads and drives over JNI.

The **Java half** is the separate **Radiance** repo (`/Users/keith/src/Radiance`). It hooks
Minecraft's render loop and forwards geometry/textures/uniforms here. Develop the two
together — see `../Radiance/CLAUDE.md`.

## Current work: the 26.2 port

- Working branch: **`main.26_2`** (matches Radiance's `main.26_2`); reference branch `main`
  is the working 1.21.x version.
- Most of the native rendering is **version-independent** — the port churn is mostly on the
  Radiance/Java side. Native changes here are targeted fixes (sync, capture plumbing,
  emission, sky).
- Remaining native work is tracked in **`TODO.md`**.

## Build & install

Native is built on **Linux** and installed into the Radiance repo; the game runs on
**Windows (RTX 2080)**.

```bash
git submodule update --init --recursive        # first time
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DJAVA_PROJECT_ROOT_DIR=/path/to/Radiance \
      -DUSE_AMD=ON -DMCVR_ENABLE_NRD=ON
cmake --build build -j
cmake --install build     # copies core.dll -> $JAVA_PROJECT_ROOT_DIR/src/main/resources/core.dll
```

- `JAVA_PROJECT_ROOT_DIR` is **required** — it's the Radiance repo root; install copies
  `core.dll` into its resources, and the build includes Radiance's generated JNI headers
  from `$JAVA_PROJECT_ROOT_DIR/src/main/native/include/`.
- After installing, rebuild the Radiance jar and deploy. Bundle native+Java changes per
  round to minimize build/run round-trips.
- **`WindowsTraps.txt`** documents Windows build hazards (MSVC-runtime version, avoid
  `INT`/`UINT`/`FLOAT`, `NON_MIN_MAX`, `long` is 32-bit on Windows → use `int64_t`).

## Layout

```
src/
  common/            shared enums/mapping (mapping.hpp: masks, geometry types)
  core/
    render/          renderer, render_framework, world, chunks, entities, buffers,
                     textures, emission, pipeline, vertex_formats
    render/modules/world/
      ray_tracing/   the RT module; submodules/ has world_prepare (per-frame TLAS build)
      nrd/ svgf/ temporal_accumulation/   denoisers
      fsr_upscaler/ xess_upscaler/ dlss/  upscalers
      tone_mapping/ post_render/ shader_pack/
    vulkan/          as (accel structures: BLAS/TLAS), instance, device, physical_device,
                     swapchain, command, descriptor, sbt, sync, image, buffer, framebuffer,
                     render_pass, pipeline, shader, window, vma
    middleware/  util/
  lib/
  shader/            GLSL: world/{ray_tracing,nrd,svgf,tone_mapping,...}, overlay/, util/
extern/              submodules (NRD, VMA, tinyexpr, ...)
```

## Frame model (mental model)

- `render_framework.cpp` owns the frame: acquire → upload → world (RT) → overlay → present.
  All real `vkQueueSubmit`s target **`mainVkQueue`** on this box; the secondary queue is
  idle (chunk BLAS builds submit to main since `useSecondaryQueue=0` on the RTX 2080).
- `world_prepare.cpp` (RT submodule) uploads per-frame RT metadata, builds entity/important
  BLAS, then builds the **per-frame TLAS**; the trace reads TLAS + arrays + geometry buffers
  by **device address** (the driver does NOT lifetime-track these — retention/barriers are
  the only guard).
- Chunk BLAS are built in **separate command-buffer submissions** (`chunks.cpp`
  ChunkBuildScheduler). Cross-submission visibility to the TLAS build must be forced with a
  barrier — see the corrupt-TLAS fix in `world_prepare.cpp`.
- Geometry is fed from Radiance as captured meshes; textures/atlases come across as GL ids
  mapped to bindless slots.

## Conventions & gotchas

- **Cannot compile-check native on this Mac.** clangd "file not found" / "undeclared
  identifier" diagnostics on these files are **false positives** — ignore them. Correctness
  is verified by building on the Linux box.
- **Commit staging discipline:** stage ONLY the specific source files you touched. **Never**
  stage `CMakeLists.txt`, `extern/nrd`, `src/core/CMakeLists.txt`, or
  `src/shader/CMakeLists.txt` (they carry unrelated local changes). Commits get a standard
  `Co-Authored-By` trailer automatically; branch off `main.26_2`.
- **Windows type traps:** see `WindowsTraps.txt`. Long is 32-bit on Windows.

## Validation (self-diagnostic, gated OFF by default)

`instance.cpp` can register `VK_LAYER_KHRONOS_validation` + a debug messenger writing
`radiance_validation.log`. It is **disabled by default** (`RADIANCE_NO_VALIDATION` is set)
because validation crashes this box's implicit overlay-layer stack.

To run it when debugging:
- unset `RADIANCE_NO_VALIDATION`
- set `VK_LOADER_LAYERS_DISABLE=GalaxyOverlayVkLayer,GalaxyOverlayVkLayer_VERBOSE,GalaxyOverlayVkLayer_DEBUG,VK_LAYER_VALVE_steam_overlay,VK_LAYER_VALVE_steam_fossilize`
  (disable overlays **by name** — `~implicit~` also kills NV_optimus/NV_present → instant
  startup crash)
- optionally `RADIANCE_GPU_AV=1` for GPU-assisted validation (AS/descriptor/OOB checks)
- read **`radiance_validation.log`** (flushed per message), not `stdout.log` (block-buffered,
  loses the crash-moment line).

Diagnostic scaffolding (env-gated probes, `radiance_*.log`) is temporary — strip it once a
fix lands.
