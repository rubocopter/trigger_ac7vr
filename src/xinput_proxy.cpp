#include <windows.h>
#include <xinput.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {
using XInputGetStateFn = decltype(&XInputGetState);
using XInputSetStateFn = decltype(&XInputSetState);

INIT_ONCE g_xinput_once = INIT_ONCE_STATIC_INIT;
HMODULE g_real_xinput = nullptr;
XInputGetStateFn g_real_get_state = nullptr;
XInputSetStateFn g_real_set_state = nullptr;

constexpr std::array<std::pair<const wchar_t*, std::uintptr_t>, 11> kProbeRvas{{
    {L"ToggleVRTestMissionMenu_command", 0x00916380},
    {L"ToggleVRTestMissionMenu_exec", 0x0091B9A0},
    {L"IsVRGameMode_exec", 0x00924560},
    {L"IsVRMode_exec", 0x00924590},
    {L"IsVRUIMode_exec", 0x009245C0},
    {L"EnableHMD_exec", 0x01192B80},
    {L"GetHMDDeviceName_exec", 0x01192DB0},
    {L"IsHMDConnected_exec", 0x011937B0},
    {L"IsHMDEnabled_exec", 0x011937E0},
    {L"IsHMDConnected_native", 0x01190460},
    {L"IsHMDEnabled_native", 0x011904C0},
}};

constexpr std::uintptr_t kEngineGlobalRva = 0x03CBBC28;
constexpr std::uintptr_t kHmdDeviceOffset = 0x0AD8;

FILE* OpenLog() {
    FILE* file = nullptr;
    _wfopen_s(&file, L"E:\\trigger_ac7vr\\probe.log", L"a, ccs=UTF-8");
    return file;
}

void LogBytes(FILE* file, const wchar_t* label, const std::uint8_t* address, std::size_t count) {
    std::array<std::uint8_t, 64> bytes{};
    const auto to_read = (count < bytes.size()) ? count : bytes.size();
    SIZE_T read = 0;
    const BOOL ok = ReadProcessMemory(GetCurrentProcess(), address, bytes.data(), to_read, &read);

    fwprintf(file, L"%ls @ %p read=%lu/%zu: ", label, address, ok ? static_cast<unsigned long>(read) : 0UL, to_read);
    if (ok) {
        for (SIZE_T i = 0; i < read; ++i) {
            fwprintf(file, L"%02X", bytes[i]);
        }
    } else {
        fwprintf(file, L"<ReadProcessMemory failed: %lu>", GetLastError());
    }
    fwprintf(file, L"\n");
}

void LogHmdState(FILE* file, const std::uint8_t* module) {
    std::uintptr_t engine = 0;
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), module + kEngineGlobalRva, &engine, sizeof(engine), &read) ||
        read != sizeof(engine)) {
        fwprintf(file, L"hmd_state engine=<read failed:%lu>\n", GetLastError());
        return;
    }

    std::uintptr_t hmd_device = 0;
    if (engine != 0) {
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(engine + kHmdDeviceOffset),
                          &hmd_device, sizeof(hmd_device), &read);
    }

    std::uintptr_t vtable = 0;
    if (hmd_device != 0) {
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(hmd_device),
                          &vtable, sizeof(vtable), &read);
    }

    std::uintptr_t connected_method = 0;
    std::uintptr_t enabled_method = 0;
    if (vtable != 0) {
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(vtable + 0xB8),
                          &connected_method, sizeof(connected_method), &read);
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(vtable + 0xD0),
                          &enabled_method, sizeof(enabled_method), &read);
    }

    fwprintf(file,
             L"hmd_state engine=%p hmd_slot=%p hmd_device=%p vtable=%p method_b8=%p method_d0=%p\n",
             reinterpret_cast<void*>(engine), reinterpret_cast<void*>(engine ? engine + kHmdDeviceOffset : 0),
             reinterpret_cast<void*>(hmd_device), reinterpret_cast<void*>(vtable),
             reinterpret_cast<void*>(connected_method), reinterpret_cast<void*>(enabled_method));
}

DWORD WINAPI ProbeThread(void*) {
    Sleep(10000);

    FILE* file = OpenLog();
    if (!file) {
        return 0;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    fwprintf(file, L"\n=== trigger_ac7vr probe %04u-%02u-%02u %02u:%02u:%02u ===\n",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    const auto module = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    fwprintf(file, L"exe=%ls\nbase=%p\n", exe_path, module);

    if (module) {
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
        fwprintf(file, L"timestamp=0x%08X size_of_image=0x%08X entry_rva=0x%08X\n",
                 nt->FileHeader.TimeDateStamp,
                 nt->OptionalHeader.SizeOfImage,
                 nt->OptionalHeader.AddressOfEntryPoint);

        for (const auto& [label, rva] : kProbeRvas) {
            LogBytes(file, label, module + rva, 64);
        }
        LogHmdState(file, module);
    }

    fflush(file);
    fclose(file);
    return 0;
}

BOOL CALLBACK InitRealXInput(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t system_dir[MAX_PATH]{};
    const UINT system_dir_len = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (system_dir_len == 0 || system_dir_len >= MAX_PATH) {
        return FALSE;
    }

    std::wstring real_xinput_path(system_dir, system_dir_len);
    real_xinput_path += L"\\xinput1_3.dll";

    g_real_xinput = LoadLibraryW(real_xinput_path.c_str());
    if (!g_real_xinput) {
        return FALSE;
    }

    g_real_get_state = reinterpret_cast<XInputGetStateFn>(GetProcAddress(g_real_xinput, MAKEINTRESOURCEA(2)));
    g_real_set_state = reinterpret_cast<XInputSetStateFn>(GetProcAddress(g_real_xinput, MAKEINTRESOURCEA(3)));
    return g_real_get_state && g_real_set_state;
}

bool EnsureRealXInput() {
    PVOID context = nullptr;
    return InitOnceExecuteOnce(&g_xinput_once, InitRealXInput, nullptr, &context) != FALSE;
}
}  // namespace

extern "C" DWORD WINAPI XInputGetState(DWORD user_index, XINPUT_STATE* state) WIN_NOEXCEPT {
    if (!EnsureRealXInput()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return g_real_get_state(user_index, state);
}

extern "C" DWORD WINAPI XInputSetState(DWORD user_index, XINPUT_VIBRATION* vibration) WIN_NOEXCEPT {
    if (!EnsureRealXInput()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return g_real_set_state(user_index, vibration);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
