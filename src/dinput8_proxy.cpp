#include <windows.h>
#include <dinput.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using ExportMouseStateFn = void(__thiscall*)(void*, float*);
using UpdatePlayerInputFn = void(__thiscall*)(void*, int);
using ApplyLookCurveFn = float*(__thiscall*)(void*, float*, float, float, float, float);

constexpr uintptr_t kPreferredImageBase = 0x00400000;
constexpr uintptr_t kExportMouseStateAddress = 0x008FF9A0;
constexpr uintptr_t kExportMouseStateRva = kExportMouseStateAddress - kPreferredImageBase;
constexpr uintptr_t kUpdatePlayerInputAddress = 0x005C67F0;
constexpr uintptr_t kUpdatePlayerInputRva = kUpdatePlayerInputAddress - kPreferredImageBase;
constexpr uintptr_t kApplyLookCurveAddress = 0x0063CE50;
constexpr uintptr_t kApplyLookCurveRva = kApplyLookCurveAddress - kPreferredImageBase;
constexpr SIZE_T kHookLength = 6;
constexpr unsigned kMaximumSamples = 800;
constexpr unsigned kMaximumWatchHits = 1200;
constexpr unsigned kMaximumCurveSamples = 1000;
constexpr bool kEnableFieldWatch = false;
constexpr bool kLinearizeMouseLook = true;
constexpr bool kEnableDiagnostics = false;
constexpr uintptr_t kPlayerLookXOffset = 0x170;
constexpr uintptr_t kPlayerLookYOffset = 0x174;

HMODULE g_realDinput8 = nullptr;
HMODULE g_proxyModule = nullptr;
DirectInput8CreateFn g_realDirectInput8Create = nullptr;
ExportMouseStateFn g_originalExportMouseState = nullptr;
UpdatePlayerInputFn g_originalUpdatePlayerInput = nullptr;
ApplyLookCurveFn g_originalApplyLookCurve = nullptr;
HANDLE g_log = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_logLock;
INIT_ONCE g_probeInit = INIT_ONCE_STATIC_INIT;
std::atomic<unsigned> g_samples{0};
std::atomic<unsigned> g_watchHits{0};
std::atomic<unsigned> g_curveSamples{0};
std::atomic<DWORD> g_watchThreadId{0};
std::atomic<uintptr_t> g_watchPlayer{0};
std::atomic<unsigned long> g_latestMouseXBits{0};
std::atomic<unsigned long> g_latestMouseYBits{0};
std::atomic<unsigned long> g_mouseSpecificXBits{0};
std::atomic<unsigned long> g_mouseSpecificYBits{0};
uintptr_t g_exeBase = 0;
SIZE_T g_exeSize = 0;
bool g_fixEnabled = true;
float g_horizontalMultiplier = 1.7f;
float g_verticalMultiplier = 4.6f;
wchar_t g_configPath[MAX_PATH]{};
HANDLE g_watchRequest = nullptr;
PVOID g_exceptionHandler = nullptr;

void Log(const char* format, ...) {
    if (g_log == INVALID_HANDLE_VALUE) {
        return;
    }

    char buffer[2048];
    va_list args;
    va_start(args, format);
    const int length = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);
    if (length <= 0) {
        return;
    }

    EnterCriticalSection(&g_logLock);
    DWORD written = 0;
    WriteFile(g_log, buffer, static_cast<DWORD>(length), &written, nullptr);
    LeaveCriticalSection(&g_logLock);
}

bool ResolveRealDinput8() {
    if (g_realDirectInput8Create != nullptr) {
        return true;
    }

    wchar_t systemDirectory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 12) {
        return false;
    }
    wcscat_s(systemDirectory, L"\\dinput8.dll");

    g_realDinput8 = LoadLibraryW(systemDirectory);
    if (g_realDinput8 == nullptr) {
        return false;
    }

    g_realDirectInput8Create = reinterpret_cast<DirectInput8CreateFn>(
        GetProcAddress(g_realDinput8, "DirectInput8Create"));
    return g_realDirectInput8Create != nullptr;
}

void OpenLog() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, path);
    if (length == 0 || length >= MAX_PATH - 36) {
        return;
    }
    wcscat_s(path, L"007_legends_mouse_probe.log");

    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

float ReadFloatSetting(const wchar_t* name, float fallback) {
    wchar_t text[64]{};
    if (GetPrivateProfileStringW(L"Mouse", name, L"", text,
                                 static_cast<DWORD>(sizeof(text) / sizeof(text[0])),
                                 g_configPath) == 0) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const float value = std::wcstof(text, &end);
    if (end == text || *end != L'\0' || !std::isfinite(value) || value < 0.05f || value > 20.0f) {
        return fallback;
    }
    return value;
}

void LoadConfiguration() {
    const DWORD length = GetModuleFileNameW(g_proxyModule, g_configPath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        g_configPath[0] = L'\0';
        return;
    }
    wchar_t* separator = std::wcsrchr(g_configPath, L'\\');
    if (separator == nullptr ||
        static_cast<size_t>(separator - g_configPath) + 18 >= MAX_PATH) {
        g_configPath[0] = L'\0';
        return;
    }
    const size_t remaining = MAX_PATH - static_cast<size_t>(separator + 1 - g_configPath);
    wcscpy_s(separator + 1, remaining, L"007MouseFix.ini");
    g_fixEnabled = GetPrivateProfileIntW(L"Mouse", L"Enabled", 1, g_configPath) != 0;
    g_horizontalMultiplier = ReadFloatSetting(L"HorizontalMultiplier", 1.7f);
    g_verticalMultiplier = ReadFloatSetting(L"VerticalMultiplier", 4.6f);
}

bool WriteRelativeJump(BYTE* source, const void* destination) {
    const intptr_t displacement = reinterpret_cast<const BYTE*>(destination) - (source + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
        return false;
    }
    source[0] = 0xE9;
    *reinterpret_cast<int32_t*>(source + 1) = static_cast<int32_t>(displacement);
    return true;
}

void LogMouseSample(void* self, float* output, void* immediateCaller) {
    if (output == nullptr || g_samples.load(std::memory_order_relaxed) >= kMaximumSamples) {
        return;
    }

    const float lookX = output[4];
    const float lookY = output[5];
    if (lookX == 0.0f && lookY == 0.0f) {
        return;
    }

    const unsigned sample = g_samples.fetch_add(1, std::memory_order_relaxed);
    if (sample >= kMaximumSamples) {
        return;
    }

    void* frames[16]{};
    const USHORT frameCount = CaptureStackBackTrace(0, 16, frames, nullptr);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(immediateCaller);

    char line[2048]{};
    int used = _snprintf_s(
        line, sizeof(line), _TRUNCATE,
        "sample=%u tid=%lu self=%08lX caller=%08lX caller_rva=%08lX "
        "move=(%.7g,%.7g) keylook=(%.7g,%.7g) mouse=(%.7g,%.7g) pad=(%.7g,%.7g) stack=",
        sample, GetCurrentThreadId(),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(self)),
        static_cast<unsigned long>(caller),
        static_cast<unsigned long>(caller >= g_exeBase ? caller - g_exeBase : caller),
        output[0], output[1], output[2], output[3], output[4], output[5], output[6], output[7]);
    if (used < 0) {
        return;
    }

    for (USHORT index = 0; index < frameCount; ++index) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(frames[index]);
        const SIZE_T remaining = sizeof(line) - static_cast<SIZE_T>(used);
        if (remaining <= 2) {
            break;
        }
        int appended = 0;
        if (address >= g_exeBase && address < g_exeBase + g_exeSize) {
            appended = _snprintf_s(
                line + used, remaining, _TRUNCATE, "+%08lX%s",
                static_cast<unsigned long>(address - g_exeBase),
                index + 1 == frameCount ? "" : ",");
        } else {
            appended = _snprintf_s(
                line + used, remaining, _TRUNCATE, "%08lX%s",
                static_cast<unsigned long>(address),
                index + 1 == frameCount ? "" : ",");
        }
        if (appended < 0) {
            break;
        }
        used += appended;
    }
    Log("%s\r\n", line);
}

void __fastcall HookExportMouseState(void* self, void*, float* output) {
    void* caller = _ReturnAddress();
    g_originalExportMouseState(self, output);
    if (output != nullptr) {
        unsigned long mouseXBits = 0;
        unsigned long mouseYBits = 0;
        unsigned long mouseSpecificXBits = 0;
        unsigned long mouseSpecificYBits = 0;
        static_assert(sizeof(mouseXBits) == sizeof(output[4]));
        memcpy(&mouseXBits, &output[6], sizeof(mouseXBits));
        memcpy(&mouseYBits, &output[7], sizeof(mouseYBits));
        memcpy(&mouseSpecificXBits, &output[4], sizeof(mouseSpecificXBits));
        memcpy(&mouseSpecificYBits, &output[5], sizeof(mouseSpecificYBits));
        g_latestMouseXBits.store(mouseXBits, std::memory_order_relaxed);
        g_latestMouseYBits.store(mouseYBits, std::memory_order_relaxed);
        g_mouseSpecificXBits.store(mouseSpecificXBits, std::memory_order_relaxed);
        g_mouseSpecificYBits.store(mouseSpecificYBits, std::memory_order_relaxed);
    }
    if (kEnableDiagnostics) {
        LogMouseSample(self, output, caller);
    }
}

LONG CALLBACK WatchExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    if (exceptionInfo == nullptr || exceptionInfo->ExceptionRecord == nullptr ||
        exceptionInfo->ContextRecord == nullptr ||
        exceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT* context = exceptionInfo->ContextRecord;
    const DWORD hitMask = static_cast<DWORD>(context->Dr6) & 0x3;
    if (hitMask == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    context->Dr6 = 0;
    const unsigned hit = g_watchHits.fetch_add(1, std::memory_order_relaxed);
    if (hit >= kMaximumWatchHits) {
        context->Dr7 &= ~static_cast<DWORD_PTR>(0x00FF0005);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    const uintptr_t player = g_watchPlayer.load(std::memory_order_acquire);
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    const unsigned long mouseXBits = g_latestMouseXBits.load(std::memory_order_relaxed);
    const unsigned long mouseYBits = g_latestMouseYBits.load(std::memory_order_relaxed);
    memcpy(&mouseX, &mouseXBits, sizeof(mouseX));
    memcpy(&mouseY, &mouseYBits, sizeof(mouseY));

    const uintptr_t nextInstruction = static_cast<uintptr_t>(context->Eip);
    uintptr_t returnAddress = 0;
    uintptr_t callerReturnAddress = 0;
    __try {
        const uintptr_t framePointer = static_cast<uintptr_t>(context->Ebp);
        returnAddress = *reinterpret_cast<const uintptr_t*>(framePointer + sizeof(uintptr_t));
        const uintptr_t callerFramePointer =
            *reinterpret_cast<const uintptr_t*>(framePointer);
        if (callerFramePointer != 0) {
            callerReturnAddress =
                *reinterpret_cast<const uintptr_t*>(callerFramePointer + sizeof(uintptr_t));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        returnAddress = 0;
        callerReturnAddress = 0;
    }
    const bool nextIsExe = nextInstruction >= g_exeBase &&
                           nextInstruction < g_exeBase + g_exeSize;
    Log("watch=%u tid=%lu dr6=%lX next_eip=%08lX next_rva=%08lX "
        "ret=%08lX caller_ret=%08lX player=%08lX mouse=(%.7g,%.7g) regs="
        "eax:%08lX ebx:%08lX ecx:%08lX edx:%08lX esi:%08lX edi:%08lX ebp:%08lX esp:%08lX\r\n",
        hit, GetCurrentThreadId(), hitMask,
        static_cast<unsigned long>(nextInstruction),
        static_cast<unsigned long>(nextIsExe
                                       ? nextInstruction - g_exeBase
                                       : 0xFFFFFFFFUL),
        static_cast<unsigned long>(returnAddress),
        static_cast<unsigned long>(callerReturnAddress),
        static_cast<unsigned long>(player), mouseX, mouseY,
        static_cast<unsigned long>(context->Eax), static_cast<unsigned long>(context->Ebx),
        static_cast<unsigned long>(context->Ecx), static_cast<unsigned long>(context->Edx),
        static_cast<unsigned long>(context->Esi), static_cast<unsigned long>(context->Edi),
        static_cast<unsigned long>(context->Ebp), static_cast<unsigned long>(context->Esp));
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI WatchInstallerThread(void*) {
    if (WaitForSingleObject(g_watchRequest, INFINITE) != WAIT_OBJECT_0) {
        return 0;
    }

    const uintptr_t player = g_watchPlayer.load(std::memory_order_acquire);
    const DWORD processId = GetCurrentProcessId();
    const DWORD installerThreadId = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Log("watch_error=thread_snapshot winerr=%lu\r\n", GetLastError());
        return 0;
    }

    unsigned installedCount = 0;
    unsigned failedCount = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != processId || entry.th32ThreadID == installerThreadId) {
                continue;
            }

            HANDLE thread = OpenThread(
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                FALSE, entry.th32ThreadID);
            if (thread == nullptr || SuspendThread(thread) == static_cast<DWORD>(-1)) {
                if (thread != nullptr) {
                    CloseHandle(thread);
                }
                ++failedCount;
                continue;
            }

            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            bool installed = false;
            if (GetThreadContext(thread, &context)) {
                context.Dr0 = player + kPlayerLookXOffset;
                context.Dr1 = player + kPlayerLookYOffset;
                context.Dr6 = 0;
                context.Dr7 &= ~static_cast<DWORD_PTR>(0x00FF0005);
                context.Dr7 |= static_cast<DWORD_PTR>(0x00FF0005);
                installed = SetThreadContext(thread, &context) != FALSE;
            }
            ResumeThread(thread);
            CloseHandle(thread);
            if (installed) {
                ++installedCount;
            } else {
                ++failedCount;
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);

    Log("watch_installed threads=%u failed=%u player=%08lX x=%08lX y=%08lX\r\n",
        installedCount, failedCount, static_cast<unsigned long>(player),
        static_cast<unsigned long>(player + kPlayerLookXOffset),
        static_cast<unsigned long>(player + kPlayerLookYOffset));
    return 0;
}

void RequestPlayerWatch(void* self) {
    if (self == nullptr || g_watchThreadId.load(std::memory_order_acquire) != 0) {
        return;
    }

    const uintptr_t player = reinterpret_cast<uintptr_t>(self);
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    __try {
        mouseX = *reinterpret_cast<const float*>(player + kPlayerLookXOffset);
        mouseY = *reinterpret_cast<const float*>(player + kPlayerLookYOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (mouseX == 0.0f && mouseY == 0.0f) {
        return;
    }

    DWORD expected = 0;
    g_watchPlayer.store(player, std::memory_order_release);
    if (g_watchThreadId.compare_exchange_strong(
            expected, GetCurrentThreadId(), std::memory_order_acq_rel)) {
        Log("watch_requested tid=%lu player=%08lX mouse=(%.7g,%.7g)\r\n",
            GetCurrentThreadId(), static_cast<unsigned long>(player), mouseX, mouseY);
        SetEvent(g_watchRequest);
    }
}

void __fastcall HookUpdatePlayerInput(void* self, void*, int source) {
    g_originalUpdatePlayerInput(self, source);
    RequestPlayerWatch(self);
}

float* __fastcall HookApplyLookCurve(void* self, void*, float* output,
                                     float inputX, float inputY,
                                     float scaleX, float scaleY) {
    float* result = g_originalApplyLookCurve(
        self, output, inputX, inputY, scaleX, scaleY);
    if (output != nullptr && (inputX != 0.0f || inputY != 0.0f)) {
        float mouseSpecificX = 0.0f;
        float mouseSpecificY = 0.0f;
        const unsigned long mouseSpecificXBits =
            g_mouseSpecificXBits.load(std::memory_order_relaxed);
        const unsigned long mouseSpecificYBits =
            g_mouseSpecificYBits.load(std::memory_order_relaxed);
        memcpy(&mouseSpecificX, &mouseSpecificXBits, sizeof(mouseSpecificX));
        memcpy(&mouseSpecificY, &mouseSpecificYBits, sizeof(mouseSpecificY));
        const bool isMouseLook = inputX == mouseSpecificX && inputY == mouseSpecificY &&
                                 (mouseSpecificX != 0.0f || mouseSpecificY != 0.0f);
        const float curvedX = output[0];
        const float curvedY = output[1];
        if (kLinearizeMouseLook && g_fixEnabled && isMouseLook) {
            output[0] = inputX * scaleX * g_horizontalMultiplier;
            output[1] = inputY * scaleY * g_verticalMultiplier;
        }
        if (!kEnableDiagnostics) {
            return result;
        }
        const unsigned sample = g_curveSamples.fetch_add(1, std::memory_order_relaxed);
        if (sample >= kMaximumCurveSamples) {
            return result;
        }
        unsigned char mode185 = 0;
        unsigned char mode1136 = 0;
        float deltaTime = 0.0f;
        __try {
            const uintptr_t player = reinterpret_cast<uintptr_t>(self);
            mode185 = *reinterpret_cast<const unsigned char*>(player + 0x185);
            mode1136 = *reinterpret_cast<const unsigned char*>(player + 0x1136);
            deltaTime = *reinterpret_cast<const float*>(player + 0x1C8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        Log("curve=%u tid=%lu player=%08lX in=(%.9g,%.9g) scale=(%.9g,%.9g) "
            "curved=(%.9g,%.9g) out=(%.9g,%.9g) mouse=%u "
            "mode185=%u mode1136=%u dt=%.9g\r\n",
            sample, GetCurrentThreadId(),
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(self)),
            inputX, inputY, scaleX, scaleY, curvedX, curvedY, output[0], output[1],
            isMouseLook ? 1U : 0U,
            static_cast<unsigned>(mode185), static_cast<unsigned>(mode1136), deltaTime);
    }
    return result;
}

bool InstallInlineHook(uintptr_t targetRva, const BYTE expected[kHookLength],
                       const void* hook, void** original, const char* name) {
    BYTE* target = reinterpret_cast<BYTE*>(g_exeBase + targetRva);
    if (memcmp(target, expected, kHookLength) != 0) {
        Log("hook_error=%s_signature_mismatch target=%08lX bytes=", name,
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(target)));
        for (SIZE_T index = 0; index < kHookLength; ++index) {
            Log("%02X", target[index]);
        }
        Log("\r\n");
        return false;
    }

    BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(
        nullptr, kHookLength + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) {
        Log("hook_error=%s_trampoline_allocation_failed winerr=%lu\r\n", name, GetLastError());
        return false;
    }
    memcpy(trampoline, target, kHookLength);
    if (!WriteRelativeJump(trampoline + kHookLength, target + kHookLength)) {
        Log("hook_error=%s_trampoline_jump_out_of_range\r\n", name);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    *original = trampoline;

    DWORD oldProtection = 0;
    if (!VirtualProtect(target, kHookLength, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        Log("hook_error=%s_virtual_protect_failed winerr=%lu\r\n", name, GetLastError());
        VirtualFree(trampoline, 0, MEM_RELEASE);
        *original = nullptr;
        return false;
    }
    if (!WriteRelativeJump(target, hook)) {
        DWORD ignored = 0;
        VirtualProtect(target, kHookLength, oldProtection, &ignored);
        Log("hook_error=%s_hook_jump_out_of_range\r\n", name);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        *original = nullptr;
        return false;
    }
    target[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, kHookLength);
    DWORD ignored = 0;
    VirtualProtect(target, kHookLength, oldProtection, &ignored);
    Log("hook_installed name=%s target=%08lX rva=%08lX trampoline=%08lX\r\n", name,
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(target)),
        static_cast<unsigned long>(targetRva),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(trampoline)));
    return true;
}

bool InstallMouseStateHook() {
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBase);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        Log("hook_error=invalid_dos_header\r\n");
        return false;
    }
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        g_exeBase + static_cast<uintptr_t>(dosHeader->e_lfanew));
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        Log("hook_error=invalid_nt_headers\r\n");
        return false;
    }
    g_exeSize = ntHeaders->OptionalHeader.SizeOfImage;
    if (ntHeaders->OptionalHeader.SizeOfImage < kExportMouseStateRva + kHookLength ||
        ntHeaders->OptionalHeader.SizeOfImage < kUpdatePlayerInputRva + kHookLength ||
        ntHeaders->OptionalHeader.SizeOfImage < kApplyLookCurveRva + kHookLength) {
        Log("hook_skipped=image_too_small size=%08lX required=%08lX\r\n",
            static_cast<unsigned long>(ntHeaders->OptionalHeader.SizeOfImage),
            static_cast<unsigned long>(kExportMouseStateRva + kHookLength));
        return false;
    }

    const BYTE exportExpected[kHookLength] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x58};
    const BYTE playerExpected[kHookLength] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08};
    void* exportOriginal = nullptr;
    if (!InstallInlineHook(kExportMouseStateRva, exportExpected,
                           reinterpret_cast<const void*>(&HookExportMouseState),
                           &exportOriginal, "export_mouse")) {
        return false;
    }
    g_originalExportMouseState = reinterpret_cast<ExportMouseStateFn>(exportOriginal);

    if (kEnableFieldWatch) {
        void* playerOriginal = nullptr;
        if (!InstallInlineHook(kUpdatePlayerInputRva, playerExpected,
                               reinterpret_cast<const void*>(&HookUpdatePlayerInput),
                               &playerOriginal, "player_input")) {
            return false;
        }
        g_originalUpdatePlayerInput = reinterpret_cast<UpdatePlayerInputFn>(playerOriginal);
    }

    const BYTE curveExpected[kHookLength] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C};
    void* curveOriginal = nullptr;
    if (!InstallInlineHook(kApplyLookCurveRva, curveExpected,
                           reinterpret_cast<const void*>(&HookApplyLookCurve),
                           &curveOriginal, "look_curve")) {
        return false;
    }
    g_originalApplyLookCurve = reinterpret_cast<ApplyLookCurveFn>(curveOriginal);
    return true;
}

BOOL CALLBACK InitializeProbe(PINIT_ONCE, PVOID, PVOID*) {
    if (kEnableDiagnostics) {
        OpenLog();
        Log("probe_start pid=%lu\r\n", GetCurrentProcessId());
    }
    LoadConfiguration();
    if (kEnableDiagnostics) {
        Log("config enabled=%u horizontal=%.6g vertical=%.6g path=%ls\r\n",
            g_fixEnabled ? 1U : 0U, g_horizontalMultiplier, g_verticalMultiplier,
            g_configPath[0] != L'\0' ? g_configPath : L"<unavailable>");
    }
    if (kEnableFieldWatch) {
        g_watchRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_exceptionHandler = AddVectoredExceptionHandler(1, &WatchExceptionHandler);
        HANDLE worker = nullptr;
        if (g_watchRequest != nullptr && g_exceptionHandler != nullptr) {
            worker = CreateThread(nullptr, 0, &WatchInstallerThread, nullptr, 0, nullptr);
        }
        if (worker != nullptr) {
            CloseHandle(worker);
        } else {
            Log("watch_error=initialization winerr=%lu\r\n", GetLastError());
        }
    }
    InstallMouseStateHook();
    return TRUE;
}

} // namespace

extern "C" HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance, DWORD version, REFIID iid, LPVOID* output, LPUNKNOWN outer) {
    InitOnceExecuteOnce(&g_probeInit, &InitializeProbe, nullptr, nullptr);
    if (!ResolveRealDinput8()) {
        return E_FAIL;
    }
    return g_realDirectInput8Create(instance, version, iid, output, outer);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_proxyModule = instance;
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&g_logLock);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_exceptionHandler != nullptr) {
            RemoveVectoredExceptionHandler(g_exceptionHandler);
            g_exceptionHandler = nullptr;
        }
        if (g_watchRequest != nullptr) {
            CloseHandle(g_watchRequest);
            g_watchRequest = nullptr;
        }
        if (g_log != INVALID_HANDLE_VALUE) {
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
        if (g_realDinput8 != nullptr) {
            FreeLibrary(g_realDinput8);
            g_realDinput8 = nullptr;
        }
        DeleteCriticalSection(&g_logLock);
    }
    return TRUE;
}
