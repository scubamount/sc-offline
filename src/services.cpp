// The services hub stand-in, and the hangar half of the ship terminal flow: personal hangar
// instances, elevators and the ATC's pad tokens (research doc "Offline ship terminals (ASOP), ship
// lifts, personal hangars and ATC", sections 11-12; rc7-rc11 and rc20-rc22 of its section 2).
// fleet.cpp has the terminal half. Every address is one of sco-core's hangar.* / atc.* / landing.*
// rows (sco/game/asop.h).
#include <intrin.h>
#include <utility>
#include "services.h"
#include "fleet.h"
#include "hooks.h"
#include "teleport.h"
#include "spawner.h"
#include "sco/hook.h"
#include "sco/caps.h"
#include "sco/signatures.h"

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

// The hub global (`gEnv + 0x188`, the mov rcx at hangar.services_hub_user +0x26) and the stand-in's
// fake objects.
static bool PrepareStandIn() {
    g_service = reinterpret_cast<uintptr_t*>(sco::Sig("hangar.services_hub"));
    if (!g_service) return false;
    static bool filled = false;
    if (!filled) { filled = true; FillFakeServices(std::make_integer_sequence<int, kFakeServices>{}); }
    return true;
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

// The stand-in answers only inside the wrapped calls (RequestInstanceImpl, and with the hangar
// feature RequestPlayerHangarPermissions): this thread's depth counter is nonzero there. Elsewhere
// the game still sees the null internal services it checks for.
static thread_local int t_inInstanceRequest = 0;
static HubFn g_realHub = nullptr;

static uintptr_t __fastcall InstanceRequestHub(uintptr_t service) {
    const uintptr_t real = g_realHub ? g_realHub(service) : 0;
    return real || !t_inInstanceRequest ? real : StandInHub();
}

static void PrepareHub() {
    __try {
        if (SwapHubSlot(&InstanceRequestHub, &g_realHub)) Log("[atc] hangar requests answered here (single player: no services hub)");
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[atc] fault preparing the hangar request"); }
}

using RequestInstanceFn = uintptr_t(__fastcall*)(uintptr_t manager, const char* name, uintptr_t request);
static RequestInstanceFn g_requestInstanceOrig = nullptr;

static uintptr_t __fastcall RequestInstanceHook(uintptr_t manager, const char* name, uintptr_t request) {
    PrepareHub();
    ++t_inInstanceRequest;
    const uintptr_t result = g_requestInstanceOrig(manager, name, request);
    --t_inInstanceRequest;
    return result;
}

// rc10: the elevators ask RequestPlayerHangarPermissions, which asks the null hub and never
// answers. With the stand-in its FindHangarsAtLocation gets an empty list (hub.10 slot 2) and the
// continuation grants the player's own instances.
using PermissionsFn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static PermissionsFn g_permissionsOrig = nullptr;

static uintptr_t __fastcall PermissionsHook(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6) {
    PrepareHub();
    ++t_inInstanceRequest;
    const uintptr_t result = g_permissionsOrig(a1, a2, a3, a4, a5, a6);
    --t_inInstanceRequest;
    return result;
}

// ---- hangar features -----------------------------------------------------------------------------

enum HangarFeature { kInstance, kTokens, kDiagnostics, kHangarFeatureCount };
struct HangarFeatureState { const char* cap; const char* title; const char* ready; bool on; char why[160]; };
static HangarFeatureState g_hangar[kHangarFeatureCount] = {
    { "hangar.instance", "personal hangars and elevators (rc7-rc11)", "the ATC's hangar streams in, joins the station and the elevators list it", false, "" },
    { "atc.tokens", "ATC pad owner and pad marker (rc20-rc22)", "hails from your hangar are take-offs and landings show the pad", false, "" },
    { "asop.diagnostics", "ASOP diagnostics (log only)", "[atc] and [iim] lines in mod.log", false, "" },
};
static bool g_hangarResolved = false;

static void HangarOff(HangarFeature f, const char* why) {
    g_hangar[f].on = false;
    strncpy_s(g_hangar[f].why, why, _TRUNCATE);
}

static volatile LONG g_iimLines = 0, g_atcLines = 0;
static bool IimBudget() { return InterlockedIncrement(&g_iimLines) <= 200; }
static bool AtcBudget() { return InterlockedIncrement(&g_atcLines) <= 200; }

static uintptr_t* g_iimEntitySystem = nullptr;   // hangar.entity_system
static const uint16_t* g_iimTypeWord = nullptr;  // hangar.iim_type_id
static volatile DWORD g_gameMainThread = 0;      // the window-message thread (first ProcessHangars)
static volatile LONG64 g_lastIimEntity = 0;      // the IIM entity the creation batch ran for

static uintptr_t IimGetEntity(uint64_t id) {
    __try {
        const uintptr_t es = g_iimEntitySystem ? *g_iimEntitySystem : 0;
        return es && id ? VCall<uintptr_t>(es, 0x120, id) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---- rc7: IIM continuations on a thread that can't see the IIM -----------------------------------
//
// The three continuations resolve the IIM's entity id again when they run. Offline the creation
// batch's runs on a loading thread, where CEntitySystem::GetEntity hides main-thread entities: the
// IIM "no longer exists" and the new hangar is dropped. The window-message thread sees it.
//
// So a continuation that can't see its IIM is handed to that thread: the loading thread queues it
// and waits; ProcessHangars (every window message) runs it and signals. Deadlock analysis:
//  - the loading thread waits at most kIimHandoffMs for the main thread to TAKE the call. If the
//    main thread is busy or itself blocked (a loading screen, or waiting on this very loading
//    thread), the call is taken back under the lock and runs in place, as the game would have run
//    it (the hangar may then be dropped; nothing hangs);
//  - once the main thread has taken the call it is executing it, not waiting on anyone, so the
//    loading thread waits for it without a timeout. It must: the lambda's state lives in the
//    caller's frame and is freed when the hook returns;
//  - a continuation that runs on the main thread, or can see its IIM where it is, runs in place,
//    so a continuation the main thread runs that triggers another one never queues behind itself.
constexpr DWORD kIimHandoffMs = 3000;
using IimContinuationFn = void(__fastcall*)(uintptr_t lambda, uintptr_t a2, uintptr_t a3);
struct IimCall { IimContinuationFn fn; uintptr_t lambda, a2, a3; HANDLE done; bool taken; };
constexpr int kMaxIimCalls = 32;
static SRWLOCK  g_iimLock = SRWLOCK_INIT;
static IimCall* g_iimPending[kMaxIimCalls];
static int      g_iimPendingCount = 0;
static IimContinuationFn g_iimFindOrig = nullptr, g_iimUnstowOrig = nullptr, g_iimCreationOrig = nullptr;

static bool IimVisibleHere(uintptr_t lambda) {
    __try { return IimGetEntity(Rd<uint64_t>(lambda)) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void RunIimCall(IimContinuationFn fn, uintptr_t lambda, uintptr_t a2, uintptr_t a3) {
    __try { fn(lambda, a2, a3); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[iim] fault in a hangar continuation run on the main thread"); }
}

static void RunIimCallback(IimContinuationFn fn, uintptr_t lambda, uintptr_t a2, uintptr_t a3) {
    const DWORD mainThread = g_gameMainThread;
    if (!g_hangar[kInstance].on || !mainThread || GetCurrentThreadId() == mainThread || IimVisibleHere(lambda)) {
        fn(lambda, a2, a3);
        return;
    }
    IimCall call{ fn, lambda, a2, a3, CreateEventW(nullptr, TRUE, FALSE, nullptr), false };
    bool queued = false;
    if (call.done) {
        AcquireSRWLockExclusive(&g_iimLock);
        if (g_iimPendingCount < kMaxIimCalls) { g_iimPending[g_iimPendingCount++] = &call; queued = true; }
        ReleaseSRWLockExclusive(&g_iimLock);
    }
    if (!queued) {
        if (call.done) CloseHandle(call.done);
        fn(lambda, a2, a3);
        return;
    }
    if (IimBudget()) Log("[iim] hangar continuation handed to the main thread (loading thread %lu can't see the IIM)", GetCurrentThreadId());
    if (WaitForSingleObject(call.done, kIimHandoffMs) == WAIT_TIMEOUT) {
        bool runHere = false;
        AcquireSRWLockExclusive(&g_iimLock);
        if (!call.taken) {
            for (int i = 0; i < g_iimPendingCount; ++i)
                if (g_iimPending[i] == &call) { g_iimPending[i] = g_iimPending[--g_iimPendingCount]; break; }
            runHere = true;
        }
        ReleaseSRWLockExclusive(&g_iimLock);
        if (runHere) {
            if (IimBudget()) Log("[iim] main thread busy for %lu ms; continuation runs on the loading thread", kIimHandoffMs);
            fn(lambda, a2, a3);
        } else {
            WaitForSingleObject(call.done, INFINITE);   // taken: the main thread is running it now
        }
    }
    CloseHandle(call.done);
}

static void RunHandedOffIim() {
    IimCall* calls[kMaxIimCalls];
    int n = 0;
    AcquireSRWLockExclusive(&g_iimLock);
    for (int i = 0; i < g_iimPendingCount; ++i) { g_iimPending[i]->taken = true; calls[n++] = g_iimPending[i]; }
    g_iimPendingCount = 0;
    ReleaseSRWLockExclusive(&g_iimLock);
    for (int i = 0; i < n; ++i) {
        RunIimCall(calls[i]->fn, calls[i]->lambda, calls[i]->a2, calls[i]->a3);
        SetEvent(calls[i]->done);
    }
}

static void __fastcall IimFindHook(uintptr_t lambda, uintptr_t a2, uintptr_t a3) { RunIimCallback(g_iimFindOrig, lambda, a2, a3); }
static void __fastcall IimUnstowHook(uintptr_t lambda, uintptr_t a2, uintptr_t a3) { RunIimCallback(g_iimUnstowOrig, lambda, a2, a3); }
static void __fastcall IimCreationHook(uintptr_t lambda, uintptr_t a2, uintptr_t a3) {
    __try {
        const uint64_t iim = Rd<uint64_t>(lambda);
        if (iim) InterlockedExchange64(&g_lastIimEntity, static_cast<LONG64>(iim));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    RunIimCallback(g_iimCreationOrig, lambda, a2, a3);
}

// ---- rc9 / rc11: queue the instance at the gateway, keep it until the player was inside ----------

using QueueInstanceFn = uint8_t(__fastcall*)(uintptr_t iimComponent, uint64_t instance);
static QueueInstanceFn g_queueInstance = nullptr;
constexpr DWORD kQueueDelayMs = 3000, kQueueRetryMs = 2000, kQueueGiveUpMs = 30000;
struct QueuedInstance { uint64_t iim, instance; DWORD at, lastTry; bool active; };
constexpr int kMaxQueued = 16, kMaxKept = 16;
static SRWLOCK        g_hangarLock = SRWLOCK_INIT;   // the queue, the kept hangars, the landing UI list
static QueuedInstance g_queued[kMaxQueued];
static uint64_t       g_kept[kMaxKept];

static void QueueInstanceLater(uint64_t iim, uint64_t instance) {
    if (!iim || !instance) return;
    const DWORD now = GetTickCount();
    bool fresh = false;
    AcquireSRWLockExclusive(&g_hangarLock);
    QueuedInstance* slot = nullptr;
    for (QueuedInstance& q : g_queued)
        if (q.active && q.instance == instance) slot = &q;
    if (!slot)
        for (QueuedInstance& q : g_queued)
            if (!q.active && !slot) slot = &q;
    if (slot && !(slot->active && slot->iim == iim)) {
        *slot = { iim, instance, now, 0, true };
        fresh = true;
    }
    bool kept = false;
    for (uint64_t k : g_kept) kept |= k == instance;
    for (uint64_t& k : g_kept)
        if (!kept && !k) { k = instance; kept = true; }
    ReleaseSRWLockExclusive(&g_hangarLock);
    if (fresh) Log("[iim] hangar instance 0x%llX will be queued at its gateway (IIM 0x%llX)", static_cast<unsigned long long>(instance),
                   static_cast<unsigned long long>(iim));
}

static uintptr_t IimComponent(uint64_t iimEntity) {
    __try {
        const uintptr_t entity = IimGetEntity(iimEntity);
        if (!entity || !g_iimTypeWord) return 0;
        uint16_t type = *g_iimTypeWord;
        if (type == 0xFFFF) return 0;   // not registered yet
        uint64_t out = 0;
        const uint64_t* h = VCall<const uint64_t*>(entity, 0x390, &out, &type);
        return h ? (*h & kPtrMask) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int CallQueueInstance(uintptr_t iim, uint64_t instance) {
    __try { return g_queueInstance(iim, instance); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Every 500 ms: first try 3 s after the instance appeared, then every 2 s until QueueInstance says
// yes or 30 s pass. A first answer of 0 has still promoted the instance (doc 11.5).
static void RunInstanceQueue(DWORD now) {
    QueuedInstance due[kMaxQueued];
    int n = 0;
    AcquireSRWLockExclusive(&g_hangarLock);
    for (QueuedInstance& q : g_queued) {
        if (!q.active) continue;
        if (now - q.at > kQueueGiveUpMs) { q.active = false; continue; }
        if (now - q.at < kQueueDelayMs || (q.lastTry && now - q.lastTry < kQueueRetryMs)) continue;
        q.lastTry = now;
        due[n++] = q;
    }
    ReleaseSRWLockExclusive(&g_hangarLock);
    for (int i = 0; i < n; ++i) {
        const uintptr_t iim = IimComponent(due[i].iim);
        const int r = iim ? CallQueueInstance(iim, due[i].instance) : -2;
        Log("[iim] QueueInstance(0x%llX) -> %s", static_cast<unsigned long long>(due[i].instance),
            r > 0 ? "queued" : r == 0 ? "0 (retrying)" : r == -1 ? "fault" : "IIM not found");
        if (r > 0) {
            AcquireSRWLockExclusive(&g_hangarLock);
            for (QueuedInstance& q : g_queued)
                if (q.active && q.instance == due[i].instance) q.active = false;
            ReleaseSRWLockExclusive(&g_hangarLock);
        }
    }
}

// A kept hangar leaves the list once the player has been inside it: from then on the game's own
// tear-down check sees the player and decides.
static void WatchKeptHangars() {
    uint64_t zones[16] = {};
    uintptr_t zonePtrs[16] = {};
    int n = 0;
    __try {
        uintptr_t actor = 0, entity = 0;
        if (!GetLocalPlayer(actor, entity)) return;
        for (uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8); zone && n < 16; zone = ZoneParent(zone)) {
            zonePtrs[n] = zone;
            zones[n++] = ZoneId(zone);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    AcquireSRWLockExclusive(&g_hangarLock);
    for (uint64_t& k : g_kept)
        for (int i = 0; k && i < n; ++i)
            if (zones[i] == k || zonePtrs[i] == k) {
                Log("[iim] you're inside hangar 0x%llX: the game decides its tear-down from now on", static_cast<unsigned long long>(k));
                k = 0;
            }
    ReleaseSRWLockExclusive(&g_hangarLock);
}

using TearDownFn = bool(__fastcall*)(uintptr_t iim, uintptr_t record, uint8_t flag);
static TearDownFn g_tearDownOrig = nullptr;

static uint64_t KeptHangarOf(uintptr_t record) {
    uint64_t ids[2] = {};
    __try { ids[0] = Rd<uint64_t>(record + 0x10); ids[1] = Rd<uint64_t>(record + 0x08); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    AcquireSRWLockExclusive(&g_hangarLock);
    uint64_t kept = 0;
    for (uint64_t k : g_kept)
        for (uint64_t id : ids)
            if (k && k == id) kept = k;
    ReleaseSRWLockExclusive(&g_hangarLock);
    return kept;
}

// rc11: the tear-down check's zone query sees nobody in a hangar the player hasn't reached yet.
static bool __fastcall TearDownHook(uintptr_t iim, uintptr_t record, uint8_t flag) {
    if (!g_tearDownOrig(iim, record, flag)) return false;
    if (!g_hangar[kInstance].on) return true;
    const uint64_t kept = KeptHangarOf(record);
    if (!kept) return true;
    if (AtcBudget()) Log("[iim] hangar 0x%llX kept: you haven't been inside yet (rc11)", static_cast<unsigned long long>(kept));
    QueueInstanceLater(static_cast<uint64_t>(InterlockedCompareExchange64(&g_lastIimEntity, 0, 0)), kept);   // relinquishing took it off its gateway
    return false;
}

// ---- rc8: the streaming bubble ----------------------------------------------------------------

using BubbleUpdateFn = void(__fastcall*)(uintptr_t mgr, uint64_t player, uint8_t slot, uint64_t instance);
using BubbleFindFn   = void(__fastcall*)(uintptr_t map, uintptr_t* out, const uint64_t* player);
static BubbleUpdateFn g_bubbleUpdateOrig = nullptr;
static BubbleFindFn   g_bubbleFind = nullptr;
constexpr size_t kBubbleSize = 0x120;

// After the update: if the player's bubble key isn't in the vector and there is exactly one
// bubble, the instance goes into that bubble's slot (offline the player maps to a key that isn't
// there: "Unknown Bubble").
static bool FixBubble(uintptr_t mgr, uint64_t player, uint8_t slot, uint64_t instance) {
    __try {
        const uintptr_t begin = Rd<uintptr_t>(mgr + 0x00), end = Rd<uintptr_t>(mgr + 0x08);
        if (!begin || end < begin || slot >= 8) return false;
        uintptr_t out[2] = {};
        g_bubbleFind(mgr + 0x20, out, &player);
        bool known = false;
        if (out[0] != Rd<uintptr_t>(mgr + 0x38) + Rd<uintptr_t>(mgr + 0x20) && out[1]) {
            const uintptr_t node = Rd<uintptr_t>(out[1]);
            const uint64_t k0 = Rd<uint64_t>(node + 0x08), k1 = Rd<uint64_t>(node + 0x10);
            for (uintptr_t b = begin; b + kBubbleSize <= end; b += kBubbleSize)
                known |= Rd<uint64_t>(b) == k0 && Rd<uint64_t>(b + 8) == k1;
        }
        if (known || (end - begin) / kBubbleSize != 1) return false;
        reinterpret_cast<uint64_t*>(begin + 0x38)[slot] = instance;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void __fastcall BubbleUpdateHook(uintptr_t mgr, uint64_t player, uint8_t slot, uint64_t instance) {
    g_bubbleUpdateOrig(mgr, player, slot, instance);
    if (!g_hangar[kInstance].on || !FixBubble(mgr, player, slot, instance)) return;
    if (AtcBudget()) Log("[iim] streaming bubble: instance 0x%llX written to your bubble's slot %u (rc8)", static_cast<unsigned long long>(instance), slot);
    if (instance) QueueInstanceLater(static_cast<uint64_t>(InterlockedCompareExchange64(&g_lastIimEntity, 0, 0)), instance);   // rc9
}

// ---- rc20-rc22: pad tokens --------------------------------------------------------------------

using ChangeTokenStateFn = uintptr_t(__fastcall*)(uintptr_t atc, const uintptr_t* token, uintptr_t state, uintptr_t owner, uintptr_t vehicle,
                                                 uintptr_t a6, uintptr_t a7, uintptr_t a8);
using ClearTokenFn = uintptr_t(__fastcall*)(uintptr_t atc, const uintptr_t* token, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
using OnReservingFn = void(__fastcall*)(uintptr_t landingArea);
static ChangeTokenStateFn g_changeTokenStateOrig = nullptr;
static ClearTokenFn       g_clearTokenOrig = nullptr;
static OnReservingFn      g_onReservingChanged = nullptr;
constexpr uintptr_t kOnReservingSlot = 257 * 8;   // landing area vtable: OnReservingEntityChanged
constexpr int kMaxLandingUi = 32;
static uintptr_t g_landingUi[kMaxLandingUi];
static int       g_landingUiCount = 0;

// The vtable comparison is a type check (tokens of docking tubes are skipped) and a liveness check.
static bool IsLandingArea(uintptr_t area) {
    __try {
        return area && Rd<uintptr_t>(Rd<uintptr_t>(area) + kOnReservingSlot) == reinterpret_cast<uintptr_t>(g_onReservingChanged);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void ReadToken(const uintptr_t* token, uintptr_t& area, uintptr_t& object) {
    area = object = 0;
    __try {
        object = token ? token[0] : 0;
        area = object ? (Rd<uintptr_t>(object) & kPtrMask) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        area = object = 0;
    }
    if (!IsLandingArea(area)) area = 0;
}

static uint64_t TokenVehicle(uintptr_t object) {
    __try { return object ? Rd<uint64_t>(object + 0x10) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void QueueLandingUi(uintptr_t area) {
    if (!area) return;
    AcquireSRWLockExclusive(&g_hangarLock);
    bool known = false;
    for (int i = 0; i < g_landingUiCount; ++i) known |= g_landingUi[i] == area;
    if (!known && g_landingUiCount < kMaxLandingUi) g_landingUi[g_landingUiCount++] = area;
    ReleaseSRWLockExclusive(&g_hangarLock);
}

// rc21: the pad marker comes from a client-only change callback of the landing area's reserving
// player; offline nothing receives the change, so it runs here after each token change.
static void RunLandingUi() {
    uintptr_t areas[kMaxLandingUi];
    int n = 0;
    AcquireSRWLockExclusive(&g_hangarLock);
    for (int i = 0; i < g_landingUiCount; ++i) areas[n++] = g_landingUi[i];
    g_landingUiCount = 0;
    ReleaseSRWLockExclusive(&g_hangarLock);
    for (int i = 0; i < n; ++i) {
        if (!IsLandingArea(areas[i])) continue;
        __try { g_onReservingChanged(areas[i]); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[atc] fault updating a landing area's marker"); }
        if (AtcBudget()) Log("[atc] landing area 0x%llX: pad marker updated (rc21)", static_cast<unsigned long long>(areas[i]));
    }
}

// rc20: the platform-spawned ship has no owner, so the ATC gave the pad to owner 0 and a hail from
// the hangar became a landing request. rc22: the ship's record follows the token to another ATC.
static uintptr_t __fastcall ChangeTokenStateHook(uintptr_t atc, const uintptr_t* token, uintptr_t state, uintptr_t owner, uintptr_t vehicle,
                                                 uintptr_t a6, uintptr_t a7, uintptr_t a8) {
    if (!g_hangar[kTokens].on) return g_changeTokenStateOrig(atc, token, state, owner, vehicle, a6, a7, a8);
    uintptr_t area = 0, object = 0;
    ReadToken(token, area, object);   // before the call: the callee releases its shared_ptr argument
    if (!owner && vehicle)
        if (const uint64_t player = Fleet_RetrievedShipOwner(vehicle)) {
            owner = player;
            if (AtcBudget()) Log("[atc] pad token for 0x%llX: owner 0 -> you (rc20)", static_cast<unsigned long long>(vehicle));
        }
    const uintptr_t r = g_changeTokenStateOrig(atc, token, state, owner, vehicle, a6, a7, a8);
    QueueLandingUi(area);
    if (const uint64_t v = area ? TokenVehicle(object) : 0) Fleet_OnPadVehicle(EntityIdOfComponent(atc), EntityIdOfComponent(area), v);
    return r;
}

static uintptr_t __fastcall ClearTokenHook(uintptr_t atc, const uintptr_t* token, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6) {
    if (!g_hangar[kTokens].on) return g_clearTokenOrig(atc, token, a3, a4, a5, a6);
    uintptr_t area = 0, object = 0;
    ReadToken(token, area, object);
    const uintptr_t r = g_clearTokenOrig(atc, token, a3, a4, a5, a6);
    QueueLandingUi(area);
    return r;
}

// ---- diagnostics (doc 14, log only) -------------------------------------------------------------

using NotifyPlayerFn = void(__fastcall*)(uintptr_t atc, const uint8_t* request, const void* data, uintptr_t a4);
using PermissionsContinuationFn = void(__fastcall*)(uintptr_t lambda, const uint8_t* result, uintptr_t a3, uintptr_t a4);
static NotifyPlayerFn            g_notifyPlayerOrig = nullptr;
// RmMulticastNotifyPlayer's client half: void(atc, context, request, data, CLocIdentifier&). It queues
// the HUD reply; logging it shows whether an ATC answer reaches the client side offline.
using NotifyClientFn = void(__fastcall*)(uintptr_t atc, uintptr_t context, const uint8_t* request, const void* data, uintptr_t loc,
                                         uintptr_t a6);
static NotifyClientFn g_notifyClientOrig = nullptr;
static PermissionsContinuationFn g_permissionsContinuationOrig = nullptr;

static void LogNotify(const uint8_t* request) {
    __try {
        Log("[atc] NotifyPlayer mode %u notification %u player 0x%llX vehicle 0x%llX", Rd<uint32_t>(reinterpret_cast<uintptr_t>(request) + 0x18),
            Rd<uint32_t>(reinterpret_cast<uintptr_t>(request) + 0x1C), static_cast<unsigned long long>(Rd<uint64_t>(reinterpret_cast<uintptr_t>(request))),
            static_cast<unsigned long long>(Rd<uint64_t>(reinterpret_cast<uintptr_t>(request) + 8)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void __fastcall NotifyPlayerHook(uintptr_t atc, const uint8_t* request, const void* data, uintptr_t a4) {
    if (g_hangar[kDiagnostics].on && request && AtcBudget()) LogNotify(request);
    g_notifyPlayerOrig(atc, request, data, a4);
}

static void LogNotifyClient(const uint8_t* request) {
    __try {
        const uintptr_t r = reinterpret_cast<uintptr_t>(request);
        Log("[atc] reply reached the client half: mode %u notification %u player 0x%llX vehicle 0x%llX", Rd<uint32_t>(r + 0x18), Rd<uint32_t>(r + 0x1C),
            static_cast<unsigned long long>(Rd<uint64_t>(r)), static_cast<unsigned long long>(Rd<uint64_t>(r + 8)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void __fastcall NotifyClientHook(uintptr_t atc, uintptr_t context, const uint8_t* request, const void* data, uintptr_t loc, uintptr_t a6) {
    if (g_hangar[kDiagnostics].on && request && AtcBudget()) LogNotifyClient(request);
    g_notifyClientOrig(atc, context, request, data, loc, a6);
}

static void LogPermissions(const uint8_t* result) {
    __try {
        const uintptr_t r = reinterpret_cast<uintptr_t>(result);
        Log("[iim] hangar permissions: success %u, %llu record(s) from the services", result[0],
            static_cast<unsigned long long>((Rd<uintptr_t>(r + 0x10) - Rd<uintptr_t>(r + 0x08)) / 0x18));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void __fastcall PermissionsContinuationHook(uintptr_t lambda, const uint8_t* result, uintptr_t a3, uintptr_t a4) {
    if (g_hangar[kDiagnostics].on && result && IimBudget()) LogPermissions(result);
    g_permissionsContinuationOrig(lambda, result, a3, a4);
}

// ---- install -----------------------------------------------------------------------------------

static bool EnsureHangarHook(const char* row, size_t stolen, void* detour, void** original) {
    uint8_t* target = sco::Sig(row);
    if (!target) return false;
    if (sco::hook::IsHooked(target)) return *original != nullptr;
    return HookFunction(target, stolen, detour, original);
}

static void HangarInstalled(HangarFeature f, bool ok) {
    if (!ok && g_hangar[f].on) HangarOff(f, "a hook couldn't be installed (see the [!] detour line above)");
}

void ResolveHangarsApi() {
    g_hangarResolved = true;
    // The hangar request with the stand-in hub: what sc-offline did before the ASOP work, kept with
    // asop = off too. hangar.request_instance is the function of the string's lea references, with
    // its prologue pinned (10 bytes moved: whole instructions, nothing RIP-relative).
    uint8_t* fn = sco::Sig("hangar.request_instance");
    if (!PrepareStandIn() || !fn
        || !EnsureHangarHook("hangar.request_instance", 10, reinterpret_cast<void*>(&RequestInstanceHook), reinterpret_cast<void**>(&g_requestInstanceOrig)))
        Log("[!] single player: hangar request not found (ATC can't give you a hangar)");
    if (!AsopEnabled()) return;

    for (HangarFeatureState& f : g_hangar) f.on = AsopCapabilityRows(f.cap, f.why, sizeof(f.why));
    if (g_hangar[kInstance].on && !g_requestInstanceOrig) HangarOff(kInstance, "the hangar request hook isn't installed");
    if (g_hangar[kInstance].on) {
        g_iimEntitySystem = reinterpret_cast<uintptr_t*>(sco::Sig("hangar.entity_system"));
        g_iimTypeWord = reinterpret_cast<const uint16_t*>(sco::Sig("hangar.iim_type_id"));
        g_queueInstance = reinterpret_cast<QueueInstanceFn>(sco::Sig("hangar.queue_instance"));
        g_bubbleFind = reinterpret_cast<BubbleFindFn>(sco::Sig("hangar.bubble_map_find"));
        HangarInstalled(kInstance,
            EnsureHangarHook("hangar.request_permissions", 5, reinterpret_cast<void*>(&PermissionsHook), reinterpret_cast<void**>(&g_permissionsOrig))
            && EnsureHangarHook("hangar.iim_find_hangars", 5, reinterpret_cast<void*>(&IimFindHook), reinterpret_cast<void**>(&g_iimFindOrig))
            && EnsureHangarHook("hangar.iim_unstow_hangar", 5, reinterpret_cast<void*>(&IimUnstowHook), reinterpret_cast<void**>(&g_iimUnstowOrig))
            && EnsureHangarHook("hangar.iim_creation_batch", 5, reinterpret_cast<void*>(&IimCreationHook), reinterpret_cast<void**>(&g_iimCreationOrig))
            && EnsureHangarHook("hangar.bubble_update_instance", 5, reinterpret_cast<void*>(&BubbleUpdateHook), reinterpret_cast<void**>(&g_bubbleUpdateOrig))
            && EnsureHangarHook("hangar.teardown_conditions_met", 5, reinterpret_cast<void*>(&TearDownHook), reinterpret_cast<void**>(&g_tearDownOrig)));
    }
    if (g_hangar[kTokens].on) {
        g_onReservingChanged = reinterpret_cast<OnReservingFn>(sco::Sig("landing.on_reserving_entity_changed"));
        HangarInstalled(kTokens,
            EnsureHangarHook("atc.change_token_state", 15, reinterpret_cast<void*>(&ChangeTokenStateHook), reinterpret_cast<void**>(&g_changeTokenStateOrig))
            && EnsureHangarHook("atc.clear_token", 15, reinterpret_cast<void*>(&ClearTokenHook), reinterpret_cast<void**>(&g_clearTokenOrig)));
    }
    if (g_hangar[kDiagnostics].on)
        HangarInstalled(kDiagnostics,
            EnsureHangarHook("atc.notify_player", 6, reinterpret_cast<void*>(&NotifyPlayerHook), reinterpret_cast<void**>(&g_notifyPlayerOrig))
            && EnsureHangarHook("atc.rm_multicast_notify_player", 5, reinterpret_cast<void*>(&NotifyClientHook), reinterpret_cast<void**>(&g_notifyClientOrig))
            && EnsureHangarHook("hangar.permissions_continuation", 5, reinterpret_cast<void*>(&PermissionsContinuationHook),
                                reinterpret_cast<void**>(&g_permissionsContinuationOrig)));
}

void LogHangars() {
    if (!g_hangarResolved || !AsopEnabled()) return;   // fleet.cpp's LogFleet says when ASOP is off
    for (const HangarFeatureState& f : g_hangar)
        if (f.on) Log("[+] %s: ready (%s)", f.title, f.ready);
        else      Log("[!] %s: %s", f.title, f.why);
}

void SetHangarCaps() {
    for (const HangarFeatureState& f : g_hangar) {
        const char* why = !g_hangarResolved ? "the game's addresses weren't resolved (teleport unavailable)"
                        : !AsopEnabled()    ? "asop = off in sc-offline.ini"
                                            : f.why;
        const sco::Result r = sco::caps::Set(f.cap, g_hangarResolved && AsopEnabled() && f.on, why);
        if (r != sco::Result::Ok) Log("[!] capability %s: %s", f.cap, sco::ResultName(r));
    }
}

// Every window message (doc 13): a loading thread may be waiting for RunHandedOffIim.
void ProcessHangars(DWORD now) {
    if (!g_hangar[kInstance].on && !g_hangar[kTokens].on) return;
    if (!g_gameMainThread) g_gameMainThread = GetCurrentThreadId();
    static DWORD last = 0;
    if (g_hangar[kInstance].on && now - last >= 500) {
        last = now;
        RunInstanceQueue(now);
        WatchKeptHangars();
    }
    if (g_hangar[kTokens].on) RunLandingUi();
    if (g_hangar[kInstance].on) RunHandedOffIim();
}
