# FH1 spike: ReGPU as a ReIGlue GPU plugin

Pinned versions (in `thirdparty/`, shallow clones):
- ReIGlue `fh1-0.10.0.24-r2` @ `317216e8d66d766bd13abdb672975458f30ca576`. This is FH1's
  `REIGLUE_TAG`/`REIGLUE_COMMIT` from `tools/build_rexglue_sdk.sh`.
- ReGPU `main` @ `b383675`.

Sketch: [`plugin/`](plugin/). It hasn't been compiled. It needs the ReGPU changes below first.

## How FH1 would load it

The plugin builds as `rexgpu-regpu.dll` next to the exe and is loaded by
`LoadGpuPlugin("regpu", backend)`. ReIGlue's `gpu_plugin` cvar (`src/ui/rex_app.cpp:51`) feeds
`config_.gpu_plugin`. FH1's `OnPreSetup` only falls back to `"xenos"` when that is empty, so
`--gpu_plugin=regpu` should need no FH1 change. *To verify:* that the cvar is applied before
`OnPreSetup` runs. FH1 passes its `gpu_backend` setting as the backend, so it must be `d3d12` or
`any`.

## What the ReXGlue side gives us (ABI 1)

`IGraphicsSystem` (`include/rex/system/interfaces/graphics.h`) provides:
- `SetupPresentation`
- `SetupGuestGpu(dispatcher, kernel_state)`
- `SetInterruptCallback`
- `InitializeRingBuffer`
- `EnableReadPointerWriteBack`
- `InitializeShaderStorage`
- `Shutdown`
- `provider()`/`presenter()`, which are optional and may return null

Everything else is the plugin's job, as it is in `GraphicsSystem::SetupGuestGpu`:
- register MMIO at 0x7FC80000
- the vblank thread
- interrupt dispatch

That suits ReGPU, which already runs its own vblank interrupt thread and command processor.

## Mapping `regpu::Host` to ReIGlue

| Host field | ReIGlue | Notes |
|---|---|---|
| `base` | `Memory::virtual_membase()` | Flat guest space; the 0xE0000000 view has a 4 KB host offset, which ReGPU doesn't use |
| `physicalSection` | null | ReIGlue's mapping is one 4.5 GB file covering all memory (physical at file offset 0x100000000), and its handle is private. ReGPU falls back to `OpenExistingHeapFromAddress1(base+0xA0000000, 512 MB)` |
| `alloc` | `SystemHeapAlloc` / `LookupHeap(0xA0000000)->Alloc` | |
| `createThread` | `new XHostThread(...)`, body gets `thread_state()->context()` | |
| `currentPcr` | `XThread::GetCurrentThread()->pcr_ptr()` | |
| `guestCall` | `FunctionDispatcher::ExecuteInterrupt(thread_state, addr, {r3, r4})` | ExecuteInterrupt takes the dispatcher's global lock, so ReGPU's interrupt-lock hooks are no-ops |
| `PPCContext`, `PPC_FUNC` | `rex/ppc/context.h`, `REX_FUNC` | ReGPU uses only `PPC_FUNC`, `r1`, `r3`–`r10` and `msr`. `shim/` covers them |

## ReGPU changes needed (a branch on ReGPU; FM2 must stay identical)

1. **Compile out the `GUEST_EXPORT`s** in `src/gpu/gpu.cpp` and expose their bodies as
   `regpu::driver` ([`plugin/include/regpu/driver.h`](plugin/include/regpu/driver.h)). The
   XenonRecomp build keeps the exports as thin wrappers.
2. **Registers in a host array.** ReGPU reads 0x7FC80000 as guest memory (22 `Reg`/`SetReg`
   uses, plus `memcpy`s of the block). Under ReXGlue that range is MMIO, handled by guarded pages
   and an exception handler. Host-side reads there would fault, or read stale memory.
3. **Accept Xenia's swap packet.** ReIGlue's `VdSwap` writes a type-0 fetch-constant packet plus
   type-3 `PM4_XE_SWAP` (0x64, `'SWAP'`, front-buffer physical address, width, height). ReGPU's
   `VdSwap` writes NOPs and queues the swap on the CPU side. Its CP must treat 0x64 as the swap. Its
   frame limiter and hang watchdog live in `VdSwap` on the guest render thread, so they need a new
   home.
4. **Refresh rate.** FH1 drives the guest vblank from `video_mode_refresh_rate` (up to 480 Hz, SDK
   patch 0037), so ReGPU's vblank thread has to follow it (`driver::SetRefreshRate`).
5. **CMake** must build without a XenonRecomp checkout or a game `ppc_config.h`.

## Open questions, in the order the spike answers them

1. Does the shim build compile, i.e. does ReGPU really need nothing else from XenonRecomp?
2. Does `OpenExistingHeapFromAddress1` accept ReIGlue's mapped 0xA0000000 view? If not, use
   ReGPU's copy path, which was 150 fps against 211 fps in FM2.
3. Is the MMIO handler re-entrant enough for writes that arrive while ReGPU's CP thread is
   running? ReGPU never had register writes interleaved through callbacks before.
4. Does the PCR `CurrentCpu` write + `ExecuteInterrupt` behave like ReXGlue's own
   `DispatchInterruptCallback` (`SetActiveCpu`)?
5. Does a second Win32 window from ReGPU conflict with ReXApp's window and input (focus, the SDL
   pad)? Expect input to go to ReXApp's window while ReGPU's shows the picture. That's fine for gate
   1, and it's why 4a matters.
6. Does the plugin link the ReXGlue runtime the same way `rexgpu-xenos` does?

## Status (2026-10-27)

- **ReGPU** branch `rexglue-driver` (local, not pushed):
  - `7079846`: `regpu::driver`, registers in a host array, `REGPU_GUEST_ABI=rexglue`.
  - `1a73511`: presents on `PM4_XE_SWAP`. The watchdog and frame limiter moved to `PaceSwap`,
    which runs on the CP thread for XE_SWAP. Adds `driver::SetRefreshRate`.
  - Both guest ABIs compile. The XenonRecomp build was checked against a dummy `ppc_config.h`,
    so **FM2 has not been re-run**.
- **ReIGlue** built locally (clang 23, Ninja, Release, `REXGLUE_USE_VULKAN=ON` as FH1's
  `build_windows.sh` does) and installed to `sdk/reiglue`. Without Vulkan, `rexruntime` fails to
  link (`FLAGS_gpu_vendor_storage_`).
- **Plugin**: `build/plugin/rexgpu-regpu.dll` builds against `find_package(rexglue)` /
  `rex::runtime`. Loaded with ctypes, it reports ABI 1 and returns an object for `d3d12`/`any`
  and null for `vulkan`. A poller thread feeds `VdQueryVideoMode`'s refresh rate to
  `driver::SetRefreshRate`.
- Nothing has run inside a game yet.

## Gate 1 result (2026-10-27): passed

FH1 base game (disc 0.0.0.7) built on Windows against `sdk/reiglue` (clang-cl, codegen 617 files)
and run with `--gpu_plugin=regpu --gpu_backend=d3d12`. Runs are in `run/`.
- Booted through ReIGlue's `Vd*` exports, the register MMIO, the interrupts and XE_SWAP to the
  intro video and the title screen ("PRESS START"), all rendered by ReGPU
  (`run/regpu-gate1b/shot_*.png`).
- **Memory (Q2):** `OpenExistingHeapFromFileMapping` fails as expected (`80070057`), but
  `OpenExistingHeapFromAddress1` on the 0xA0000000 view works, so the GPU reads guest memory
  zero-copy. ReGPU's aliasing self-test ran after the plugin's physical `Alloc` was fixed to
  allocate whole 64 KB pages through `AllocRange`. It passed: no "copied instead" line.
- **Pacing:** `FORZA_FPSLOG` is identical to stock Xenos D3D12. Both run 60 fps in the intros
  and the game's 30 fps cap on the title.
- **Open:**
  - One guest physical allocation fails at boot (`PhysicalHeap::Alloc unable to pin`) on a
    guest thread, under ReGPU only. Effect unknown.
  - Pressing start didn't happen under ReGPU in the timed runs. ReGPU's own window doesn't
    route input to ReXApp, so getting past the title needs input wiring (4a) or ReGPU's
    debug pad (`regpuctl` buttons).
  - No visual A/B yet: the stock run got past the title, so the shots show different screens.

## Toward free roam (2026-10-27)

- Input: FH1's `FORZA_AUTOPRESS` schedule works on Windows under either renderer. The FIFO variant
  is Linux-only. ReGPU's debug pad isn't read by FH1.
- Save: the user played the intro on stock Xenos into `run/profile`; there's a pristine copy in
  `run/profile-save`.
- ReGPU, from the save (`run/load-regpu`): title → SINGLE PLAYER → loading screen render. Then,
  entering the world at about 78 s, **ReGPU freezes**. The last log line is
  `EDRAM colour surface at base 2D0` after a 1643-draw frame. There is no `hang:` report and no
  swaps after it, and FH1 reports "not presenting". There's no debugger on this machine and the
  plugin doesn't implement `Host::logThreadStacks`, so there are no thread stacks yet.
- Fresh-profile run (`run/drive2`): ReGPU stalled after the intro movie in a shader wait
  (`hang: ... parked 45`). Cause: `GetShader`'s per-thread cache skipped `Promote` for a shader
  first seen by the memory scan and parked. Fixed in `renderer.cpp`; not committed, and not
  re-tested on that path.
- ReGPU-only render difference: the brightness-calibration box is black (stock shows its text).

## Free roam comparison (2026-10-27, ReGPU b383675 + this branch)

Both runs start from the user's save, use the same A-press schedule,
`--frame_rate=240 --vsync=false`, and stand still at the festival (`run/perf-xenos`,
`run/perf-regpu`, `run/freeroam_compare.png`).

| | Stock Xenos D3D12 | ReGPU |
|---|---|---|
| Intro videos | ~360 fps | ~100 fps |
| Free roam, 60–180 s | **~82 fps** | **~51 fps** |

- ReGPU is about 40% **slower** here. This is a first run with no shader cache, and runtime
  shader compiles use `D3DCOMPILE_SKIP_OPTIMIZATION`. `REGPU_DEBUG=1` was on and the build was
  RelWithDebInfo. None of this has been profiled yet.
- ReGPU's frame cap read `vsync on` (cap 180) even with `--vsync=false`, so the plugin's cvar read
  needs checking. It didn't limit these numbers.
- ReGPU-only visual bugs:
  - green rear window
  - missing HUD pieces (speedometer ring, lock and info icons, minimap pin)
  - car badge and plate detail wrong
  - purple flag shapes
  - a heavy blur over the scene
  - the black brightness box
- Freeze entering the world: 1 of 3 runs from the save (`run/load-regpu`). Not reproduced since,
  with the stack dumper in place (`plugin/src/stack_dump.cpp`).
- ReGPU's GitHub `main` moved to `bdd133a` today. The branch is rebased onto it (now `ddae01e`,
  `eefe4b8`); the numbers above predate that.

Re-run on ReGPU `bdd133a` + this branch, with a warm shader cache (`run/perf-regpu2`,
`run/freeroam_compare2.png`): free roam averaged **54.6 fps** over 60–180 s (stock 82), with
the same visual bugs. No freeze or hang.

## Profile: where ReGPU's frame goes in FH1 free roam (2026-10-27)

All runs start from the save, stand still at the festival, use
`--frame_rate=240 --vsync=false` and have ReGPU's debug server off. GPU utilisation is from
`nvidia-smi` (RTX 4070, driver 617.14). Guest waits come from `FORZA_GPU_WAIT_LOG` (time FH1's
render thread spends waiting for GPU progress words), per 10 s.

| Run | fps | ReGPU "GPU time"/frame | GPU util | Guest waits on GPU |
|---|---|---|---|---|
| Stock Xenos D3D12 | ~83 | n/a | 81% | 1–8 ms |
| ReGPU 1080p (plugin default then) | 51.7 | 19.0 ms | | |
| ReGPU 720p | 53.5 | 16.9 ms | | |
| ReGPU 720p, optimised shaders | 56.6–60 | 16.5 ms | 58% | **~3,800 ms** |
| ReGPU 720p, optimised, `REGPU_EARLY_FENCES=1` | **85.3** | **8.1 ms** | 73% | 2–140 ms |

- **The cost isn't GPU work.** It's fence latency. `WriteBack` (`MEM_WRITE`/`EVENT_WRITE_SHD`)
  queues each fence on `PendingWrites` until the host GPU has finished the draws before it.
  That's the safe behaviour with zero-copy guest memory, but FH1's render thread waits on those
  words about twice a frame. CPU and GPU take turns: the GPU is 58% busy, and the 16.5 ms
  "GPU time" is the GPU's wall span including idle gaps. Xenos writes fences when its CP reaches
  them, because it reads copies of guest memory.
- **ReGPU's real GPU cost** is about 8 ms/frame. ReGPU's CPU draw path is about 6.3 ms/frame
  (constants 2.5, textures 1.1, submit 1.0). With early fences the frame is CPU/CP-bound at
  ~11 ms.
- Resolution: the plugin rendered at 1920x1080 (2.25x stock's 720p). It now defaults to 1280x720;
  `GLUE2GPU_RES=WxH` overrides that.
- Optimised fxc (`REGPU_OPTIMIZE_SHADERS=1`, new, opt-in) is worth about 0.4 ms GPU and ~3 fps.
- **`REGPU_EARLY_FENCES` is a measurement switch, not a fix.** It's unsafe with zero-copy: the
  title may overwrite vertex or index memory the host GPU is still reading. Visual correctness
  with it was not checked.
- What a real fix needs: early fences made safe. That means fences written at CP time while the
  memory the GPU reads behind them is protected: copy what's read through the upload ring, as
  Xenia does with shared memory, or copy just the ranges the title rewrites. A narrower option is
  early writes only for the progress words FH1 polls, if those are known not to guard reused
  memory. Either is a ReGPU design decision.
- Uncommitted on `rexglue-driver`: the `GetShader` promote fix, `REGPU_OPTIMIZE_SHADERS`,
  `REGPU_EARLY_FENCES`. Uncommitted in `plugin/` (not a git repo): the stack dumper, the
  frame-cap cvars, the 720p default.

## Safe early fences, prototype (2026-10-27)

`REGPU_EARLY_FENCES=1` is now a guarded mode, not just a measurement switch (ReGPU working tree,
uncommitted):
- Fences land when the CP reaches them (`RendererEarlyFences()` in `WriteBack`). The work before
  them is still submitted.
- Vertex fetch: ranges under 16 KB are always copied through the upload ring (the old default from
  when fences were written at record time). Larger ranges are read in place only while
  `DynamicBuffer` hasn't seen them change; once they change they're copied for 120 frames.
- Index reads in place (rect lists, and full-arena fallbacks) are refused for ranges
  `DynamicBuffer` has seen change, so they take the copying index path. The index arena itself
  already copies at record time.
- Default off: the XenonRecomp build and FM2 behave exactly as before. Both configurations build.

FH1 free roam, same setup as the profile runs (`run/safe-regpu`, `run/safe-visual`,
`run/fences_compare.png`):

| | fps | GPU util | Guest waits on GPU / 10 s | ReGPU CPU draw |
|---|---|---|---|---|
| Stock | ~83 | 81% | 1–8 ms | n/a |
| ReGPU, fences after host GPU | ~59 | 58% | ~3,800 ms | 6.3 ms |
| ReGPU, early fences, unguarded | 85.3 | 73% | 2–140 ms | 6.3 ms |
| **ReGPU, early fences + copies** | **80.4** | 74% | 132–917 ms | 8.2 ms |

- No new visual corruption in the standing-still scene. The existing ReGPU bugs (green glass,
  HUD pieces, flags, blur) are unchanged.
- The copies cost about 1.9 ms of CPU per frame (indices 0.5 → 1.1 ms, constants/vertex 2.5 →
  3.3 ms). The frame is now CPU-bound on ReGPU's draw path.
- **Remaining risk:** a large range read in place that the title rewrites *for the first time*
  isn't caught until `DynamicBuffer` next hashes it (up to 16 frames for ranges that look static),
  while host frames may still be reading it. World streaming into freed memory is the likely case.
  Closing that needs real write detection, for example a `Host` hook over ReXGlue's
  physical-memory write watches. Not tested: driving, races, streaming.

Driving (2026-10-28): from the save, RT held for 50 s from 100 s, stock vs ReGPU with early
fences + copies + optimised shaders (`run/drive-xenos`, `run/drive-regpu`,
`run/drive_compare.png`). The cars take different paths (the game isn't deterministic across
renderers), and both end against a barrier.
- No new corruption while driving or streaming; only the known ReGPU bugs.
- fps while driving: stock 79–83, ReGPU 78–111, except one 5 s stall at about 112 s.
- That stall wasn't fences: a vertex shader missing from the cache took **12.7 s** to compile
  with `REGPU_OPTIMIZE_SHADERS=1`, and the frame waited for it. Optimised fxc is only viable for
  shaders compiled ahead of time or in the background; draw-time compiles should stay
  unoptimised (or compile unoptimised first and swap in the optimised version later).
- Races not tested.

Background-optimised shaders (2026-10-28, ReGPU working tree, uncommitted):
`REGPU_OPTIMIZE_SHADERS=1` no longer optimises the compiles anything waits for. Every compile is
unoptimised and ready immediately, as without the switch. Each shader is then also built
optimised on one lowest-priority thread and stored as `<key>.o.dxbc`. Cache loads prefer that
file and queue the optimised build when it's missing. The running session keeps the bytecode its
pipelines were built from.
- Drive test, cold then warm cache (`run/drive-opt-cold`, `run/drive-opt-warm`): **no stalls**
  in either; the 12.7 s draw-time compile is gone. Background throughput is about 100 shaders per
  minute. After two runs, 346 of 348 shaders had optimised builds.
- **Measurement bug found:** the plugin set ReGPU's `userDir` from `InitializeShaderStorage`,
  which ReXGlue calls after `SetupGuestGpu`. Every ReGPU run so far therefore shared one shader
  cache and pipeline library in the exe folder (`fh1/.../windows/shadercache`), whatever
  `--cache_root` said. So the earlier "optimised shaders" runs mostly loaded unoptimised
  bytecode, and **that run's ~3 fps / 0.4 ms optimisation figure is not valid**. The early-fence
  comparisons stand: those runs shared the same cache. Fixed: the plugin now reads the
  `cache_root` cvar.
- Run with all shaders optimised (`run/drive-opt-hot`): 75–87 fps standing, 73–84 fps driving.
  But its pipeline library was cold (new location), so it opened with a 1.8 s stall and pipeline
  builds. The optimisation gain still needs a clean A/B: same warm pipelines, optimised vs plain.

Optimised vs plain shaders, clean A/B (2026-10-28): `cache-regpu-opt2` copied to
`cache-regpu-opt2-plain` (same shader set). Each copy was warmed once in its own mode, then
measured. Drive test from the save, early fences on, debug server off. fps averaged over 60–100 s
(standing) and 105–150 s (driving); ReGPU GPU time and draw time from `REGPU_PERF` after 70 s.

| Run | Standing | Driving | GPU time | CPU draw | Stalls >100 ms |
|---|---|---|---|---|---|
| warm, optimised | 80.7 | 80.8 | 8.36 ms | 7.14 ms | 0 |
| warm, plain | 77.0 | 76.6 | 8.90 ms | 7.79 ms | 0 |
| **measured, optimised** | **79.6** | **78.6** | 8.68 ms | 7.42 ms | 0 |
| **measured, plain** | **79.3** | **82.2** | 8.51 ms | 7.21 ms | 0 |

- **No measurable gain from optimised shaders in FH1.** The differences are within run-to-run
  noise (about ±3 fps, ±0.4 ms). The frame isn't GPU-bound (8.5 ms of GPU in a ~12.5 ms frame),
  and FH1's translated shaders don't get meaningfully faster with fxc's optimiser.
- Keep `REGPU_OPTIMIZE_SHADERS` opt-in (off by default). The background design is still worth
  keeping for a GPU-bound title.
- What limits ReGPU in FH1 now is the CPU side: ReGPU's draw path (~7.2–7.8 ms per frame) plus
  the CP and guest.

## Visual bugs (2026-10-28)

Found by binary search over tinted pixel shaders (scratchpad `bisect_ps.py`), then live
output edits (`probe.py`).
- **Fixed (ReGPU `efb0787`):** Xenos `_AS_16_16_16_16` texture formats (50–56)
  weren't recognised. They store their base format's bytes, so `TextureFetch::Format()` now maps
  them to the base. This restored the HUD (speedometer ring, icons, minimap pin), the car badge
  and plate detail, the ground textures, and removed the scene-wide blur
  (`run/format_fix_compare.png`).
- **Green glass (open):** the glass pixel shader `d46f6f62d4f15ffb` refracts the scene resolve
  at `1C4E1000` (2_10_10_10). Its colour term `r1` is green. Before the fix, the car-interior
  region of that resolve was solid magenta; after it, the interior shows through with a green
  tint. Next: check how ReGPU resolves to and samples 2_10_10_10, which it has no texture decoder
  for, so only the resolve path covers it.
- **Purple sky shapes (open):** moving objects drawn over the sky, last coloured by the sky/cloud
  pass `299007422ef6aee0` (DXN cloud textures). The stock renderer shows no such shapes.

## Next steps

1. Build FH1 (`forza-recomp`) against `sdk/reiglue` with the user's game copy. Record the
   D3D12/Vulkan baseline (`tools/perf_scenario.sh`).
2. Copy `rexgpu-regpu.dll` beside the exe and run with `--gpu_plugin=regpu --gpu_backend=d3d12`.
   That's gate 1, which answers open questions 2–5.
3. Re-run FM2 on the ReGPU branch.
