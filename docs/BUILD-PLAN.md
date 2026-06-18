# Project: Cross-platform Vulkan MilkDrop-preset Visualizer

**Status:** Pre-implementation / research phase
**Author intent:** Drafted in conversation with Claude (claude.ai), to be handed to Claude Code for implementation
**Last updated:** 2026-06-17

---

## 1. What this project is

A standalone, cross-platform application that renders the existing community library of MilkDrop (`.milk`) audio-reactive presets, using a custom Vulkan render backend instead of projectM's native OpenGL renderer, with OpenGL retained as a fallback backend behind the same abstraction.

**Hard constraint:** Must remain compatible with the existing `.milk` preset ecosystem. We are not writing a new preset format or a new preset-authoring math layer. New presets, new math, and new wrapper features are welcome additions *on top of* that compatibility, not replacements for it.

**Explicitly not in scope (initially):** Hosting the actual closed-source Winamp `MilkDrop2.dll` via a Winamp plugin-host shim. We are building on **libprojectM** (MIT-licensed, actively maintained, MilkDrop-compatible reimplementation), not the original binary.

---

## 2. Why projectM, and what it actually gives us

Researched directly against the current `projectM-visualizer/projectm` repository (not assumed from older versions):

- libprojectM is structured as: **parse presets → analyze audio (FFT, beat detection) → evaluate preset math against audio features → render output via OpenGL**. It can render to a dedicated GL context *or to a texture*. The texture-output path is our safety-net integration point if the deeper interception (below) proves harder than expected.
- It ships its own embedded shader translator (`src/libprojectM/Renderer/hlslparser/`, files `HLSLTree.cpp` / `GLSLGenerator.cpp`) that parses the pseudo-HLSL shader code embedded in `.milk` presets and emits **GLSL**. This is the component that does the hard work of "MilkDrop shader language → something a modern GPU can run." We do not need to reimplement this — we need to **intercept its GLSL output** before projectM's own GL compiler consumes it, and feed that GLSL into our own SPIR-V cross-compile step instead.
- Current stable line is **4.x** (4.0 final released ~2 years after 3.1, with ongoing 4.1.x maintenance releases improving preset rendering fidelity). Target this line, not 2.x/3.x.
- **Known, documented limitation:** some preset shaders still fail to translate correctly (HLSL/GLSL incompatibilities, e.g. `refract()` not supported in the transpiler) and render black or incorrectly. This is an upstream limitation of the GLSL generator itself — our Vulkan backend inherits it as-is. Document this as a known fidelity ceiling; it is not a bug in our wrapper, and not something we should scope ourselves to fix.
- **CMake 3.21+ is the only supported build system** since 4.0 (autotools fully retired). Simplifies cross-platform CI/build setup considerably.
- **Active upstream signal worth tracking:** PR `projectM-visualizer/projectm#877` ("Renderer backend selection", WIP, unmerged at time of writing) is refactoring the render item hierarchy to separate a generic base from an explicit `OpenGLRenderItem`/OpenGL-backend concept. This is the maintainers moving toward the same kind of backend-pluggability we need. **Do not depend on it landing** — but check its status periodically; if/when merged, it may substantially reduce the fork-and-patch surface described in Phase 2 below.

---

## 3. Target architecture

```
.milk preset files (existing community library, unmodified)
        |
projectM preset-eval layer (vendored libprojectM)
  - parses presets
  - runs per-frame/per-vertex audio-reactive math
  - hlslparser translates embedded shader code -> GLSL source (existing component)
        |
   [INTERCEPTION POINT] <-- our integration code lives here
        |
GLSL -> SPIR-V cross-compile (glslang or shaderc), at preset-load/switch time,
        NOT per-frame -- this is a load-time cost, not a render-loop cost
        |
Our Vulkan render graph:
  - swapchain management
  - ping-pong framebuffers (warp mesh feedback pattern)
  - composite pass
  - explicit synchronization/barriers
        |
   App shell (GLFW or SDL3, cross-platform window/input)
        |
   GL fallback backend (thinner, shares render-graph abstraction, built last)
```

**Audio path (parallel, reuses prior work):**
```
Platform audio capture (WASAPI loopback / PulseAudio-PipeWire monitor / CoreAudio tap)
        |
Same abstraction interface already built for the vu-meter project
        |
projectM PCM ingest (projectm_pcm_add_float / add_int16)
        |
projectM's internal FFT + beat detection (let projectM own this --
presets expect projectM's specific spectral variables, don't run a
second independent FFT for this app)
```

---

## 4. Sequencing rationale (why this order, not GL-first or Vulkan-everything-at-once)

1. **Vulkan render-graph skeleton first**, with no projectM involved yet. This is infrastructure needed regardless of how Phase 2 research turns out: swapchain setup, basic textured quad, ping-pong framebuffer pattern, SPIR-V shader loading from disk. If this phase is slow or hits driver issues, it's isolated and doesn't block anything else.
2. **projectM integration research second.** Vendor libprojectM (as a git submodule, not a hard fork — see Section 6), confirm exactly where the hlslparser's GLSL output can be captured, and prove the render-to-texture fallback path works as a safety net. This phase is explicitly research/spike work, not feature work — expect throwaway code.
3. **Shader cross-compile + feedback pipeline third.** This is where projectM's evaluated preset data and our Vulkan code actually meet. Highest uncertainty phase. The ping-pong/warp-mesh feedback pattern (rendering last frame into a distorted UV mesh, layering new shapes on top, feeding back) is exactly the kind of explicit multi-pass pipeline Vulkan is suited for — but it requires correct explicit barriers, which is new territory if the team hasn't done heavy Vulkan synchronization work before.
4. **GL fallback last, deliberately.** It should be built as a thinner second implementation behind an already-proven render-graph abstraction, not designed in parallel with the Vulkan path. Building both at once risks them drifting (a preset that looks right on one backend and wrong on the other) — the most underestimated cost of dual-backend projects.

This ordering means: if Phase 2/3 research turns out harder than expected, there is still a working, tested Vulkan visual pipeline that could be fed hand-written or third-party audio-reactive shaders, rather than nothing to show.

---

## 5. Phase breakdown

### Phase 0 — Foundation (parallel, low risk)
- Repo scaffold, CMake 3.21+ build setup targeting Windows/Linux/macOS from day one.
- Port/adapt the existing vu-meter audio capture abstraction (WASAPI loopback / PulseAudio-PipeWire / CoreAudio) into a standalone interface that just needs to hand PCM frames to a consumer — no FFT of its own for this app.
- Decide on GLFW vs SDL3 for window/input/swapchain creation. (Lean SDL3 if cross-platform audio input glue is also wanted from the same library; GLFW if windowing-only is preferred and audio stays fully separate.)

### Phase 1 — Vulkan render-graph skeleton
- Swapchain creation and presentation loop.
- Ping-pong framebuffer pattern (two off-screen render targets, alternate read/write per frame) — this is the structural primitive the warp-mesh feedback pattern will need later, so get it right here with a trivial test shader before projectM is involved at all.
- SPIR-V shader loading path: compile a couple of hand-written GLSL test shaders to SPIR-V via `glslang` (build-time first; runtime compilation via `glslang`'s C API or `shaderc` is the actual requirement for Phase 3, since preset shaders are generated dynamically at preset-load time — confirm runtime compilation works here, don't defer that risk to Phase 3).
- Validation layers on by default in debug builds.
- **Exit criterion:** a textured quad bouncing through a ping-pong pass, fed by a runtime-compiled SPIR-V shader, with no projectM code involved.

### Phase 2 — projectM integration research (spike)
- Vendor libprojectM 4.x as a git submodule (see Section 6 for why submodule-and-patch, not fork).
- Build it standalone first (with `-DENABLE_SDL_UI=ON` for the legacy test UI) to confirm the toolchain works before touching internals.
- Locate and document the exact call path from preset-load to `hlslparser`'s GLSL generation output. Identify the smallest patch that exposes that GLSL string (and associated uniform/sampler metadata) to external code, without disrupting projectM's own GL rendering path (which we may still want as the literal fallback-of-the-fallback during development, to A/B against).
- In parallel, prove the simpler "render-to-texture" path actually works as a documented capability — this is the fallback safety net if the hlslparser interception turns out to be more invasive than expected. Treat this as a checkpoint: if Phase 2 research stalls, this path keeps the project moving.
- Watch PR #877 status.
- **Exit criterion:** a documented, minimal patch (ideally <a few hundred lines) against vendored libprojectM that exposes per-preset GLSL source + metadata to host code, plus a working render-to-texture fallback as backup.

### Phase 3 — Shader cross-compile + feedback pipeline
- GLSL → SPIR-V at preset-load/switch time using the runtime path proven in Phase 1.
- Wire projectM's audio-reactive per-frame uniform updates into the Vulkan descriptor/uniform-buffer update path.
- Implement the warp mesh + composite pass using the ping-pong framebuffer primitive from Phase 1.
- **Exit criterion:** a real `.milk` preset rendering and audio-reacting through the Vulkan pipeline, end to end.

### Phase 4 — GL fallback + packaging
- Thinner GL backend implementing the same render-graph interface.
- Preset browser/cycling UI, hotkeys, fullscreen toggle (projectM already has preset-switching logic to lean on).
- Cross-platform packaging.

---

## 6. Decisions already made (don't re-litigate without new information)

| Decision | Choice | Rationale |
|---|---|---|
| Preset engine | libprojectM 4.x, not original MilkDrop2.dll | Active maintenance, real API, cross-platform, MIT license |
| Preset format compatibility | Required, non-negotiable | Existing community preset library is the point |
| Render API | Vulkan primary, OpenGL fallback | Explicit requirement; fallback for driver/platform pain points |
| Vendoring strategy | Git submodule + small patch, not a hard fork | Keeps upstream bugfixes/PR #877 mergeable; smaller reviewable diff |
| Shader translation | Reuse projectM's existing hlslparser GLSL output, cross-compile to SPIR-V | Don't reimplement HLSL→GLSL translation; it already exists and works |
| Audio FFT/beat detection | Let projectM's internal analysis own this | Presets expect projectM's specific spectral variables |
| Build system | CMake 3.21+ | Required by projectM 4.x anyway; only sane cross-platform option |
| Platforms | Windows, Linux, macOS from day one | Explicit requirement |

## 7. Open questions for Claude Code to resolve during Phase 2 spike (not decided yet)

- GLFW vs SDL3 for the app shell — leaning toward whichever simplifies swapchain + audio-input glue most, not yet decided.
- Exact shape of the libprojectM patch — can't be specified precisely until Phase 2 research reads the real current source tree (this doc's Section 2 findings are from documentation/PR descriptions, not a full source read — Claude Code should do a full read of `src/libprojectM/Renderer/` and `src/libprojectM/Renderer/hlslparser/` before writing the patch).
- glslang vs shaderc for runtime GLSL→SPIR-V — both are viable; pick based on whichever has the cleaner runtime (non-CLI) API for the target platforms.
- Whether macOS needs MoltenVK considerations baked in from Phase 1, given Vulkan isn't native there.

## 8. Non-goals / explicit scope boundaries

- Not reimplementing MilkDrop's preset math or shader language from scratch.
- Not hosting the original closed-source Winamp DLL.
- Not fixing projectM's known HLSL→GLSL translation gaps (e.g. `refract()`) — inherited limitation, documented, not owned by this project.
- Not building new preset-authoring tools in v1 (explicitly open to later, per author).
