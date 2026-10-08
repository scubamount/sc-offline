// tar-shim.exe: a stand-in for Windows' System32\tar.exe (bsdtar), which Wine doesn't ship.
// Handles only what the launcher runs, `tar -xf <zip> -C <dir>`, and only zips whose entries are
// stored (no compression); make_fixtures.py writes its zips that way. Like bsdtar it refuses entries
// with a '..' part or an absolute path. Exit 0 = everything extracted, 1 = an entry was refused or
// couldn't be written, 2 = bad arguments or not a zip it can read.
// run.sh copies it to System32\tar.exe in its own throwaway Wine prefix, never in a real one.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static unsigned U16(const std::vector<unsigned char>& b, size_t o) { return b[o] | (b[o + 1] << 8); }
static unsigned U32(const std::vector<unsigned char>& b, size_t o) { return U16(b, o) | (U16(b, o + 2) << 16); }

static void MakeParents(const std::wstring& path) {
    for (size_t i = 3; i < path.size(); ++i)
        if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}

int wmain(int argc, wchar_t** argv) {
    std::wstring zip, dir = L".";
    bool extract = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.size() > 1 && a[0] == L'-' && a != L"-C") {
            if (a.find(L'x') != std::wstring::npos) extract = true;
            if (a.back() == L'f' && i + 1 < argc) zip = argv[++i];
        } else if (a == L"-C" && i + 1 < argc) dir = argv[++i];
    }
    if (!extract || zip.empty()) { std::fprintf(stderr, "tar-shim: only -xf <zip> [-C <dir>]\n"); return 2; }

    HANDLE h = CreateFileW(zip.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "tar-shim: can't open %ls\n", zip.c_str()); return 2; }
    std::vector<unsigned char> b(GetFileSize(h, nullptr));
    DWORD got = 0;
    const BOOL read = ReadFile(h, b.data(), (DWORD)b.size(), &got, nullptr);
    CloseHandle(h);
    if (!read || got != b.size() || b.size() < 22) { std::fprintf(stderr, "tar-shim: short read\n"); return 2; }

    size_t eocd = std::string::npos;
    for (size_t i = b.size() - 22 + 1; i-- > 0;)
        if (U32(b, i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) { std::fprintf(stderr, "tar-shim: not a zip\n"); return 2; }

    int rc = 0;
    size_t p = U32(b, eocd + 16);
    for (unsigned n = U16(b, eocd + 10); n--;) {
        if (p + 46 > b.size() || U32(b, p) != 0x02014b50) { std::fprintf(stderr, "tar-shim: bad central directory\n"); return 2; }
        const unsigned method = U16(b, p + 10), size = U32(b, p + 20), nameLen = U16(b, p + 28);
        const size_t local = U32(b, p + 42);
        std::string name((const char*)&b[p + 46], nameLen);
        p += 46 + nameLen + U16(b, p + 30) + U16(b, p + 32);

        if (name.empty() || name[0] == '/' || name.find(':') == 1 || ("/" + name + "/").find("/../") != std::string::npos) {
            std::fprintf(stderr, "tar-shim: %s: Path contains '..' or is absolute\n", name.c_str());
            rc = 1; continue;
        }
        std::wstring out = dir + L"\\";
        for (char c : name) out += c == '/' ? L'\\' : (wchar_t)(unsigned char)c;
        if (name.back() == '/') { MakeParents(out); continue; }
        if (method != 0) { std::fprintf(stderr, "tar-shim: %s is compressed; only stored entries\n", name.c_str()); return 2; }
        const size_t data = local + 30 + U16(b, local + 26) + U16(b, local + 28);
        if (data + size > b.size()) { std::fprintf(stderr, "tar-shim: %s runs past the end\n", name.c_str()); return 2; }
        MakeParents(out);
        HANDLE o = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD wrote = 0;
        if (o == INVALID_HANDLE_VALUE || !WriteFile(o, &b[data], size, &wrote, nullptr) || wrote != size) {
            std::fprintf(stderr, "tar-shim: can't write %s (error %lu)\n", name.c_str(), GetLastError());
            rc = 1;
        }
        if (o != INVALID_HANDLE_VALUE) CloseHandle(o);
    }
    return rc;
}
