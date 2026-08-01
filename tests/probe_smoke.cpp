#include <windows.h>
#include <unknwn.h>

#include <cstdio>

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

int main() {
    HMODULE proxy = LoadLibraryW(L"dinput8.dll");
    if (proxy == nullptr) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    auto create = reinterpret_cast<DirectInput8CreateFn>(
        GetProcAddress(proxy, "DirectInput8Create"));
    if (create == nullptr) {
        std::printf("GetProcAddress failed: %lu\n", GetLastError());
        return 2;
    }

    constexpr GUID iidDirectInput8A = {
        0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}
    };
    IUnknown* directInput = nullptr;
    const HRESULT result = create(GetModuleHandleW(nullptr), 0x0800,
                                  iidDirectInput8A,
                                  reinterpret_cast<void**>(&directInput), nullptr);
    if (FAILED(result) || directInput == nullptr) {
        std::printf("DirectInput8Create failed: 0x%08lX\n", static_cast<unsigned long>(result));
        return 3;
    }

    directInput->Release();
    std::printf("proxy smoke test passed\n");
    return 0;
}
