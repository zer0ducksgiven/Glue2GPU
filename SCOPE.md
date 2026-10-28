# Glue2GPU: scope

Goal: let a ReXGlue port swap ReXGlue's Xenia-derived GPU for ReGPU's native renderer, first for our
own conversions, then as something any ReXGlue developer can switch on.

Surveyed: `rexglue/rexglue-sdk` (main, shallow) and `zer0ducksgiven/ReGPU` (main, shallow), 2026-10-27.

## What the two codebases look like at the seam

| | ReXGlue | ReGPU |
|---|---|---|
| Recompiler | Its own, derived from XenonRecomp | Assumes XenonRecomp |
| `PPCContext` | `rex/ppc/context.h`, "based on XenonRecomp/UnleashedRecomp", same field names (`r1.u32`, `r3.u64`...) | Includes XenonRecomp's `ppc_context.h` + the game's `ppc_config.h` |
| Function ABI | `void f(PPCContext&, uint8_t* base)` via `REX_FUNC` | Same signature via `PPC_FUNC` |
| `Vd*` exports | Implemented in `src/kernel/xboxkrnl/xboxkrnl_video.cpp` (`REX_EXPORT`), forwarding to an `IGraphicsSystem` | Implements ~20 `__imp__Vd*` itself (`GUEST_EXPORT`) |
| GPU | Xenia-style D3D12/Vulkan `GraphicsSystem`, ~254k lines, **loaded as a plugin** (`rex_gpu_create`, ABI v1, chosen by `config_.gpu_plugin`) | ~16k lines, D3D12 only, static lib |
| GPU registers (0x7FC80000) | MMIO range with read/write callbacks (`AddVirtualMappedRange`) | Plain memory in the guest address space |
| Physical memory | File-mapped, mirrored views (Xenia layout) | Expects the 512 MB physical view at 0xA0000000; `OpenExistingHeapFromAddress` on it, optional `physicalSection` handle |
| Window / input / UI | ReXGlue owns the window, ImGui, input (`WindowedAppContext`, `Presenter`) | Creates its own Win32 window and swap chain |
| Platforms | Windows, Linux x64, Linux arm64 | Windows (and Wine/Proton) |

The most useful finding is the GPU plugin ABI. ReXGlue already loads its renderer from a DLL through a
small interface (`IGraphicsSystem`: `SetupPresentation`, `SetupGuestGpu`, `InitializeRingBuffer`,
`EnableReadPointerWriteBack`, `SetInterruptCallback`, plus `provider()`/`presenter()`). The CPU-side
calling conventions are effectively the same as XenonRecomp's. So we don't have to *convert*
projects. We can ship ReGPU as a ReXGlue GPU plugin, and a project switches by changing one setting.

## Options

### A. ReGPU as a ReXGlue GPU plugin (recommended)

`regpu-rex.dll` exports `rex_gpu_abi_version` / `rex_gpu_create` and returns a
`ReGpuGraphicsSystem : rex::system::IGraphicsSystem` that drives ReGPU's core. ReXGlue's kernel keeps
its `Vd*` exports and calls into the plugin. ReGPU's own `GUEST_EXPORT`s are left out of this build.

For the user: build or download the DLL, put it next to the exe, set the GPU plugin to `regpu`. No
regeneration, no source edits. Per-game fixes go through ReGPU's hooks from a small optional
`game_hooks` module.

### B. Source conversion (swap ReXGlue's video kernel for ReGPU's)

Link ReGPU statically and replace `xboxkrnl_video.cpp`'s exports with ReGPU's. That means duplicate
symbols, patching ReXGlue's tree per port, and redoing the work on every ReXGlue update. Use it only
if the plugin boundary turns out to cost measurable performance, which is unlikely because the
boundary is crossed only by ring-buffer setup and interrupts, not by every draw.

### C. A "converter" tool that rewrites projects

Not needed. Generated code and the CPU runtime stay ReXGlue's; only the GPU changes. A converter
would mostly emit CMake glue, which A replaces with a DLL and a setting.

## Work breakdown for option A

### 1. Make ReGPU game-agnostic at build time (ReGPU repo) — M
- ReGPU currently compiles against each game's `ppc_config.h` (`REGPU_PPC_DIR`). A plugin has to be
  built once for every game. Audit what it actually uses from `ppc_config.h`/`ppc_context.h`, then
  replace that with a small `regpu/guest_abi.h` (context layout, `base`, guest call through
  `Host::guestCall`).
- Split the build into `regpu_core` (renderer, command processor, ucode→HLSL, EDRAM, presenter) and
  `regpu_xenonrecomp` (today's `GUEST_EXPORT` Vd* layer). FM2 keeps working unchanged. That is the
  regression gate.

### 2. Expose a driver API from the core — M
ReXGlue's `Vd*` layer needs entry points instead of ReGPU's own exports:
- `regpu::InitRingBuffer(ptr, log2)`, `EnableRptrWriteback(ptr, log2)`, `SetInterruptCallback(cb, data)`
- `regpu::ReadRegister(index)` / `WriteRegister(index, value)` for ReXGlue's 0x7FC80000 MMIO
  callbacks. Today ReGPU reads that range as plain memory (`gpu.cpp` `Reg`/`SetReg`, a `memcpy` of
  the register file). Simplest fix: back it with a host-side array and point the accessors at it.
- **VdSwap semantics.** ReXGlue's `VdSwap_entry` writes the swap packet into the ring itself and
  resolves the front-buffer address from the fetch constant. ReGPU implements `VdSwap` its own way.
  Align them: either ReGPU accepts ReXGlue's packet (`PM4_XE_SWAP`-style), or the plugin interface
  gets a `Swap(...)` hook. Check this first; it decides how invasive step 3 is.

### 3. The plugin shim (new repo, this one) — M
- `ReGpuGraphicsSystem` implementing `IGraphicsSystem`.
- Fill `regpu::Host` from ReXGlue's `Memory`/`KernelState`/`FunctionDispatcher`: guest base,
  physical allocation (`SystemHeapAlloc`/physical heap), `XHostThread` for ReGPU's threads,
  current PCR, guest calls through the dispatcher, the interrupt lock (ReXGlue's
  `DispatchInterruptCallback` equivalent).
- Physical memory: confirm ReXGlue's 0xA0000000 view is a single contiguous mapping that
  `OpenExistingHeapFromAddress` accepts. Pass the section handle as `physicalSection` if it's
  available. **This is the biggest technical unknown.** If it fails, ReGPU needs an upload-copy
  fallback, which costs performance.
- Config: map ReXGlue cvars (resolution, msaa, vsync, upscaler) to `regpu::Config`.

### 4. Presentation and window ownership — M/L
ReGPU makes its own window. Under ReXGlue the plugin must either:
- (a) render into ReXGlue's window: adopt its HWND, own the swap chain, and return a `Presenter`
  that ReXGlue's ImGui overlay can draw into. This is the right answer and the larger job.
- (b) Stopgap: give ReGPU the HWND, let it own presentation, and disable ReXGlue's overlay.

Input and focus (`WindowHasFocus`, `DebugPad`) must then come from ReXGlue's window, not ReGPU's.

### 5. Per-game hooks for ReXGlue projects — S
A header plus an optional `regpu_game_hooks(Host&)` export in the game exe, which the plugin looks up
at load. This lets projects use `rewriteShader`, `resolveWriteback` and the others without forking.

### 6. Developer pipeline — S/M
- CI builds `regpu-rex.dll` against each tagged ReXGlue release (the ABI is versioned; fail loudly
  on mismatch).
- Docs: "switch your ReXGlue port to ReGPU" in three steps, plus the known-limits table.
- A `glue2gpu doctor` script: check D3D12 FL12_0, `d3dcompiler_47.dll`, 1280x720 assumption, the
  plugin ABI version, and the shader cache dir.
- Bring regpuctl.py's debug server along (`REGPU_DEBUG=1`) so people can triage their own games.

### 7. Validation — L, ongoing
- FM2 via XenonRecomp must still be pixel-identical (gate for 1 and 2).
- Pick 2–3 ReXGlue titles with different engines, compare against the stock ReXGlue D3D12 backend
  (screenshots + frame times). Expect each new game to surface Xenos gaps that FM2 never exercised
  (formats, resolve paths, memexport, tessellation, non-720p render targets).

## Rough effort

| Phase | Size | Outcome |
|---|---|---|
| Spike: steps 2-VdSwap + 3-physical memory, hacked, one ReXGlue title to first frame | 1–2 wks | Go/no-go |
| Steps 1–3 + 4b | 3–5 wks | Works for us, own window |
| 4a + 5 + 6 | 3–4 wks | Usable by others |
| 7 | per game, open-ended | Compatibility |

## Risks and limits

- **Compatibility, not plumbing, is the real cost.** ReGPU renders one game completely. Xenia's
  backend is about 15× larger because it covers many games' edge cases. A plugin that boots any
  ReXGlue title is a few weeks of work. A plugin that renders most of them correctly takes much longer.
  Present it as an opt-in "fast path" with a per-game compatibility list, not a replacement.
- **Platform:** ReGPU is D3D12 only. ReXGlue's Linux builds would still use Vulkan, apart from
  Proton.
- **1280x720 assumption** in ReGPU breaks some titles until it is generalised.
- **ReXGlue API churn:** it is marked early-development with breaking changes expected. The plugin
  ABI is at v1. Pin versions in CI and talk to the ReXGlue maintainers. Upstreaming a "third-party
  GPU plugin" doc or test would protect us.
- **Physical memory aliasing** (step 3) could force a copy path that eats into the performance win.
- Licensing is fine: both MIT. ReGPU contains no Xenia code, and the plugin only links ReXGlue
  headers.

## Recommendation

Do it as a plugin (option A), and start with a time-boxed spike that answers the two unknowns:
VdSwap/ring-packet compatibility and whether `OpenExistingHeapFromAddress` works on ReXGlue's
physical view. If both pass, the "framework for others" is mostly packaging, and the ongoing work
is per-game compatibility, done with tooling ReGPU already has.

## Test project: Forza Horizon 1 ReImagined

Surveyed `FH1-ReImagined/ForzaHorizon1-ReImagined` (main, shallow), 2026-10-27.

### Why it's a good first target
- **The seam is already there.** `forza-recomp/src/forza_app.h` loads the GPU itself:
  `config.gpu_plugin = "xenos"` then `rex::system::LoadGpuPlugin(gpu_plugin, ForzaGpuBackend())`.
  Pointing that at `regpu` is a one-line change, or no change at all if it's set from the config.
- **It tolerates a non-Vulkan GPU.** `InstallVulkanHooks` is documented as "a no-op returning
  false for other backends".
- **Many of its rendering features are guest-side.** For example, `hooks/scene_resolution.cpp` and
  `scene_tiling.cpp` patch the game's own render scenarios and tables. They change what the game
  submits, so they should carry over to any GPU.
- **Same engine family as FM2.** Horizon was built on Turn 10's Forza tech, so ReGPU has likely
  already met many of its shader and resolve patterns.
- **There's a clear performance case.** Its README says races above 60 fps are "bound by the GPU
  emulation" (120 fps target: 75.7 fps in a 7-car race on a 7800X3D + RX 9060 XT), and the Steam
  Deck gets 15–20 fps in free roam. It also lists NVIDIA D3D12 flicker and stutter issues. Those
  figures give a ready-made benchmark (`tools/perf_scenario.sh`, `docs/perf.md`).

### What makes it harder than a stock ReXGlue game
- **It isn't on stock ReXGlue.** It pins SDK 0.10.0.24 through its own fork,
  [ReIGlue](https://github.com/FH1-ReImagined/ReIGlue) (`tools/build_rexglue_sdk.sh`, `REIGLUE_TAG`).
  About 130 patches are listed in `docs/sdk_patches.md`, and many are inside the Xenos GPU plugin.
  Our plugin must build against **ReIGlue's** headers and plugin ABI, not upstream's.
- **Features that live in the forked GPU plugin are lost or need re-implementing with ReGPU:**
  - HDR presenter
  - VR (OpenXR needs Vulkan, which rules it out for a D3D12 ReGPU)
  - the `fh1_upscaler` scene reconstruction (FSR 3 / DLSS / SMAA): ReGPU has its own FSR 3 and DLSS
  - separate native-HUD render scale
  - texture packs
  - world AO
  - pipeline-cache work
  - resolve-readback keying (photo mode, car thumbnails: ReGPU's `resolveWriteback` hook covers
    this)

  The `fh1_*` cvars read in `src/` (`fh1_upscaler`, `fh1_native_scene_scale`, `fh1_world_ao`,
  `fh1_scene_720p_native_hud`...) are the list to audit.
- **The ImGui overlays and the native menus** (performance overlay, HUD overlay, graphics menu)
  draw through the SDK presenter, so they depend on presentation path 4a. With 4b they disappear.
- **Windows only** for the test. Linux/Deck players stay on Vulkan.

### FH1 spike plan (go/no-go, ~2 weeks)
1. Build FH1 from source on Windows with its pinned ReIGlue and our own game copy. Record a
   D3D12 and Vulkan baseline with `perf_scenario.sh` (free roam + Clear Springs race).
2. Build a minimal `regpu` plugin against ReIGlue's `rex/system/gpu_plugin.h`: steps 2–3 hacked,
   own window (4b), all ReIGlue-only features off.
3. **Gate 1:** the boot screens and menus render. This tests the VdSwap path, the register
   MMIO and the 0xA0000000 heap.
4. **Gate 2:** free roam and a race render correctly enough to compare. Triage with
   `regpuctl.py`.
5. **Gate 3:** frame times against step 1 at 60/120/144. Viable if the race at 120 clearly beats
   75.7 fps with no CPU-side regression.

If gate 3 passes, decide which of the forked-plugin features FH1 can't ship without (likely the
upscaler, HUD scale and photo mode) before building the general pipeline. Also talk to the FH1
maintainers early: they own ReIGlue and would be the first users.
