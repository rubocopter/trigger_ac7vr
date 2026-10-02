#include "graphics_bridge.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ac7vr {
namespace {

#ifdef TRIGGER_AC7VR_ABI_TEST
constexpr wchar_t kGraphicsLogPath[] = L"NUL";
#else
constexpr wchar_t kGraphicsLogPath[] = L"E:\\trigger_ac7vr\\evidence\\graphics_bridge.log";
#endif

SRWLOCK g_log_lock = SRWLOCK_INIT;
SRWLOCK g_pose_lock = SRWLOCK_INIT;
BridgeHeadPose g_latest_pose{{0, 0, 0, 1}, {0, 0, 0}, false, false};
std::array<BridgeEyeFov, 2> g_latest_eye_fovs{};
bool g_eye_fovs_available = false;

void Log(const char* format, ...) {
    char line[1024]{};
    va_list args;
    va_start(args, format);
    const int length = _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    if (length <= 0) return;

    AcquireSRWLockExclusive(&g_log_lock);
    HANDLE file = CreateFileW(kGraphicsLogPath, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

double ClockMilliseconds() {
    static const double scale = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return 1000.0 / static_cast<double>(frequency.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * scale;
}

struct FrameTimings {
    bool frame_begun = false;
    bool submitted = false;
    double wait_ms = 0;
    double copy_cpu_ms = 0;
    double end_ms = 0;
    double period_ms = 0;
};

void RecordFrameTimings(IDXGISwapChain* swapchain, UINT sync_interval, UINT flags,
                        double entry, double bridge_end, double present_end,
                        const FrameTimings& frame) {
    // Per render thread; no extra lock or file write on ordinary frames.
    // The gap includes game rendering/backpressure. Copy time measures CPU
    // waits and command enqueueing, not asynchronous GPU execution.
    struct Window {
        IDXGISwapChain* swapchain = nullptr;
        double previous_end = 0;
        unsigned count = 0;
        unsigned submitted = 0;
        std::array<double, 7> sums{};
        std::array<double, 7> maxima{};
    };
    static thread_local Window window;
    if (window.swapchain != swapchain || !frame.frame_begun) {
        window = {};
        window.swapchain = swapchain;
    }
    if (!frame.frame_begun) return;
    if (window.previous_end > 0) {
        const std::array<double, 7> values{
            entry - window.previous_end, bridge_end - entry, frame.wait_ms,
            frame.copy_cpu_ms, frame.end_ms, present_end - bridge_end,
            present_end - window.previous_end};
        for (std::size_t i = 0; i < values.size(); ++i) {
            window.sums[i] += values[i];
            window.maxima[i] = std::max(window.maxima[i], values[i]);
        }
        ++window.count;
        window.submitted += frame.submitted ? 1u : 0u;
    }
    window.previous_end = present_end;
    if (window.count < 120) return;
    const double n = static_cast<double>(window.count);
    Log("frame_timing samples=%u submitted=%u xr_period_ms=%.3f sync_last=%u flags_last=0x%X "
        "present_hz=%.2f avg_ms(game_gap,bridge,xr_wait,copy_cpu,xr_end,monitor,interval)="
        "(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f) "
        "max_ms=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\r\n",
        window.count, window.submitted, frame.period_ms, sync_interval, flags,
        window.sums[6] > 0 ? 1000.0 * n / window.sums[6] : 0.0,
        window.sums[0] / n, window.sums[1] / n, window.sums[2] / n,
        window.sums[3] / n, window.sums[4] / n, window.sums[5] / n, window.sums[6] / n,
        window.maxima[0], window.maxima[1], window.maxima[2], window.maxima[3],
        window.maxima[4], window.maxima[5], window.maxima[6]);
    window.count = 0;
    window.submitted = 0;
    window.sums = {};
    window.maxima = {};
}

void PublishPose(const XrView* views, std::uint32_t count, XrViewStateFlags flags) {
    if (!views || count < 2) return;

    BridgeHeadPose pose{};
    pose.orientation_valid = (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    pose.position_valid = (flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    pose.orientation = ConvertOpenXrOrientationToUnreal(
        views[0].pose.orientation.x, views[0].pose.orientation.y,
        views[0].pose.orientation.z, views[0].pose.orientation.w);
    const float center_x = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
    const float center_y = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
    const float center_z = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);
    pose.position_meters = ConvertOpenXrPositionToUnreal(center_x, center_y, center_z);

    AcquireSRWLockExclusive(&g_pose_lock);
    g_latest_pose = pose;
    for (std::size_t eye = 0; eye < g_latest_eye_fovs.size(); ++eye) {
        const auto& fov = views[eye].fov;
        g_latest_eye_fovs[eye] = {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
    }
    g_eye_fovs_available = true;
    ReleaseSRWLockExclusive(&g_pose_lock);
}

bool EqualsIgnoreCase(const char* a, const char* b) {
    return a && b && _stricmp(a, b) == 0;
}

bool PatchImport(HMODULE module, const char* dll_name, const char* function_name,
                 void* replacement, void** original) {
    if (!module || !dll_name || !function_name || !replacement || !original) return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* imported_dll = reinterpret_cast<const char*>(base + descriptor->Name);
        if (!EqualsIgnoreCase(imported_dll, dll_name)) continue;
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) return false;

        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(by_name->Name), function_name) != 0) continue;

            auto** slot = reinterpret_cast<void**>(&iat->u1.Function);
            DWORD old_protect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) return false;
            if (!*original) *original = *slot;
            *slot = replacement;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            return true;
        }
    }
    return false;
}

bool PatchVtableSlot(void** vtable, std::size_t slot, void* replacement, void** original) {
    if (!vtable || !replacement || !original) return false;
    auto** entry = &vtable[slot];
    if (*entry == replacement) return true;
    DWORD old_protect = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) return false;
    if (!*original) *original = *entry;
    *entry = replacement;
    DWORD ignored = 0;
    VirtualProtect(entry, sizeof(void*), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void*));
    return true;
}

using CreateDxgiFactoryFn = HRESULT (WINAPI*)(REFIID, void**);
using FactoryCreateSwapChainFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
                                                              DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using FactoryCreateSwapChainForHwndFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using FactoryCreateSwapChainForCoreWindowFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*,
    const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using FactoryCreateSwapChainForCompositionFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*,
    const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using SwapChainPresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using SwapChainResizeBuffersFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using SwapChainPresent1Fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

CreateDxgiFactoryFn g_original_create_factory = nullptr;
CreateDxgiFactoryFn g_original_create_factory1 = nullptr;
std::atomic<bool> g_import_hooks_installed{false};
std::atomic<bool> g_logged_present_hook{false};
std::atomic<bool> g_logged_present1_hook{false};

struct FactoryHooks {
    void** vtable = nullptr;
    FactoryCreateSwapChainFn create_swap_chain = nullptr;
    FactoryCreateSwapChainForHwndFn create_swap_chain_for_hwnd = nullptr;
    FactoryCreateSwapChainForCoreWindowFn create_swap_chain_for_core_window = nullptr;
    FactoryCreateSwapChainForCompositionFn create_swap_chain_for_composition = nullptr;
};

struct SwapChainHooks {
    void** vtable = nullptr;
    SwapChainPresentFn present = nullptr;
    SwapChainResizeBuffersFn resize_buffers = nullptr;
    SwapChainPresent1Fn present1 = nullptr;
};

constexpr std::size_t kMaxHookedVtables = 12;
SRWLOCK g_factory_hook_lock = SRWLOCK_INIT;
SRWLOCK g_swapchain_hook_lock = SRWLOCK_INIT;
std::array<FactoryHooks, kMaxHookedVtables> g_factory_hooks{};
std::array<SwapChainHooks, kMaxHookedVtables> g_swapchain_hooks{};

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE HookCreateSwapChainForCoreWindow(IDXGIFactory2*, IUnknown*, IUnknown*,
    const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE HookCreateSwapChainForComposition(IDXGIFactory2*, IUnknown*,
    const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain*, UINT, UINT);
HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

FactoryHooks* FindFactoryHooks(void** vtable) {
    for (auto& hooks : g_factory_hooks) if (hooks.vtable == vtable) return &hooks;
    return nullptr;
}

SwapChainHooks* FindSwapChainHooks(void** vtable) {
    for (auto& hooks : g_swapchain_hooks) if (hooks.vtable == vtable) return &hooks;
    return nullptr;
}

void PatchFactoryVtable(void* object, bool factory2) {
    if (!object) return;
    auto** vtable = *reinterpret_cast<void***>(object);

    AcquireSRWLockExclusive(&g_factory_hook_lock);
    FactoryHooks* hooks = FindFactoryHooks(vtable);
    if (!hooks) {
        for (auto& candidate : g_factory_hooks) {
            if (!candidate.vtable) {
                candidate.vtable = vtable;
                hooks = &candidate;
                break;
            }
        }
    }
    if (hooks) {
        PatchVtableSlot(vtable, 10, reinterpret_cast<void*>(&HookCreateSwapChain),
                        reinterpret_cast<void**>(&hooks->create_swap_chain));
        if (factory2) {
            PatchVtableSlot(vtable, 15, reinterpret_cast<void*>(&HookCreateSwapChainForHwnd),
                            reinterpret_cast<void**>(&hooks->create_swap_chain_for_hwnd));
            PatchVtableSlot(vtable, 16, reinterpret_cast<void*>(&HookCreateSwapChainForCoreWindow),
                            reinterpret_cast<void**>(&hooks->create_swap_chain_for_core_window));
            PatchVtableSlot(vtable, 24, reinterpret_cast<void*>(&HookCreateSwapChainForComposition),
                            reinterpret_cast<void**>(&hooks->create_swap_chain_for_composition));
        }
    }
    ReleaseSRWLockExclusive(&g_factory_hook_lock);
}

void PatchAllFactoryInterfaces(IUnknown* object) {
    if (!object) return;
    PatchFactoryVtable(object, false);

    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
        PatchFactoryVtable(factory, false);
        factory->Release();
    }
    IDXGIFactory1* factory1 = nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory1)))) {
        PatchFactoryVtable(factory1, false);
        factory1->Release();
    }
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2)))) {
        PatchFactoryVtable(factory2, true);
        factory2->Release();
    }
}

void PatchSwapChain(void* object) {
    if (!object) return;
    auto** vtable = *reinterpret_cast<void***>(object);

    AcquireSRWLockExclusive(&g_swapchain_hook_lock);
    SwapChainHooks* hooks = FindSwapChainHooks(vtable);
    if (!hooks) {
        for (auto& candidate : g_swapchain_hooks) {
            if (!candidate.vtable) {
                candidate.vtable = vtable;
                hooks = &candidate;
                break;
            }
        }
    }
    if (hooks) {
        PatchVtableSlot(vtable, 8, reinterpret_cast<void*>(&HookPresent),
                        reinterpret_cast<void**>(&hooks->present));
        PatchVtableSlot(vtable, 13, reinterpret_cast<void*>(&HookResizeBuffers),
                        reinterpret_cast<void**>(&hooks->resize_buffers));
    }
    ReleaseSRWLockExclusive(&g_swapchain_hook_lock);

    IDXGISwapChain1* swapchain1 = nullptr;
    auto* unknown = reinterpret_cast<IUnknown*>(object);
    if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapchain1)))) {
        auto** vtable1 = *reinterpret_cast<void***>(swapchain1);
        AcquireSRWLockExclusive(&g_swapchain_hook_lock);
        SwapChainHooks* hooks1 = FindSwapChainHooks(vtable1);
        if (!hooks1) {
            for (auto& candidate : g_swapchain_hooks) {
                if (!candidate.vtable) {
                    candidate.vtable = vtable1;
                    hooks1 = &candidate;
                    break;
                }
            }
        }
        if (hooks1) {
            PatchVtableSlot(vtable1, 8, reinterpret_cast<void*>(&HookPresent),
                            reinterpret_cast<void**>(&hooks1->present));
            PatchVtableSlot(vtable1, 13, reinterpret_cast<void*>(&HookResizeBuffers),
                            reinterpret_cast<void**>(&hooks1->resize_buffers));
            PatchVtableSlot(vtable1, 22, reinterpret_cast<void*>(&HookPresent1),
                            reinterpret_cast<void**>(&hooks1->present1));
        }
        ReleaseSRWLockExclusive(&g_swapchain_hook_lock);
        swapchain1->Release();
    }
}

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
};

struct OpenXrContext {
    HMODULE loader = nullptr;
    PFN_xrGetInstanceProcAddr get_instance_proc_addr = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties enumerate_instance_extensions = nullptr;
    PFN_xrCreateInstance create_instance = nullptr;
    PFN_xrDestroyInstance destroy_instance = nullptr;
    PFN_xrGetInstanceProperties get_instance_properties = nullptr;
    PFN_xrGetSystem get_system = nullptr;
    PFN_xrGetSystemProperties get_system_properties = nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR get_d3d11_requirements = nullptr;
    PFN_xrCreateSession create_session = nullptr;
    PFN_xrDestroySession destroy_session = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerate_view_configuration_views = nullptr;
    PFN_xrEnumerateEnvironmentBlendModes enumerate_environment_blend_modes = nullptr;
    PFN_xrCreateReferenceSpace create_reference_space = nullptr;
    PFN_xrDestroySpace destroy_space = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerate_swapchain_formats = nullptr;
    PFN_xrCreateSwapchain create_swapchain = nullptr;
    PFN_xrDestroySwapchain destroy_swapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerate_swapchain_images = nullptr;
    PFN_xrPollEvent poll_event = nullptr;
    PFN_xrBeginSession begin_session = nullptr;
    PFN_xrEndSession end_session = nullptr;
    PFN_xrWaitFrame wait_frame = nullptr;
    PFN_xrBeginFrame begin_frame = nullptr;
    PFN_xrLocateViews locate_views = nullptr;
    PFN_xrAcquireSwapchainImage acquire_swapchain_image = nullptr;
    PFN_xrWaitSwapchainImage wait_swapchain_image = nullptr;
    PFN_xrReleaseSwapchainImage release_swapchain_image = nullptr;
    PFN_xrEndFrame end_frame = nullptr;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local_space = XR_NULL_HANDLE;
    XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
    bool session_running = false;
    XrEnvironmentBlendMode blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    EyeSwapchain eyes[2];
    ID3D11Device* bound_device = nullptr;
    DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint64_t submitted_frames = 0;
    ULONGLONG next_init_attempt = 0;
    bool logged_source = false;
};

OpenXrContext g_xr;
SRWLOCK g_xr_lock = SRWLOCK_INIT;

class ExclusiveSrwLockGuard {
public:
    explicit ExclusiveSrwLockGuard(SRWLOCK& lock) : lock_(lock) {
        AcquireSRWLockExclusive(&lock_);
    }
    ~ExclusiveSrwLockGuard() { ReleaseSRWLockExclusive(&lock_); }

    ExclusiveSrwLockGuard(const ExclusiveSrwLockGuard&) = delete;
    ExclusiveSrwLockGuard& operator=(const ExclusiveSrwLockGuard&) = delete;

private:
    SRWLOCK& lock_;
};

template <typename T>
bool LoadInstanceFunction(const char* name, T& target) {
    PFN_xrVoidFunction function = nullptr;
    if (!g_xr.get_instance_proc_addr ||
        XR_FAILED(g_xr.get_instance_proc_addr(g_xr.instance, name, &function)) || !function) {
        Log("openxr missing_function name=%s\r\n", name);
        return false;
    }
    target = reinterpret_cast<T>(function);
    return true;
}

HMODULE LoadOpenXrLoader() {
    if (HMODULE loader = LoadLibraryW(L"openxr_loader.dll")) return loader;

    wchar_t active_runtime[2048]{};
    DWORD bytes = sizeof(active_runtime);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                     RRF_RT_REG_SZ, nullptr, active_runtime, &bytes) != ERROR_SUCCESS) {
        return nullptr;
    }
    std::wstring runtime_path(active_runtime);
    const auto slash = runtime_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return nullptr;
    std::wstring directory = runtime_path.substr(0, slash);
    std::wstring loader_path = directory + L"\\bin\\win64\\openxr_loader.dll";
    return LoadLibraryW(loader_path.c_str());
}

bool HasD3D11Extension() {
    std::uint32_t count = 0;
    if (XR_FAILED(g_xr.enumerate_instance_extensions(nullptr, 0, &count, nullptr)) || !count) return false;
    std::vector<XrExtensionProperties> properties(count);
    for (auto& property : properties) property = {XR_TYPE_EXTENSION_PROPERTIES};
    if (XR_FAILED(g_xr.enumerate_instance_extensions(nullptr, count, &count, properties.data()))) return false;
    return std::any_of(properties.begin(), properties.end(), [](const XrExtensionProperties& property) {
        return strcmp(property.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    });
}

bool LoadOpenXrCoreFunctions() {
    return LoadInstanceFunction("xrDestroyInstance", g_xr.destroy_instance) &&
           LoadInstanceFunction("xrGetInstanceProperties", g_xr.get_instance_properties) &&
           LoadInstanceFunction("xrGetSystem", g_xr.get_system) &&
           LoadInstanceFunction("xrGetSystemProperties", g_xr.get_system_properties) &&
           LoadInstanceFunction("xrGetD3D11GraphicsRequirementsKHR", g_xr.get_d3d11_requirements) &&
           LoadInstanceFunction("xrCreateSession", g_xr.create_session) &&
           LoadInstanceFunction("xrDestroySession", g_xr.destroy_session) &&
           LoadInstanceFunction("xrEnumerateViewConfigurationViews", g_xr.enumerate_view_configuration_views) &&
           LoadInstanceFunction("xrEnumerateEnvironmentBlendModes", g_xr.enumerate_environment_blend_modes) &&
           LoadInstanceFunction("xrCreateReferenceSpace", g_xr.create_reference_space) &&
           LoadInstanceFunction("xrDestroySpace", g_xr.destroy_space) &&
           LoadInstanceFunction("xrEnumerateSwapchainFormats", g_xr.enumerate_swapchain_formats) &&
           LoadInstanceFunction("xrCreateSwapchain", g_xr.create_swapchain) &&
           LoadInstanceFunction("xrDestroySwapchain", g_xr.destroy_swapchain) &&
           LoadInstanceFunction("xrEnumerateSwapchainImages", g_xr.enumerate_swapchain_images) &&
           LoadInstanceFunction("xrPollEvent", g_xr.poll_event) &&
           LoadInstanceFunction("xrBeginSession", g_xr.begin_session) &&
           LoadInstanceFunction("xrEndSession", g_xr.end_session) &&
           LoadInstanceFunction("xrWaitFrame", g_xr.wait_frame) &&
           LoadInstanceFunction("xrBeginFrame", g_xr.begin_frame) &&
           LoadInstanceFunction("xrLocateViews", g_xr.locate_views) &&
           LoadInstanceFunction("xrAcquireSwapchainImage", g_xr.acquire_swapchain_image) &&
           LoadInstanceFunction("xrWaitSwapchainImage", g_xr.wait_swapchain_image) &&
           LoadInstanceFunction("xrReleaseSwapchainImage", g_xr.release_swapchain_image) &&
           LoadInstanceFunction("xrEndFrame", g_xr.end_frame);
}

void ClearPublishedPose() {
    AcquireSRWLockExclusive(&g_pose_lock);
    g_latest_pose = {{0, 0, 0, 1}, {0, 0, 0}, false, false};
    g_latest_eye_fovs = {};
    g_eye_fovs_available = false;
    ReleaseSRWLockExclusive(&g_pose_lock);
}

void DestroyEyeSwapchain(EyeSwapchain& eye) {
    if (eye.handle != XR_NULL_HANDLE && g_xr.destroy_swapchain) {
        const XrResult result = g_xr.destroy_swapchain(eye.handle);
        if (XR_FAILED(result)) {
            Log("openxr destroy_swapchain_failed handle=%p xr=%d\r\n",
                reinterpret_cast<void*>(eye.handle), static_cast<int>(result));
        }
    }
    eye = {};
}

void ResetOpenXrResources(bool preserve_retry_deadline = true) {
    const HMODULE loader = g_xr.loader;
    const auto get_instance_proc_addr = g_xr.get_instance_proc_addr;
    const auto enumerate_instance_extensions = g_xr.enumerate_instance_extensions;
    const auto create_instance = g_xr.create_instance;
    const ULONGLONG retry_deadline = preserve_retry_deadline ? g_xr.next_init_attempt : 0;

    if (g_xr.session_running && g_xr.session_state == XR_SESSION_STATE_STOPPING &&
        g_xr.session != XR_NULL_HANDLE && g_xr.end_session) {
        const XrResult result = g_xr.end_session(g_xr.session);
        if (XR_FAILED(result)) Log("openxr reset_end_session_failed xr=%d\r\n", static_cast<int>(result));
        g_xr.session_running = false;
    }
    DestroyEyeSwapchain(g_xr.eyes[0]);
    DestroyEyeSwapchain(g_xr.eyes[1]);
    if (g_xr.local_space != XR_NULL_HANDLE && g_xr.destroy_space) {
        const XrResult result = g_xr.destroy_space(g_xr.local_space);
        if (XR_FAILED(result)) Log("openxr destroy_space_failed xr=%d\r\n", static_cast<int>(result));
    }
    if (g_xr.session != XR_NULL_HANDLE && g_xr.destroy_session) {
        const XrResult result = g_xr.destroy_session(g_xr.session);
        if (XR_FAILED(result)) Log("openxr destroy_session_failed xr=%d\r\n", static_cast<int>(result));
    }
    if (g_xr.bound_device) {
        g_xr.bound_device->Release();
        g_xr.bound_device = nullptr;
    }
    if (g_xr.instance != XR_NULL_HANDLE && g_xr.destroy_instance) {
        const XrResult result = g_xr.destroy_instance(g_xr.instance);
        if (XR_FAILED(result)) Log("openxr destroy_instance_failed xr=%d\r\n", static_cast<int>(result));
    }

    g_xr = {};
    g_xr.loader = loader;
    g_xr.get_instance_proc_addr = get_instance_proc_addr;
    g_xr.enumerate_instance_extensions = enumerate_instance_extensions;
    g_xr.create_instance = create_instance;
    g_xr.next_init_attempt = retry_deadline;
    ClearPublishedPose();
}

bool OpenXrReadyForFrames() {
    return g_xr.instance != XR_NULL_HANDLE && g_xr.session != XR_NULL_HANDLE &&
           g_xr.local_space != XR_NULL_HANDLE && g_xr.eyes[0].handle != XR_NULL_HANDLE &&
           g_xr.eyes[1].handle != XR_NULL_HANDLE;
}

bool SourceMatchesOpenXr(const D3D11_TEXTURE2D_DESC& source_desc) {
    return g_xr.source_width == source_desc.Width && g_xr.source_height == source_desc.Height &&
           g_xr.source_format == source_desc.Format;
}

bool DeviceMatchesOpenXr(ID3D11Device* device) {
    return g_xr.bound_device == device;
}

bool AdapterMatchesOpenXr(ID3D11Device* device, const XrGraphicsRequirementsD3D11KHR& requirements) {
    IDXGIDevice* dxgi_device = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device)))) return false;
    IDXGIAdapter* adapter = nullptr;
    const HRESULT adapter_result = dxgi_device->GetAdapter(&adapter);
    dxgi_device->Release();
    if (FAILED(adapter_result) || !adapter) return false;
    DXGI_ADAPTER_DESC description{};
    const HRESULT desc_result = adapter->GetDesc(&description);
    adapter->Release();
    if (FAILED(desc_result)) return false;
    const bool matches = description.AdapterLuid.LowPart == requirements.adapterLuid.LowPart &&
                         description.AdapterLuid.HighPart == requirements.adapterLuid.HighPart;
    Log("openxr adapter game_luid=%08X:%08X runtime_luid=%08X:%08X match=%u feature=0x%X min_feature=0x%X\r\n",
        static_cast<unsigned>(description.AdapterLuid.HighPart), description.AdapterLuid.LowPart,
        static_cast<unsigned>(requirements.adapterLuid.HighPart), requirements.adapterLuid.LowPart,
        static_cast<unsigned>(matches), static_cast<unsigned>(device->GetFeatureLevel()),
        static_cast<unsigned>(requirements.minFeatureLevel));
    return matches;
}

bool CreateEyeSwapchain(EyeSwapchain& eye, std::int32_t width, std::int32_t height,
                        DXGI_FORMAT format) {
    DestroyEyeSwapchain(eye);
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = static_cast<std::int64_t>(format);
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XrResult result = g_xr.create_swapchain(g_xr.session, &info, &eye.handle);
    if (XR_FAILED(result)) {
        Log("openxr create_swapchain_failed width=%d height=%d format=%d xr=%d\r\n",
            width, height, static_cast<int>(format), static_cast<int>(result));
        return false;
    }
    std::uint32_t count = 0;
    result = g_xr.enumerate_swapchain_images(eye.handle, 0, &count, nullptr);
    if (XR_FAILED(result) || !count) {
        Log("openxr enumerate_swapchain_images_failed stage=count xr=%d count=%u\r\n",
            static_cast<int>(result), count);
        DestroyEyeSwapchain(eye);
        return false;
    }
    eye.images.resize(count);
    for (auto& image : eye.images) image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
    result = g_xr.enumerate_swapchain_images(
        eye.handle, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
    if (XR_FAILED(result)) {
        Log("openxr enumerate_swapchain_images_failed stage=images xr=%d\r\n",
            static_cast<int>(result));
        DestroyEyeSwapchain(eye);
        return false;
    }
    eye.width = width;
    eye.height = height;
    return true;
}

bool InitializeOpenXr(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source_desc) {
    if (OpenXrReadyForFrames()) {
        if (SourceMatchesOpenXr(source_desc) && DeviceMatchesOpenXr(device)) return true;
        Log("openxr source_or_device_changed old=%ux%u/%d device=%p new=%ux%u/%d device=%p\r\n",
            g_xr.source_width, g_xr.source_height, static_cast<int>(g_xr.source_format),
            g_xr.bound_device, source_desc.Width, source_desc.Height, static_cast<int>(source_desc.Format), device);
        ResetOpenXrResources(false);
    } else if (g_xr.instance != XR_NULL_HANDLE || g_xr.session != XR_NULL_HANDLE ||
               g_xr.local_space != XR_NULL_HANDLE || g_xr.eyes[0].handle != XR_NULL_HANDLE ||
               g_xr.eyes[1].handle != XR_NULL_HANDLE) {
        Log("openxr clearing_partial_initialization\r\n");
        ResetOpenXrResources(true);
    }
    const ULONGLONG now = GetTickCount64();
    if (now < g_xr.next_init_attempt) return false;
    g_xr.next_init_attempt = now + 5000;

    if (!g_xr.loader) {
        g_xr.loader = LoadOpenXrLoader();
        if (!g_xr.loader) {
            Log("openxr loader_not_found win32=%lu\r\n", GetLastError());
            return false;
        }
    }
    if (!g_xr.get_instance_proc_addr || !g_xr.enumerate_instance_extensions || !g_xr.create_instance) {
        g_xr.get_instance_proc_addr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
            GetProcAddress(g_xr.loader, "xrGetInstanceProcAddr"));
        g_xr.enumerate_instance_extensions = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(
            GetProcAddress(g_xr.loader, "xrEnumerateInstanceExtensionProperties"));
        g_xr.create_instance = reinterpret_cast<PFN_xrCreateInstance>(
            GetProcAddress(g_xr.loader, "xrCreateInstance"));
        if (!g_xr.get_instance_proc_addr || !g_xr.enumerate_instance_extensions || !g_xr.create_instance) {
            Log("openxr loader_missing_exports\r\n");
            return false;
        }
    }

    if (!HasD3D11Extension()) {
        Log("openxr d3d11_extension_missing\r\n");
        return false;
    }

    const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(instance_info.applicationInfo.applicationName, "trigger_ac7vr");
    instance_info.applicationInfo.applicationVersion = 1;
    strcpy_s(instance_info.applicationInfo.engineName, "AC7 UE4.18 bridge");
    instance_info.applicationInfo.engineVersion = 1;
    // SteamVR's currently active OpenXR runtime rejects a 1.1 application
    // version with XR_ERROR_API_VERSION_UNSUPPORTED. The bridge only uses
    // OpenXR 1.0 entry points, so request the 1.0 API explicitly while still
    // building against the newer Khronos headers.
    instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instance_info.enabledExtensionCount = 1;
    instance_info.enabledExtensionNames = extensions;
    XrResult result = g_xr.create_instance(&instance_info, &g_xr.instance);
    if (XR_FAILED(result)) {
        Log("openxr create_instance_failed xr=%d api=%u.%u.%u\r\n", static_cast<int>(result),
            XR_VERSION_MAJOR(instance_info.applicationInfo.apiVersion),
            XR_VERSION_MINOR(instance_info.applicationInfo.apiVersion),
            XR_VERSION_PATCH(instance_info.applicationInfo.apiVersion));
        g_xr.instance = XR_NULL_HANDLE;
        return false;
    }
    Log("openxr instance_created api=%u.%u.%u\r\n",
        XR_VERSION_MAJOR(instance_info.applicationInfo.apiVersion),
        XR_VERSION_MINOR(instance_info.applicationInfo.apiVersion),
        XR_VERSION_PATCH(instance_info.applicationInfo.apiVersion));
    if (!LoadOpenXrCoreFunctions()) {
        ResetOpenXrResources(true);
        return false;
    }

    XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(g_xr.get_instance_properties(g_xr.instance, &instance_properties))) {
        Log("openxr runtime name=%s version=%u.%u.%u\r\n", instance_properties.runtimeName,
            XR_VERSION_MAJOR(instance_properties.runtimeVersion), XR_VERSION_MINOR(instance_properties.runtimeVersion),
            XR_VERSION_PATCH(instance_properties.runtimeVersion));
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = g_xr.get_system(g_xr.instance, &system_info, &g_xr.system);
    if (XR_FAILED(result)) {
        Log("openxr get_system_failed xr=%d\r\n", static_cast<int>(result));
        ResetOpenXrResources(true);
        return false;
    }
    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(g_xr.get_system_properties(g_xr.instance, g_xr.system, &system_properties))) {
        Log("openxr system name=%s vendor=%u max=%ux%u layers=%u\r\n",
            system_properties.systemName, system_properties.vendorId,
            system_properties.graphicsProperties.maxSwapchainImageWidth,
            system_properties.graphicsProperties.maxSwapchainImageHeight,
            system_properties.graphicsProperties.maxLayerCount);
    }

    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    result = g_xr.get_d3d11_requirements(g_xr.instance, g_xr.system, &requirements);
    if (XR_FAILED(result) || !AdapterMatchesOpenXr(device, requirements)) {
        Log("openxr graphics_requirements_failed xr=%d\r\n", static_cast<int>(result));
        ResetOpenXrResources(true);
        return false;
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = g_xr.system;
    result = g_xr.create_session(g_xr.instance, &session_info, &g_xr.session);
    if (XR_FAILED(result)) {
        Log("openxr create_session_failed xr=%d\r\n", static_cast<int>(result));
        g_xr.session = XR_NULL_HANDLE;
        ResetOpenXrResources(true);
        return false;
    }
    g_xr.bound_device = device;
    g_xr.bound_device->AddRef();

    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    result = g_xr.create_reference_space(g_xr.session, &space_info, &g_xr.local_space);
    if (XR_FAILED(result)) {
        Log("openxr create_space_failed xr=%d\r\n", static_cast<int>(result));
        ResetOpenXrResources(true);
        return false;
    }

    std::uint32_t view_count = 0;
    result = g_xr.enumerate_view_configuration_views(
        g_xr.instance, g_xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0, &view_count, nullptr);
    if (XR_FAILED(result) || view_count < 2) {
        Log("openxr stereo_views_unavailable xr=%d count=%u\r\n", static_cast<int>(result), view_count);
        ResetOpenXrResources(true);
        return false;
    }
    std::vector<XrViewConfigurationView> configs(view_count);
    for (auto& config : configs) config = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
    result = g_xr.enumerate_view_configuration_views(
        g_xr.instance, g_xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        view_count, &view_count, configs.data());
    if (XR_FAILED(result)) {
        Log("openxr enumerate_views_failed xr=%d\r\n", static_cast<int>(result));
        ResetOpenXrResources(true);
        return false;
    }
    Log("openxr views left_recommended=%ux%u right_recommended=%ux%u\r\n",
        configs[0].recommendedImageRectWidth, configs[0].recommendedImageRectHeight,
        configs[1].recommendedImageRectWidth, configs[1].recommendedImageRectHeight);

    std::uint32_t blend_count = 0;
    if (XR_SUCCEEDED(g_xr.enumerate_environment_blend_modes(
            g_xr.instance, g_xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &blend_count, nullptr)) && blend_count) {
        std::vector<XrEnvironmentBlendMode> modes(blend_count);
        if (XR_SUCCEEDED(g_xr.enumerate_environment_blend_modes(
                g_xr.instance, g_xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                blend_count, &blend_count, modes.data()))) {
            g_xr.blend_mode = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != modes.end()
                ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : modes[0];
        }
    }

    std::uint32_t format_count = 0;
    result = g_xr.enumerate_swapchain_formats(g_xr.session, 0, &format_count, nullptr);
    if (XR_FAILED(result) || !format_count) {
        Log("openxr enumerate_formats_failed stage=count xr=%d count=%u\r\n",
            static_cast<int>(result), format_count);
        ResetOpenXrResources(true);
        return false;
    }
    std::vector<std::int64_t> formats(format_count);
    result = g_xr.enumerate_swapchain_formats(g_xr.session, format_count, &format_count, formats.data());
    if (XR_FAILED(result)) {
        Log("openxr enumerate_formats_failed stage=formats xr=%d\r\n", static_cast<int>(result));
        ResetOpenXrResources(true);
        return false;
    }
    const auto source_format = static_cast<std::int64_t>(source_desc.Format);
    if (std::find(formats.begin(), formats.end(), source_format) == formats.end()) {
        Log("openxr source_format_unsupported format=%d runtime_formats=", static_cast<int>(source_desc.Format));
        for (auto format : formats) Log("%lld,", static_cast<long long>(format));
        Log("\r\n");
        ResetOpenXrResources(true);
        return false;
    }
    g_xr.source_format = source_desc.Format;
    g_xr.source_width = source_desc.Width;
    g_xr.source_height = source_desc.Height;

    const std::int32_t left_width = static_cast<std::int32_t>(source_desc.Width / 2);
    const std::int32_t right_width = static_cast<std::int32_t>(source_desc.Width - source_desc.Width / 2);
    const std::int32_t height = static_cast<std::int32_t>(source_desc.Height);
    if (!CreateEyeSwapchain(g_xr.eyes[0], left_width, height, source_desc.Format) ||
        !CreateEyeSwapchain(g_xr.eyes[1], right_width, height, source_desc.Format)) {
        ResetOpenXrResources(true);
        return false;
    }
    Log("openxr initialized source=%ux%u eye=%dx%d/%dx%d format=%d\r\n",
        source_desc.Width, source_desc.Height, left_width, height, right_width, height,
        static_cast<int>(source_desc.Format));
    return true;
}

bool PollOpenXrEvents() {
    if (!g_xr.session || !g_xr.poll_event) return false;
    for (;;) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult result = g_xr.poll_event(g_xr.instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) break;
        if (XR_FAILED(result)) {
            Log("openxr poll_event_failed xr=%d\r\n", static_cast<int>(result));
            g_xr.next_init_attempt = GetTickCount64() + 5000;
            ResetOpenXrResources(true);
            return false;
        }
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
        const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
        g_xr.session_state = changed->state;
        Log("openxr session_state=%d\r\n", static_cast<int>(changed->state));
        if (changed->state == XR_SESSION_STATE_READY && !g_xr.session_running) {
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            const XrResult begin_result = g_xr.begin_session(g_xr.session, &begin);
            if (XR_SUCCEEDED(begin_result)) g_xr.session_running = true;
            Log("openxr begin_session xr=%d running=%u\r\n", static_cast<int>(begin_result),
                static_cast<unsigned>(g_xr.session_running));
            if (XR_FAILED(begin_result)) {
                g_xr.next_init_attempt = GetTickCount64() + 5000;
                ResetOpenXrResources(true);
                return false;
            }
        } else if (changed->state == XR_SESSION_STATE_STOPPING && g_xr.session_running) {
            const XrResult end_result = g_xr.end_session(g_xr.session);
            g_xr.session_running = false;
            Log("openxr end_session xr=%d\r\n", static_cast<int>(end_result));
        } else if (changed->state == XR_SESSION_STATE_EXITING ||
                   changed->state == XR_SESSION_STATE_LOSS_PENDING) {
            Log("openxr session_terminal_state=%d\r\n", static_cast<int>(changed->state));
            g_xr.next_init_attempt = GetTickCount64() + 5000;
            ResetOpenXrResources(true);
            return false;
        }
    }
    return g_xr.session != XR_NULL_HANDLE;
}

bool CopyEye(ID3D11DeviceContext* context, ID3D11Texture2D* source, int eye,
             const D3D11_BOX& box, std::uint32_t& acquired_index, bool& reset_required) {
    EyeSwapchain& swapchain = g_xr.eyes[eye];
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = g_xr.acquire_swapchain_image(swapchain.handle, &acquire, &acquired_index);
    if (XR_FAILED(result)) {
        Log("openxr acquire_swapchain_image_failed eye=%d xr=%d\r\n", eye, static_cast<int>(result));
        reset_required = true;
        return false;
    }
    if (acquired_index >= swapchain.images.size()) {
        Log("openxr acquire_swapchain_image_bad_index eye=%d index=%u count=%zu\r\n",
            eye, acquired_index, swapchain.images.size());
        reset_required = true;
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    result = g_xr.wait_swapchain_image(swapchain.handle, &wait);
    if (XR_FAILED(result)) {
        Log("openxr wait_swapchain_image_failed eye=%d xr=%d\r\n", eye, static_cast<int>(result));
        reset_required = true;
        return false;
    }
    context->CopySubresourceRegion(swapchain.images[acquired_index].texture, 0, 0, 0, 0,
                                   source, 0, &box);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    result = g_xr.release_swapchain_image(swapchain.handle, &release);
    if (XR_FAILED(result)) {
        Log("openxr release_swapchain_image_failed eye=%d xr=%d\r\n", eye, static_cast<int>(result));
        reset_required = true;
    }
    return XR_SUCCEEDED(result);
}

void SubmitOpenXrFrame(IDXGISwapChain* swapchain, FrameTimings& timing) {
    if (!swapchain) return;
    ExclusiveSrwLockGuard xr_guard(g_xr_lock);
    ID3D11Device* device = nullptr;
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device) return;
    ID3D11Texture2D* source = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source))) || !source) {
        device->Release();
        return;
    }
    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);
    if (!g_xr.logged_source) {
        g_xr.logged_source = true;
        Log("dxgi source_swapchain=%p backbuffer=%ux%u format=%d samples=%u bind=0x%X\r\n",
            swapchain, source_desc.Width, source_desc.Height, static_cast<int>(source_desc.Format),
            source_desc.SampleDesc.Count, source_desc.BindFlags);
    }

    if (source_desc.Width < 2 || source_desc.Height == 0 || source_desc.SampleDesc.Count != 1 ||
        !InitializeOpenXr(device, source_desc)) {
        source->Release();
        device->Release();
        return;
    }
    if (!PollOpenXrEvents() || !g_xr.session_running) {
        source->Release();
        device->Release();
        return;
    }

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    const double wait_start = ClockMilliseconds();
    XrResult result = g_xr.wait_frame(g_xr.session, &wait_info, &frame_state);
    timing.wait_ms = ClockMilliseconds() - wait_start;
    timing.period_ms = static_cast<double>(frame_state.predictedDisplayPeriod) / 1000000.0;
    if (XR_FAILED(result)) {
        Log("openxr wait_frame_failed xr=%d\r\n", static_cast<int>(result));
        g_xr.next_init_attempt = GetTickCount64() + 5000;
        ResetOpenXrResources(true);
        source->Release();
        device->Release();
        return;
    }
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    result = g_xr.begin_frame(g_xr.session, &begin_info);
    if (XR_FAILED(result)) {
        Log("openxr begin_frame_failed xr=%d\r\n", static_cast<int>(result));
        g_xr.next_init_attempt = GetTickCount64() + 5000;
        ResetOpenXrResources(true);
        source->Release();
        device->Release();
        return;
    }
    timing.frame_begun = true;

    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = frame_state.predictedDisplayTime;
    locate_info.space = g_xr.local_space;
    std::uint32_t view_count = 0;
    result = g_xr.locate_views(g_xr.session, &locate_info, &view_state,
                               static_cast<std::uint32_t>(views.size()), &view_count, views.data());
    const bool located = XR_SUCCEEDED(result) && view_count >= 2;
    if (!located) {
        Log("openxr locate_views_failed xr=%d count=%u flags=0x%llX\r\n",
            static_cast<int>(result), view_count,
            static_cast<unsigned long long>(view_state.viewStateFlags));
    }
    if (located) PublishPose(views.data(), view_count, view_state.viewStateFlags);

    bool copied = false;
    bool reset_required = false;
    const double copy_start = ClockMilliseconds();
    if (located && frame_state.shouldRender) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        if (context) {
            const UINT left_width = source_desc.Width / 2;
            D3D11_BOX left_box{0, 0, 0, left_width, source_desc.Height, 1};
            D3D11_BOX right_box{left_width, 0, 0, source_desc.Width, source_desc.Height, 1};
            std::uint32_t left_index = 0, right_index = 0;
            copied = CopyEye(context, source, 0, left_box, left_index, reset_required) &&
                     CopyEye(context, source, 1, right_box, right_index, reset_required);
            context->Release();
        }
    }
    timing.copy_cpu_ms = ClockMilliseconds() - copy_start;

    std::array<XrCompositionLayerProjectionView, 2> projection_views{{
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    }};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    if (copied) {
        for (int eye = 0; eye < 2; ++eye) {
            projection_views[eye].pose = views[eye].pose;
            projection_views[eye].fov = views[eye].fov;
            projection_views[eye].subImage.swapchain = g_xr.eyes[eye].handle;
            projection_views[eye].subImage.imageRect.extent = {g_xr.eyes[eye].width, g_xr.eyes[eye].height};
            projection_views[eye].subImage.imageArrayIndex = 0;
        }
        projection.space = g_xr.local_space;
        projection.viewCount = 2;
        projection.views = projection_views.data();
    }
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
    };
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = g_xr.blend_mode;
    end_info.layerCount = copied ? 1u : 0u;
    end_info.layers = copied ? layers : nullptr;
    const double end_start = ClockMilliseconds();
    result = g_xr.end_frame(g_xr.session, &end_info);
    timing.end_ms = ClockMilliseconds() - end_start;
    timing.submitted = XR_SUCCEEDED(result) && copied;
    if (XR_SUCCEEDED(result) && copied) {
        ++g_xr.submitted_frames;
        if (g_xr.submitted_frames == 1 || (g_xr.submitted_frames % 900) == 0) {
            Log("openxr submitted_frames=%llu pose_flags=0x%llX\r\n",
                static_cast<unsigned long long>(g_xr.submitted_frames),
                static_cast<unsigned long long>(view_state.viewStateFlags));
        }
    } else if (XR_FAILED(result)) {
        Log("openxr end_frame_failed xr=%d\r\n", static_cast<int>(result));
    }
    if (reset_required || XR_FAILED(result)) {
        g_xr.next_init_attempt = GetTickCount64() + 5000;
        ResetOpenXrResources(true);
    }
    source->Release();
    device->Release();
}

FactoryHooks SnapshotFactoryHooks(void** vtable) {
    FactoryHooks result{};
    AcquireSRWLockShared(&g_factory_hook_lock);
    if (auto* hooks = FindFactoryHooks(vtable)) result = *hooks;
    ReleaseSRWLockShared(&g_factory_hook_lock);
    return result;
}

SwapChainHooks SnapshotSwapChainHooks(void** vtable) {
    SwapChainHooks result{};
    AcquireSRWLockShared(&g_swapchain_hook_lock);
    if (auto* hooks = FindSwapChainHooks(vtable)) result = *hooks;
    ReleaseSRWLockShared(&g_swapchain_hook_lock);
    return result;
}

HRESULT WINAPI HookCreateFactoryCommon(CreateDxgiFactoryFn original, REFIID riid, void** out) {
    if (!original) return E_FAIL;
    const HRESULT result = original(riid, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchAllFactoryInterfaces(reinterpret_cast<IUnknown*>(*out));
        Log("dxgi factory_created object=%p\r\n", *out);
    }
    return result;
}

HRESULT WINAPI HookCreateDXGIFactory(REFIID riid, void** out) {
    return HookCreateFactoryCommon(g_original_create_factory, riid, out);
}

HRESULT WINAPI HookCreateDXGIFactory1(REFIID riid, void** out) {
    return HookCreateFactoryCommon(g_original_create_factory1, riid, out);
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                               DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    const auto hooks = SnapshotFactoryHooks(*reinterpret_cast<void***>(self));
    if (!hooks.create_swap_chain) return E_FAIL;
    const HRESULT result = hooks.create_swap_chain(self, device, desc, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchSwapChain(*out);
        Log("dxgi create_swapchain result=%p size=%ux%u format=%d\r\n", *out,
            desc ? desc->BufferDesc.Width : 0, desc ? desc->BufferDesc.Height : 0,
            desc ? static_cast<int>(desc->BufferDesc.Format) : -1);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGIOutput* output, IDXGISwapChain1** out) {
    const auto hooks = SnapshotFactoryHooks(*reinterpret_cast<void***>(self));
    if (!hooks.create_swap_chain_for_hwnd) return E_FAIL;
    const HRESULT result = hooks.create_swap_chain_for_hwnd(self, device, hwnd, desc, fullscreen, output, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchSwapChain(*out);
        Log("dxgi create_swapchain_for_hwnd result=%p size=%ux%u format=%d\r\n", *out,
            desc ? desc->Width : 0, desc ? desc->Height : 0, desc ? static_cast<int>(desc->Format) : -1);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* device, IUnknown* window,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out) {
    const auto hooks = SnapshotFactoryHooks(*reinterpret_cast<void***>(self));
    if (!hooks.create_swap_chain_for_core_window) return E_FAIL;
    const HRESULT result = hooks.create_swap_chain_for_core_window(self, device, window, desc, output, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchSwapChain(*out);
        Log("dxgi create_swapchain_for_core_window result=%p\r\n", *out);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out) {
    const auto hooks = SnapshotFactoryHooks(*reinterpret_cast<void***>(self));
    if (!hooks.create_swap_chain_for_composition) return E_FAIL;
    const HRESULT result = hooks.create_swap_chain_for_composition(self, device, desc, output, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchSwapChain(*out);
        Log("dxgi create_swapchain_for_composition result=%p\r\n", *out);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    const auto hooks = SnapshotSwapChainHooks(*reinterpret_cast<void***>(self));
    if (!hooks.present) return E_FAIL;
    if (!g_logged_present_hook.exchange(true, std::memory_order_relaxed)) {
        Log("dxgi present_hook=Present swapchain=%p\r\n", self);
    }
    const double entry = ClockMilliseconds();
    FrameTimings timing{};
    SubmitOpenXrFrame(self, timing);
    const double bridge_end = ClockMilliseconds();
    const HRESULT result = hooks.present(self, sync_interval, flags);
    const double present_end = ClockMilliseconds();
    RecordFrameTimings(self, sync_interval, flags, entry, bridge_end, present_end, timing);
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* self, UINT sync_interval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* parameters) {
    const auto hooks = SnapshotSwapChainHooks(*reinterpret_cast<void***>(self));
    if (!hooks.present1) return E_FAIL;
    if (!g_logged_present1_hook.exchange(true, std::memory_order_relaxed)) {
        Log("dxgi present_hook=Present1 swapchain=%p\r\n", self);
    }
    const double entry = ClockMilliseconds();
    FrameTimings timing{};
    SubmitOpenXrFrame(self, timing);
    const double bridge_end = ClockMilliseconds();
    const HRESULT result = hooks.present1(self, sync_interval, flags, parameters);
    const double present_end = ClockMilliseconds();
    RecordFrameTimings(self, sync_interval, flags, entry, bridge_end, present_end, timing);
    return result;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT buffer_count, UINT width, UINT height,
                                            DXGI_FORMAT format, UINT flags) {
    const auto hooks = SnapshotSwapChainHooks(*reinterpret_cast<void***>(self));
    if (!hooks.resize_buffers) return E_FAIL;
    Log("dxgi resize_buffers count=%u size=%ux%u format=%d flags=0x%X\r\n",
        buffer_count, width, height, static_cast<int>(format), flags);
    {
        ExclusiveSrwLockGuard xr_guard(g_xr_lock);
        ResetOpenXrResources(false);
    }
    return hooks.resize_buffers(self, buffer_count, width, height, format, flags);
}

}  // namespace

BridgeVector3 ConvertOpenXrPositionToUnreal(float x, float y, float z) {
    return {-z, x, y};
}

BridgeQuat ConvertOpenXrOrientationToUnreal(float x, float y, float z, float w) {
    // M * R(q) * M^-1 for M:(x,y,z)_xr -> (-z,x,y)_ue. The equivalent
    // quaternion with the original scalar sign is (z,-x,-y,w).
    return {z, -x, -y, w};
}

bool InstallGraphicsBridgeHooks() {
    if (g_import_hooks_installed.load(std::memory_order_acquire)) return true;
    HMODULE executable = GetModuleHandleW(nullptr);
    void* original_factory = nullptr;
    void* original_factory1 = nullptr;
    const bool factory = PatchImport(executable, "dxgi.dll", "CreateDXGIFactory",
                                     reinterpret_cast<void*>(&HookCreateDXGIFactory), &original_factory);
    const bool factory1 = PatchImport(executable, "dxgi.dll", "CreateDXGIFactory1",
                                      reinterpret_cast<void*>(&HookCreateDXGIFactory1), &original_factory1);
    if (factory) g_original_create_factory = reinterpret_cast<CreateDxgiFactoryFn>(original_factory);
    if (factory1) g_original_create_factory1 = reinterpret_cast<CreateDxgiFactoryFn>(original_factory1);
    const bool installed = factory || factory1;
    g_import_hooks_installed.store(installed, std::memory_order_release);
    return installed;
}

bool TryGetLatestOpenXrHeadPose(BridgeHeadPose& pose) {
    AcquireSRWLockShared(&g_pose_lock);
    pose = g_latest_pose;
    ReleaseSRWLockShared(&g_pose_lock);
    return pose.orientation_valid || pose.position_valid;
}

bool TryGetLatestOpenXrEyeFov(std::uint32_t eye, BridgeEyeFov& fov) {
    fov = {};
    if (eye >= g_latest_eye_fovs.size()) return false;
    AcquireSRWLockShared(&g_pose_lock);
    const bool available = g_eye_fovs_available;
    if (available) fov = g_latest_eye_fovs[eye];
    ReleaseSRWLockShared(&g_pose_lock);
    return available;
}

}  // namespace ac7vr
