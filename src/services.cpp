#include <intrin.h>
#include <utility>
#include "services.h"
#include "hooks.h"

constexpr int kFakeServices = 32;
constexpr int kFakeSlots = 96;
struct FakeService { const uintptr_t* vtable; int id; };
static uintptr_t   g_fakeVtables[kFakeServices][kFakeSlots];
static FakeService g_fakeServices[kFakeServices];
static char        g_fakeNames[kFakeServices][32] = { "hub" };
static int         g_fakeChild[kFakeServices][kFakeSlots];
static volatile LONG g_fakeSeen[kFakeServices][kFakeSlots];
static LONG        g_fakeCount = 1;
static SRWLOCK     g_fakeLock = SRWLOCK_INIT;
static uintptr_t*  g_service = nullptr;
static uintptr_t   g_serviceVtable[64];

static const struct { const char* service; int slot; } kObjectSlots[] = {
    { "hub", 3 },
    { "hub", 10 },
    { "hub", 14 },
    { "hub", 23 },
    { "hub", 26 },
    { "hub", 29 },
    { "hub", 31 },
    { "hub", 37 },
};

static bool UncheckedOnlySlot(int s, int n) { return s == 0 && n == 14; }

static bool ReadsThroughResult(const void* caller) {
    const uint8_t* p = static_cast<const uint8_t*>(caller);
    __try {
        for (int i = 0; i < 24; ++i) {
            if (p[i] == 0x48 && p[i + 1] == 0x85 && p[i + 2] == 0xC0) return false;
            if ((p[i] == 0x48 || p[i] == 0x4C) && p[i + 1] == 0x8B && (p[i + 2] & 0xC7) == 0x00) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

enum class ValueKind { Future, Handle, EmptyList };
static const struct { const char* service; int slot; ValueKind kind; } kValueSlots[] = {
    { "hub.29", 1, ValueKind::Future },
    { "hub.29", 2, ValueKind::Future },
    { "hub.29", 3, ValueKind::Future },
    { "hub.29", 4, ValueKind::Future },
    { "hub.29", 5, ValueKind::Future },
    { "hub.29", 9, ValueKind::Future },
    { "hub.29", 11, ValueKind::Handle },
    { "hub.10", 2, ValueKind::EmptyList },
};

struct DeadControl {
    uint8_t bytes[256] = {};
    DeadControl() { bytes[4] = bytes[5] = bytes[6] = bytes[7] = 0xFF; bytes[41] = 1; }
};
alignas(16) static DeadControl g_deadControl;
alignas(16) static uint8_t g_deadState[1024];
struct HeldControl {
    uint8_t bytes[256] = {};
    HeldControl() { bytes[4] = bytes[5] = bytes[6] = bytes[7] = 0xFF; bytes[40] = 1; }
};
alignas(16) static HeldControl g_heldControl;

static const char* AnswerByValue(int s, int n, uintptr_t out) {
    const ValueKind* kind = nullptr;
    for (const auto& v : kValueSlots) if (v.slot == n && !strcmp(v.service, g_fakeNames[s])) kind = &v.kind;
    if (!kind) return nullptr;
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (out < low || out + 32 > high) return nullptr;
    if (*kind == ValueKind::Handle) {
        *reinterpret_cast<uint32_t*>(out) = 0xFFFFFFFF;
        return "no handle";
    }
    uintptr_t* f = reinterpret_cast<uintptr_t*>(out);
    if (*kind == ValueKind::EmptyList) {
        uint8_t* state = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x400));
        if (state) {
            state[0xC8] = 1;
            state[0x118] = 1;
            f[0] = reinterpret_cast<uintptr_t>(g_heldControl.bytes);
            f[1] = 0;
            f[2] = reinterpret_cast<uintptr_t>(state);
            f[3] = 0;
            return "a future holding an empty list";
        }
    }
    f[0] = reinterpret_cast<uintptr_t>(g_deadControl.bytes);
    f[1] = 0;
    f[2] = reinterpret_cast<uintptr_t>(g_deadState);
    f[3] = 0;
    return "an abandoned future";
}

static uintptr_t FakeCall(int s, int n, const void* caller, uintptr_t out = 0) {
    if (const char* answer = AnswerByValue(s, n, out)) {
        if (!InterlockedExchange(&g_fakeSeen[s][n], 1)) {
            const uintptr_t at = reinterpret_cast<uintptr_t>(caller) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x140000000;
            Log("[hub] %s slot %d (+%Xh) asked for by 0x%llX, answered %s", g_fakeNames[s], n, n * 8, static_cast<unsigned long long>(at), answer);
        }
        return out;
    }
    if (UncheckedOnlySlot(s, n) && !ReadsThroughResult(caller)) {
        static const void* logged[16];
        static volatile LONG loggedCount = 0;
        bool known = false;
        for (LONG i = 0; i < loggedCount && i < 16 && !known; ++i) known = logged[i] == caller;
        if (!known && loggedCount < 16) {
            logged[InterlockedIncrement(&loggedCount) - 1] = caller;
            const uintptr_t at = reinterpret_cast<uintptr_t>(caller) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x140000000;
            Log("[hub] %s slot %d (+%Xh) asked for by 0x%llX, which checks it: answered 0", g_fakeNames[s], n, n * 8, static_cast<unsigned long long>(at));
        }
        return 0;
    }
    int child = g_fakeChild[s][n];
    if (!child) {
        bool object = false;
        for (const auto& o : kObjectSlots) object |= o.slot == n && !strcmp(o.service, g_fakeNames[s]);
        if (object) {
            AcquireSRWLockExclusive(&g_fakeLock);
            if (!(child = g_fakeChild[s][n]) && g_fakeCount < kFakeServices) {
                const LONG c = g_fakeCount++;
                sprintf_s(g_fakeNames[c], "%s.%d", g_fakeNames[s], n);
                g_fakeChild[s][n] = child = c + 1;
            }
            ReleaseSRWLockExclusive(&g_fakeLock);
        }
    }
    if (!InterlockedExchange(&g_fakeSeen[s][n], 1)) {
        const uintptr_t at = reinterpret_cast<uintptr_t>(caller) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x140000000;
        Log("[hub] %s slot %d (+%Xh) asked for by 0x%llX, answered %s", g_fakeNames[s], n, n * 8, static_cast<unsigned long long>(at),
            child ? g_fakeNames[child - 1] : "0");
    }
    return child ? reinterpret_cast<uintptr_t>(&g_fakeServices[child - 1]) : 0;
}

template <int S, int N> static uintptr_t __fastcall FakeSlot(uintptr_t, uintptr_t out, uintptr_t, uintptr_t) { return FakeCall(S, N, _ReturnAddress(), out); }

template <int S, int... N> static void FillFakeService(std::integer_sequence<int, N...>) {
    ((g_fakeVtables[S][N] = reinterpret_cast<uintptr_t>(&FakeSlot<S, N>)), ...);
    g_fakeServices[S] = { g_fakeVtables[S], S };
}

template <int... S> static void FillFakeServices(std::integer_sequence<int, S...>) {
    (FillFakeService<S>(std::make_integer_sequence<int, kFakeSlots>{}), ...);
}

uintptr_t StandInHub() { return reinterpret_cast<uintptr_t>(&g_fakeServices[0]); }

uint8_t* FindServicesObject(const Section& text) {
    int n = 0;
    uint8_t* first = FindUniquePattern(text, "40 55 53 41 55 41 57 48 8D AC 24 B8 FE FF FF 48 81 EC 48 02 00 00 45 33 ED 44 38 2D ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 18 48 8B 08 48 8B 51 40", n);
    if (!first) return nullptr;
    g_service = reinterpret_cast<uintptr_t*>(first + 0x2D + Rel32(first + 0x29));
    static bool filled = false;
    if (!filled) { filled = true; FillFakeServices(std::make_integer_sequence<int, kFakeServices>{}); }
    return first;
}

bool SwapHubSlot(HubFn hub, HubFn* real) {
    const uintptr_t service = g_service ? *g_service : 0;
    uintptr_t** vt = reinterpret_cast<uintptr_t**>(service);
    if (!service || *vt == g_serviceVtable) return false;
    memcpy(g_serviceVtable, *vt, sizeof(g_serviceVtable));
    if (real) *real = reinterpret_cast<HubFn>(g_serviceVtable[3]);
    g_serviceVtable[3] = reinterpret_cast<uintptr_t>(hub);
    *vt = g_serviceVtable;
    return true;
}

static thread_local int t_inInstanceRequest = 0;
static HubFn g_realHub = nullptr;

static uintptr_t __fastcall InstanceRequestHub(uintptr_t service) {
    const uintptr_t real = g_realHub ? g_realHub(service) : 0;
    return real || !t_inInstanceRequest ? real : StandInHub();
}

using RequestInstanceFn = uintptr_t(__fastcall*)(uintptr_t manager, const char* name, uintptr_t request);
static RequestInstanceFn g_requestInstanceOrig = nullptr;

static uintptr_t __fastcall RequestInstanceHook(uintptr_t manager, const char* name, uintptr_t request) {
    __try {
        if (SwapHubSlot(&InstanceRequestHub, &g_realHub)) Log("[atc] hangar requests answered here (single player: no services hub)");
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[atc] fault preparing the hangar request"); }
    ++t_inInstanceRequest;
    const uintptr_t result = g_requestInstanceOrig(manager, name, request);
    --t_inInstanceRequest;
    return result;
}

static uint8_t* FunctionOf(const uint8_t* site) {
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = site ? RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(site), &base, nullptr) : nullptr;
    for (int i = 0; rf && i < 8; ++i) {
        const uint8_t* info = reinterpret_cast<const uint8_t*>(base + rf->UnwindData);
        if (!((info[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(info + 4 + ((info[2] + 1) & ~1) * 2));
    }
    return rf ? reinterpret_cast<uint8_t*>(base + rf->BeginAddress) : nullptr;
}

static uint8_t* FindLeaAny(const Section& text, const uint8_t* target) {
    static const uint8_t kRex[] = { 0x48, 0x4C };
    static const uint8_t kModRm[] = { 0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D };
    for (uint8_t r : kRex)
        for (uint8_t m : kModRm)
            if (uint8_t* p = target ? FindRipLea(text, r, 0x8D, m, target) : nullptr) return p;
    return nullptr;
}

void ResolveHangarsApi(const Section& text, const Section& rdata) {
    uint8_t* fn = FunctionOf(FindLeaAny(text, FindCString(rdata, "IIM_RequestInstanceImpl_Requesting")));
    if (!FindServicesObject(text) || !fn || !BytesMatch(fn, "48 89 5C 24 10 48 89 4C 24 08 55 56 57")
        || !HookFunction(fn, 10, reinterpret_cast<void*>(&RequestInstanceHook), reinterpret_cast<void**>(&g_requestInstanceOrig)))
        Log("[!] single player: hangar request not found (ATC can't give you a hangar)");
}
