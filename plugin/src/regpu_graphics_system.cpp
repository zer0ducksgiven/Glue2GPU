// rexgpu-regpu against ReIGlue fh1-0.10.0.24-r2 (317216e) and ReGPU's rexglue-driver branch.
#include "regpu_graphics_system.h"
#include "stack_dump.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>

#include <regpu/driver.h>
#include <regpu/regpu.h>

namespace glue2gpu
{
namespace
{
constexpr uint32_t kRegisterBase = 0x7FC80000;

// ReGPU's Host takes plain function pointers, so its callbacks reach ReXGlue through these.
rex::memory::Memory* g_memory = nullptr;
rex::runtime::FunctionDispatcher* g_dispatcher = nullptr;
rex::system::KernelState* g_kernel = nullptr;

// ReGPU's threads (vblank interrupt, command processor) are guest threads: they need a
// PPCContext, and the interrupt one calls the title's callback on it.
std::mutex g_threadsMutex;
std::vector<rex::system::object_ref<rex::system::XHostThread>> g_threads;

std::atomic<bool> g_running{ false };

// ReGPU presents in its own window, so ReXGlue's window never has focus and its input system
// reads pads as idle. Patch 0392's export lets pads work without that focus (looked up at run
// time: a runtime without it keeps the old behaviour).
void EnablePadsWithoutFocus()
{
    using Fn = void (*)(int);
    Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(nullptr), "rexinput_SetPadsWithoutFocus"));
    for (const wchar_t* dll : { L"rexruntime.dll", L"rex_runtime.dll", L"rexglue.dll" })
        if (!fn)
            if (HMODULE m = GetModuleHandleW(dll))
                fn = reinterpret_cast<Fn>(GetProcAddress(m, "rexinput_SetPadsWithoutFocus"));
    if (fn)
        fn(1);
    else
        REXLOG_WARN("glue2gpu: rexinput_SetPadsWithoutFocus not found; pads need ReXGlue's window focused");
}

uint32_t Alloc(bool physical, uint32_t size, uint32_t align)
{
    if (!physical)
        return g_memory->SystemHeapAlloc(size, align);
    // The 0xA0000000 view (64 KB pages), where ReGPU expects physical memory. As
    // MmAllocatePhysicalMemoryEx does: whole pages, over the heap's whole range.
    constexpr uint32_t kPage = 64 * 1024;
    auto* heap = g_memory->LookupHeapByType(true, kPage);
    const uint32_t base = heap->heap_base();
    uint32_t address = 0;
    if (!heap->AllocRange(base, base + heap->heap_size() - 1, (size + kPage - 1) & ~(kPage - 1),
                          std::max(align, kPage),
                          rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                          rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                          /*top_down=*/true, &address))
    {
        REXLOG_ERROR("regpu: physical alloc of {:#x} bytes (align {:#x}) failed", size, align);
        return 0;
    }
    REXLOG_INFO("regpu: physical alloc {:#x} bytes at {:08X}", size, address);
    return address;
}

void CreateThread(const char* name, void (*body)(PPCContext&))
{
    auto* thread = new rex::system::XHostThread(g_kernel, 128 * 1024, 0, [body]() {
        body(*rex::system::XThread::GetCurrentThread()->thread_state()->context());
        return 0;
    });
    thread->set_name(name);
    thread->Create();
    std::lock_guard lock(g_threadsMutex);
    g_threads.emplace_back(thread);
}

uint32_t CurrentPcr()
{
    return rex::system::XThread::GetCurrentThread()->pcr_ptr();
}

// ReGPU sets r3/r4 (source, user data) and calls this between interruptLockAcquire/Release.
// ExecuteInterrupt takes the dispatcher's global lock and sets the arguments itself, so the
// lock hooks are empty. ReGPU writes the PCR's CurrentCpu itself; ReXGlue's own dispatch calls
// XThread::SetActiveCpu (SPIKE.md, open question 4).
void GuestCall(PPCContext& ctx, uint32_t address)
{
    auto* thread = rex::system::XThread::GetCurrentThread();
    uint64_t args[] = { ctx.r3.u64, ctx.r4.u64 };
    g_dispatcher->ExecuteInterrupt(thread->thread_state(), address, args, 2);
}

// GLUE2GPU_WRITEBACK: which resolves are also written to guest memory, for titles that read them
// on the CPU. "some" (the default, as stock ReXGlue's readback_resolve=some): an image the first
// time it is resolved, and again after 60 frames without it; "all" (every resolve, deferred: ~4 fps
// in FH1), "now" (every one at once), "none". FH1 TU4's render thread waits on (or reads a null
// from) a resolved image otherwise.
std::atomic<uint64_t> g_swapCount{ 0 };
regpu::Writeback ResolveWriteback(const regpu::ResolveCopy& copy)
{
    static const std::string mode = std::getenv("GLUE2GPU_WRITEBACK") ? std::getenv("GLUE2GPU_WRITEBACK") : "some";
    regpu::Writeback w;
    if (mode == "none")
        return w;
    if (mode == "some")
    {
        static std::mutex mutex;
        static std::unordered_map<uint64_t, uint64_t> lastSeen; // image -> swap of its last resolve
        const uint64_t key = uint64_t(copy.base) << 32 ^ uint64_t(copy.width) << 18 ^ uint64_t(copy.height) << 6 ^ copy.format ^ (copy.depth ? 1ull << 63 : 0);
        const uint64_t now = g_swapCount.load(std::memory_order_relaxed);
        std::lock_guard lock(mutex);
        auto [it, fresh] = lastSeen.try_emplace(key, now);
        const bool stale = !fresh && now - it->second > 60;
        it->second = now;
        if (!fresh && !stale)
            return w;
    }
    w.mode = mode == "now" ? regpu::Writeback::Mode::Now : regpu::Writeback::Mode::Deferred;
    // Every 600 frames: how many were asked for, and the biggest.
    static std::atomic<uint64_t> requested{ 0 }, lastLog{ 0 };
    static std::atomic<uint32_t> biggest{ 0 };
    requested++;
    if (copy.width * copy.height > biggest.load())
        biggest = copy.width * copy.height;
    if (const uint64_t now = g_swapCount.load(); now >= lastLog + 600)
    {
        lastLog = now;
        REXLOG_INFO("regpu: {} resolve write-backs in the last 600 frames (biggest {} pixels)", requested.exchange(0), biggest.exchange(0));
    }
    return w;
}

void InterruptLockAcquire(int) {}
void InterruptLockRelease() {}

// ReXGlue's VdQueryVideoMode reports the refresh rate the title paces itself by (FH1:
// video_mode_refresh_rate, changed from its graphics menu while running), so ReGPU's vblank
// follows it, as ReXGlue's own vsync thread does.
void RefreshRateLoop()
{
    while (g_running)
    {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        regpu::driver::SetRefreshRate(std::max(1.0, double(float(mode.refresh_rate))));
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}
} // namespace

ReGpuGraphicsSystem::~ReGpuGraphicsSystem() { Shutdown(); }

X_STATUS ReGpuGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext*)
{
    // ReGPU creates its window and device in regpu::Start(), which needs Host filled, so the real
    // work waits for SetupGuestGpu. TODO(4a): adopt ReXGlue's window instead.
    presentation_ = true;
    return X_STATUS_SUCCESS;
}

void ReGpuGraphicsSystem::FillHost()
{
    regpu::Host& host = regpu::GetHost();
    host.base = g_memory->virtual_membase();
    // ReIGlue keeps one 4.5 GB file mapping for all of guest memory (physical at file offset
    // 0x100000000) and doesn't expose its handle, so ReGPU opens the 0xA0000000 view by address
    // (OpenExistingHeapFromAddress1), or falls back to copying.
    host.physicalSection = nullptr;
    host.alloc = Alloc;
    host.createThread = CreateThread;
    host.currentPcr = CurrentPcr;
    host.guestCall = GuestCall;
    host.interruptLockAcquire = InterruptLockAcquire;
    host.interruptLockRelease = InterruptLockRelease;
    host.logThreadStacks = LogThreadStacks;
    host.resolveWriteback = ResolveWriteback;
    host.onSwap = [](uint64_t frame) { g_swapCount.store(frame, std::memory_order_relaxed); };
}

X_STATUS ReGpuGraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                                            rex::system::KernelState* kernel_state)
{
    g_dispatcher = dispatcher;
    g_memory = dispatcher->memory();
    g_kernel = kernel_state;
    FillHost();

    regpu::Config& config = regpu::GetConfig();
    // The frame cap follows the host's settings (FH1: frame_rate, vsync), as its own presenter
    // does. TODO: resolution, msaa, present_effect.
    // ReXGlue's own presenter doesn't wait for the display (FH1 has no vsync setting): the title's
    // frame cap limits it. ReGPU's vsync would cap it at the display's rate instead.
    // GLUE2GPU_VSYNC=1, or a "vsync" setting where the host has one, turns it back on.
    config.vsync = false;
    if (const std::string v = rex::cvar::GetFlagByName("vsync"); !v.empty())
        config.vsync = v == "true" || v == "1";
    if (const char* v = std::getenv("GLUE2GPU_VSYNC"))
        config.vsync = *v == '1';
    if (const std::string fps = rex::cvar::GetFlagByName("frame_rate"); !fps.empty())
        config.maxFps = std::max(30, std::atoi(fps.c_str()));
    // ReGPU draws the scene at this size when no upscaler is on; ReXGlue's default is the
    // console's 720p (resolution_scale=1). GLUE2GPU_RES=WxH overrides it.
    config.width = 1280;
    config.height = 720;
    if (const char* res = std::getenv("GLUE2GPU_RES"))
        std::sscanf(res, "%ux%u", &config.width, &config.height);
    // GLUE2GPU_UPSCALER=fsr1 (ReGPU's FSR 1; fsr3/dlss need ReGPU built with them) and
    // GLUE2GPU_QUALITY=0-4 (native, quality, balanced, performance, ultra performance).
    if (const char* up = std::getenv("GLUE2GPU_UPSCALER"))
    {
        const std::string u = up;
        config.upscaler = u == "fsr1" ? regpu::Config::Upscaler::Fsr1 : u == "fsr3" ? regpu::Config::Upscaler::Fsr3
                        : u == "dlss" ? regpu::Config::Upscaler::Dlss : regpu::Config::Upscaler::Off;
    }
    if (const char* q = std::getenv("GLUE2GPU_QUALITY"))
        config.quality = uint32_t(std::atoi(q));
    config.windowTitle = L"Forza Horizon (ReGPU)";
    // ReXGlue has already opened its game window (and keeps its input, focus, closing and
    // fullscreen handling on it); ReGPU presents there instead of opening a second one, which left
    // ReXGlue's black and empty. GLUE2GPU_OWN_WINDOW=1: ReGPU's own window, as before.
    if (!std::getenv("GLUE2GPU_OWN_WINDOW"))
        if (auto* runtime = rex::Runtime::instance(); runtime && runtime->display_window())
            // (The swap chain stays the output size and is stretched to the window. Same frame rate
            // as ReGPU's own window in a back-to-back test.)
            config.hostWindow = runtime->display_window()->GetNativeWindowHandle();
    // ReXGlue calls InitializeShaderStorage after this, so the cache root comes from its setting.
    if (cacheRoot_.empty())
        if (const std::string root = rex::cvar::GetFlagByName("cache_root"); !root.empty())
            cacheRoot_ = root;
    if (!cacheRoot_.empty())
        config.userDir = cacheRoot_ / "regpu";

    // Same range, mask and size as ReXGlue's own GraphicsSystem.
    g_memory->AddVirtualMappedRange(
        kRegisterBase, 0xFFFF0000, 0x0000FFFF, this,
        reinterpret_cast<rex::runtime::MMIOReadCallback>(ReadRegisterThunk),
        reinterpret_cast<rex::runtime::MMIOWriteCallback>(WriteRegisterThunk));

    // ReGPU switches this host needs that are off for XenonRecomp ports (each can still be set
    // to 0 in the environment).
    // (REGPU_CUBE_Y_SWAP is no longer set: with surfaces keyed by pitch and homogeneous depth the
    // cube faces come out right, and the swap made the car glass reflect the ground.)
    EnablePadsWithoutFocus();
    // REGPU_RESOLVE_IMAGE_BASE: FH1's shadow atlas resolves one quadrant at a time into the image's
    // own base; otherwise the quadrants below the first row land elsewhere and the world has no
    // sun shadows.
    if (!std::getenv("REGPU_RESOLVE_IMAGE_BASE"))
        _putenv_s("REGPU_RESOLVE_IMAGE_BASE", "1");
    // REGPU_ASYNC_SHADERS, REGPU_ASYNC_PIPELINES: a draw whose shaders or pipeline are still building
    // is skipped instead of waited for. On a first boot on a slow PC (the FX-8350 + GTX 970) the
    // waits held the command processor for tens of seconds while FH1 loaded the world, and the title
    // never recovered (no VdSwap again); with both, it reaches free roam.
    // REGPU_TEXTURE_GAMMA, REGPU_RESOLVE_GAMMA: Xenos gamma-signed fetches and gamma-encoding
    // resolves of k_8_8_8_8_GAMMA surfaces. Without both, FH1 comes out washed out (FX, festival:
    // mean luma 118 against stock's 87, walls 59 against 19); with both, 85 and 28.
    if (!std::getenv("REGPU_TEXTURE_GAMMA"))
        _putenv_s("REGPU_TEXTURE_GAMMA", "1");
    if (!std::getenv("REGPU_RESOLVE_GAMMA"))
        regpu::driver::SetSwitch("REGPU_RESOLVE_GAMMA", true);
    // (REGPU_ASYNC_SHADERS is read when ReGPU loads, before this runs: set through the driver.)
    for (const char* name : { "REGPU_ASYNC_SHADERS", "REGPU_ASYNC_PIPELINES" })
        if (!std::getenv(name))
            regpu::driver::SetSwitch(name, true);

    regpu::Start();
    started_ = true;
    g_running = true;
    std::thread(RefreshRateLoop).detach();
    return X_STATUS_SUCCESS;
}

uint32_t ReGpuGraphicsSystem::ReadRegisterThunk(void*, ReGpuGraphicsSystem*, uint32_t addr)
{
    return regpu::driver::ReadRegister((addr & 0xFFFF) >> 2);
}

void ReGpuGraphicsSystem::WriteRegisterThunk(void*, ReGpuGraphicsSystem*, uint32_t addr,
                                             uint32_t value)
{
    regpu::driver::WriteRegister((addr & 0xFFFF) >> 2, value);
}

void ReGpuGraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data)
{
    regpu::driver::SetInterruptCallback(callback, user_data);
}

void ReGpuGraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2)
{
    regpu::driver::InitializeRingBuffer(ptr, size_log2);
}

void ReGpuGraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2)
{
    regpu::driver::EnableReadPointerWriteBack(ptr, block_size_log2);
}

void ReGpuGraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                  uint32_t, bool)
{
    cacheRoot_ = cache_root;
}

void ReGpuGraphicsSystem::Shutdown()
{
    if (!started_)
        return;
    started_ = false;
    g_running = false;
    regpu::SaveCaches();
    // ReGPU has no Stop(); its threads run until process exit.
}
} // namespace glue2gpu
