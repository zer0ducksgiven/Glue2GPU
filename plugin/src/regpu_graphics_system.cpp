// rexgpu-regpu against ReIGlue fh1-0.10.0.24-r2 (317216e) and ReGPU's rexglue-driver branch.
#include "regpu_graphics_system.h"
#include "fh1_boot_screen.h"
#include "fh1_shaders.h"
#include "stack_dump.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW

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

// Settings ReXGlue's own GPU backends define (rex/graphics/flags.h), which aren't loaded with this
// plugin. The game's graphics screen sets them, and saves nothing when one is missing; the plugin
// follows them (FollowGraphicsMenu). Registered after ReXGlue read the command line, so their
// starting values are read from it again (StartingFlag).
REXCVAR_DEFINE_BOOL(vsync, false, "Display", "Wait for the display's refresh (ReGPU)");
REXCVAR_DEFINE_INT32(anisotropic_override, -1, "Display",
                     "Texture filtering: -1 the game's, 0 off, 1-5 1x-16x anisotropic (ReGPU)");

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

// The game window's title: ReGPU and the frame rate over the last half second (ReXGlue's own title
// names ReXGlue). Frames are counted on the renderer's thread (Host::onSwap, FillHost); the title is
// set from a thread of its own, as WM_SETTEXT waits for the window's thread.
std::atomic<uint64_t> g_swaps{ 0 };

// The graphics screen's rebuild settings: output resolution, upscaler, MSAA scene resolution and
// anti-aliasing. Its Save sends them as fh1_graphics_request ("id width height scale mode") and
// waits for "id|ok" in fh1_graphics_result, then writes graphics-settings.json; ReXGlue's Vulkan
// renderer defines these and rebuilds live. ReGPU sizes its targets once, so here they apply at the
// next start: the plugin answers ok and reads the file then (MenuRenderSettings).
REXCVAR_DEFINE_STRING(fh1_graphics_request, "", "Display", "Graphics screen request (ReGPU)");
REXCVAR_DEFINE_STRING(fh1_graphics_result, "", "Display", "Graphics screen result (ReGPU)");
REXCVAR_DEFINE_STRING(fh1_upscaler, "off", "Display", "Upscaler: off, fsr1_quality, fsr1_balanced, fsr1_performance (ReGPU)");
REXCVAR_DEFINE_STRING(resolution_scale, "1", "Display", "Game MSAA scene: 1 1280x720, 2 2560x1440, 3 3840x2160 (ReGPU)");

std::string StartingFlag(const char* name);

// graphics-settings.json's "key": "value" pairs (the graphics screen writes it flat).
std::unordered_map<std::string, std::string> SavedGraphicsSettings()
{
    std::filesystem::path path;
    if (const char* p = std::getenv("FORZA_GRAPHICS_SETTINGS"); p && *p)
        path = p;
    else
        path = std::filesystem::path(rex::cvar::GetFlagByName("user_data_root")) / "graphics-settings.json";
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::unordered_map<std::string, std::string> out;
    for (size_t at = 0; (at = text.find('"', at)) != std::string::npos;)
    {
        const size_t keyEnd = text.find('"', at + 1);
        const size_t colon = keyEnd == std::string::npos ? keyEnd : text.find_first_not_of(" \t", keyEnd + 1);
        if (colon == std::string::npos || text[colon] != ':')
        {
            at = keyEnd == std::string::npos ? keyEnd : keyEnd + 1;
            continue;
        }
        const size_t valueStart = text.find_first_not_of(" \t\r\n", colon + 1);
        if (valueStart == std::string::npos || text[valueStart] != '"')
        {
            at = colon + 1;
            continue;
        }
        const size_t valueEnd = text.find('"', valueStart + 1);
        if (valueEnd == std::string::npos)
            break;
        out[text.substr(at + 1, keyEnd - at - 1)] = text.substr(valueStart + 1, valueEnd - valueStart - 1);
        at = valueEnd + 1;
    }
    return out;
}

// The saved choices: the menu's flags (so the screen shows them) and ReGPU's render size and
// upscaler. Output 720p is the launcher's mode, where the screen leaves the other three alone.
void MenuRenderSettings(regpu::Config& config)
{
    const auto saved = SavedGraphicsSettings();
    const auto get = [&](const char* key, const char* fallback) {
        const auto it = saved.find(key);
        return it != saved.end() && !it->second.empty() ? it->second : std::string(fallback);
    };
    const std::string output = get("output", "720p"), upscaler = get("upscaler", "off"),
                      scale = get("resolution_scale", "1");
    for (const auto& [flag, value] : { std::pair{ "resolution", output == "720p" ? std::string() : output },
                                       std::pair{ "fh1_upscaler", upscaler }, std::pair{ "resolution_scale", scale } })
        rex::cvar::SetFlagByName(flag, value);
    for (const char* flag : { "vsync", "anisotropic_override", "frame_rate", "world_ambient_occlusion", "world_shadow_filter" })
        if (StartingFlag(flag).empty())
            if (const auto it = saved.find(flag); it != saved.end() && !it->second.empty())
                rex::cvar::SetFlagByName(flag, it->second);
    if (output == "720p")
        return;
    const uint32_t outW = output == "4k" ? 3840 : output == "1440p" ? 2560 : 1920;
    const uint32_t outH = output == "4k" ? 2160 : output == "1440p" ? 1440 : 1080;
    if (upscaler.rfind("fsr1_", 0) == 0)
    {
        // FSR 1 draws a smaller scene and scales it up to the output.
        config.width = outW;
        config.height = outH;
        config.upscaler = regpu::Config::Upscaler::Fsr1;
        config.quality = upscaler == "fsr1_performance" ? 3 : upscaler == "fsr1_balanced" ? 2 : 1;
    }
    else
    {
        // Game MSAA: the scene at the chosen multiple of 720p, shown at the window's size.
        const uint32_t s = uint32_t(std::clamp(std::atoi(scale.c_str()), 1, 3));
        config.width = 1280 * s;
        config.height = 720 * s;
    }
    REXLOG_INFO("glue2gpu: graphics screen: output {}, upscaler {}, scene scale {} -> render {}x{}", output, upscaler,
                scale, config.width, config.height);
}

// Ambient occlusion (the graphics screen's world_ambient_occlusion, defined by FH1R's world_ao.cpp):
// ReXGlue's Vulkan renderer runs it as compute passes (its fh1_world_ao.cpp). Here it is computed
// inside the game's own shadow-mask passes (ApplyShadows, one full-screen pass per cascade, which
// read the scene depth as fetch 0 and have its projection in c0, c1 and c25): the same scalable
// ambient obscurance from depth, multiplied into the mask's red channel (the sun's visibility).
// The cascades after the first combine with MIN, and min(a*ao, b*ao) = ao*min(a, b), so every pass
// multiplies the same factor. The spiral turns with a 2x2 pattern that the game's 2x2 blur of the
// mask evens out. Unlike Vulkan's, the cars and trees aren't left out (their stencil isn't bound
// to these passes).
// The shadow passes by ReGPU's hash (the seven of ReXGlue's kShadowPasses).
constexpr uint64_t kShadowPasses[] = { 0x0289a01b9007b9a9ull, 0x1a70e13f88601106ull, 0x64db60b4b009cf21ull,
                                       0x7434d68269c7567cull, 0x95444a07316f3f67ull, 0xab467afbd5193c44ull,
                                       0xbe7eb6f8d883ff48ull };
constexpr uint32_t kShadowPassTag = 0xA0;
std::atomic<float> g_aoIntensity{ 0.0f }; // 0 off, 0.6 low, 1 high

constexpr char kWorldAoHlsl[] = R"(
// Glue2GPU: ambient occlusion from the scene depth (fetch 0), scalable ambient obscurance.
float Fh1AoZ(Texture2D<float4> t, int2 p, int2 size, float4 proj)
{
    if (any(p < 0) || any(p >= size)) return 0.0;
    float d = t.Load(int3(p, 0)).x;
    float den = 1.0 - proj.x - d;
    float z = abs(proj.y / (abs(den) > 1e-20 ? den : 1e-20));
    return (d <= 0.0 || isinf(z) || isnan(z)) ? 0.0 : z;
}
float3 Fh1AoPosition(int2 p, float z, int2 size, float4 proj)
{
    float2 ndc = (float2(p) + 0.5) / float2(size) * 2.0 - 1.0;
    return float3(ndc * proj.zw * z, z);
}
float Fh1WorldAo(float2 pixel)
{
    const float intensity = vuser[0].x;
    if (intensity <= 0.0 || abs(pc[0].x) < 1e-3 || abs(pc[1].y) < 1e-3 || pc[25].y == 0.0) return 1.0;
    const float4 proj = float4(pc[25].x, pc[25].y, 1.0 / abs(pc[0].x), 1.0 / abs(pc[1].y));
    const float radius = 1.5, fadeStart = 60.0, fadeEnd = 160.0;
    Texture2D<float4> t = gtex[asuint(tbias[0].y)];
    uint w, h;
    t.GetDimensions(w, h);
    const int2 size = int2(w, h), p = int2(pixel);
    const float zc = Fh1AoZ(t, p, size, proj);
    if (zc <= 0.0 || zc >= fadeEnd) return 1.0;
    const float3 c = Fh1AoPosition(p, zc, size, proj);
    // The normal from the neighbours on the same surface (the smaller depth step on each axis).
    const float zl = Fh1AoZ(t, p - int2(1, 0), size, proj), zr = Fh1AoZ(t, p + int2(1, 0), size, proj);
    const float zu = Fh1AoZ(t, p - int2(0, 1), size, proj), zd = Fh1AoZ(t, p + int2(0, 1), size, proj);
    const bool right = zr > 0.0 && (zl <= 0.0 || abs(zr - zc) < abs(zc - zl));
    const bool down = zd > 0.0 && (zu <= 0.0 || abs(zd - zc) < abs(zc - zu));
    const float3 dx = right ? Fh1AoPosition(p + int2(1, 0), zr, size, proj) - c : c - Fh1AoPosition(p - int2(1, 0), zl, size, proj);
    const float3 dy = down ? Fh1AoPosition(p + int2(0, 1), zd, size, proj) - c : c - Fh1AoPosition(p - int2(0, 1), zu, size, proj);
    float3 n = cross(dx, dy);
    const float nl = length(n);
    if (nl < 1e-12) return 1.0;
    n /= nl;
    if (dot(n, c) > 0.0) n = -n; // towards the camera
    float pixels = radius * 0.5 * float(size.y) / (zc * proj.w);
    if (pixels < 1.0) return 1.0;
    pixels = min(pixels, 0.1 * float(size.y));
    // Four turns of the spiral in each 2x2 block (the game blurs the mask 2x2).
    const float turn = 6.2831853 * (float(((p.x & 1) << 1) | (p.y & 1)) + 0.5) * 0.25;
    const float r2 = radius * radius, bias = 0.01 * radius + 0.001 * zc;
    float sum = 0.0;
    [loop] for (int i = 0; i < 12; ++i)
    {
        const float a = (float(i) + 0.5) / 12.0;
        const float angle = a * 5.0 * 6.2831853 + turn;
        const int2 q = int2(float2(p) + 0.5 + a * pixels * float2(cos(angle), sin(angle)));
        const float zq = Fh1AoZ(t, q, size, proj);
        if (zq <= 0.0) continue;
        const float3 v = Fh1AoPosition(q, zq, size, proj) - c;
        const float vv = dot(v, v), vn = dot(v, n);
        const float f = max(r2 - vv, 0.0);
        sum += f * f * f * max((vn - bias) / (0.01 * r2 + vv), 0.0);
    }
    const float ao = max(0.0, 1.0 - sum * intensity / (r2 * r2 * r2) * (5.0 / 12.0));
    const float fade = saturate((fadeEnd - zc) / (fadeEnd - fadeStart));
    return lerp(1.0, ao, fade);
}
)";

uint32_t RewriteShader(uint64_t hash, std::string& hlsl, bool pixel)
{
    if (!pixel || std::find(std::begin(kShadowPasses), std::end(kShadowPasses), hash) == std::end(kShadowPasses))
        return 0;
    const size_t main = hlsl.find("float4 main("), output = hlsl.find("    switch (pinfo.x)");
    if (main == std::string::npos || output == std::string::npos || hlsl.find("TFetch") == std::string::npos)
    {
        REXLOG_WARN("glue2gpu: shadow pass {:016x} isn't laid out as expected; no ambient occlusion in it", hash);
        return 0;
    }
    hlsl.insert(output, "    oC0.r *= Fh1WorldAo(input.pos.xy); // Glue2GPU ambient occlusion\n");
    hlsl.insert(main, kWorldAoHlsl);
    return kShadowPassTag;
}

void OnDraw(uint64_t, uint64_t, uint32_t, uint32_t psTag, const float (*)[4], float user[4][4])
{
    if (psTag == kShadowPassTag)
        user[0][0] = g_aoIntensity.load(std::memory_order_relaxed);
}

// "--name=value" from the command line ("" when absent).
std::string StartingFlag(const char* name)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string value;
    const std::wstring prefix = L"--" + std::wstring(name, name + strlen(name)) + L"=";
    for (int i = 1; argv && i < argc; i++)
        if (std::wstring_view(argv[i]).rfind(prefix, 0) == 0)
            for (const wchar_t* p = argv[i] + prefix.size(); *p; p++)
                value += char(*p);
    LocalFree(argv);
    return value;
}

// The game's graphics screen (FH1R's graphics_menu.cpp) changes ReXGlue's flags while the game runs;
// its live ones reach ReGPU here: frame rate limit and vsync, and texture filtering
// (anisotropic_override: -1 the game's, 0 off, 1-5 1x-16x).
void FollowGraphicsMenu()
{
    static std::string lastRate, lastVsync, lastAniso;
    const std::string rate = rex::cvar::GetFlagByName("frame_rate"), vsync = rex::cvar::GetFlagByName("vsync");
    if (rate != lastRate || vsync != lastVsync)
    {
        lastRate = rate;
        lastVsync = vsync;
        const int fps = std::atoi(rate.c_str());
        regpu::SetFrameLimit(fps > 0 ? uint32_t(fps) : 60u, vsync != "false");
        REXLOG_INFO("glue2gpu: frame rate limit {} fps, vsync {}", fps, vsync);
    }
    // A rebuild request (MenuRenderSettings): accepted; the menu then saves it for the next start.
    static std::string lastRequest;
    if (const std::string request = rex::cvar::GetFlagByName("fh1_graphics_request"); request != lastRequest)
    {
        lastRequest = request;
        if (!request.empty())
        {
            rex::cvar::SetFlagByName("fh1_graphics_result", request + "|ok");
            REXLOG_INFO("glue2gpu: graphics screen request '{}' saved; applies at the next start", request);
        }
    }
    static std::string lastAo = "?";
    if (const std::string ao = rex::cvar::GetFlagByName("world_ambient_occlusion"); ao != lastAo)
    {
        lastAo = ao;
        g_aoIntensity = ao == "high" ? 1.0f : ao == "low" ? 0.6f : 0.0f;
        REXLOG_INFO("glue2gpu: ambient occlusion {}", ao.empty() ? "off" : ao);
    }
    if (const std::string aniso = rex::cvar::GetFlagByName("anisotropic_override"); aniso != lastAniso)
    {
        lastAniso = aniso;
        const int level = aniso.empty() ? -1 : std::atoi(aniso.c_str());
        regpu::SetAnisotropy(level <= 0 ? level : 1 << (level - 1));
        REXLOG_INFO("glue2gpu: texture filtering {}", aniso);
    }
}

void StartTitleFps(HWND hwnd)
{
    if (!hwnd)
        return;
    std::thread([hwnd] {
        auto last = std::chrono::steady_clock::now();
        uint64_t lastSwaps = g_swaps.load();
        SetWindowTextW(hwnd, L"Forza Horizon - ReGPU");
        while (IsWindow(hwnd))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const auto now = std::chrono::steady_clock::now();
            const uint64_t swaps = g_swaps.load();
            const double fps = double(swaps - lastSwaps) / std::chrono::duration<double>(now - last).count();
            last = now;
            lastSwaps = swaps;
            wchar_t title[96];
            swprintf_s(title, L"Forza Horizon - ReGPU - %.0f fps", fps);
            SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(title), SMTO_ABORTIFHUNG, 200, nullptr);
            FollowGraphicsMenu();
        }
    }).detach();
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
    host.onSwap = [](uint64_t frame) {
        g_swapCount.store(frame, std::memory_order_relaxed);
        g_swaps.fetch_add(1, std::memory_order_relaxed); // the title's frame rate (StartTitleFps)
    };
    host.rewriteShader = RewriteShader;
    host.onDraw = OnDraw;
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
    for (const char* name : { "vsync", "anisotropic_override" })
        if (const std::string v = StartingFlag(name); !v.empty())
            rex::cvar::SetFlagByName(name, v);
    config.width = 1280;
    config.height = 720;
    MenuRenderSettings(config); // (also vsync, filtering and frame rate the command line didn't give)
    config.vsync = rex::cvar::GetFlagByName("vsync") == "true";
    if (const char* v = std::getenv("GLUE2GPU_VSYNC"))
        config.vsync = *v == '1';
    if (const std::string fps = rex::cvar::GetFlagByName("frame_rate"); !fps.empty())
        config.maxFps = std::max(30, std::atoi(fps.c_str()));
    // ReGPU draws the scene at this size when no upscaler is on; ReXGlue's default is the
    // console's 720p (resolution_scale=1). The graphics screen's saved choices
    // (MenuRenderSettings) come next, and GLUE2GPU_RES=WxH overrides both.
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
    StartTitleFps(static_cast<HWND>(config.hostWindow));
    // ReXGlue calls InitializeShaderStorage after this, so the cache root comes from its setting.
    if (cacheRoot_.empty())
        if (const std::string root = rex::cvar::GetFlagByName("cache_root"); !root.empty())
            cacheRoot_ = root;
    if (!cacheRoot_.empty())
        config.userDir = cacheRoot_ / "regpu";
    // Every shader in the game's files, compiled on the first boot behind the boot screen
    // (GLUE2GPU_NO_STATIC_SHADERS=1: compile as the game first draws with each, as before).
    if (!std::getenv("GLUE2GPU_NO_STATIC_SHADERS"))
    {
        SetShaderSources(rex::cvar::GetFlagByName("game_data_root"), rex::cvar::GetFlagByName("update_data_root"), config.userDir,
                         g_memory->virtual_membase());
        regpu::Host& host = regpu::GetHost();
        host.staticShaders = StaticShaders;
        host.staticShadersSaved = StaticShadersSaved;
        host.bootScreen = BootScreenCompose;
        if (std::getenv("GLUE2GPU_BOOTSCREEN_PREVIEW"))
            BootScreenPreview();
    }

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
    // REGPU_NO_TILE_MERGE: FH1 draws its scene in three tiles. Merging them once lost the sky for a frame now
    // and then (fixed in ReGPU: later-tile stencil draws are kept); with that fix it saves nothing here (FX
    // 31.5 fps merged, 32.8 not), so FH1 draws its tiles as they come. GLUE2GPU_TILE_MERGE=1 merges.
    if (!std::getenv("GLUE2GPU_TILE_MERGE") && !std::getenv("REGPU_NO_TILE_MERGE"))
        regpu::driver::SetSwitch("REGPU_NO_TILE_MERGE", true);
    // REGPU_ANISO_FROM_FETCH: ReGPU filters every linear-wrap texture 16x anisotropically; FH1's fetches
    // ask for none on most (its ground) and 2x-8x on some, which is what stock (Xenia) gives; 16x made the
    // distant ground sharper than stock's. GLUE2GPU_ANISO=1 keeps ReGPU's 16x.
    if (!std::getenv("GLUE2GPU_ANISO") && !std::getenv("REGPU_ANISO_FROM_FETCH"))
        regpu::driver::SetSwitch("REGPU_ANISO_FROM_FETCH", true);
    // REGPU_NO_FETCH_BOUNDS: vertex fetches are not held to the fetch constant's size, as on the console
    // and in Xenia. FH1's skid marks read past it once many are on screen; bounded, those vertices came
    // back as zeros and the marks stretched to the map origin. GLUE2GPU_FETCH_BOUNDS=1 keeps the bound.
    if (!std::getenv("GLUE2GPU_FETCH_BOUNDS") && !std::getenv("REGPU_NO_FETCH_BOUNDS"))
        regpu::driver::SetSwitch("REGPU_NO_FETCH_BOUNDS", true);
    // REGPU_EARLY_FENCES: fences written when the command processor reaches them (as Xenia and the
    // Xenos do), with rewritten vertex and index ranges copied. With fences after the host GPU, FH1
    // waits on every frame: FX free roam 18 fps against stock's 37, 34 with them, and the same
    // picture. GLUE2GPU_NO_EARLY_FENCES=1 keeps the late fences (ReGPU only checks the variable is set).
    if (!std::getenv("GLUE2GPU_NO_EARLY_FENCES") && !std::getenv("REGPU_EARLY_FENCES"))
    {
        _putenv_s("REGPU_EARLY_FENCES", "1");
        regpu::driver::SetSwitch("REGPU_EARLY_FENCES", true);
    }
    // REGPU_DRAW_THREAD: draws translated on a thread of their own while the command processor parses
    // on. FH1 free roam (5800X + 4070, the replayed drive): 98-100 -> 106-108 fps, three runs each, the
    // same picture. GLUE2GPU_NO_DRAW_THREAD=1 keeps one thread (ReGPU only checks the variable is set).
    if (!std::getenv("GLUE2GPU_NO_DRAW_THREAD") && !std::getenv("REGPU_DRAW_THREAD"))
        _putenv_s("REGPU_DRAW_THREAD", "1");
    // REGPU_LEAN_FETCH: one sample per 2D pixel fetch (no branch per sampler), which FH1's full-screen
    // post shaders make a dozen of a pixel. FX at 2560x1440 (GTX 970): 20.2 -> 21.7 fps, two boot pairs,
    // the same picture. Its shaders cache apart (one recompile). GLUE2GPU_NO_LEAN_FETCH=1 keeps the full fetch.
    if (!std::getenv("GLUE2GPU_NO_LEAN_FETCH") && !std::getenv("REGPU_LEAN_FETCH"))
        _putenv_s("REGPU_LEAN_FETCH", "1");

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
