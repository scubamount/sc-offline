#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "sco/scan.h"

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

// Scanners live in sco-core (external/sco-core); these names keep the feature code unchanged.
using sco::Section;
using sco::FindCString;
using sco::Rel32;
using sco::BytesMatch;
using sco::FindRipLea;
using sco::FindPattern;
using sco::FindUniquePattern;
extern Section g_text, g_rdata;

Section        FindSection(const char* name);
bool           WriteCode(uint8_t* at, const uint8_t* bytes, size_t n, DWORD& err);
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
