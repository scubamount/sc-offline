#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void InitLog();
void OpenConsole();
void Log(const char* fmt, ...);

enum class PatchResult { NotRun, NotFound, WrongMatchCount, ProtectFailed, Applied };

struct PatchStatus {
    PatchResult result   = PatchResult::NotRun;
    int         sites    = 0;
    int         expected = 0;
    void*       at       = nullptr;
    DWORD       err      = 0;
};

struct Section { uint8_t* base = nullptr; size_t size = 0; };
extern Section g_text, g_rdata;

Section        FindSection(const char* name);
const uint8_t* FindCString(const Section& s, const char* str);
int32_t        Rel32(const uint8_t* p);
bool           BytesMatch(const uint8_t* p, const char* pattern);
bool           WriteCode(uint8_t* at, const uint8_t* bytes, size_t n, DWORD& err);
uint8_t*       FindRipLea(const Section& text, uint8_t reg0, uint8_t reg1, uint8_t reg2, const uint8_t* target);
int            FindPattern(const Section& text, const char* pattern, uint8_t** out, int max);
uint8_t*       FindUniquePattern(const Section& text, const char* pattern, int& matches);
void           LogPatch(const char* name, const PatchStatus& st);

template <typename T> T Rd(uintptr_t p) { return *reinterpret_cast<const T*>(p); }

template <typename R, typename... A> R VCall(uintptr_t obj, size_t off, A... a) {
    using Fn = R(__fastcall*)(uintptr_t, A...);
    return reinterpret_cast<Fn>(Rd<uintptr_t>(Rd<uintptr_t>(obj) + off))(obj, a...);
}

constexpr uint64_t kPtrMask = 0xFFFFFFFFFFFFull;

bool GameHasFocus();
bool ShipsFilePath(char* path, DWORD n);
// <directory of the path in envVar>\file. False when the variable is unset or the result won't fit.
bool SiblingPath(const char* envVar, const char* file, char* path, DWORD n);
// The menu's lists sit beside ships.txt; the mod's own state and test files sit beside mod.log.
bool DataFilePath(char* path, DWORD n, const char* file);
bool ModLogSibling(char* path, DWORD n, const char* file);
