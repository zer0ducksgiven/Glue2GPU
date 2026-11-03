# Glue2GPU

Glue2GPU lets a [ReXGlue](https://github.com/rexglue/rexglue-sdk) port run on
[ReGPU](https://github.com/zer0ducksgiven/ReGPU) instead of ReXGlue's own Xenia-derived renderer.
The port's code doesn't change, and nothing is regenerated. You build one DLL, `rexgpu-regpu.dll`,
put it beside the game's executable and start the game with `--gpu_plugin=regpu`.

ReGPU is a native Direct3D 12 Xbox 360 GPU, written for XenonRecomp ports. It is a small library of
about 16k lines, against roughly 254k for ReXGlue's renderer. Once the renderer is the limit, it is
much faster. In Forza Horizon 1 on an RTX 4070:

| Output | ReXGlue D3D12 | ReGPU through Glue2GPU |
|---|---|---|
| 1280×720 | 97 fps | 95 fps (both limited by the game's CPU side) |
| 2560×1440 | 67 fps | 91 fps |
| 3840×2160 | 36 fps | 79–86 fps |

Since those runs, the renderer and plugin work has taken FH1 at 720p from about 88 to about 105 fps.
That came from render-thread sync, a draw thread and a profile-guided build.

## How it works

ReXGlue already loads its renderer from a DLL. Its kernel implements the `Vd*` video exports and
forwards them to an `IGraphicsSystem` that it gets from `rex_gpu_create` (`rex/system/gpu_plugin.h`,
ABI 1). The CPU side of ReXGlue and XenonRecomp uses the same `PPCContext` and function signature.
So Glue2GPU doesn't convert anything. It is a GPU plugin that wraps ReGPU's core behind that
interface:

| ReXGlue asks for | Glue2GPU answers with |
|---|---|
| `rex_gpu_abi_version`, `rex_gpu_create` | A `ReGpuGraphicsSystem` (`plugin_main.cpp`). It turns down any backend except `d3d12`/`any`, so ReXGlue can fall back to its own. |
| `SetupGuestGpu` | Fills `regpu::Host` from ReXGlue's runtime (below), maps the GPU registers at `0x7FC80000` as an MMIO range of ReXGlue's memory, and starts ReGPU. |
| `InitializeRingBuffer`, `EnableReadPointerWriteBack` | ReGPU's command processor, on the title's ring buffer. |
| `SetInterruptCallback` | ReGPU's vblank and command-processor interrupts, delivered through ReXGlue's interrupt dispatch. |
| `InitializeShaderStorage` | ReGPU's shader and pipeline cache under `--cache_root`. |
| `SetupPresentation` | ReGPU's swap chain in ReXGlue's window. `GLUE2GPU_OWN_WINDOW=1` uses ReGPU's own window instead. |

`regpu::Host` is ReGPU's view of the host. Glue2GPU fills it from ReXGlue as follows:

- **Memory:** guest memory is ReXGlue's virtual membase. Allocations come from the physical heap at
  `0xA0000000`, as `MmAllocatePhysicalMemoryEx` does them.
- **Threads:** ReGPU's vblank and command-processor threads are created as real guest threads (`XThread`), so they have a
  `PPCContext` and a PCR.
- **Guest calls:** they go through ReXGlue's function dispatcher, under its interrupt lock.
- **Resolve write-back:** resolves are copied back to guest memory for titles that read them on the
  CPU. `GLUE2GPU_WRITEBACK=some|all|now|none` controls this; `some` matches ReXGlue's
  `readback_resolve=some`.
- **Settings:** the frame-rate limit, vsync, anisotropic filtering and window size follow ReXGlue's
  own flags, so a game's settings menu keeps working.

ReGPU needs one change to work this way. Its `rexglue-driver` branch builds the core without its own
`Vd*` exports (`REGPU_GUEST_ABI=rexglue`) and against ReXGlue's `PPCContext`. The XenonRecomp build
of ReGPU is unaffected.

## Game-specific fixes

Every renderer fix that only one game needs goes through ReGPU's hooks, never into ReGPU itself. The
hooks are:

- `rewriteShader`: edits a translated shader's HLSL before it compiles.
- `onDraw`: per-draw values for a tagged shader.
- `clearBeforeDraw`: clears a draw's colour target first.
- `resolveWriteback`: decides which resolves go back to the CPU.
- `staticShaders`, `pipelineSeed`, `bootScreen`: the first-boot compile, covered below.

The plugin in this repository is the Forza Horizon 1 one, and it is the worked example. Its FH1 parts
are:

| File | What it does for FH1 |
|---|---|
| `fh1_shaders.cpp` | Finds every shader in FH1's `.fxobj` effect files (loose, in the tracks' zips and in TU4's `media.zip`, LZX-decompressed) and in the executable: 1,548 unique. ReGPU compiles them all on the first boot. |
| `fh1_boot_screen.cpp` | The screen shown during that compile: a progress bar over the game's own festival art. |
| `regpu_graphics_system.cpp` | Shadow-pass shader fixes for fractional render scales, ambient occlusion inside the game's shadow-mask passes, the cockpit shadow-mask clear, and the link to the game's graphics screen (`fh1_graphics_request`). |

To bring up another game, start from this plugin and keep `plugin_main.cpp` and the `Host` and
register plumbing. Replace the FH1 hooks with that game's own, or start with none: a game with no
hooks still runs on ReGPU's console-accurate defaults.

## Shaders on the first boot

A Xenos shader has to be translated to HLSL and compiled before its first draw. Without help, every
new shader stalls the game. Glue2GPU covers this in three layers:

1. **Static shaders (`Host::staticShaders`):** the plugin searches the game's files for every shader
   it ships. ReGPU compiles them on the first boot, behind the boot screen.
2. **Runtime vertex-shader variants:** the game's D3D layer patches each shipped vertex shader's fetch
   instructions to fit the vertex layout it draws with, so those shaders aren't in any file. ReGPU
   records each one in `pipelines.manifest` as its shipped shader plus the patched words (`V`
   lines).
3. **Pipeline seed (`Host::pipelineSeed`):** a list of pipelines and variants, merged from recorded
   play. It holds hashes and render state only, no game data. On the first boot, ReGPU rebuilds the
   variants from the player's own copy of the game and builds the pipelines after the static
   shaders.

To record a seed, play with a fresh `--cache_root` (free roam, races, menus, different times of day),
then merge the `P` and `V` lines of each run's `<cache_root>/regpu/pipelines.manifest`. FH1's seed
(2,234 pipelines and 140 variants) ships with FH1R in `forza-recomp/regpu-plugin/seed/`.

## Building

You need Windows, CMake, Ninja, LLVM (clang), a ReXGlue SDK install, the ReGPU `rexglue-driver`
branch, and the FidelityFX SDK's FSR 1 headers. You also need `fxc` (Windows SDK) on PATH.

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;" + $env:PATH
cmake -S plugin -B build/plugin -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ `
  "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" -DCMAKE_PREFIX_PATH=<rexglue-sdk> `
  -DGLUE2GPU_REXGLUE=<rexglue-source> -DGLUE2GPU_REGPU=<ReGPU checkout> -DREGPU_FFX=<FidelityFX-SDK>/Kits/FidelityFX
cmake --build build/plugin
```

`GLUE2GPU_REXGLUE` and `GLUE2GPU_REGPU` default to `thirdparty/ReIGlue` and `thirdparty/ReGPU`.

Build with the same compiler as the SDK. The plugin imports `rexruntime.dll`, so an SDK that doesn't
match the game's can fail to load.

If a configure ever fails, delete `CMakeCache.txt` and `CMakeFiles` before you configure again. One
failed configure here once cached empty optimisation flags and produced a half-speed DLL.

### Profile-guided build

The PGO build is worth about 6% in FH1 (99.3 → 105.5 fps):

1. Build with `-DGLUE2GPU_PGO=generate`.
2. Play with that DLL. It writes its own profile 70 s in, over 60 s. `GLUE2GPU_PGO_START` and
   `GLUE2GPU_PGO_SECONDS` change the timing, and `LLVM_PROFILE_FILE` names the file.
3. Merge the profile with `llvm-profdata merge`.
4. Rebuild with `-DGLUE2GPU_PGO=<file>.profdata`.

`plugin/pgo/drive.profdata` is FH1's profile.

## Running

```
forza.exe <the usual arguments> --gpu_plugin=regpu --gpu_backend=d3d12
```

Logs:

- `regpu.log`, beside the executable.
- A crash or a 10-second hang writes every thread's stack, named from the PDB, to `regpu_stacks.txt`.

Start with `REGPU_DEBUG=1` to turn on ReGPU's debug server. Then ReGPU's `tools/regpuctl.py` can take
screenshots, capture frames, tint or edit a shader live, and flip `REGPU_*` switches without a
restart.

Every behaviour that could be wrong for some game has an environment switch, so it can be A/B tested:

| Switch | Effect |
|---|---|
| `GLUE2GPU_RES=WxH` | Drawing size (the output is upscaled to the window). |
| `GLUE2GPU_UPSCALER` | FSR 1 or FSR 3, where built in. |
| `GLUE2GPU_VSYNC`, `GLUE2GPU_ANISO` | Override the game's settings. |
| `GLUE2GPU_WRITEBACK` | Which resolves go back to guest memory (see above). |
| `GLUE2GPU_NO_STATIC_SHADERS` | Compile each shader at its first draw instead of at boot. |
| `GLUE2GPU_NO_DRAW_THREAD` | Turn off ReGPU's draw thread. |
| `GLUE2GPU_NO_EARLY_FENCES` | Turn off ReGPU's early fences. |
| `GLUE2GPU_OWN_WINDOW` | Use ReGPU's own window instead of ReXGlue's. |
| `REGPU_NO_SEED_PIPELINES` | Skip the shipped pipeline seed. |

ReGPU's own `REGPU_*` switches are listed in its README.

## Status

- **FH1:** Forza Horizon 1 ReImagined runs on Glue2GPU from boot through free roam, races and
  menus, on an RTX 4070 and on an FX-8350 with a GTX 970. The plugin and its seed are proposed for
  FH1R in
  [FH1-ReImagined/ForzaHorizon1-ReImagined#611](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/pull/611).
- **Platforms:** Windows only. ReGPU is Direct3D 12, so on Linux it runs under Wine or Proton.
  ReXGlue's Vulkan renderer stays the native Linux choice.
- **Other games:** none yet. `SCOPE.md` is the original survey of both codebases and the plan.
  `SPIKE.md` is the FH1 bring-up log: measurements, the bugs found and what fixed them.

## Licence

Glue2GPU's plugin code is MIT, like ReGPU. FidelityFX's FSR 1 headers are MIT.
