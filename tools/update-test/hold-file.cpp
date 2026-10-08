// hold-file.exe <file> <ms> [<ready-file>]
// Opens <file> with share mode 0 (no other process may open, move or delete it), writes
// <ready-file> once the handle is held, sleeps <ms>, then closes it. Stands in for an antivirus
// scanner that locks a fresh exe or DLL (issue #31 D2). Exit 0 = held for the whole time,
// 2 = couldn't open <file>, 3 = couldn't write <ready-file>, 4 = bad arguments.
#include <windows.h>
#include <cwchar>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 4;
    HANDLE h = CreateFileW(argv[1], GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 2;
    if (argc > 3) {
        HANDLE r = CreateFileW(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (r == INVALID_HANDLE_VALUE) { CloseHandle(h); return 3; }
        CloseHandle(r);
    }
    Sleep(wcstoul(argv[2], nullptr, 10));
    CloseHandle(h);
    return 0;
}
