# vilk

A cross-platform, Vulkan-first audio visualizer that plays the existing MilkDrop preset ecosystem (`.milk` files), built on top of libprojectM's preset-parsing/audio-analysis layer with a custom Vulkan render backend (OpenGL fallback planned).

Named after vodka + milk, because that's funnier than `vkmilk`.

See [`docs/BUILD-PLAN.md`](docs/BUILD-PLAN.md) for the full architecture, phase roadmap, and decisions log. Start there before writing code.

## Status

Pre-implementation. Phase 0/1 (foundation + Vulkan render-graph skeleton) not yet started.

## Quick facts

- **Preset compatibility:** required, non-negotiable -- leverages the existing `.milk` community library via vendored libprojectM 4.x.
- **Render backend:** Vulkan primary, OpenGL fallback (built last, after Vulkan path is proven).
- **Platforms:** Windows, Linux, macOS from day one.
- **Build system:** CMake 3.21+.
