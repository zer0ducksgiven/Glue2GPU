# FH1 spike: ReGPU as a ReIGlue GPU plugin

## Overnight results (2026-10-28, read this first)

FH1 base game from the user's save, RTX 4070, stock ReXGlue/Xenos D3D12 against ReGPU through the
plugin (early fences on). fps averaged over 60–100 s standing and 105–150 s driving
(`run/<name>/forza.log`, `FORZA_FPSLOG`); GPU busy from `nvidia-smi`.

| Resolution | Stock standing / driving | ReGPU standing / driving | GPU busy (stock / ReGPU) |
|---|---|---|---|
| 1280×720 | 97.4 / 97.1 | 95.0 / 95.9 | 78% / 71% |
| 2560×1440 | 67.4 / 66.3 | **91.1 / 92.5** (+37%) | 96% / 96% |
| 3840×2160 | 36.3 / 35.3 | **79.1 / 86.1** (2.2–2.4×) | 99% / 100% |

(Final runs with every fix in, `run/final-*`. No crashes in any run. A 20-minute ReGPU drive soak,
`run/soak`, averaged 102 fps with a minimum of 84.5, and had no hang or crash.)

At 720p both renderers are limited by the game's CPU side. Once the renderer is the limit, ReGPU is
much faster. The 4K screenshots are both real 3840×2160 (`run/k4_detail.png`).

Visual fixes tonight (ReGPU branch `rexglue-driver`, commits after `8606951`):

| Bug | Cause | Fix |
|---|---|---|
| HUD missing, car detail and ground textures wrong, scene blur | `_AS_16_16_16_16` texture formats (50–56) unsupported | Decode as their base formats (`efb0787`) |
| Green car glass | Live cube reflection's ±Y faces exchanged against stock | Opt-in `REGPU_CUBE_Y_SWAP`, set by the plugin (`550e104`); root cause not found |
| Purple shapes in the sky | Resolve clear value read as ARGB 8888 for every format (FH1's 2_10_10_10_FLOAT scene was cleared to `C8521485`, a grey in 7e3) | Clear decoded per colour format (`90accac`) |
| Whole scene blurred standing still | Speed blur reads stencil via an 8888 fetch of the depth resolve; ours held depth only | Depth resolves also keep the D24S8 words (`25b3309`) |
| Crashes under RenderDoc / copy path | Shader scan and vertex uploads read ReXGlue's no-access guest pages | Readable-memory checks (`90accac`, `c30d875`) |
| 60 fps cap | Plugin left ReGPU's vsync on (FH1 has no vsync setting) | vsync off by default, like stock (plugin `e8d3d1c`) |

Comparisons: `run/format_fix_compare.png`, `run/cube_fix_compare.png`,
`run/clear_fix_compare.png`, `run/pack_compare.png`, `run/menu_compare.png`.

Still different from stock:
- **Update (FX): car glass fixed.** The plugin's REGPU_CUBE_Y_SWAP default had become wrong after the pitch and depth fixes; dropping it gives rear-window (35,47,59) against stock (30,38,44), was (106,80,56).
- **Car glass too bright.** The interior shows through, but the reflection is warm and bright
  (window ~(139,112,80) against stock's (28,36,42)). Stock takes the same cube-reflection branch
  with the same bools. The reflection inputs need a step-by-step comparison against stock.
- **Update (FX, later): fixed by turning both gamma switches on.** `REGPU_TEXTURE_GAMMA=1` +
  `REGPU_RESOLVE_GAMMA=1` together (not texture gamma alone, which is what went too dark) bring
  the festival to stock's look: mean luma 85 vs stock 87 (was 118), walls 28 vs 19 (was 59),
  stddev 66 vs 74 (was 58). The plugin turns both on by default. The original notes follow.
- **Contrast slightly lower** (menu luma stddev 53.7 against 59.2; same mean). Xenos gamma-signed
  fetches aren't applied by default. With them on (`REGPU_TEXTURE_GAMMA=1`) the image goes far
  too dark. The split modes show resolves carry all of it (mode 5); colour maps change nothing
  (mode 4).
- **Weak sun shadows: where it stands (captures `run/rd-shadow`, traces `run/sh-*`).** Lit ground
  matches stock; shaded surfaces are ~10 levels too bright (stand wall 27 vs 17). Not the atlas
  resolution (1440p same), not today's depth/surface/tile-merge switches (live A/B, unchanged).
  The wall shader's inputs match stock except the screen-space sun-shadow mask: stock 0.45 (in
  shadow), ReGPU 1.0, and ReGPU's whole mask is white. The mask is built by cascade boxes (MIN
  blend, stencil == 0 then increment: the first box containing the pixel wins). At the wall the
  cascade-2 box wins (correctly: box depth 0.012 vs scene 0.042) and its shadow map lookup reads
  1.0 everywhere (texture 4535, a 520x520 resolve of the atlas taken right after a depth clear of
  that region). The 1024x1024 atlas is drawn as a tall pass: each frame the full-atlas cascade
  plus one of the four 512x512 quadrant cascades in rotation (TR, BL, BR, TL), each quadrant
  resolved to its own address; casters use the whole-atlas scissor and the quadrant's viewport.
  **Stock compared** (`stockmask.txt`, `cmpmaps.txt`, `maps.png`): the mask pass is the same on
  both, three near-cascade boxes (no blend, each on its own 1024x1024 map: guest 1D295000,
  1CE95000, 1CA95000) then 8 MIN boxes on the rotating quadrant atlas (1D6C5000). The atlas and
  the first and third near maps match stock. **The second near map (1CE95000) differs**: stock
  6831 has casters in its top-left 520 region, ReGPU 4535 has the region empty. ReGPU's frame
  trace (`run/sh-cap2/capture.log`): after the full-atlas resolve the game draws a 40-px strip
  clear (pitch 1040) and a depth clear through a 4x MSAA view of the same EDRAM (pitch 520, rect
  240x256, depthctl 0x76: z write, always), then resolves 0..520 to 1CE95000 with no casters in
  between. ReGPU applies that clear to the atlas's top-left 480x512, so the map is empty and the
  wall is lit. Stock keeps the casters there (from the full-atlas cascade). Experiment (temporary
  switch, reverted): skipping every 4x-view depth clear brings the wall to 18.7 (stock 16.9) but
  over-darkens lit ground (22.7 vs 45) and leaves a seam on the car. **Correction, and the real
  cause (fixed):** stock's near map is cleared there too (8x8 value grids, `grid.txt`); its wall
  pixel is also "lit" by the near cascade. Stock's pixel history (`stockwin.txt`) shows the
  difference: after the near boxes, a stencil reset (event 19011) lets the far MIN-blended boxes
  reach every pixel, and the wall ends at 0.45. The game sends that reset as a 640x360 rectangle
  through a 4x MSAA view (pitch 640) that shares only the mask pass's depth base; ReGPU's MSAA
  rectangle scaling matched views by colour base only, so it reset a quarter of the stencil.
  ReGPU now records each depth base's native layout and scales such rectangles
  (`REGPU_NO_DEPTH_VIEW_SCALE=1`: as before). Stand wall 26.9 -> 21.8 (stock 16.9), sunlit ground
  unchanged. The car's own shadow beside it is still lighter than stock (54.8 vs 35.8): a separate
  map. **Car shadow fixed:** the car body was missing from the near cascade (1D295000). Its
  casters (ps 686394d59122cfc1) sample slot 0, whose fetch constant the game leaves unbound (type
  0, `[0 0 0 06280000 C03 18]`); ReGPU kept the alpha test in depth-only passes for any draw whose
  pixel shader samples a texture (an FM2 fix), and the unbound slot reads 0, so every pixel was
  discarded. Xenia doesn't run the pixel shader in depth-only mode at all. An unbound slot no
  longer counts (`REGPU_DEPTH_ALPHATEST_EMPTY=1`: as before). Live A/B while driving
  (`run/car-fix2`): the map holds the whole car with the fix and only the spoiler without; the car
  shadow on the ground now matches stock's. In photo mode the car uses another shader with a
  bound texture (unchanged). (The earlier note follows:) the same clear pair
  before the 1CA95000 map (followed by its casters) is needed. Open: how the hardware (and Xenia)
  treat a depth write through a 4x view of a 1x surface: likely not the plain 2x2-per-pixel
  overwrite ReGPU does. Next: read Xenia's render-target cache for MSAA-mismatched depth views
  (ReIGlue src/graphics/d3d12/render_target_cache.cpp, ownership transfer), and match it.
- **Festival ground and walls brighter than stock** (wall 51 against 14). The sun-shadow mask
  matches stock at those points (ground 255/255 in both). A broken shadow atlas was fixed
  (`36e90b4`, opt-in `REGPU_RESOLVE_IMAGE_BASE`, set by the plugin), but it didn't change this.
  Likely the same as the contrast item: FH1's final composite reads a plain-8888 resolve
  (`1D728000`) with gamma signs (`sg3F`), which on the hardware darkens it; stock is still the
  brighter one. Needs a matching stock capture of that pass.
- **Title Update 4** (the user's `tu00000007_00000000`, 0.0.4.7) builds with FH1's TU4 hints and
  runs on stock at 93.8 / 92.5 fps. On ReGPU it first froze entering the world, or crashed on the
  render thread with a null read at the same code (`8249E108`). The game reads resolved images on
  the CPU. The plugin now implements `Host::resolveWriteback` with stock's `readback_resolve=some`
  policy (an image the first time it's resolved, and again after 60 unused frames;
  `GLUE2GPU_WRITEBACK=some|all|now|none`). With it TU4 runs without freeze or crash. It first
  managed only 60.8 / 58.9 fps: the early-fence index guard sent TU4's rewritten index buffers
  through the CPU's index expansion (7 ms a frame). Limited to rect lists (ReGPU `HEAD` of the
  branch), **TU4 runs 91.7 / 92.5 fps against stock's 93.8 / 92.5**. The base game is unchanged
  (93.8 / 94.1). A 10-minute TU4
  drive soak (`run/tu4-soak`, before the index fix): no crash, freeze or stall over 1 s; 67.3 fps
  average, 58.8 minimum. TU4 at higher resolutions: 1440p ReGPU 90.6 / 88.1 vs stock 66.9 / 67.4;
  4K ReGPU 83.5 / 80.5 vs stock 36.3 / 35.2.
- **Open: TU4 varied-route soak is slower than stock.** The same 10-minute route after the index
  fix: ReGPU 64.9 avg / 57.6 min (`run/tu4-soak2`) vs stock 87.9 / 83.4 (`run/tu4-soak-xenos`).
  `REGPU_PERF` on that route (`run/tu4-soakprof`, 58.6 fps): GPU time only 5.8 ms; the renderer's
  draw loop takes 12.7 ms, of which **constants 8.7 ms, almost all texture fetch constants
  (6.95 ms)** at ~3400 draws/frame (slow frames reach 16–18 ms there). This is CPU-bound
  fetch-constant processing (likely per-draw texture lookup/descriptor work), the next thing
  to optimise. Note the `fetches` bucket is the *vertex* fetch loop (DynamicBuffer check +
  UploadGuest of small buffers). Ruled out tonight: the copy threshold (`REGPU_DIRECT_FETCH_KB=4`,
  `run/tu4-fetch4k`: 7.5 ms, unchanged) and DynamicBuffer's hashing (sampling buffers >64 KB,
  `run/tu4-samplehash2`: 7.6 ms, unchanged; reverted). DynamicBuffer tracks ~57k large buffers on
  this route but only ~100 are copied. Next step: a sampling profiler (e.g. VTune/Superluminal)
  on that loop rather than more guessing.
- Plugin build note: `build/plugin/rexgpu-regpu.dll` must be copied into
  `fh1/forza-recomp/out/build/<build>/` after a build.
- TU4 on ReGPU renders correctly overall (`run/tu4_compare.png`). As in the base game, sun
  shadows on the ground are much weaker than stock's crisp tower shadows. ReGPU also implements no
  polygon offset (`PA_SU_POLY_OFFSET_*`), though missing depth bias would add shadow, not remove
  it.
- **The base-game freeze** from earlier is very likely the same thing (same code, same point,
  intermittent there). The write-back policy covers both builds.
- FM2 wasn't re-run: it keeps its renderer in-tree, so tonight's ReGPU branch can't be tested
  there without porting. `90accac` changes 8888 clears to Xenia's channel order (red in the low
  byte), and FM2 could see that.

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
- **Green glass: fixed by `REGPU_CUBE_Y_SWAP` (2026-10-28, ReGPU working tree; the plugin turns it
  on).** Compared against a RenderDoc capture of the stock D3D12 build (`run/rd-stock`, scripts
  `list.py`, `bools.py`, `cube.py`, run through `qrenderdoc --python`):
  - The stock glass draws (events 6960/6964) have the **same PS bools** as ReGPU
    (`xe_bool_constants[1].x = 0x211`: 128, 132, 137). Both take the live-cube reflection path.
    ReGPU's bool decode and writes are correct.
  - Stock cube faces at their centres, linear ×16: +X 0.094, −X 0.047, **+Y 0.375/0.25/0.078
    (warm)**, **−Y 0.28/0.36/0.375 (cool)**, +Z 0.063, −Z 0.28. ReGPU has X and Z the same and
    **±Y exchanged**.
  - Xenia's cube-coordinate conversion (`dxbc_translator_fetch.cpp`) is identical to ReGPU's
    `CubeDirection`, and both read the faces from `base + i × stride` in the same order. All six
    faces are resolved (order 0, 4, 2, 1, 3, 5, bias −4). So ReGPU's *resolve* of the title's ±Y
    faces holds the other face's view. Root cause unknown.
  - The fix copies resolved faces 2/3 into slices 3/2 when assembling a live cube. Behind an opt-in
    `LiveSwitch` so FM2 is unchanged; the plugin sets it for ReXGlue hosts.
  - Result (`run/cube_fix_compare.png`): no green; the interior shows through. The window is still
    brighter and warmer than stock: (129,102,73) vs (16,26,36).
  - Also in the working tree: biased 2_10_10_10 resolves kept at 16-bit float (didn't fix the
    glass; more precise), and the `ps HASH @ANCHOR|STATEMENT` probe.
- **Purple sky shapes (open, 2026-10-28):** flat purple polygons in the upper frame that move and
  come and go between frames; stock shows none. Not found yet:
  - Output edits of the sky/cloud pass `299007422ef6aee0` (the earlier single-pixel bisect hit)
    don't change them, so that hit was the shapes moving during the bisect.
  - A bisect that counts purple pixels over several shots (scratchpad `bisect_count.py`):
    tinting the first 80 of 161 pixel shaders dropped the count 2433 → 383 once, but no smaller
    group came close again. So either several shaders draw them, or the colour doesn't come from
    a pixel shader's output (blending with a bad destination, or a stale or garbage texture
    sampled by many shaders).
  - Next: a deterministic single-frame method, e.g. a RenderDoc capture of ReGPU's own frame,
    or ReGPU's frame dump with a per-draw skip, then the draw's vertex data. The shape and the
    flicker suggest bad vertex positions in particles or streamers.

## Next steps

1. Build FH1 (`forza-recomp`) against `sdk/reiglue` with the user's game copy. Record the
   D3D12/Vulkan baseline (`tools/perf_scenario.sh`).
2. Copy `rexgpu-regpu.dll` beside the exe and run with `--gpu_plugin=regpu --gpu_backend=d3d12`.
   That's gate 1, which answers open questions 2–5.
3. Re-run FM2 on the ReGPU branch.

### Grey sky shapes (TU4, festival, live debug session `run/sky-debug`)
- They move while the camera is still (animated, not static geometry), have hard triangle edges,
  and the cloud/haze layers (`ps 299007422ef6aee0`, `c99dc898d92fe36c`: 8.7k/9.3k-vertex meshes
  sampling DXN) do not cover them: those layers fail there. Turning both layers off removes the
  clouds but not the shapes (they get darker).
- No single scene pixel shader's red tint reaches them: not the sky dome (`29c8f1947b280cc3`),
  not any blended shader, not any opaque one except full-screen post passes. Dropping the
  z-prepass vertex shaders and forcing vertex copies (`REGPU_NO_DIRECT_FETCH`) don't remove them.
- So they are likely drawn into an intermediate target that a post pass composites (a
  light-shaft/sun-occlusion or sky-occlusion buffer, which also fits the dark noisy square near
  the horizon), or they are depth written outside the scene pass.
- **RenderDoc pixel history** (`run/rd-tu4-sky`, `fh1_capture_2`, pixel 1050,50) corrected that:
  the shape pixel is written by an ordinary main-pass ground draw (event 32607, guest
  `vs 318016058c8ed5a2 ps 5624eb5f90520974`, triangle strip, 10758 indices, depth GreaterEqual,
  brown 0.27/0.25/0.20 at depth 0.087), then resolved (event 33512); the clouds after it fail
  depth there. Its primitive 666 has two vertices in front of the camera already projecting
  above the top of the screen and one behind it (w = -1): a ground triangle whose vertices are
  wrong, stretched up past the camera. The VS reads its position as 32_32_32_FLOAT (fetch slot
  95, stride 8 dwords) through the object and view-projection constants: nothing exotic.
  (The earlier tints missed it because they were judged by colour in a band the stage also covers.)
- Stock shows no shapes at the same spot (`run/stock-sky`). Ruled out on ReGPU, each live or by
  re-run: user clip planes (FH1 never enables them: PA_CL_CLIP_CNTL is always 0x00090000,
  CLIP_DISABLE + DX clip space; an implementation was tried and reverted), in-place index reads
  (`REGPU_NO_DIRECT_INDEX=1`), in-place vertex reads (`REGPU_NO_DIRECT_FETCH=1`), and early fences
  (unset: `run/nofence-sky2`). Note `REGPU_EARLY_FENCES=0` still turns them on (presence check).
- That draw's vertex fetch constant (slot 95) is sane: type 3, plausible physical addresses
  (`12D222E0`, `12C78D80`...), sizes 1-97 KB, stable across frames (`run/vfetch-trace`).
- Also ruled out: vertex fetch bounds (`REGPU_NO_FETCH_BOUNDS=1`, `run/bounds-test`), and the Xenos
  vertex index handling Xenia does and ReGPU doesn't (24-bit mask after VGT_INDX_OFFSET, clamp to
  VGT_MIN/MAX_VTX_INDX, from dxbc_translator.cpp): implemented, A/B'd live (`run/clamp-test`), no
  change, reverted.
- The shapes always appear next to the stage confetti and move like it.
- **Primitive 666 dumped** (`run/rd-tu4-sky/dump666b.txt`, `consts.txt`): the draw is a triangle
  list built from ReGPU's expanded strip (restart on), with copied vertices. Its indices
  (505, 487, 457) sit among normal neighbours and inside the buffer; the vertices are plausible
  (a 130 m sheet of 2670 vertices at y ~ -9; these three within ~9 m of each other, ~2 m from
  the camera); the constants match the neighbouring draws, and pushing vertex 505 through them
  by hand gives the post-VS position RenderDoc shows. Viewport: vscale (640, -360, -1), voffset
  (640, 360, 1): by the game's own matrices the sliver lands where ReGPU draws it.
- **Stock** (`run/rd-tu4-stock-rtv`, captured with `--render_target_path_d3d12=rtv` so draws are
  visible): stock renders the scene in 1280x512 tile passes; in the top tile's pass only the sky
  dome and cloud layers touch pixel 1050,50, so stock has no ground there in that frame. (Its
  history at that pixel also shows a lower tile's ground at depth ~0.02, failing depth.)
- More ruled out: depth clip following CLIP_DISABLE as Xenia does (tried, no change, reverted),
  tile merging (`REGPU_NO_TILE_MERGE=1`, `run/tilemerge-test`, still there). ReGPU skips no draws.
- **Guest draw streams diffed** (`run/dt-stock`, `run/dt-regpu`: every triangle-strip DRAW_INDX
  logged at the PM4 packet on both sides, temporary trace since reverted): the game submits the
  same strip draws at the same per-frame rate on stock and ReGPU (no index count / vertex-buffer
  size pair differs by more than 0.15 draws a frame; addresses differ between runs, as the game's
  allocations do). The sheet is a 4878-index strip (plus a 142-index one) on a 21360-dword vertex
  buffer, submitted once a frame on both, never viz-query-conditional. So **stock submits that
  sheet too**; the difference is in how it is rendered, not what the game sends. (Note: the
  capture log's `n` is ReGPU's expanded list length, not the guest index count.)
- Stock's RenderDoc history shows that same 4878-index draw (event 3616) touching pixel 1050,50
  of its tile target with fragment depth 0.015 and failing depth there; ReGPU's fragment at the
  same screen pixel has depth 0.087. (That turned out to be a different tile; see below.)
- **Stock's tile passes mapped** (`run/rd-tu4-stock-rtv/toptile.txt`, from Xenia's xe_ndc_scale /
  offset): in a full frame the scene runs in three passes on one 1280x512 EDRAM target: top tile
  (screen rows 0-256, ndc y scale 1.0) from event 22422, rows 256-512 (scale 1.55) from 25080,
  rows 512-720 (scale 3.46, 208-row viewport) from 33016. (The first captured frame starts after
  its top tile, which is why the earlier history mixed tiles.)
- **At the same screen pixel (1050,50), stock draws the sliver too** (`topfrag.txt`): its top
  tile's sheet draw (event 23357, 4878 indices) writes *primitive 666*, ground colour, depth 0.043;
  the 1058-index draw after it (primitive 351) fails depth, exactly as on ReGPU (33463, primitive
  351). Stock's scene resolve (texture 14292 / 8952) holds the sliver's colour there.
- **Stock removes it in post** (`later.txt`, `p37300.txt`): the final full-screen composite
  (event 37300, no depth/stencil test) reads the scene (8952, which has the sliver), a 320x192 bloom
  buffer (8976), a 640x360 R8G8B8A8 buffer (13513, a resolve at event 36753; it reads
  0, 0.498, 0.498, 0 there) and a 16x16 table (9046), and writes sky (0.805, 0.871, 0.871) at that
  pixel. So the shapes are geometry the game itself draws into the scene and then hides in
  post-processing; on ReGPU that post step doesn't hide it.
- **Correction: ReGPU hides it the same way.** ReGPU's composite (guest `ps 60de2277c4f1b1dd`,
  events 38831/38835 in `fh1_capture_2`) has the same four inputs (scene, 320x192 bloom, 640x360
  8888, 16x16 table) with matching values at 1050,50 and writes sky there too. The shape picked for
  the first pixel history came from a stale swap buffer (285, a frame before the capture): all of
  capture 2's own frames show sky at 1050,50. Earlier conclusions about 32607 still stand (both
  sides draw the sliver), but that frame wasn't a broken one.
- Frames that do show a shape inside a capture: `run/rd-tu4-sky/in_*.png` (`inframes.txt`), e.g.
  capture 1's frame copied at event 27920, shape at ~1020,70. There the composite (event 27395)
  has the same inputs at the pixel (scene 0.068/0.063/0.051, a 0.25; 640x360 0.498/0.498; bloom
  similar; only the 16x16 table differs, 0.843 vs 0.91) but outputs the shape (0.59, 0.558, 0.511)
  instead of sky. The composite (`run/comp-debug/comp.hlsl`) takes several jittered scene samples
  (offsets from a frac() hash and pc[42]) and picks between them by exact alpha equality
  (`r3.w != r7.w` ...), then grades through a texture (slot 7).
- Ruled out: 2-bit alpha quantisation of k_2_10_10_10_FLOAT targets (stock's resolved scene alpha
  reads 1/3, ReGPU's 0.25; quantising at the output was tried, A/B'd live in `run/alphaq-test`,
  shapes unchanged, reverted).
- The composite's sample selection is by exact alpha equality, and stock's resolved scene alpha is
  1/3 where ReGPU's is 0.25: the scene resolve's destination is k_2_10_10_10 (texture format 7),
  2 bits of alpha, but ReGPU's copy kept 8/16. **Rounding alpha to 2 bits in the resolve**
  (`REGPU_NO_RESOLVE_ALPHA2=1` to A/B) cut the shapes clearly in a live A/B (`run/alpha2-test`:
  most frames clean, the rest with smaller shapes right at the stage confetti, against shapes in
  nearly every frame without it). Rounding the draw output on 2_10_10_10_FLOAT surfaces too
  (formats 3 and 12, `REGPU_NO_ALPHA2`) is built but its A/B wasn't finished (moved to the FX).

### FX machine (FX-8350, GTX 970)
- Reached over SSH (key auth). FH1 TU4 copied to a local folder
  (build, game, tu4, sdkbin, profile-save; 8.3 GB). Launched by the interactive scheduled task
  `FH1Test` (`fh1-run.cmd`, settings in `run\env.cmd`), driven from here with
  `scratchpad\fxdrive.ps1` (debug server tunnelled to local port 7351).
- **Stock runs** on the FX (30 fps in free roam). **ReGPU froze** there entering the world on its
  first run: no VdSwap after #952-956, command processor idle on an empty ring (the title waiting
  on the GPU), while early draws waited ~30 s on first-boot shader compiles. With
  `GLUE2GPU_WRITEBACK=all` it got into the world but crawled at 1 fps on first-boot pipeline
  builds (the GTX 970 driver is slow to build them).
- **With caches warm, ReGPU runs on the FX**: default write-back policy, 33-34 fps in free roam
  (`run/fx-some-warm`), stock 30 fps (flat, apparently capped). So the freeze was first-boot only
  (shader and pipeline builds stalling the title), and `GLUE2GPU_WRITEBACK=all` is far too slow
  there (1 fps). First boot on a slow PC is still a problem for anyone else: a precompiled cache
  or a less blocking first boot is needed.
- **Alpha rounding A/B on the FX** (`run/fx-alpha-ab`, both switches, 8 pairs): off, shapes in
  nearly every frame; on, about half, smaller, mostly at the stage confetti. Same as on this PC.
  Committed to ReGPU `rexglue-driver` (switches `REGPU_NO_RESOLVE_ALPHA2`, `REGPU_NO_ALPHA2`).
  The rest isn't blended alpha: keeping blended draws from writing alpha on those surfaces
  (a temporary test switch, `run/fx-blendalpha-ab`) left the shapes as they were.
- **First-boot freeze fixed.** Cold cache on the FX, the plugin's old defaults: hangs at swap
  ~950-1050 every time (`run/fx-cold-*`), also with small resolves always written back. With
  `REGPU_ASYNC_SHADERS` alone it still waited 20 s on a pipeline and hung; with
  `REGPU_ASYNC_SHADERS` + `REGPU_ASYNC_PIPELINES` it reaches free roam (30-34 fps,
  `run/fx-cold-asyncall`). The plugin now turns both on (through a new
  `regpu::driver::SetSwitch`: `REGPU_ASYNC_SHADERS` is read at library load, before the plugin's
  environment writes), verified cold with no settings (`run/fx-cold-default2`). Risk to watch: a
  one-off draw skipped while its shader builds stays missing (nothing visible in free roam).
- **Grey sky shapes: root cause found and fixed.** The scene render target is stored rotated 180
  degrees (the composite samples it at 1 - uv), so the pixel traced earlier (1050,50 in the target)
  was ground, not sky. Debugging the composite in RenderDoc showed the shape frame's scene image
  itself holds the shape. At the right target pixel (259,649), the sky dome and cloud draws are
  *depth-clipped* in every pass, leaving the clear colour (0.26, resolved at 1/4: the grey).
  ReGPU's vertex shader divided z by w and multiplied it back; the sky dome sits on the far plane
  (z = w) and the round trip put fragments a hair past it, so they were clipped as the camera
  moved. Depth is now homogeneous (z * scale + offset * w, as Xenia), plus no depth clipping
  under CLIP_DISABLE. FX A/B (`run/fx-depthw-ab`): old depth 4 of 8 frames with shapes, new 0 of
  8; plain run 0 of 8 (`run/fx-depthw-confirm`). The alpha rounding committed earlier wasn't the
  cause (it reduced the shapes for reasons not understood, likely noise between runs); it stays as
  hardware-accurate behaviour with its switches.
- **Photo mode's dark quadrant reproduced on the FX** (`run/fx-photo8`, `fx-photo9`): the scripted
  route is START, DOWN (one press moves two items), A, A at 125-134 s (`fxdrive.ps1 -PressExtra`).
  The quadrant right and below the screen centre lacks the haze the other three have. Not
  `REGPU_RESOLVE_IMAGE_BASE` (A/B, unchanged) and not tile merging (`REGPU_NO_TILE_MERGE`, unchanged).
  **Fixed.** Photo mode freezes a scene image when it opens (built during the "PLEASE WAIT"
  frames, then copied in every frame by a 48-vertex strip draw), so A/B switches flipped after
  entry never changed it (the earlier A/Bs were invalid). RenderDoc on the FX
  (`renderdoc` copied there, task `FH1RD`, captures `run/rd-photo/entry2.rdc`) showed the build:
  a full-screen pass into the scene surface, then a 640x360 pass (scissor 0,0-640,360) into the
  *same* host texture. ReGPU keyed colour surfaces by EDRAM base only; in EDRAM a 640-pitch
  surface at that base lays its tiles out differently and doesn't overwrite the 1280 scene's
  top-left quarter. Colour surfaces are now keyed by base and pitch (`REGPU_SURFACE_BY_BASE=1`:
  as before). FX: photo mode even (`run/fx-photo-pitch`), free roam unchanged at 33 fps.
- Note for the FX: `regpuctl press` does nothing under ReXGlue (ReGPU's debug pad isn't wired);
  input has to go through `FORZA_AUTOPRESS`.
- **Correction: photo mode's haze was the bug, the dark quadrant the right picture (fixed).**
  Against stock on the FX (`run/fx-rg-stock`) ReGPU's photo view was veiled all over (stand 111 vs
  49, no car shadow to speak of). The game draws texture-less rectangles (one blended, 07060141)
  through a 4x 640x360 view of its 1x 1280x720 scene; in EDRAM that view covers the same samples.
  Keyed by base alone the rectangle landed unscaled in a quarter (the "dark quadrant", which was
  the correct look); keyed by pitch (yesterday's fix) it missed the scene altogether. Colour
  surfaces are now keyed by layout, base and row width in samples (`REGPU_SURFACE_BY_PITCH=1`:
  by pitch). FX photo view (`run/fx-ph-fix2`): car shadow 41.6, ground 145.5/73.6, stand 50.5 vs
  stock 36.5, 152.2/73.3, 49.4. Free roam unchanged (`run/fx-rg-fix`).
  Testing photo mode: the scripted DOWN must be two short presses (`128:DOWN:0.15,129.5:DOWN:0.15`;
  a 0.3 s hold moves one or two items with the frame rate). Then B leaves photo mode and A
  re-enters it with a rebuilt image, so switches can be A/B'd in one run (`fxphoto.ps1`).
- **Remaining shadow gap fixed: the display's PWL gamma ramp.** A fresh ReGPU capture on the FX
  (`run/rd-shadow2`, `wall.txt`) shows the wall pixel's sun-shadow mask now matches stock (0.436 vs
  0.451, same box sequence), and the whole mask looks the same (`masks.png`), so the shadows were
  right and the darks were not. FH1 resolves its front buffer as k_2_10_10_10, for which the display
  (and Xenia) uses the piecewise-linear gamma ramp (DC_LUT_PWL_DATA), not the 256-entry table; ReGPU
  only had the table, which FH1 leaves at identity. FH1's PWL ramp darkens the low end (1/16 to
  0.017). ReGPU now records it and grades a 2_10_10_10 front buffer with it (`REGPU_NO_GAMMA_PWL=1`:
  the table). FX, festival parked (`run/fx-pwl1`): stand wall 16.8, lit ground 46.5, car shadow
  36.7, sky 207.8 vs stock 16.6, 44.9, 36.4, 209.2 (was 21.5, 48.1, 40.3, 198.0). Photo mode
  (`run/fx-ph-pwl`): 38.7, 154.1, 75.5, 49.2 vs stock 36.5, 152.2, 73.3, 49.4.
- **Overnight 2026-10-29: parity survey and performance.**
  - Survey, stock vs ReGPU on the FX (`run/fx-sv-*`, `run/fx-dr-*`, `run/sv_all.png`,
    `run/dr_all.png`): title screen, festival parked at five times, a 45 s drive, photo mode. No
    differences left beyond streaming/timing (a Ferris wheel showing earlier on ReGPU in one run).
    This PC (`run/fin-*`): parked wall 16.8/16.6, ground 46.8/45.0, sky 208.7/207.9; photo mode
    car shadow 38.8/36.8, stand 49.3/49.9 (ReGPU/stock).
  - **Early fences on by default** (plugin `0e975aa`): FX free roam 18 -> 34 fps (stock 37), same
    picture. `GLUE2GPU_NO_EARLY_FENCES=1` opts out.
  - **Forward-only control flow without the dispatch loop** (ReGPU `40c0f90`): GPU time on the
    GTX 970 30.9 -> ~29 ms. The FX is CPU-bound after it (ReGPU draw ~19 ms of a 29 ms frame,
    3,700 draws); draw workers (25 fps) and the draw thread (34) don't help there.
  - **The zero-copy mapping failed on this PC in TU4 runs** (half of all local runs since 10-28,
    the copy path: vertex uploads 7.3 ms a frame, 63/72 fps against stock 96/95). FH1 TU4 makes the
    64 KB page after its minimap geometry ring (guest AA860000) PAGE_NOACCESS; present before the
    renderer starts, it made every heap call fail with E_INVALIDARG. ReGPU now makes such pages
    read-write while it opens the heap and restores them (`53d77ce`; `REGPU_NO_MAP_UNGUARD=1`),
    also tries ID3D12Device3 after ID3D12Device13 fails, and retries a failed mapping later.
    This PC: 94.4 standing, 98.8 driving (stock 96.5, 94.9). FX unchanged (mapped, 34 fps).
  - **Minimap route and arrow flicker: fixed.** The same guard page: the ring's last block
    (0A855000, 48 KB) ends 4 KB into it, so on the copy path the upload was zeroed whole and the
    route and arrow vanished every time the game used that block. The readable start is copied now
    (`REGPU_UPLOAD_ZERO_UNREADABLE=1`: all zeros). Copy path forced (`run/flick15`): 0 of 113
    shots on that block without the route, 43 of 43 with the old behaviour. With the mapping (now
    the norm) the GPU reads the block in place.
  - **Sky flickering grey: fixed by turning tile merging off for FH1** (plugin). Moving, about 1 in
    100 shots had most of the sky at the clear colour (a band of sky left in some). Not depth
    clipping (a no-clip debug switch and a depth-plane snap left it), not async pipelines (no waits).
    `REGPU_NO_TILE_MERGE=1`: 0 of the flagged frames grey (`run/flick22`, `run/flick23`: 999 shots,
    none grey), against most flagged frames grey with merging (`run/flick19`, `run/flick20`). The
    merge keys later-tile draws by packet and fetch constants, not shader constants; the dome's
    widened first-tile draw evidently doesn't cover what the later tiles' (dropped) draws did. Cost:
    FX 33.6 -> 31.4 fps, this PC none. Open: make the merge key safe instead (`GLUE2GPU_TILE_MERGE=1`
    turns merging back on). Keying later-tile draws on the first 16 vertex constants as well was
    tried and doesn't fix it (`run/flick24`). The same merge also cut off the top of the tall stand
    tower in some frames (the "Ferris wheel showing on ReGPU" difference in the survey): whole with
    merging off (`run/vf_cmp.png`). Narrowed further: with merging on and `REGPU_TILE_MERGE_KEEP=1`
    (the first tile still widened, every later-tile draw kept) the sky stays blue (`run/flick25`),
    so the widening is fine and dropping some later-tile draw is the fault. Adding the vertex and
    pixel shader hashes to `DrawKey` does not fix it (`run/flick26`): a later-tile draw the key
    calls identical to a first-tile one still has to run in its own tile. Next: bisect the dropped
    draws with `out/tile_keep.txt` over frames that go grey. **Root cause found and fixed in ReGPU**
    (`2d31115`): bisecting the kept range pointed at several ranges, and the common factor is the
    stencil. The sky draws where its stencil marks allow; a later tile's stencil-marking draws were
    dropped as duplicates of the widened first-tile ones, whose marks don't survive in that tile.
    Later-tile draws with the stencil test on are now always drawn (`REGPU_TILE_MERGE_DROP_STENCIL=1`:
    as before). Merging on, this PC: 0 of 1327 shots grey (`run/tk4`), 9 of 813 with the old rule
    (`run/tk5`). With the fix merging saves nothing on the FX (31.5 fps merged, 32.8 not), so the
    plugin keeps it off for FH1.
  - CPU on the FX: vertex fetch entries zeroed per used slot only (`f926dab`, fetch setup 2.9 ->
    1.95 ms a frame), last-pipeline memo (`7dfa1e8`, 2.6 -> 1.8 ms). Final FX free roam ~32-34 fps
    (stock 37); this PC 94-99 (stock 95-96).
- **FX frame rate:** yesterday's 32 fps FX runs had `REGPU_EARLY_FENCES=1`; with the plugin's
  defaults (and `--frame_rate=240`) the FX runs free roam at ~19 fps, stock at ~39. Not a
  regression (yesterday's build measures the same).
- **Minimap route and arrow flicker (this PC; not reproducible on the FX).** Moving, about one shot
  in eight loses the green route and the player arrow; stock never does. The game draws the
  minimap into an offscreen target (resolved to 1DAC5000, 256x256) from a ring of 64 KB vertex
  blocks; the route is missing exactly when its block is 0A855000 (all 230 such shots, no others).
  Not direct vertex or index reads, indexed draws or fetch bounds (A/B). The FX at ~19 fps never
  reaches that block. Open.
- **Shadow and minimap shimmer at a 720p render size (29 Oct).** Reported on this PC: shadows (the
  fence's on the ground, the car's own) and the minimap shimmer between two states; gone with
  `GLUE2GPU_RES=1920x1080`. Ruled out by A/B with the user watching: early fences, the six
  reuse/caching switches, surface-by-pitch, direct indices, and waiting for the GPU every frame.
  Found and fixed in ReGPU (`3f6b0ac`), neither of which removes the shimmer by itself:
  - The 1024x1024 shadow cascade atlas (1D6C5000, four 512 quadrants, one redrawn per frame in
    turn) was squeezed into the output's 720 rows (70% resolution). EDRAM surfaces now hold 1024
    rows (`REGPU_SURFACE_ROWS=720`: as before).
  - Polygon offset was ignored; FH1's shadow maps use slope 16/16, offset 5e-5. Without it the
    parked car's rear and roof were 15% darker than stock (acne); with it 68.8/110.7 against
    stock's 68.1/108.8 (`REGPU_NO_POLY_OFFSET=1`: as before).
  Not reproduced by scripted runs: debug-server shots of the parked start, side views and a short
  drive show no two-state or period-4 change, and the user's OBS clip (parked, fence shadow
  jumping about 7.5 times a second, the near-cascade redraw rate at 30 fps) is not caught by any
  per-region metric tried. Leads: the shadow mask pass (rect list, scene depth t0 + atlas t1,
  four taps rotated by a screen-position dither) is smoothed by a later blur that matches 2-bit
  alpha (`REGPU_NO_RESOLVE_ALPHA2` untested by eye); the 520x520 car shadow maps (1CE95000,
  1CA95000, the latter reused for 640x360 and 320x180 blur buffers in the same frame) also gain
  1.5x at 1080p. Open.
  Update (late morning): reproduced and measured. The user's example (start view, the shadow on
  the ground ahead of the car below the HORIZON boards; region 560-880 x 300-450 at 1280x720,
  `--frame_rate=240`): mean per-frame change over consecutive frames is 0.2-0.3 on stock
  (`FORZA_SCREENSHOT_BURST`), 1.0-1.9 on ReGPU (debug-server shots), with the same idle camera
  sway in both (about 1 px per 30 frames, measured on stock and ReGPU). With the three
  shadow-mask pixel shaders (95444a07316f3f67, 1a70e13f88601106, be7eb6f8d883ff48: rect list,
  t0 scene depth 1DAC5000, t1 cascade atlas 1D6C5000) edited to "no shadow" the score drops to
  0.2-0.6: the shimmer is in that pass. Atlas quadrant redraws while parked do not move (0.00
  texel shift between captures), and the pass's dither constants are fixed. Not it (scores
  within noise): aniso, mips, alpha2 resolve rounding, depth view scale, PWL, forward CF, unorm
  clamp, exponent bias, fixed16 clamp, MSAA scale, rect snap, old depth W. Turning polygon offset
  off makes it worse (2.5). Next: split the pass's inputs (scene depth vs atlas taps) with `ps`
  edits, e.g. output the reconstructed shadow-space position, and compare its frame-to-frame
  change with stock's. Scripts: `spot.py`, `spotab.py` in the session scratchpad.
  Continued: tried feeding the pixel-position input as the Xenos integer corner (SV_Position -
  0.5 when PA_SU_VTX_CNTL bit 0 is clear: the mask shader adds the half texel itself before
  reading scene depth). No measurable change at the spot (fixed 1.44-1.98, old 1.45-2.04);
  reverted. Per-frame score swings with the idle sway's phase (baseline 0.95-1.91), so single
  A/B pairs mislead: interleave and average several. Mask-pass intermediates scored with `ps`
  edits (tap coordinate fraction, atlas tap depth) were inside that noise. The amplified diff at
  the spot shows the change along the painted stripes' edges and the car outline inside the
  shadowed area. `startspot.ps1` retries until the session spawns at the start view (some
  sessions spawn at the Autoshow).
- **Shimmer found and fixed (29 Oct afternoon, FX).** Debug-server shots slow the game (a PNG a frame),
  so the camera's idle sway moved more between ReGPU's captured frames than between stock's: those
  per-frame comparisons were wrong. Real-time capture (ffmpeg ddagrab on the FX through
  `fh1-run.cmd`'s RECORD hook, `fxrec.ps1`, `recan.py`), change per second at the user's example:
  stock 15.7-17.0, ReGPU before 37-88, now 12-15. Bisected (two runs each): without the 3D
  half-pixel shift 22-30, the rest within noise. The shift (ReGPU `58adc47`, as Xenia: viewport-
  transformed draws with D3D9 pixel centres move half a pixel) also aligns ReGPU's image with
  stock's (HUD and banners 0.00 px off, 0.6-0.7 without). Same commit: non-power-of-two mips (the
  768x256 and 2048x768 ground textures had none) and anisotropy from the fetch (plugin `7db63d4`
  turns it on for FH1; distant-ground fine detail 20% above stock's with forced 16x, now close).
  The near ground broke for one build (aniso samplers clamping); fixed and checked. Not done: the
  user's eye on this PC, other scenes (only the start view checked), the minimap area (one final
  recording showed 50/s there with a cursor and the save icon on screen, 5-9 in every other run).
- **Dark band ahead of the camera (29 Oct evening, this PC, user parked on the road out of the
  start).** Stock has none (user checked). Not the road's textures, mips, UVs, decals, today's
  half-pixel/poly-offset/aniso changes or the shadow maps' contents (frozen). It is the sun-shadow
  mask: `REGPU_NO_DEPTH_VIEW_SCALE=1` lifts the band (118 -> 153) but makes the start view's stand
  and far ground brighter than stock (FX: stand 45 vs stock 39 / 40 with the scale), so that
  switch is not the fix. Mask pass order: #835 640x360 rect through a 4x view (surface 0A020280,
  colour base 2D0, stencil), #837-839 car maps (1x), #840 4x rect again with MIN blend and colour
  base 000 (= the depth base) resetting stencil, #841-848 atlas cascades (stencil-gated,
  depthctl 08718233 / 08700233). With the 4x rects scaled to the whole 1280x720 mask the far
  cascades process the far ground and write a dithered, partly-shadowed pattern there (mask dump);
  unscaled they only reach the top-left quadrant. So either #840's coverage differs from the
  console's, or the far cascades' shadow test fails on that ground where the console's passes.
  Next: dump the mask after each of #840-848 (REGPU_DUMP_FRAME with per-draw stops) at a fixed
  parked view, find the first draw that darkens the band pixels, then compare its inputs (stencil
  state, atlas quadrant, depth) with Xenia's handling of the 4x-view reset.
  Follow-up (same evening): `REGPU_SKIP_SHADERS` (ReGPU, live) drops the five atlas-only cascade
  passes (1a70e13f, be7eb6f8, 64db60b4, 7434d682, ab467afb): band/road ratio 0.74 -> 0.88 on the road
  out of the start, the same as the scale off (0.87); any single one ~0. Not depth precision (20e4
  truncation of depth resolves: 0.74 vs 0.73, reverted). Atlas switches (tall pass, image base, tile
  bpp, depth pack, stale resolves) no effect; NO_TALL_VIEW and NO_HEIGHT_KEEP runs invalid (view
  moved). Scripted FX drives (RT from the start) hit the barrier: no reproducible view yet. Next:
  a fixed spot on that road (user parks once), then per-cascade input edits (scene depth t0 vs atlas
  t1 taps vs stencil) on one of the five passes.
