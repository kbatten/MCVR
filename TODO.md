# TODO — MCVR (native) 26.2 port

Native-side work items. The feature roadmap and ordering are driven from the Java side —
see **`../Radiance/TODO.md`** for the full prioritized list. This file lists the native work
each feature needs, plus native-only items.

**State:** ✅ done · ⚠️ wired but broken/wrong · ❌ not implemented · 🐞 crash

## Native work behind the feature roadmap

| Radiance # | Feature | Native work | Files |
|---|---------|-------------|-------|
| 2 | Mobs render red | Fix entity texture GL-id → bindless slot resolution / hit-shading sample (missing-texture default is being sampled) | `entities.cpp`, `textures.cpp`, RT hit shaders |
| 3 | Glowing / emissive tiles | Make emissive surfaces act as light emitters in the path tracer — verify `buildLightInfos` output feeds the RT light sampling, not just the AlbedoEmission channel | `emission.cpp`, `chunks.cpp` (`buildLightInfos`), `shader/world/ray_tracing` |
| 4 | Animated textures | Replay/CPU-copy animated atlas frames (26.2's `animate_sprite_blit` render-to-texture is never replayed → first frame only) | `textures.cpp` |
| 5 | Sun & moon discs | Sample the sun/moon atlas sprites in the sky miss shader once Java supplies GL-id + UVs | `shader/world/...` sky path |
| 12 | World shader translation | ChunkSection block fields (ChunkPosition/TextureSize/ChunkVisibility) + CloudFaces/PORTAL_LAYERS missing from BuiltinUniforms → undeclared. Low priority (world is RT'd from meshes) | shader translator |

## Native-only / stability

- 🔧 **Clean exit** — fix applied Java-side, awaiting confirm (2026-08-16). NOT a native teardown-order
  bug: `render_framework.hpp` member order is already correct (contexts/pipeline/swapchain before
  device/vma, instance last). The crash was WHEN teardown ran — the mod called `RendererProxy.close()`
  at `Minecraft.close()V` **TAIL**, after MC's `window.close()` (`glfwDestroyWindow`) + `glfwTerminate()`
  (javap-verified order). `vk::Window::~Window()` `vkDestroySurfaceKHR` (+ swapchain) on the dead HWND
  crashed the WSI. Fixed in Radiance `MinecraftClientMixins` (8db0e03): teardown now injects BEFORE
  `Window.close()`. No native change.
- 🧹 **Strip diagnostic scaffolding** — env-gated probes and `radiance_*.log` output are
  temporary; remove once the corresponding fix has landed and been confirmed.

## Done this port (recent)

- ✅ Intermittent corrupt-TLAS crash (GPU-AV VUID-12281/11819, ~10s–2min in-world) — a GPU
  **barrier-scope gap** before the per-frame TLAS build; chunk BLAS built in separate
  submissions weren't made visible to the TLAS build. Fixed with a broad `ALL_COMMANDS`
  memory barrier in `world_prepare.cpp` (30d11f7). User-confirmed.
- ✅ World-render fix chain (compositing, reverse-Z flip, sky, terrain lighting) — mostly
  Radiance-side; native atlas/G-buffer/present diagnostics supported it.

## Notes

- Cannot compile-check native on Mac (clangd errors are false positives); build on Linux.
- Validation is available but gated off by default — see `CLAUDE.md`.
- Stage only touched files; never `CMakeLists.txt`, `extern/nrd`,
  `src/core/CMakeLists.txt`, `src/shader/CMakeLists.txt`.
