#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::fwprintf(stderr, L"usage: read_process_memory <pid> <hex-address> <byte-count>\n");
        return 1;
    }

    wchar_t* end = nullptr;
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], &end, 0));
    if (end == argv[1] || *end != L'\0') {
        std::fwprintf(stderr, L"invalid pid\n");
        return 2;
    }
    const uintptr_t address = static_cast<uintptr_t>(std::wcstoull(argv[2], &end, 0));
    if (end == argv[2] || *end != L'\0') {
        std::fwprintf(stderr, L"invalid address\n");
        return 3;
    }
    const size_t byteCount = static_cast<size_t>(std::wcstoull(argv[3], &end, 0));
    if (end == argv[3] || *end != L'\0' || byteCount == 0 || byteCount > 4096) {
        std::fwprintf(stderr, L"invalid byte count (1..4096)\n");
        return 4;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        std::fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 5;
    }

    std::vector<unsigned char> bytes(byteCount);
    SIZE_T bytesRead = 0;
    const BOOL ok = ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                                      bytes.data(), bytes.size(), &bytesRead);
    CloseHandle(process);
    if (!ok) {
        std::fwprintf(stderr, L"ReadProcessMemory failed: %lu\n", GetLastError());
        return 6;
    }

    for (SIZE_T offset = 0; offset < bytesRead; offset += 16) {
        std::printf("%08lX:", static_cast<unsigned long>(address + offset));
        const SIZE_T lineEnd = (offset + 16 < bytesRead) ? offset + 16 : bytesRead;
        for (SIZE_T index = offset; index < lineEnd; ++index) {
            std::printf(" %02X", bytes[index]);
        }
        std::printf("\n");
    }
    return 0;
}
