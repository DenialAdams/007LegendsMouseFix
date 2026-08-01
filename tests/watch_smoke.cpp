#include <windows.h>

#include <cstdio>

namespace {

volatile LONG g_watchedValue = 123;
volatile LONG g_hits = 0;
HANDLE g_ready = nullptr;

LONG CALLBACK ExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    if (exceptionInfo == nullptr || exceptionInfo->ExceptionRecord == nullptr ||
        exceptionInfo->ContextRecord == nullptr ||
        exceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP ||
        (exceptionInfo->ContextRecord->Dr6 & 1) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    exceptionInfo->ContextRecord->Dr6 = 0;
    exceptionInfo->ContextRecord->Dr7 &= ~static_cast<DWORD_PTR>(0x000F0001);
    InterlockedIncrement(&g_hits);
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct InstallRequest {
    DWORD threadId;
    uintptr_t address;
};

DWORD WINAPI InstallWatchpoint(void* parameter) {
    const auto* request = static_cast<const InstallRequest*>(parameter);
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                               FALSE, request->threadId);
    if (thread == nullptr || SuspendThread(thread) == static_cast<DWORD>(-1)) {
        SetEvent(g_ready);
        return 1;
    }

    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool installed = false;
    if (GetThreadContext(thread, &context)) {
        context.Dr0 = request->address;
        context.Dr6 = 0;
        context.Dr7 &= ~static_cast<DWORD_PTR>(0x000F0001);
        context.Dr7 |= static_cast<DWORD_PTR>(0x000F0001);
        installed = SetThreadContext(thread, &context) != FALSE;
    }

    ResumeThread(thread);
    CloseHandle(thread);
    SetEvent(g_ready);
    return installed ? 0 : 2;
}

} // namespace

int main() {
    PVOID handler = AddVectoredExceptionHandler(1, &ExceptionHandler);
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (handler == nullptr || g_ready == nullptr) {
        std::printf("watchpoint setup failed: %lu\n", GetLastError());
        return 1;
    }

    const InstallRequest request{GetCurrentThreadId(),
                                 reinterpret_cast<uintptr_t>(&g_watchedValue)};
    HANDLE worker = CreateThread(nullptr, 0, &InstallWatchpoint,
                                 const_cast<InstallRequest*>(&request), 0, nullptr);
    if (worker == nullptr || WaitForSingleObject(g_ready, 5000) != WAIT_OBJECT_0) {
        std::printf("watchpoint installer failed: %lu\n", GetLastError());
        return 2;
    }

    const LONG observed = g_watchedValue;
    WaitForSingleObject(worker, 5000);
    CloseHandle(worker);
    CloseHandle(g_ready);
    RemoveVectoredExceptionHandler(handler);

    if (observed != 123 || g_hits != 1) {
        std::printf("watchpoint smoke test failed: value=%ld hits=%ld\n", observed, g_hits);
        return 3;
    }
    std::printf("watchpoint smoke test passed\n");
    return 0;
}
