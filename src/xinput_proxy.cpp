#include <windows.h>
#include <xinput.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace {
using XInputGetStateFn = decltype(&XInputGetState);
using XInputSetStateFn = decltype(&XInputSetState);

INIT_ONCE g_xinput_once = INIT_ONCE_STATIC_INIT;
HMODULE g_real_xinput = nullptr;
XInputGetStateFn g_real_get_state = nullptr;
XInputSetStateFn g_real_set_state = nullptr;

// Persistent feasibility-probe objects. They intentionally implement only the
// small ABI surface already recovered from runtime disassembly. Every other
// vtable slot is backed by a numbered neutral stub so a single manual run can
// reveal which additional calls the retained VR path requires.
struct FakeInterface {
    void** vtable;
};

enum class FakeInterfaceKind : std::uint32_t {
    Hmd = 1,
    Device = 2,
    Stereo = 3,
};

constexpr std::size_t kFakeVtableSlots = 128;
constexpr wchar_t kImmediateSlotLogPath[] = L"E:\\trigger_ac7vr\\evidence\\persistent_slots.log";

std::array<std::atomic<std::uint32_t>, kFakeVtableSlots> g_hmd_slot_hits{};
std::array<std::atomic<std::uint32_t>, kFakeVtableSlots> g_device_slot_hits{};
std::array<std::atomic<std::uint32_t>, kFakeVtableSlots> g_stereo_slot_hits{};
std::atomic<std::uint32_t> g_last_interface{0};
std::atomic<std::uint32_t> g_last_slot{0};

const char* InterfaceName(FakeInterfaceKind kind) {
    switch (kind) {
        case FakeInterfaceKind::Hmd: return "hmd";
        case FakeInterfaceKind::Device: return "device";
        case FakeInterfaceKind::Stereo: return "stereo";
    }
    return "unknown";
}

std::array<std::atomic<std::uint32_t>, kFakeVtableSlots>& SlotHits(FakeInterfaceKind kind) {
    switch (kind) {
        case FakeInterfaceKind::Hmd: return g_hmd_slot_hits;
        case FakeInterfaceKind::Device: return g_device_slot_hits;
        case FakeInterfaceKind::Stereo: return g_stereo_slot_hits;
    }
    return g_hmd_slot_hits;
}

void AppendImmediateDiagnostic(const char* text, DWORD length) {
    HANDLE file = CreateFileW(kImmediateSlotLogPath, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text, length, &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

bool RecordSlot(FakeInterfaceKind kind, std::size_t slot, bool unknown) {
    if (slot >= kFakeVtableSlots) {
        return false;
    }

    auto& counter = SlotHits(kind)[slot];
    const auto previous = counter.fetch_add(1, std::memory_order_relaxed);
    g_last_interface.store(static_cast<std::uint32_t>(kind), std::memory_order_relaxed);
    g_last_slot.store(static_cast<std::uint32_t>(slot), std::memory_order_relaxed);

    // The first hit to an unknown slot is persisted synchronously. If the
    // neutral return value immediately leads to an access violation, this line
    // still survives even when the monitoring thread never gets another tick.
    if (unknown && previous == 0) {
        char line[160]{};
        const int length = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                       "unknown_slot interface=%s slot=%zu offset=0x%zX\r\n",
                                       InterfaceName(kind), slot, slot * sizeof(void*));
        if (length > 0) {
            AppendImmediateDiagnostic(line, static_cast<DWORD>(length));
        }
    }
    return previous == 0;
}

template <FakeInterfaceKind Kind, std::size_t Slot>
std::uintptr_t FakeUnknownSlot(FakeInterface* self, std::uintptr_t arg1,
                               std::uintptr_t arg2, std::uintptr_t arg3) {
    const bool first = RecordSlot(Kind, Slot, true);
    if (first) {
        char line[256]{};
        const int length = _snprintf_s(
            line, sizeof(line), _TRUNCATE,
            "unknown_args interface=%s slot=%zu this=%p arg1=%p arg2=%p arg3=%p\r\n",
            InterfaceName(Kind), Slot, self, reinterpret_cast<void*>(arg1),
            reinterpret_cast<void*>(arg2), reinterpret_cast<void*>(arg3));
        if (length > 0) {
            AppendImmediateDiagnostic(line, static_cast<DWORD>(length));
        }
    }
    return 0;
}

template <FakeInterfaceKind Kind, std::size_t... Slots>
std::array<void*, sizeof...(Slots)> MakeStubVtable(std::index_sequence<Slots...>) {
    return {reinterpret_cast<void*>(&FakeUnknownSlot<Kind, Slots>)...};
}

std::array<void*, kFakeVtableSlots> g_fake_hmd_vtable =
    MakeStubVtable<FakeInterfaceKind::Hmd>(std::make_index_sequence<kFakeVtableSlots>{});
std::array<void*, kFakeVtableSlots> g_fake_device_vtable =
    MakeStubVtable<FakeInterfaceKind::Device>(std::make_index_sequence<kFakeVtableSlots>{});
std::array<void*, kFakeVtableSlots> g_fake_stereo_vtable =
    MakeStubVtable<FakeInterfaceKind::Stereo>(std::make_index_sequence<kFakeVtableSlots>{});
FakeInterface g_fake_hmd{g_fake_hmd_vtable.data()};
FakeInterface g_fake_device{g_fake_device_vtable.data()};
FakeInterface g_fake_stereo{g_fake_stereo_vtable.data()};
bool g_fake_hmd_enabled = false;
bool g_fake_stereo_enabled = false;
PVOID g_vectored_exception_handler = nullptr;

constexpr std::array<std::pair<const wchar_t*, std::uintptr_t>, 21> kProbeRvas{{
    {L"ToggleVRTestMissionMenu_command", 0x00916380},
    {L"ToggleVRTestMissionMenu_exec", 0x0091B9A0},
    {L"IsVRGameMode_exec", 0x00924560},
    {L"IsVRMode_exec", 0x00924590},
    {L"IsVRUIMode_exec", 0x009245C0},
    {L"bIsVRMode_SetBit", 0x0213D160},
    {L"bStartInVR_SetBit", 0x01456B70},
    {L"bStartFromVRHangar_SetBit", 0x01456B80},
    {L"bStartInAR_SetBit", 0x01456BC0},
    {L"GeneralProjectSettings_thunk", 0x01456970},
    {L"ConsoleSettings_thunk", 0x01456650},
    {L"GameNetworkManagerSettings_thunk", 0x01456790},
    {L"GameSessionSettings_thunk", 0x01456830},
    {L"EnableHMD_exec", 0x01192B80},
    {L"EnableHMD_native", 0x0118F1E0},
    {L"GetHMDDeviceName_exec", 0x01192DB0},
    {L"GetHMDDeviceName_native", 0x0118F690},
    {L"IsHMDConnected_exec", 0x011937B0},
    {L"IsHMDEnabled_exec", 0x011937E0},
    {L"IsHMDConnected_native", 0x01190460},
    {L"IsHMDEnabled_native", 0x011904C0},
}};

constexpr std::uintptr_t kEngineGlobalRva = 0x03CBBC28;
constexpr std::uintptr_t kHmdDeviceOffset = 0x0AD8;
constexpr std::uintptr_t kStereoRenderingDeviceOffset = 0x0AC8;
constexpr std::uintptr_t kEnableHmdNativeRva = 0x0118F1E0;
constexpr std::uintptr_t kIsHmdConnectedNativeRva = 0x01190460;
constexpr std::uintptr_t kIsHmdEnabledNativeRva = 0x011904C0;
constexpr std::uintptr_t kGeneralProjectSettingsClassGlobalRva = 0x03C93398;
constexpr std::uintptr_t kGeneralProjectSettingsRuntimeRegionRva = 0x01455000;
constexpr std::size_t kGeneralProjectSettingsRuntimeRegionSize = 0x4000;

void* FakeHmdGetDeviceName(FakeInterface*, void* out_name) {
    RecordSlot(FakeInterfaceKind::Hmd, 0, false);
    if (out_name) {
        *reinterpret_cast<std::uint64_t*>(out_name) = 0;
    }
    return out_name;
}

void* FakeHmdGetDevice(FakeInterface*) {
    RecordSlot(FakeInterfaceKind::Hmd, 0xB8 / sizeof(void*), false);
    return &g_fake_device;
}

bool FakeHmdIsEnabled(FakeInterface*) {
    RecordSlot(FakeInterfaceKind::Hmd, 0xD0 / sizeof(void*), false);
    return g_fake_hmd_enabled;
}

bool FakeDeviceIsConnected(FakeInterface*) {
    RecordSlot(FakeInterfaceKind::Device, 0x40 / sizeof(void*), false);
    return true;
}

void FakeDeviceEnable(FakeInterface*, bool enabled) {
    RecordSlot(FakeInterfaceKind::Device, 0x58 / sizeof(void*), false);
    g_fake_hmd_enabled = enabled;
}

bool FakeStereoIsEnabled(FakeInterface*) {
    RecordSlot(FakeInterfaceKind::Stereo, 0, false);
    return g_fake_stereo_enabled;
}

bool FakeStereoIsEnabledOnNextFrame(FakeInterface*) {
    RecordSlot(FakeInterfaceKind::Stereo, 1, false);
    return g_fake_stereo_enabled;
}

bool FakeStereoEnable(FakeInterface*, bool enabled) {
    RecordSlot(FakeInterfaceKind::Stereo, 0x10 / sizeof(void*), false);
    g_fake_stereo_enabled = enabled;
    g_fake_hmd_enabled = enabled;
    return true;
}

void InitializeFakeInterfaces() {
    g_fake_hmd_vtable = MakeStubVtable<FakeInterfaceKind::Hmd>(std::make_index_sequence<kFakeVtableSlots>{});
    g_fake_device_vtable = MakeStubVtable<FakeInterfaceKind::Device>(std::make_index_sequence<kFakeVtableSlots>{});
    g_fake_stereo_vtable = MakeStubVtable<FakeInterfaceKind::Stereo>(std::make_index_sequence<kFakeVtableSlots>{});

    g_fake_hmd_vtable[0] = reinterpret_cast<void*>(&FakeHmdGetDeviceName);
    g_fake_hmd_vtable[0xB8 / sizeof(void*)] = reinterpret_cast<void*>(&FakeHmdGetDevice);
    g_fake_hmd_vtable[0xD0 / sizeof(void*)] = reinterpret_cast<void*>(&FakeHmdIsEnabled);
    g_fake_device_vtable[0x40 / sizeof(void*)] = reinterpret_cast<void*>(&FakeDeviceIsConnected);
    g_fake_device_vtable[0x58 / sizeof(void*)] = reinterpret_cast<void*>(&FakeDeviceEnable);
    g_fake_stereo_vtable[0] = reinterpret_cast<void*>(&FakeStereoIsEnabled);
    g_fake_stereo_vtable[1] = reinterpret_cast<void*>(&FakeStereoIsEnabledOnNextFrame);
    g_fake_stereo_vtable[0x10 / sizeof(void*)] = reinterpret_cast<void*>(&FakeStereoEnable);
    g_fake_hmd_enabled = false;
    g_fake_stereo_enabled = false;

    for (auto& hit : g_hmd_slot_hits) hit.store(0, std::memory_order_relaxed);
    for (auto& hit : g_device_slot_hits) hit.store(0, std::memory_order_relaxed);
    for (auto& hit : g_stereo_slot_hits) hit.store(0, std::memory_order_relaxed);
    g_last_interface.store(0, std::memory_order_relaxed);
    g_last_slot.store(0, std::memory_order_relaxed);
}

LONG CALLBACK ProbeVectoredExceptionHandler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto* context = info->ContextRecord;
    char line[1024]{};
    const auto kind = static_cast<FakeInterfaceKind>(g_last_interface.load(std::memory_order_relaxed));
    const auto slot = g_last_slot.load(std::memory_order_relaxed);
    const auto access_kind = info->ExceptionRecord->NumberParameters >= 1
        ? info->ExceptionRecord->ExceptionInformation[0]
        : 0;
    const auto fault_address = info->ExceptionRecord->NumberParameters >= 2
        ? info->ExceptionRecord->ExceptionInformation[1]
        : 0;
    const int length = _snprintf_s(
        line, sizeof(line), _TRUNCATE,
        "exception code=0x%08lX address=%p last_interface=%s last_slot=%u last_offset=0x%X "
        "access=%llu fault=%p "
        "rip=%p rsp=%p rbp=%p rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p "
        "r8=%p r9=%p r10=%p r11=%p r12=%p r13=%p r14=%p r15=%p\r\n",
        static_cast<unsigned long>(code), info->ExceptionRecord->ExceptionAddress,
        InterfaceName(kind), slot, slot * static_cast<unsigned>(sizeof(void*)),
        static_cast<unsigned long long>(access_kind), reinterpret_cast<void*>(fault_address),
        reinterpret_cast<void*>(context->Rip), reinterpret_cast<void*>(context->Rsp),
        reinterpret_cast<void*>(context->Rbp), reinterpret_cast<void*>(context->Rax),
        reinterpret_cast<void*>(context->Rbx), reinterpret_cast<void*>(context->Rcx),
        reinterpret_cast<void*>(context->Rdx), reinterpret_cast<void*>(context->Rsi),
        reinterpret_cast<void*>(context->Rdi), reinterpret_cast<void*>(context->R8),
        reinterpret_cast<void*>(context->R9), reinterpret_cast<void*>(context->R10),
        reinterpret_cast<void*>(context->R11), reinterpret_cast<void*>(context->R12),
        reinterpret_cast<void*>(context->R13), reinterpret_cast<void*>(context->R14),
        reinterpret_cast<void*>(context->R15));
    if (length > 0) {
        AppendImmediateDiagnostic(line, static_cast<DWORD>(length));
    }

    std::array<std::uint8_t, 256> code_bytes{};
    SIZE_T code_read = 0;
    const auto code_address = reinterpret_cast<const void*>(
        context->Rip >= 96 ? context->Rip - 96 : context->Rip);
    if (ReadProcessMemory(GetCurrentProcess(), code_address, code_bytes.data(), code_bytes.size(), &code_read)) {
        char bytes_line[1024]{};
        int cursor = _snprintf_s(bytes_line, sizeof(bytes_line), _TRUNCATE,
                                 "exception_code_bytes start=%p read=%llu data=",
                                 code_address, static_cast<unsigned long long>(code_read));
        if (cursor > 0) {
            for (SIZE_T i = 0; i < code_read && cursor + 2 < static_cast<int>(sizeof(bytes_line)); ++i) {
                const int wrote = _snprintf_s(bytes_line + cursor, sizeof(bytes_line) - cursor,
                                              _TRUNCATE, "%02X", code_bytes[i]);
                if (wrote <= 0) {
                    break;
                }
                cursor += wrote;
            }
            if (cursor + 2 < static_cast<int>(sizeof(bytes_line))) {
                bytes_line[cursor++] = '\r';
                bytes_line[cursor++] = '\n';
                AppendImmediateDiagnostic(bytes_line, static_cast<DWORD>(cursor));
            }
        }
    }

    std::array<std::uint8_t, 256> stack_bytes{};
    SIZE_T stack_read = 0;
    const auto stack_address = reinterpret_cast<const void*>(context->Rsp);
    if (ReadProcessMemory(GetCurrentProcess(), stack_address, stack_bytes.data(), stack_bytes.size(), &stack_read)) {
        char stack_line[1024]{};
        int cursor = _snprintf_s(stack_line, sizeof(stack_line), _TRUNCATE,
                                 "exception_stack start=%p read=%llu qwords=",
                                 stack_address, static_cast<unsigned long long>(stack_read));
        if (cursor > 0) {
            for (SIZE_T i = 0; i + sizeof(std::uint64_t) <= stack_read &&
                             cursor + 18 < static_cast<int>(sizeof(stack_line));
                 i += sizeof(std::uint64_t)) {
                std::uint64_t value = 0;
                memcpy(&value, stack_bytes.data() + i, sizeof(value));
                const int wrote = _snprintf_s(stack_line + cursor, sizeof(stack_line) - cursor,
                                              _TRUNCATE, "%016llX ",
                                              static_cast<unsigned long long>(value));
                if (wrote <= 0) {
                    break;
                }
                cursor += wrote;
            }
            if (cursor + 2 < static_cast<int>(sizeof(stack_line))) {
                stack_line[cursor++] = '\r';
                stack_line[cursor++] = '\n';
                AppendImmediateDiagnostic(stack_line, static_cast<DWORD>(cursor));
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

FILE* OpenLog() {
    FILE* file = nullptr;
    _wfopen_s(&file, L"E:\\trigger_ac7vr\\probe.log", L"a, ccs=UTF-8");
    return file;
}

void LogBytes(FILE* file, const wchar_t* label, const std::uint8_t* address, std::size_t count) {
    std::array<std::uint8_t, 160> bytes{};
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

void DumpRuntimeRegion(FILE* file, const std::uint8_t* module) {
    std::array<std::uint8_t, kGeneralProjectSettingsRuntimeRegionSize> bytes{};
    SIZE_T read = 0;
    const auto address = module + kGeneralProjectSettingsRuntimeRegionRva;
    if (!ReadProcessMemory(GetCurrentProcess(), address, bytes.data(), bytes.size(), &read)) {
        fwprintf(file, L"general_project_settings_runtime_dump=<read failed:%lu>\n", GetLastError());
        return;
    }

    FILE* dump = nullptr;
    if (_wfopen_s(&dump, L"E:\\trigger_ac7vr\\evidence\\GeneralProjectSettings.runtime.bin", L"wb") != 0 || !dump) {
        fwprintf(file, L"general_project_settings_runtime_dump=<open failed>\n");
        return;
    }

    const auto written = fwrite(bytes.data(), 1, read, dump);
    fclose(dump);
    fwprintf(file,
             L"general_project_settings_runtime_dump rva=0x%08llX address=%p read=%llu written=%zu\n",
             static_cast<unsigned long long>(kGeneralProjectSettingsRuntimeRegionRva), address,
             static_cast<unsigned long long>(read), written);
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
    std::uintptr_t stereo_device = 0;
    if (engine != 0) {
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(engine + kStereoRenderingDeviceOffset),
                          &stereo_device, sizeof(stereo_device), &read);
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
             L"hmd_state engine=%p stereo_slot=%p stereo_device=%p hmd_slot=%p hmd_device=%p vtable=%p method_b8=%p method_d0=%p\n",
             reinterpret_cast<void*>(engine), reinterpret_cast<void*>(engine ? engine + kStereoRenderingDeviceOffset : 0),
             reinterpret_cast<void*>(stereo_device),
             reinterpret_cast<void*>(engine ? engine + kHmdDeviceOffset : 0),
             reinterpret_cast<void*>(hmd_device), reinterpret_cast<void*>(vtable),
             reinterpret_cast<void*>(connected_method), reinterpret_cast<void*>(enabled_method));
}

struct PersistentProbeState {
    std::uintptr_t engine = 0;
    void** stereo_slot = nullptr;
    void** hmd_slot = nullptr;
    bool installed = false;
};

void ResetImmediateSlotLog() {
    HANDLE file = CreateFileW(kImmediateSlotLogPath, GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    static constexpr char header[] = "trigger_ac7vr persistent interface slot trace\r\n";
    DWORD written = 0;
    WriteFile(file, header, static_cast<DWORD>(sizeof(header) - 1), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

bool TryInstallPersistentFakeInterfaces(FILE* file, std::uint8_t* module, PersistentProbeState& state) {
    std::uintptr_t engine = 0;
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), module + kEngineGlobalRva, &engine, sizeof(engine), &read) ||
        read != sizeof(engine) || engine == 0) {
        return false;
    }

    auto* stereo_slot = reinterpret_cast<void**>(engine + kStereoRenderingDeviceOffset);
    auto* hmd_slot = reinterpret_cast<void**>(engine + kHmdDeviceOffset);
    if (*stereo_slot != nullptr || *hmd_slot != nullptr) {
        return false;
    }

    InitializeFakeInterfaces();

    // Only claim null slots. If another backend appears between the reads and
    // the exchanges, leave it alone and undo our first insertion.
    void* prior_stereo = InterlockedCompareExchangePointer(stereo_slot, &g_fake_stereo, nullptr);
    if (prior_stereo != nullptr) {
        return false;
    }
    void* prior_hmd = InterlockedCompareExchangePointer(hmd_slot, &g_fake_hmd, nullptr);
    if (prior_hmd != nullptr) {
        InterlockedCompareExchangePointer(stereo_slot, nullptr, &g_fake_stereo);
        return false;
    }

    state.engine = engine;
    state.stereo_slot = stereo_slot;
    state.hmd_slot = hmd_slot;
    state.installed = true;

    if (!g_vectored_exception_handler) {
        g_vectored_exception_handler = AddVectoredExceptionHandler(1, ProbeVectoredExceptionHandler);
    }

    fwprintf(file,
             L"persistent_fake_hmd installed engine=%p stereo_slot=%p hmd_slot=%p "
             L"stereo=%p hmd=%p veh=%p\n",
             reinterpret_cast<void*>(engine), stereo_slot, hmd_slot,
             &g_fake_stereo, &g_fake_hmd, g_vectored_exception_handler);
    fflush(file);
    return true;
}

template <std::size_t N>
bool LogSlotChanges(FILE* file, const wchar_t* interface_name,
                    const std::array<std::atomic<std::uint32_t>, N>& hits,
                    std::array<std::uint32_t, N>& previous) {
    bool changed = false;
    for (std::size_t slot = 0; slot < N; ++slot) {
        const auto current = hits[slot].load(std::memory_order_relaxed);
        if (current == previous[slot]) {
            continue;
        }
        fwprintf(file, L"slot_hit interface=%ls slot=%zu offset=0x%zX count=%u delta=%u\n",
                 interface_name, slot, slot * sizeof(void*), current, current - previous[slot]);
        previous[slot] = current;
        changed = true;
    }
    return changed;
}

void LogGeneralProjectSettings(FILE* file, const std::uint8_t* module) {
    SIZE_T read = 0;
    std::uintptr_t klass = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), module + kGeneralProjectSettingsClassGlobalRva,
                           &klass, sizeof(klass), &read) || read != sizeof(klass)) {
        fwprintf(file, L"general_project_settings class=<read failed:%lu>\n", GetLastError());
        return;
    }

    fwprintf(file, L"general_project_settings class_global=%p class=%p\n",
             module + kGeneralProjectSettingsClassGlobalRva, reinterpret_cast<void*>(klass));
    if (!klass) {
        return;
    }

    std::array<std::uint8_t, 0x220> class_bytes{};
    SIZE_T class_read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(klass),
                           class_bytes.data(), class_bytes.size(), &class_read)) {
        fwprintf(file, L"general_project_settings class_dump=<read failed:%lu>\n", GetLastError());
        return;
    }

    const auto module_begin = reinterpret_cast<std::uintptr_t>(module);
    const auto module_end = module_begin + 0x0441A000;
    for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= class_read; offset += sizeof(std::uintptr_t)) {
        std::uintptr_t candidate = 0;
        memcpy(&candidate, class_bytes.data() + offset, sizeof(candidate));
        if (!candidate || (candidate >= module_begin && candidate < module_end)) {
            continue;
        }

        std::uintptr_t first_qword = 0;
        SIZE_T candidate_read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(candidate),
                               &first_qword, sizeof(first_qword), &candidate_read) ||
            candidate_read != sizeof(first_qword)) {
            continue;
        }
        if (first_qword < module_begin || first_qword >= module_end) {
            continue;
        }

        // UE4 UObjectBase stores ClassPrivate at +0x10 on this 64-bit build.
        // The GeneralProjectSettings CDO must point back to the UClass object
        // we are scanning, which lets us reject unrelated UObject pointers.
        std::uintptr_t candidate_class = 0;
        SIZE_T candidate_class_read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(candidate + 0x10),
                               &candidate_class, sizeof(candidate_class), &candidate_class_read) ||
            candidate_class_read != sizeof(candidate_class) || candidate_class != klass) {
            continue;
        }

        std::uint8_t start_in_vr = 0xFF;
        std::uint8_t start_from_vr_hangar = 0xFF;
        std::uint8_t start_in_ar = 0xFF;
        SIZE_T byte_read = 0;
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(candidate + 0x10B),
                          &start_in_vr, sizeof(start_in_vr), &byte_read);
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(candidate + 0x10C),
                          &start_from_vr_hangar, sizeof(start_from_vr_hangar), &byte_read);
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(candidate + 0x10D),
                          &start_in_ar, sizeof(start_in_ar), &byte_read);

        fwprintf(file,
                 L"general_project_settings candidate class_off=0x%zX object=%p vtable=%p bStartInVR=%u bStartFromVRHangar=%u bStartInAR=%u\n",
                 offset, reinterpret_cast<void*>(candidate), reinterpret_cast<void*>(first_qword),
                 static_cast<unsigned>(start_in_vr), static_cast<unsigned>(start_from_vr_hangar),
                 static_cast<unsigned>(start_in_ar));
        LogBytes(file, L"general_project_settings object+0x100", reinterpret_cast<const std::uint8_t*>(candidate + 0x100), 0x18);
    }
}

DWORD WINAPI ProbeThread(void*) {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const std::wstring full_exe_path(exe_path);
    const auto separator = full_exe_path.find_last_of(L"\\/");
    const std::wstring exe_name = separator == std::wstring::npos
        ? full_exe_path
        : full_exe_path.substr(separator + 1);
    if (_wcsicmp(exe_name.c_str(), L"Ace7Game.exe") != 0) {
        return 0;
    }

    FILE* file = OpenLog();
    if (!file) {
        return 0;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    fwprintf(file, L"\n=== trigger_ac7vr probe %04u-%02u-%02u %02u:%02u:%02u ===\n",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    const auto module = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    fwprintf(file, L"exe=%ls\nbase=%p\n", exe_path, module);
    fflush(file);

    if (module) {
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
        fwprintf(file, L"timestamp=0x%08X size_of_image=0x%08X entry_rva=0x%08X\n",
                 nt->FileHeader.TimeDateStamp,
                 nt->OptionalHeader.SizeOfImage,
                 nt->OptionalHeader.AddressOfEntryPoint);
        fflush(file);

        ResetImmediateSlotLog();

        PersistentProbeState state{};
        std::array<std::uint32_t, kFakeVtableSlots> previous_hmd_hits{};
        std::array<std::uint32_t, kFakeVtableSlots> previous_device_hits{};
        std::array<std::uint32_t, kFakeVtableSlots> previous_stereo_hits{};
        const ULONGLONG start = GetTickCount64();
        bool detailed_dump_done = false;
        bool ownership_lost = false;

        for (;;) {
            if (!state.installed && !ownership_lost) {
                TryInstallPersistentFakeInterfaces(file, module, state);
            }

            bool changed = false;
            changed |= LogSlotChanges(file, L"hmd", g_hmd_slot_hits, previous_hmd_hits);
            changed |= LogSlotChanges(file, L"device", g_device_slot_hits, previous_device_hits);
            changed |= LogSlotChanges(file, L"stereo", g_stereo_slot_hits, previous_stereo_hits);

            if (state.installed &&
                (*state.hmd_slot != &g_fake_hmd || *state.stereo_slot != &g_fake_stereo)) {
                fwprintf(file,
                         L"persistent_fake_hmd ownership_lost hmd_now=%p stereo_now=%p\n",
                         *state.hmd_slot, *state.stereo_slot);
                state.installed = false;
                ownership_lost = true;
                changed = true;
            }

            if (!detailed_dump_done && GetTickCount64() - start >= 10000) {
                for (const auto& [label, rva] : kProbeRvas) {
                    LogBytes(file, label, module + rva, 160);
                }
                DumpRuntimeRegion(file, module);
                LogGeneralProjectSettings(file, module);
                LogHmdState(file, module);
                fwprintf(file, L"persistent_fake_hmd snapshot installed=%u hmd_enabled=%u stereo_enabled=%u\n",
                         static_cast<unsigned>(state.installed),
                         static_cast<unsigned>(g_fake_hmd_enabled),
                         static_cast<unsigned>(g_fake_stereo_enabled));
                detailed_dump_done = true;
                changed = true;
            }

            if (changed) {
                fflush(file);
            }
            Sleep(25);
        }
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
