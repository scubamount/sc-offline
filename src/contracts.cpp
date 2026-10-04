#include "contracts.h"
#include "missions.h"
#include "spawner.h"
#include "teleport.h"
#include "hooks.h"
#include "menu.h"
#include "build.h"
#include "npc.h"
#include <intrin.h>
#include <share.h>
#include <cmath>

using QueryReplyFn    = char(__fastcall*)(const void* capture, const void* reply);
using AcceptFn        = void*(__fastcall*)(uintptr_t broker, void* out, const uint8_t* request);
using FactoryCreateFn = void*(__fastcall*)(uintptr_t factory, void* future, void* request, void* shardGraph);
using GetShardGraphFn = void*(__fastcall*)(uintptr_t store, uintptr_t* graph, const char* shard);
using UrnFromEntityFn = void*(__fastcall*)(void* urn, uint64_t entityId);
using AssignUrnFn     = void*(__fastcall*)(void* dst, const void* src);
using StringInitFn    = void*(__fastcall*)(void* str);
using ResolveFn       = void*(__fastcall*)(void* future, const void* result);

static uintptr_t*      g_missionSystem = nullptr;
static size_t          g_generatorOff  = 0x70;
static QueryReplyFn    g_queryReply    = nullptr;
static AcceptFn        g_acceptStub    = nullptr;
static DWORD           g_autoAccept[4];
static int             g_autoAcceptCount = 0;
static FactoryCreateFn g_factoryCreate = nullptr;
static GetShardGraphFn g_getShardGraph = nullptr;
static uintptr_t*      g_shardStoreOwner = nullptr;
static int32_t         g_shardStoreOff = 0;
static UrnFromEntityFn g_urnFromEntity = nullptr;
static AssignUrnFn     g_assignUrn = nullptr;
static StringInitFn    g_stringInit = nullptr;
static ResolveFn       g_resolve = nullptr;
static uintptr_t       g_offlineServiceVtbl = 0;
using AddMissionFn = void(__fastcall*)(uintptr_t log, const void* missionId, const void* details, int, bool, bool);
using AddPlayerFn  = void(__fastcall*)(uintptr_t log, const void* missionId, uint64_t player);
static AddMissionFn    g_addMission = nullptr;
static AddPlayerFn     g_addPlayer = nullptr;
using LogHandleFn = uint64_t*(__fastcall*)(uintptr_t object, uint64_t* out);
using MakePhaseHandlerFn = void(__fastcall*)(uintptr_t entry, uint64_t logHandle);
static LogHandleFn        g_logHandle = nullptr;
static MakePhaseHandlerFn g_makePhaseHandler = nullptr;
using PhaseActivateFn = void(__fastcall*)(uintptr_t handler, uintptr_t tokens, const uint8_t* token, const int32_t* update,
                                          const uint8_t* guid, bool flag);
static PhaseActivateFn    g_phaseActivate = nullptr;
using HaulingAssignFn = void*(__fastcall*)(uintptr_t dst, uintptr_t src);
static HaulingAssignFn    g_haulingAssign = nullptr;
using CopyDetailsFn = void*(__fastcall*)(void* dst, const void* src);
static CopyDetailsFn   g_copyDetails = nullptr;
using CopyMapFn = void*(__fastcall*)(void* dst, const void* src);
static CopyMapFn       g_copyPropertyMap = nullptr;
static DWORD           g_mainThread = 0;

constexpr size_t kContractMap = 0x9F0;

static const uint8_t* FindLeaTo(const Section& text, const uint8_t* target, const uint8_t* from = nullptr) {
    const uint8_t* const end = text.base + text.size - 7;
    for (const uint8_t* p = from ? from : text.base + 1; target && p < end; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, 0x8D, static_cast<size_t>(end - p)));
        if (!p) break;
        if ((p[-1] & 0xFB) == 0x48 && (p[1] & 0xC7) == 0x05 && p + 6 + Rel32(p + 2) == target) return p - 1;
    }
    return nullptr;
}

static PRUNTIME_FUNCTION FunctionOf(const void* p, DWORD64& base) {
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(p), &base, nullptr);
    for (int i = 0; rf && i < 8; ++i) {
        const uint8_t* info = reinterpret_cast<const uint8_t*>(base + rf->UnwindData);
        if (!((info[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(info + 4 + ((info[2] + 1) & ~1) * 2));
    }
    return rf;
}

static uint8_t* FunctionStart(const void* p) {
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = p ? FunctionOf(p, base) : nullptr;
    return rf ? reinterpret_cast<uint8_t*>(base + rf->BeginAddress) : nullptr;
}

static void FindMissionSystem(const Section& text, uint8_t* fn) {
    uint8_t* end = fn + 0x1000;
    if (end > text.base + text.size - 13) end = text.base + text.size - 13;
    for (uint8_t* p = fn; p < end; ++p) {
        if (p[0] != 0xE8 || !BytesMatch(p + 5, "48 8B C8 E8")) continue;
        const uint8_t* get = p + 5 + Rel32(p + 1);
        const uint8_t* field = p + 13 + Rel32(p + 9);
        if (!BytesMatch(get, "48 8B 05 ?? ?? ?? ?? C3")) continue;
        g_missionSystem = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(get + 7 + Rel32(get + 3)));
        if (BytesMatch(field, "48 8B 41 ?? C3")) {
            g_generatorOff = field[3];
            return;
        }
    }
}

static const uint8_t* FindInRange(const uint8_t* from, size_t len, const char* pattern) {
    for (const uint8_t* p = from; p < from + len; ++p)
        if (BytesMatch(p, pattern)) return p;
    return nullptr;
}

template <typename Fn> static Fn CallTarget(const uint8_t* call) {
    return reinterpret_cast<Fn>(const_cast<uint8_t*>(call + 5 + Rel32(call + 1)));
}

static void FindOfflineMissionService(const Section& text, const Section& rdata) {
    const uint8_t* fn = FunctionStart(FindLeaTo(text, FindCString(rdata,
        "CMissionServiceOffline::RequestEndHaulingObjectiveAndPhase is not implemented yet")));
    if (!fn) return;
    auto* q = reinterpret_cast<uintptr_t*>(rdata.base);
    for (size_t i = 0x18; i < rdata.size / 8; ++i)
        if (q[i] == reinterpret_cast<uintptr_t>(fn)) { g_offlineServiceVtbl = reinterpret_cast<uintptr_t>(q + i - 0x18); return; }
}

static void FindMissionLogCalls(const Section& text, const Section& rdata) {
    uint8_t* fn = FunctionStart(FindLeaTo(text, FindCString(rdata,
        "void __cdecl CSCPlayerMissionLog::AddMission(const struct CryGUID &,const struct SMissionEntryDetails &,int,bool,const bool)")));
    if (!fn || !BytesMatch(fn, "44 89 4C 24 20 4C 89 44 24 18")) return;
    g_addMission = reinterpret_cast<AddMissionFn>(fn);
    if (const uint8_t* p = FindInRange(fn, 0x800, "48 8B 01 FF 50 60 48 39 18 75 1A 48 8D 95 ?? ?? ?? ?? 49 8B CE E8 ?? ?? ?? ?? 48 8B CE 48 8B 10 E8")) {
        g_logHandle = CallTarget<LogHandleFn>(p + 0x15);
        g_makePhaseHandler = CallTarget<MakePhaseHandlerFn>(p + 0x20);
    }
    int found = 0;
    if (const uint8_t* p = FindUniquePattern(text,
            "49 8B 8D 48 02 00 00 4C 8D 43 20 48 8D 44 24 30 44 88 64 24 28 4D 8D 48 10 48 89 44 24 20 48 8B D5 E8", found)) {
        const uint8_t* activate = CallTarget<const uint8_t*>(p + 33);
        if (BytesMatch(activate, "41 56 48 83 EC 30 41 83 79 04 01"))
            g_phaseActivate = reinterpret_cast<PhaseActivateFn>(const_cast<uint8_t*>(activate));
    }
    if (const uint8_t* p = FindUniquePattern(text, "38 42 68 0F 85 ?? ?? ?? ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 85 FF 0F 84", found)) {
        const uint8_t* assign = CallTarget<const uint8_t*>(p + 13);
        if (BytesMatch(assign, "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8"))
            g_haulingAssign = reinterpret_cast<HaulingAssignFn>(const_cast<uint8_t*>(assign));
    }
    if (const uint8_t* p = FindInRange(fn, 0x400, "48 8B D6 48 8D 4C 24 ?? 48 89 44 24 ?? C5 F8 11 44 24 ?? E8")) {
        const uint8_t* copy = CallTarget<const uint8_t*>(p + 19);
        if (BytesMatch(copy, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56"))
            g_copyDetails = reinterpret_cast<CopyDetailsFn>(const_cast<uint8_t*>(copy));
    }
    uint8_t* const end = text.base + text.size - 0x30;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0xE8, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p + 5 + Rel32(p + 1) != fn) continue;
        for (uint8_t* q = p + 5; q < p + 0x20; ++q) {
            if (q[0] != 0xE8) continue;
            bool leaRdx80 = false;
            for (uint8_t* r = p + 5; r + 7 <= q && !leaRdx80; ++r) leaRdx80 = BytesMatch(r + 1, "8D ?? 80 00 00 00") && (r[0] & 0xF8) == 0x48;
            if (leaRdx80) { g_addPlayer = CallTarget<AddPlayerFn>(q); return; }
            break;
        }
    }
}

using AddObjectiveFn    = void(__fastcall*)(uintptr_t log, const uint8_t* objective, bool);
using AddActivePlayerFn = void(__fastcall*)(uintptr_t missionEntity, uint64_t player, bool);
static AddObjectiveFn    g_addObjectiveOrig = nullptr;
static AddActivePlayerFn g_addActivePlayerOrig = nullptr;

struct ObjectiveNote { const char* id; const void* parent; uint32_t flags, shortText, longText; int64_t timer; float progress; };
using NotifyObjectiveFn = void(__fastcall*)(uintptr_t log, int event, const uint8_t* mission, uint32_t text, bool silent, void* id,
                                            int value, bool timed);
using ObjectiveHiddenFn = bool(__fastcall*)(uintptr_t log, const uint8_t* mission, const void* parent, bool, bool);
static NotifyObjectiveFn g_notifyObjective = nullptr;
static NotifyObjectiveFn g_notifyObjectiveOrig = nullptr;
static ObjectiveHiddenFn g_objectiveHidden = nullptr;
static thread_local int  t_gameNotifies = 0;
static thread_local bool t_ourNotify = false;
struct NotedObjective { uint8_t mission[16]; uint32_t idHash; DWORD at; };
static NotedObjective g_notedObjectives[32];
static int            g_notedObjectiveNext = 0;

static uint32_t HashText(const char* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
    return h;
}

static bool AlreadyNotified(const uint8_t* mission, const char* id) {
    const uint32_t h = HashText(id);
    const DWORD now = GetTickCount();
    for (const NotedObjective& n : g_notedObjectives)
        if (n.at && now - n.at < 60000 && n.idHash == h && !memcmp(n.mission, mission, 16)) return true;
    return false;
}

static void __fastcall NotifyObjectiveHook(uintptr_t log, int event, const uint8_t* mission, uint32_t text, bool silent, void* id,
                                           int value, bool timed) {
    if (!t_ourNotify) {
        ++t_gameNotifies;
        const char* s = nullptr;
        __try { s = id ? *static_cast<const char* const*>(id) : nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (event == 0 && mission && AlreadyNotified(mission, s)) { FreeCryString(id); return; }
    }
    g_notifyObjectiveOrig(log, event, mission, text, silent, id, value, timed);
}

static void NotifyObjectiveAdded(uintptr_t log, const uint8_t mission[16], const ObjectiveNote& n);

using ProcessQueueFn = char(__fastcall*)(uintptr_t handler);
static ProcessQueueFn    g_processQueueOrig = nullptr;
static thread_local int  t_inProcessQueue = 0;

static char __fastcall ProcessQueueHook(uintptr_t handler) {
    if (t_inProcessQueue) return 0;
    ++t_inProcessQueue;
    const char r = g_processQueueOrig(handler);
    --t_inProcessQueue;
    return r;
}

static void __fastcall AddObjectiveHook(uintptr_t log, const uint8_t* objective, bool flag) {
    __try {
        const char* id = *reinterpret_cast<const char* const*>(objective + 8);
        const uint32_t* m = reinterpret_cast<const uint32_t*>(objective + 0x18);
        Log("[contracts] objective '%s' -> a mission log (mission %08x%08x%08x%08x)", id ? id : "?", m[0], m[1], m[2], m[3]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const int notified = t_gameNotifies;
    g_addObjectiveOrig(log, objective, flag);
    if (t_gameNotifies == notified) {
        ObjectiveNote n{};
        __try {
            n.id = Rd<const char*>(reinterpret_cast<uintptr_t>(objective) + 8);
            n.parent = objective + 0x10;
            n.flags = Rd<uint32_t>(reinterpret_cast<uintptr_t>(objective) + 0x74);
            n.shortText = Rd<uint32_t>(reinterpret_cast<uintptr_t>(objective) + 0x28);
            n.longText = Rd<uint32_t>(reinterpret_cast<uintptr_t>(objective) + 0x2C);
            n.timer = Rd<int64_t>(reinterpret_cast<uintptr_t>(objective) + 0xB8);
            n.progress = Rd<float>(reinterpret_cast<uintptr_t>(objective) + 0xC8);
        } __except (EXCEPTION_EXECUTE_HANDLER) { n.id = nullptr; }
        NotifyObjectiveAdded(log, objective + 0x18, n);
    }
}

static void __fastcall AddActivePlayerHook(uintptr_t missionEntity, uint64_t player, bool flag) {
    Log("[contracts] mission entity %p takes on player %llu", reinterpret_cast<void*>(missionEntity), player);
    g_addActivePlayerOrig(missionEntity, player, flag);
}

static AddActivePlayerFn g_addActivePlayer = nullptr;

static void HookMissionDiagnostics(const Section& text, const Section& rdata) {
    uint8_t* fn = FunctionStart(FindLeaTo(text, FindCString(rdata,
        "void __cdecl CSCPlayerMissionLog::AddActiveObjective(const struct ActiveMissionObjective &,const bool)")));
    const bool objective = fn && BytesMatch(fn, "44 88 44 24 18 48 89 54 24 10 48 89 4C 24 08 55")
        && HookFunction(fn, 15, reinterpret_cast<void*>(&AddObjectiveHook), reinterpret_cast<void**>(&g_addObjectiveOrig));
    fn = FunctionStart(FindLeaTo(text, FindCString(rdata, "void __cdecl CMissionEntity::AddActivePlayer(class EntityId,bool)")));
    const bool player = fn && BytesMatch(fn, "44 88 44 24 18 48 89 54 24 10 55 53 56 57")
        && HookFunction(fn, 11, reinterpret_cast<void*>(&AddActivePlayerHook), reinterpret_cast<void**>(&g_addActivePlayerOrig));
    if (player) g_addActivePlayer = reinterpret_cast<AddActivePlayerFn>(fn);
    fn = FunctionStart(FindLeaTo(text, FindCString(rdata, "Notify UI Objective")));
    if (fn && BytesMatch(fn, "44 89 4C 24 20 89 54 24 10 55 53 56 41 54")
        && HookFunction(fn, 5, reinterpret_cast<void*>(&NotifyObjectiveHook), reinterpret_cast<void**>(&g_notifyObjectiveOrig)))
        g_notifyObjective = reinterpret_cast<NotifyObjectiveFn>(fn);
    fn = FunctionStart(FindLeaTo(text, FindCString(rdata,
        "bool __cdecl CSCPlayerMissionLog::IsObjectiveHidden(const struct CryGUID &,const class CryStringT<char> &,bool,bool) const")));
    if (fn && BytesMatch(fn, "48 8B C4 44 88 48 20 48 89 48 08 55 53 56 41 57"))
        g_objectiveHidden = reinterpret_cast<ObjectiveHiddenFn>(fn);
    fn = FunctionStart(FindLeaTo(text, FindCString(rdata, "void __cdecl CMissionWarehouseOrderHandler::ProcessQueue(void)")));
    const bool queue = fn && BytesMatch(fn, "48 8B C4 55 41 54 48 8D A8")
        && HookFunction(fn, 6, reinterpret_cast<void*>(&ProcessQueueHook), reinterpret_cast<void**>(&g_processQueueOrig));
    Log("[contracts] mission diagnostics: objective hook %s, active player hook %s, UI objective notice %s (hidden check %s), "
        "warehouse order queue %s", objective ? "ok" : "MISSING", player ? "ok" : "MISSING", g_notifyObjective ? "ok" : "MISSING",
        g_objectiveHidden ? "ok" : "MISSING", queue ? "guarded" : "MISSING");
}

using CreateLogEntryFn = void*(__fastcall*)(uintptr_t entity, const uint8_t* details, bool);
static CreateLogEntryFn g_createLogEntryOrig = nullptr;
struct Joining { uint8_t mission[16]; uintptr_t entity; };
constexpr int  kMaxOurs = 16;
static uint8_t  g_ours[kMaxOurs][16];
static uint64_t g_oursPlayer[kMaxOurs];
static uint8_t* g_oursDetails[kMaxOurs];
static int      g_oursNext = 0;
static Joining  g_joining[kMaxOurs];
static int      g_joiningCount = 0;
static SRWLOCK  g_joinLock = SRWLOCK_INIT;

static void NoteOurMission(const uint8_t* mission, uint64_t player, uint8_t* details) {
    AcquireSRWLockExclusive(&g_joinLock);
    memcpy(g_ours[g_oursNext], mission, 16);
    g_oursPlayer[g_oursNext] = player;
    g_oursDetails[g_oursNext] = details;
    g_oursNext = (g_oursNext + 1) % kMaxOurs;
    ReleaseSRWLockExclusive(&g_joinLock);
}

static uint8_t* MissionDetails(const uint8_t* mission) {
    uint8_t* details = nullptr;
    AcquireSRWLockShared(&g_joinLock);
    for (int i = 0; i < kMaxOurs && !details; ++i)
        if (!memcmp(g_ours[i], mission, 16)) details = g_oursDetails[i];
    ReleaseSRWLockShared(&g_joinLock);
    return details;
}

static uint64_t MissionPlayer(const uint8_t* mission) {
    uint64_t player = 0;
    AcquireSRWLockShared(&g_joinLock);
    for (int i = 0; i < kMaxOurs && !player; ++i)
        if (!memcmp(g_ours[i], mission, 16)) player = g_oursPlayer[i];
    ReleaseSRWLockShared(&g_joinLock);
    return player ? player : LocalPlayerEntityId();
}

static void* __fastcall CreateLogEntryHook(uintptr_t entity, const uint8_t* details, bool flag) {
    void* const result = g_createLogEntryOrig(entity, details, flag);
    uint8_t mission[16] = {};
    __try { memcpy(mission, details + 0x80, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { return result; }
    static const uint8_t none[16] = {};
    if (!memcmp(mission, none, 16)) return result;
    bool ours = false;
    AcquireSRWLockExclusive(&g_joinLock);
    for (int i = 0; i < kMaxOurs && !ours; ++i) ours = !memcmp(g_ours[i], mission, 16);
    if (ours && g_joiningCount < kMaxOurs) {
        memcpy(g_joining[g_joiningCount].mission, mission, 16);
        g_joining[g_joiningCount++].entity = entity;
    }
    ReleaseSRWLockExclusive(&g_joinLock);
    const uint32_t* m = reinterpret_cast<const uint32_t*>(mission);
    Log("[contracts] mission entity %p gets its details: mission %08x%08x%08x%08x%s", reinterpret_cast<void*>(entity),
        m[0], m[1], m[2], m[3], ours ? " (ours)" : "");
    return result;
}

static void HookMissionEntity(const Section& text, const Section& rdata) {
    const uint8_t* name = FindCString(rdata, "CMissionEntity::CreateMissionLogEntry");
    uint8_t* fn = nullptr;
    for (const uint8_t* p = FindLeaTo(text, name); p && !fn; p = FindLeaTo(text, name, p + 1)) {
        uint8_t* f = FunctionStart(p);
        if (f && BytesMatch(f, "48 8B C4 44 88 40 18 48 89 50 10 48 89 48 08 55 53 41 54")) fn = f;
    }
    const bool ok = fn && HookFunction(fn, 15, reinterpret_cast<void*>(&CreateLogEntryHook), reinterpret_cast<void**>(&g_createLogEntryOrig));
    Log("[contracts] mission entity hook %s, add player %s", ok ? "ok" : "MISSING", g_addActivePlayer ? "ok" : "MISSING");
}

using ModuleInitFn   = void(__fastcall*)(uintptr_t module);
using AuthorityFn    = void(__fastcall*)(uintptr_t module, const uint8_t* event, uintptr_t, uintptr_t, uintptr_t);
using EntryAnswerFn  = void(__fastcall*)(const uintptr_t* capture, const uint8_t* result);
using ToPlayerLogsFn = void(__fastcall*)(uintptr_t missionEntity, const uint8_t* objective);
using StartMissionFn = void(__fastcall*)(uintptr_t module, int reason);
static ModuleInitFn   g_moduleInit = nullptr;
static ModuleInitFn   g_moduleInitOrig = nullptr;
static AuthorityFn    g_authorityOrig = nullptr;
static EntryAnswerFn  g_entryAnswerOrig = nullptr;
static ToPlayerLogsFn g_toPlayerLogsOrig = nullptr;
static StartMissionFn g_startMission = nullptr;
static uint8_t*       g_createObjective = nullptr;
using LocIdFn = void(__fastcall*)(uint32_t* id, const char* key);
static LocIdFn        g_locId = nullptr;
constexpr size_t kModuleState = 0x130, kModuleMission = 0x150;
constexpr size_t kModuleHasInstance = 0x1D0, kModuleInstanceFailed = 0x528, kCreateInstanceSlot = 0x6C8;
constexpr DWORD  kStartAfterMs = 2500, kAnswerWaitMs = 2000, kWatchMs = 30000;
struct Module { uintptr_t module; DWORD since, preparedAt; bool authority, done, seen, prepared; };
constexpr int  kMaxModules = 256;
static Module  g_modules[kMaxModules];
static int     g_moduleNext = 0;
static SRWLOCK g_moduleLock = SRWLOCK_INIT;

static bool IsOurMission(const uint8_t* id) {
    static const uint8_t none[16] = {};
    if (!memcmp(id, none, 16)) return false;
    bool ours = false;
    AcquireSRWLockShared(&g_joinLock);
    for (int i = 0; i < kMaxOurs && !ours; ++i) ours = !memcmp(g_ours[i], id, 16);
    ReleaseSRWLockShared(&g_joinLock);
    return ours;
}

static bool ModuleMission(uintptr_t module, uint8_t id[16]) {
    __try {
        if (Rd<uintptr_t>(Rd<uintptr_t>(module) + 0x6C0) != reinterpret_cast<uintptr_t>(g_moduleInit)) return false;
        memcpy(id, reinterpret_cast<const uint8_t*>(module + kModuleMission), 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int ModuleState(uintptr_t module) {
    __try { return Rd<int>(module + kModuleState); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static void __fastcall ModuleInitHook(uintptr_t module) {
    g_moduleInitOrig(module);
    AcquireSRWLockExclusive(&g_moduleLock);
    g_modules[g_moduleNext] = { module, GetTickCount(), 0, false, false, false, false };
    g_moduleNext = (g_moduleNext + 1) % kMaxModules;
    ReleaseSRWLockExclusive(&g_moduleLock);
}

static void __fastcall AuthorityHook(uintptr_t module, const uint8_t* event, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
    uint8_t id[16];
    if (ModuleMission(module, id) && IsOurMission(id)) {
        AcquireSRWLockExclusive(&g_moduleLock);
        for (Module& m : g_modules) if (m.module == module) m.authority = true;
        ReleaseSRWLockExclusive(&g_moduleLock);
        bool gained = false;
        __try { gained = event && *event; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("[contracts] our mission module %p %s authority (state %d)", reinterpret_cast<void*>(module), gained ? "gained" : "lost", ModuleState(module));
    }
    g_authorityOrig(module, event, a3, a4, a5);
}

static void __fastcall EntryAnswerHook(const uintptr_t* capture, const uint8_t* result) {
    uintptr_t module = 0;
    uint8_t id[16];
    __try { module = capture ? capture[0] : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const bool ours = module && ModuleMission(module, id) && IsOurMission(id);
    const int before = ours ? ModuleState(module) : 0;
    g_entryAnswerOrig(capture, result);
    if (ours) {
        bool ok = false;
        __try { ok = result && *result == 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("[contracts] mission service answered our mission module %p: %s, state %d -> %d", reinterpret_cast<void*>(module),
            ok ? "found" : "NOT found", before, ModuleState(module));
    }
}

static void __fastcall ToPlayerLogsHook(uintptr_t missionEntity, const uint8_t* objective) {
    uint8_t id[16] = {};
    const char* name = nullptr;
    __try { memcpy(id, objective + 0x18, 16); name = *reinterpret_cast<const char* const*>(objective + 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (IsOurMission(id)) Log("[contracts] our mission's objective '%s' goes to its players' mission logs", name ? name : "?");
    g_toPlayerLogsOrig(missionEntity, objective);
}

static uint8_t* FunctionNaming(const Section& text, const Section& rdata, const char* name, const char* prologue) {
    const uint8_t* s = FindCString(rdata, name);
    for (const uint8_t* p = FindLeaTo(text, s); p; p = FindLeaTo(text, s, p + 1)) {
        uint8_t* f = FunctionStart(p);
        if (f && BytesMatch(f, prologue)) return f;
    }
    return nullptr;
}

static void HookMissionModules(const Section& text, const Section& rdata) {
    uint8_t* init = FunctionNaming(text, rdata, "void __cdecl CSubsumptionMissionComponent::Initialize(void)", "48 89 4C 24 08 55 53 56 57");
    uint8_t* start = FunctionNaming(text, rdata, "CSubsumptionMissionComponent::StartMission", "48 8B C4 48 89 48 08 55 48 8D A8");
    const bool ok = init && start
        && HookFunction(init, 5, reinterpret_cast<void*>(&ModuleInitHook), reinterpret_cast<void**>(&g_moduleInitOrig));
    if (ok) { g_moduleInit = reinterpret_cast<ModuleInitFn>(init); g_startMission = reinterpret_cast<StartMissionFn>(start); }

    uint8_t* fn = FunctionNaming(text, rdata, "HandleAuthorityChangeEvent", "48 8B C4 48 89 50 10 48 89 48 08 55");
    const bool authority = fn && HookFunction(fn, 11, reinterpret_cast<void*>(&AuthorityHook), reinterpret_cast<void**>(&g_authorityOrig));
    fn = FunctionNaming(text, rdata, "Aborting subsumption mission module $$($$)", "40 55 53 56 41 56 48 8D AC 24 58 FF FF FF 48 81 EC A8 01 00 00");
    const bool answer = fn && HookFunction(fn, 6, reinterpret_cast<void*>(&EntryAnswerHook), reinterpret_cast<void**>(&g_entryAnswerOrig));
    fn = FunctionNaming(text, rdata, "AddActiveObjectiveToPlayerLogs", "40 55 53 56 57 48 8D 6C 24 C1");
    const bool logs = fn && HookFunction(fn, 10, reinterpret_cast<void*>(&ToPlayerLogsHook), reinterpret_cast<void**>(&g_toPlayerLogsOrig));
    g_createObjective = FunctionNaming(text, rdata, "$$[$$] - Created: $$[$$], parent id=$$, flags=$$", "44 88 4C 24 20 55 56 41 55");
    int n = 0;
    g_locId = reinterpret_cast<LocIdFn>(FindUniquePattern(text,
        "48 89 5C 24 10 56 48 83 EC 20 C7 01 00 00 00 00 48 8B DA 48 8B F1 48 85 D2 0F 84 ?? ?? ?? ?? 80 3A 00 0F 84", n));
    if (!g_locId) g_createObjective = nullptr;
    Log("[contracts] mission modules: start %s, authority %s, service answer %s, objectives to logs %s, create objective %s",
        ok ? "ok" : "MISSING", authority ? "ok" : "MISSING", answer ? "ok" : "MISSING", logs ? "ok" : "MISSING",
        g_createObjective ? "ok" : "MISSING");
}

constexpr float kMissionStreamRadius = 1.0e11f;
static uintptr_t* g_missionSettings = nullptr;

static void FindMissionStreamRadius(const Section& text) {
    int matches = 0;
    const uint8_t* p = FindUniquePattern(text,
        "48 8B 05 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? C5 FA 10 88 74 01 00 00 48 8B 01 C5 F2 5A C9 48 FF A0 20 03 00 00", matches);
    if (p) g_missionSettings = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(p + 7 + Rel32(p + 3)));
    Log("[contracts] mission stream radius %s", p ? "ok" : "MISSING");
}

static void WidenMissionStreamRadius() {
    const uintptr_t settings = g_missionSettings ? *g_missionSettings : 0;
    if (!settings) return;
    __try {
        float& radius = *reinterpret_cast<float*>(settings + 0x174);
        if (radius >= kMissionStreamRadius) return;
        Log("[contracts] mission entities stay loaded from anywhere (stream radius %.0f m -> %.0f m)", radius, kMissionStreamRadius);
        radius = kMissionStreamRadius;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

struct ObjectiveParams {
    uint32_t  title, description;
    uint32_t  type, order;
    uint32_t  a, b, c, flags;
    uint8_t   parent[16];
    uintptr_t id, subtitle;
    uint64_t  r40, r48, q50;
    uint8_t   b58, r59[7];
    uint64_t  q60;
};
static_assert(sizeof(ObjectiveParams) == 0x68, "CreateObjective params");
struct MarkerParams {
    const uintptr_t* objective;
    const void* entities;
    const char* label;
    double    pos[3];
    uint64_t  zoneHost;
    uint8_t   b38, b39, r3A[2];
    float     distance;
    int32_t   mode;
    uint8_t   hasPlayers, r45[3];
    const void* players;
    uint8_t   b50, r51[7];
    uint8_t   guid[16];
    float     offset[3];
    uint32_t  r74;
    uintptr_t text;
    uint8_t   b80, b81, r82[6];
};
static_assert(sizeof(MarkerParams) == 0x88, "SetObjectiveMarker params");
constexpr size_t kMasterMission = 0x1B8, kEntityMissionId = 0x2A0;

static bool ContainsNoCase(const char* s, const char* sub);

struct Plan { uint8_t mission[16]; const char* contract; bool done; DWORD builtAt; };
static Plan g_plans[kMaxOurs];
static int  g_planNext = 0;

static void NotePlan(const uint8_t mission[16], const char* contract) {
    Plan& p = g_plans[g_planNext];
    g_planNext = (g_planNext + 1) % kMaxOurs;
    memcpy(p.mission, mission, 16);
    p.contract = contract;
    p.done = false;
    p.builtAt = 0;
}

static void NoteBuilt(const uint8_t mission[16]) {
    for (Plan& p : g_plans)
        if (!memcmp(p.mission, mission, 16)) p.builtAt = GetTickCount() | 1;
}

enum class Act : uint8_t { None, Travel, Deliver, Fight, Search, Away };
struct Step { Act act; const char* id; const char* title; const char* text; };
struct Flow { const char* name; const char* kinds; Step steps[2]; };
static const Flow kFlows[] = {
    { "salvage", "Salvage",
      { { Act::Travel, "GoToLocation", "@TravelObjective_Marker_1", "@TravelObjective_Marker_1" },
        { Act::Search, "Salvage", "@FPSSalvage_obj_marker_01", "@BasicSalvage_obj_marker_01" } } },
    { "mining", "Mine|Mining|Rockcracker|ScanRocks|ResourceGathering",
      { { Act::Travel, "GoToLocation", "@TravelObjective_Marker_1", "@TravelObjective_Marker_1" },
        { Act::Search, "MineResources", "@mg_battaglia_mining_objective_short_001", "@mg_battaglia_mining_objective_short_001" } } },
    { "recovery", "Recover|BlackBox|RetrieveCargo|MissingPerson|Rescue",
      { { Act::Travel, "GoToLocation", "@TravelObjective_Marker_1", "@TravelObjective_Marker_1" },
        { Act::Search, "RecoverCargo", "@mg_battaglia_recover_cargo_obj_short_02", "@mg_battaglia_recover_cargo_obj_long_02" } } },
    { "delivery", "HaulCargo|AtoB|ToMulti|ToSingle|Supply|Bulk|Courier|Delivery|Hauling|Covalex|Transport",
      { { Act::Travel, "PickUpShipment", "@delivery_obj_short_02", "@delivery_obj_long_02" },
        { Act::Deliver, "DeliverShipment", "@delivery_obj_short_03", "@HaulCargo_obj_short_02" } } },
    { "race", "Race",
      { { Act::Travel, "Race", "@missionManager_DeathRace_Finish", "@MissionManager_GrimHex_Race_Checkpoint_Long" }, {} } },
    { "combat", "Kill|Eliminate|Bounty|Criminal|HeadHunter|Certification|Defend|Escort|Patrol|Ambush|Bombing|Wave|Hijack|"
                "Mercenary|FacilityDelve|Destroy|Disable|BoardShip|ASD|FPS|Assassin|Sweep",
      { { Act::Travel, "GoToLocation", "@TravelObjective_Marker_1", "@outlawsweep_obj_long_01" },
        { Act::Fight, "EliminateHostiles", "@basesweep_obj_marker_01", "@sectorsweep_obj_long_02" } } },
    { "job", "",
      { { Act::Travel, "GoToLocation", "@TravelObjective_Marker_1", "@TravelObjective_Marker_1" },
        { Act::Search, "SearchArea", "@SP_InvisibleTimer_Obj", "@SP_InvisibleTimer_Long" } } },
};

static const Flow kAwayFlows[] = {
    { "mine and deliver", "Mine|Mining|Rockcracker|ScanRocks|ResourceGathering|Salvage",
      { { Act::Away, "MineResources", "@mg_battaglia_mining_objective_short_001", "@mg_battaglia_mining_objective_short_001" },
        { Act::Deliver, "DeliverShipment", "@delivery_obj_short_03", "@HaulCargo_obj_short_02" } } },
    { "out and back", "",
      { { Act::Away, "CollectShipment", "@HaulCargo_obj_short_01", "@delivery_obj_long_02" },
        { Act::Deliver, "DeliverShipment", "@delivery_obj_short_03", "@HaulCargo_obj_short_02" } } },
};

static int LogFault(const EXCEPTION_POINTERS* ep, const char* what);

static bool MatchesAny(const char* contract, const char* kinds) {
    char kind[48];
    for (const char* k = kinds; *k; ) {
        const size_t n = strcspn(k, "|");
        if (n && n < sizeof(kind)) {
            memcpy(kind, k, n);
            kind[n] = 0;
            if (ContainsNoCase(contract, kind)) return true;
        }
        k += n + (k[n] == '|');
    }
    return false;
}

static const Flow& FlowFor(const char* contract) {
    for (const Flow& f : kFlows)
        if (*f.kinds && contract && MatchesAny(contract, f.kinds)) return f;
    return kFlows[sizeof(kFlows) / sizeof(kFlows[0]) - 1];
}

struct StandIn { const char* kind; const char* missionId; const char* name; };
static const StandIn kStandIns[] = {
    { "KillShip",          "97953a13-55d5-4996-907e-9c6606979a62", "Pirate scout" },
    { "EliminateSpecific", "97953a13-55d5-4996-907e-9c6606979a62", "Pirate scout" },
    { "Criminals",         "97953a13-55d5-4996-907e-9c6606979a62", "Pirate scout" },
    { "Bounty",            "97953a13-55d5-4996-907e-9c6606979a62", "Pirate scout" },
    { "Defend",            "654df1da-26ec-4449-a62c-c6a4cc1749dc", "Combat assist (NPC asks for help)" },
    { "Escort",            "654df1da-26ec-4449-a62c-c6a4cc1749dc", "Combat assist (NPC asks for help)" },
    { "",                  "ae6044a5-48ed-46c5-9ccf-4dfd1aec2719", "Pirate blockade" },
};
static const char* const kGroundEnemies[] = {
    "PU_Human_Enemy_GroundCombat_NPC_Ninetails_grunt", "PU_Human_Enemy_GroundCombat_NPC_Ninetails_soldier",
    "PU_Human_Enemy_GroundCombat_NPC_GenericCriminal", "PU_Human_Enemy_GroundCombat_NPC_Ninetails_cqc",
    "PU_Human_Enemy_GroundCombat_NPC_Ninetails_sniper", "PU_Human_Enemy_GroundCombat_NPC_Grunt",
};
constexpr double kNearSpaceM = 5000.0, kNearGroundM = 600.0, kProbeM = 8000.0, kDecideSpaceM = 1500.0, kGroundProbeM = 250.0;
constexpr double kSearchSpaceM = 3000.0, kSearchGroundM = 250.0, kMinDropoffM = 2000.0, kAwayM = 20000.0, kHomeM = 2000.0;
constexpr DWORD  kSearchMs = 20000, kDetachedAfterMs = 8000, kOwnEntityWaitMs = 10000;
constexpr float  kHostilesMinM = 1500.0f, kHostilesMaxM = 3000.0f;

using EndMissionFn = void*(__fastcall*)(uintptr_t log, const uint8_t* mission, int state, int players, int reason,
                                        const char* text, uint32_t loc);
static EndMissionFn g_endMission = nullptr;
using EmFinishedFn = char(__fastcall*)(uintptr_t manager, const uint64_t* entity, uint32_t reason);
static EmFinishedFn g_emFinishedOrig = nullptr;
static volatile LONG g_emFinished = -1;
using ActorKillFn = void(__fastcall*)(uintptr_t actor, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);
static ActorKillFn g_actorKillOrig = nullptr;
static volatile LONG64 g_deaths[32];
static volatile LONG   g_deathNext = 0;
using FindEntryFn     = uintptr_t(__fastcall*)(uintptr_t log, const uint8_t* mission);
using TotalRewardFn   = int(__fastcall*)(uintptr_t entry);
using UpdateBalanceFn = char(__fastcall*)(uintptr_t wallet, uint8_t currency, int64_t delta);
static FindEntryFn     g_findEntry = nullptr;
static FindEntryFn     g_findEntryAny = nullptr;
static TotalRewardFn   g_totalReward = nullptr;
static UpdateBalanceFn g_updateBalance = nullptr;

static uintptr_t LogEntry(uintptr_t log, const uint8_t mission[16]) {
    if (g_findEntryAny) return g_findEntryAny(log, mission);
    return g_findEntry ? g_findEntry(log, mission) : 0;
}

constexpr int kMaxNpcs = 8, kDone = 99, kPending = -1;
struct Running {
    uint8_t mission[16];
    const char* contract;
    const Flow* flow;
    const StandIn* hostiles;
    uint64_t meId;
    uint64_t target;
    uint64_t dropoff;
    uint64_t homeZone; double home[3];
    int step;
    uintptr_t objective;
    int ground;
    bool started;
    DWORD since;
    uint64_t npcs[kMaxNpcs]; int npcCount;
};
static Running g_running[kMaxOurs];
static int     g_runningCount = 0;

using SendCommsFn = void(__fastcall*)(uintptr_t log, const uint8_t* record, const uint8_t* mission, float delay, bool flag);
static SendCommsFn g_sendComms = nullptr;
enum CommsEvent { kCommsAccept, kCommsArrive, kCommsHostiles, kCommsCleared };
struct GiverComms {
    const char* keys;
    const char* accept; const char* ambushAccept; const char* defendShipAccept; const char* defendAccept; const char* patrolAccept;
    const char* ambushArrive; const char* hostilesStart; const char* hostilesCleared; const char* defendShipStart; const char* defendShipCleared;
};
static const GiverComms kGiverComms[] = {
    { "Foxwell",
      "0d405ab0b48777f644b6d6b650ba7682", "5e4c695c3d70f79327847d53056f06a1", "464d1bfeb19f787b4c0b380edff237bf",
      "074550b1e5109e1f48276d23e58370ae", "2545e6ecbb2b42731c229bd28aba5caa", "014a790b7e9da4c29f40679d8e3fe4a1",
      "fa47a47ec5223c4b102e0583c800478d", "d84d0235ab1fc1f05a12cdcad89c6baf", "454944ef1826dbd38270d9320ccb46a0", "b04ef7bbc751198ec26618a1363f458d" },
    { "CFP|CitizensForProsperity",
      "b64fd13a618775520c217cfa2f8e19a5", "e1449c1712b7f7d1a4e5df681b8d0ba9", "d341f00c1bab41bdfe39ef65d027a4a0",
      "e6414fd7e2c995541f62865070e494bd", "8249d8e2bc746faae033052b0215b190", "65422ffb0727e4aa7c1d188c6d1ff1bb",
      "4b4ee6f02dafaf929ddde57e8b98f0a3", "cb47a5c4cb4a97b58da3bf1de81dd0b9", "17413538965ad28dd830eac956a98a98", "9849803d728e7b50e183c86a07275381" },
    { "HeadHunters",
      "e1496fc798b018c7c247a345a88a549d", "e241cd4b2e715e8b45a79bb6ce4b6dbb", "fb4a6ba6c016bfcdc35a34af48b52892",
      "e046bee69b6ab22eaed67c1b18cdaaa2", "5d43b9396faafa288a265616d21f89ae", "914bb1e68b709a10c8ac5d76ebeceeac",
      "e24b57135d59fad2af2f8246ed8d53a0", "ac420e2883254b1fed4adec409da7795", "8040682b917cbd1a1d5a11199b8b0a9f", "b6416a42c98c0b9bdd86daa0deea0998" },
    { "NorthRock",
      "7041d7e6ac8f2728bd200080254c3586", nullptr, nullptr, nullptr, nullptr, "d84ad5bf0bd8d84bfb82cbe8a19fd598",
      "bd402b0e817510ba1b1205b8458cf493", "634a5b1dc813b587650ba139545665b2", nullptr, nullptr },
    { "BountyHuntersGuild",
      "ba41112e072b7736b2b7a21be7020fb4", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
};

static bool MatchesAny(const char* contract, const char* kinds);

static const char* CommsRecord(const char* contract, CommsEvent event) {
    const GiverComms* g = nullptr;
    for (const GiverComms& c : kGiverComms)
        if (MatchesAny(contract, c.keys)) { g = &c; break; }
    if (!g) return nullptr;
    const bool ambush = ContainsNoCase(contract, "Ambush"), defendShip = ContainsNoCase(contract, "DefendShip");
    const bool defend = !defendShip && (ContainsNoCase(contract, "Defend") || ContainsNoCase(contract, "Escort"));
    switch (event) {
    case kCommsAccept:
        if (ContainsNoCase(contract, "BountyHuntersGuild")) {
            if (ContainsNoCase(contract, "Super") || ContainsNoCase(contract, "VeryHard")) return "dc4c362cb526bc0f505254d08f8213b0";
            if (ContainsNoCase(contract, "Hard")) return "794ec4e2e1e4f033f033a5c4f0e2f0a6";
            if (ContainsNoCase(contract, "Intro")) return "bd41e346559e35c428a13298ff5f85a2";
        }
        if (ambush && g->ambushAccept) return g->ambushAccept;
        if (defendShip && g->defendShipAccept) return g->defendShipAccept;
        if (defend && g->defendAccept) return g->defendAccept;
        if (ContainsNoCase(contract, "Patrol") && g->patrolAccept) return g->patrolAccept;
        return g->accept;
    case kCommsArrive:   return ambush ? g->ambushArrive : nullptr;
    case kCommsHostiles: return defendShip && g->defendShipStart ? g->defendShipStart : g->hostilesStart;
    case kCommsCleared:  return defendShip && g->defendShipCleared ? g->defendShipCleared : g->hostilesCleared;
    }
    return nullptr;
}

static uintptr_t EntityById(uint64_t id);

static void SendComms(const char* contract, const uint8_t mission[16], CommsEvent event, float delay = 0.0f) {
    static const char* const kNames[] = { "accept", "arrival", "hostiles inbound", "hostiles cleared" };
    const char* hex = g_sendComms && contract ? CommsRecord(contract, event) : nullptr;
    if (!hex || strlen(hex) != 32) return;
    uint8_t record[16];
    for (int i = 0; i < 16; ++i) {
        char byte[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        record[i] = static_cast<uint8_t>(strtoul(byte, nullptr, 16));
    }
    const uint64_t id = MissionPlayer(mission);
    const uintptr_t player = id ? EntityById(id) : 0;
    const uintptr_t log = player ? EntityComponent(player, "SCPlayerMissionLog") : 0;
    if (!log) return;
    g_sendComms(log, record, mission, delay, true);
    Log("[contracts] '%s': %s comms sent", contract, kNames[event]);
}

static char __fastcall EmFinishedHook(uintptr_t manager, const uint64_t* entity, uint32_t reason) {
    uint64_t id = 0;
    __try { id = entity ? *entity : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Log("[contracts] an environmental mission ended (entity %llu, reason %u)", static_cast<unsigned long long>(id), reason);
    InterlockedExchange(&g_emFinished, static_cast<LONG>(reason));
    return g_emFinishedOrig(manager, entity, reason);
}

static void __fastcall ActorKillHook(uintptr_t actor, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
    uint64_t id = 0;
    __try { id = EntityIdOfComponent(actor); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (id) InterlockedExchange64(&g_deaths[InterlockedIncrement(&g_deathNext) & 31], static_cast<LONG64>(id));
    Log("[contracts] actor %llu died", static_cast<unsigned long long>(id));
    g_actorKillOrig(actor, a2, a3, a4, a5);
}

static bool DiedRecently(uint64_t id) {
    for (const volatile LONG64& d : g_deaths) if (static_cast<uint64_t>(d) == id) return true;
    return false;
}

static const char* CryText(uintptr_t s) { return s ? reinterpret_cast<const char*>(s) : ""; }
static const char* g_objectiveStep = "";

static uintptr_t EntityById(uint64_t id) { return id ? VCall<uintptr_t>(*g_tp.entitySystem, 0x120, id) : 0; }

static double Distance(const double a[3], const double b[3]) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}

static uintptr_t MissionEntityOf(uintptr_t module, uint64_t& id) {
    const uintptr_t entity = Rd<uint64_t>(module + 8) & kPtrMask;
    uintptr_t me = entity ? EntityComponent(entity, "MissionEntity") : 0;
    if (!me) return 0;
    id = EntityIdOfComponent(me);
    const uint64_t master = Rd<uint64_t>(me + kMasterMission);
    if (!master || master == id) return master ? me : 0;
    const uintptr_t e = EntityById(master);
    id = master;
    return e ? EntityComponent(e, "MissionEntity") : 0;
}

static uintptr_t MissionEntityById(uint64_t meId, uintptr_t& entity) {
    entity = EntityById(meId);
    const uintptr_t me = entity ? EntityComponent(entity, "MissionEntity") : 0;
    return me && Rd<uintptr_t>(Rd<uintptr_t>(me) + 0x708) == reinterpret_cast<uintptr_t>(g_createObjective) ? me : 0;
}

static void EndOurObjective(uintptr_t me, const uintptr_t* id) {
    VCall<void>(me, 0x710, 3, false, id, false, 0u);
}

static bool TargetOf(const Running& r, double world[3], uintptr_t& zone) {
    const Act act = r.flow->steps[r.step].act;
    const bool drop = act == Act::Deliver;
    if (act == Act::Away || (drop && !r.dropoff)) {
        zone = ZoneFromId(r.homeZone);
        if (!zone) return false;
        LocalToWorld(zone, r.home, world);
        return true;
    }
    const uintptr_t e = EntityById(drop ? r.dropoff : r.target);
    if (!e) return false;
    Vec3Out(e, 0x318, world);
    zone = VCall<uintptr_t>(e, 0x6B8);
    return zone != 0;
}

static uintptr_t PlanetZoneOf(uintptr_t zone) {
    for (int i = 0; zone && i < 16; ++i) {
        const uintptr_t parent = ZoneParent(zone);
        if (!parent) return 0;
        if (!ZoneParent(parent)) return zone;
        zone = parent;
    }
    return 0;
}

static bool UpAt(uintptr_t planet, const double world[3], double up[3]) {
    const double zero[3] = {};
    double centre[3];
    LocalToWorld(planet, zero, centre);
    double len = 0;
    for (int i = 0; i < 3; ++i) { up[i] = world[i] - centre[i]; len += up[i] * up[i]; }
    len = sqrt(len);
    if (len < 1) return false;
    for (int i = 0; i < 3; ++i) up[i] /= len;
    return true;
}

static int ProbeGround(uintptr_t zone, const double at[3], double distance) {
    const uintptr_t planet = PlanetZoneOf(zone);
    double up[3];
    if (!planet || !UpAt(planet, at, up)) return 0;
    double from[3], to[3], hit[3];
    for (int i = 0; i < 3; ++i) { from[i] = at[i] + up[i] * 25.0; to[i] = at[i] - up[i] * kGroundProbeM; }
    if (GroundRay(zone, from, to, hit)) return 1;
    return distance < kDecideSpaceM ? 0 : -1;
}

static void MarkStep(const Running& r, uintptr_t me, const uintptr_t* objective, const char* label) {
    const bool drop = r.flow->steps[r.step].act == Act::Deliver;
    uint64_t ids[1] = { drop ? r.dropoff : r.target };
    struct { uint64_t* b; uint64_t* e; uint64_t* c; } marked = { ids, ids + (ids[0] ? 1 : 0), ids + 1 };
    MarkerParams m{};
    m.objective = objective;
    m.entities = &marked;
    m.label = label;
    if (!ids[0]) { memcpy(m.pos, r.home, sizeof(m.pos)); m.zoneHost = r.homeZone; }
    m.b50 = 1;
    g_stringInit(&m.text);
    uintptr_t marker = 0;
    g_objectiveStep = "SetObjectiveMarker";
    VCall<void*>(me, 0x718, &marker, &m);
    Log("[contracts] marker for '%s' on %s: %s", CryText(*objective), ids[0] ? "the target entity" : "the spot you took the contract at",
        marker && *CryText(marker) ? "set" : "NOT set");
}

static void StartStep(Running& r, DWORD now) {
    const Step& s = r.flow->steps[r.step];
    r.objective = 0; r.started = false; r.since = now; r.npcCount = 0;
    uintptr_t entity = 0;
    const uintptr_t me = MissionEntityById(r.meId, entity);
    if (!me) { Log("[contracts] '%s': the mission entity is gone; step %d has no objective", r.contract, r.step + 1); return; }
    g_objectiveStep = "building an objective";
    ObjectiveParams p{};
    g_locId(&p.title, s.title);
    g_locId(&p.description, s.text);
    p.order = static_cast<uint32_t>(r.step);
    p.flags = 8;
    p.q50 = 0xFFFF;
    memcpy(p.parent, r.mission, 16);
    if (!MakeCryString(&p.id, s.id)) g_stringInit(&p.id);
    g_stringInit(&p.subtitle);
    uintptr_t id = 0;
    g_objectiveStep = "CreateObjective";
    VCall<void*>(me, 0x708, &id, &p, true);
    const bool added = id && *CryText(id);
    Log("[contracts] '%s' step %d/%s: objective '%s' (%s / %s): %s", r.contract, r.step + 1, r.flow->name, added ? CryText(id) : s.id,
        s.title, s.text, added ? "added" : "NOT added");
    if (!added) return;
    r.objective = id;
    if (s.act == Act::Travel || s.act == Act::Deliver) MarkStep(r, me, &r.objective, s.title);
}

static void Finish(Running& r);

static void NextStep(Running& r, DWORD now) {
    uintptr_t entity = 0;
    if (r.objective) if (const uintptr_t me = MissionEntityById(r.meId, entity)) EndOurObjective(me, &r.objective);
    if (r.step >= 0 && r.step < 2 && r.flow->steps[r.step].act == Act::Fight && r.started) SendComms(r.contract, r.mission, kCommsCleared);
    const int ground = r.ground;
    if (++r.step >= 2 || r.flow->steps[r.step].act == Act::None) { Finish(r); return; }
    StartStep(r, now);
    r.ground = r.flow->steps[r.step].act == Act::Deliver ? -1 : ground;
}

static int HostileCount(const char* contract) {
    if (ContainsNoCase(contract, "VeryEasy") || ContainsNoCase(contract, "Intro")) return 3;
    if (ContainsNoCase(contract, "VeryHard") || ContainsNoCase(contract, "Super")) return 7;
    if (ContainsNoCase(contract, "Hard")) return 6;
    if (ContainsNoCase(contract, "Medium")) return 5;
    return 4;
}

static int SpawnGroundHostiles(Running& r, uintptr_t zone, const double at[3]) {
    const uintptr_t planet = PlanetZoneOf(zone);
    const uint64_t zoneId = ZoneId(zone);
    double up[3];
    if (!planet || !zoneId || !UpAt(planet, at, up)) return 0;
    double a[3] = { 1, 0, 0 };
    if (fabs(up[0]) > 0.9) { a[0] = 0; a[1] = 1; }
    double t1[3] = { up[1] * a[2] - up[2] * a[1], up[2] * a[0] - up[0] * a[2], up[0] * a[1] - up[1] * a[0] };
    const double l1 = sqrt(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
    for (double& v : t1) v /= l1;
    const double t2[3] = { up[1] * t1[2] - up[2] * t1[1], up[2] * t1[0] - up[0] * t1[2], up[0] * t1[1] - up[1] * t1[0] };
    double atL[3], tipW[3], tipL[3], u[3];
    for (int i = 0; i < 3; ++i) tipW[i] = at[i] + up[i];
    if (!WorldToLocal(zone, at, atL) || !WorldToLocal(zone, tipW, tipL)) return 0;
    for (int i = 0; i < 3; ++i) u[i] = tipL[i] - atL[i];
    const double lu = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (double& v : u) v /= lu;
    double rot[4] = { -u[1], u[0], 0, 1 + u[2] };
    const double lr = sqrt(rot[0] * rot[0] + rot[1] * rot[1] + rot[3] * rot[3]);
    if (lr < 1e-6) { rot[0] = 1; rot[1] = 0; rot[3] = 0; } else for (double& v : rot) v /= lr;
    const int want = HostileCount(r.contract);
    const int kinds = static_cast<int>(sizeof(kGroundEnemies) / sizeof(kGroundEnemies[0]));
    for (int i = 0; i < want && r.npcCount < kMaxNpcs; ++i) {
        const double angle = i * 6.283185307179586 / want + 0.4, radius = 10.0 + (i % 3) * 8.0;
        double p[3], from[3], to[3], hit[3], local[3];
        for (int k = 0; k < 3; ++k) p[k] = at[k] + (t1[k] * cos(angle) + t2[k] * sin(angle)) * radius;
        for (int k = 0; k < 3; ++k) { from[k] = p[k] + up[k] * 4.0; to[k] = p[k] - up[k] * 40.0; }
        if (GroundRay(zone, from, to, hit)) memcpy(p, hit, sizeof(p));
        for (int k = 0; k < 3; ++k) p[k] += up[k] * 0.1;
        if (!WorldToLocal(zone, p, local)) continue;
        const char* cls = kGroundEnemies[(i + r.mission[3]) % kinds];
        uint64_t id = 0;
        const char* err = SpawnEntityInZone(cls, zoneId, local, rot, id);
        if (err || !id) { Log("[contracts] couldn't put %s at the location: %s", cls, err ? err : "no entity"); continue; }
        r.npcs[r.npcCount++] = id;
    }
    return r.npcCount;
}

static void StartFight(Running& r, uintptr_t zone, const double at[3]) {
    r.started = true;
    const bool arriveComms = CommsRecord(r.contract, kCommsArrive) != nullptr;
    SendComms(r.contract, r.mission, kCommsArrive);
    SendComms(r.contract, r.mission, kCommsHostiles, arriveComms ? 8.0f : 0.0f);
    if (r.ground == 1 && SpawnGroundHostiles(r, zone, at)) {
        Log("[contracts] '%s': %d hostiles on foot at the location", r.contract, r.npcCount);
        return;
    }
    if (r.ground == 1) Log("[contracts] '%s': no hostiles could be put on the ground there; pirates in the sky instead", r.contract);
    r.ground = 0;
    Log("[contracts] '%s': hostiles inbound ('%s')", r.contract, r.hostiles->name);
    StartMissionNearPlayer(r.hostiles->missionId, r.hostiles->name, kHostilesMinM, kHostilesMaxM);
}

static void UpdateGroundHostiles(Running& r) {
    for (int i = 0; i < r.npcCount; ) {
        if (DiedRecently(r.npcs[i]) || !EntityById(r.npcs[i])) { r.npcs[i] = r.npcs[--r.npcCount]; continue; }
        ++i;
    }
}

static uint64_t DropoffFor(const uint8_t mission[16], uint64_t target) {
    uintptr_t mods[16];
    int n = 0;
    AcquireSRWLockShared(&g_moduleLock);
    for (const Module& m : g_modules) {
        uint8_t id[16];
        if (m.module && n < 16 && ModuleMission(m.module, id) && !memcmp(id, mission, 16)) mods[n++] = m.module;
    }
    ReleaseSRWLockShared(&g_moduleLock);
    const uintptr_t t = EntityById(target);
    if (!t) return 0;
    double at[3];
    Vec3Out(t, 0x318, at);
    uint64_t best = 0;
    double farthest = kMinDropoffM;
    for (int i = 0; i < n; ++i) {
        const uint64_t id = EntityIdOfComponent(mods[i]);
        const uintptr_t e = id && id != target ? EntityById(id) : 0;
        if (!e) continue;
        double p[3];
        Vec3Out(e, 0x318, p);
        const double d = Distance(p, at);
        if (d > farthest) { farthest = d; best = id; }
    }
    return best;
}

static void StartContract(uintptr_t module, Plan& plan) {
    plan.done = true;
    uint64_t meId = 0;
    g_objectiveStep = "finding the mission entity";
    const uintptr_t me = MissionEntityOf(module, meId);
    if (!me || !meId) { Log("[contracts] our mission has no mission entity to put objectives on"); return; }
    if (!g_createObjective || Rd<uintptr_t>(Rd<uintptr_t>(me) + 0x708) != reinterpret_cast<uintptr_t>(g_createObjective)) {
        Log("[contracts] the mission entity isn't the CMissionEntity we know; no objectives added");
        return;
    }
    if (g_runningCount >= kMaxOurs) { Log("[contracts] too many running contracts; this one gets no objectives"); return; }
    Running& r = g_running[g_runningCount++];
    r = Running{};
    memcpy(r.mission, plan.mission, 16);
    r.contract = plan.contract ? plan.contract : "?";
    r.flow = &FlowFor(r.contract);
    r.hostiles = &kStandIns[sizeof(kStandIns) / sizeof(kStandIns[0]) - 1];
    for (const StandIn& s : kStandIns)
        if (*s.kind && ContainsNoCase(r.contract, s.kind)) { r.hostiles = &s; break; }
    r.meId = meId;
    r.target = EntityIdOfComponent(module);
    if (!r.target || !EntityById(r.target)) r.target = meId;
    r.ground = -1;
    uintptr_t actor = 0, player = 0;
    if (GetLocalPlayer(actor, player)) {
        const uintptr_t zone = VCall<uintptr_t>(player, 0x6B8);
        r.homeZone = zone ? ZoneId(zone) : 0;
        Vec3Out(player, 0x2B8, r.home);
        if (g_addActivePlayer) g_addActivePlayer(me, LocalPlayerEntityId(), true);
    }
    if (r.flow->steps[1].act == Act::Deliver) r.dropoff = DropoffFor(r.mission, r.target);
    double at[3] = {}, you[3] = {};
    if (const uintptr_t t = EntityById(r.target)) Vec3Out(t, 0x318, at);
    if (player) Vec3Out(player, 0x318, you);
    const uintptr_t tz = EntityById(r.target) ? VCall<uintptr_t>(EntityById(r.target), 0x6B8) : 0;
    Log("[contracts] '%s': %s contract, location %.1f km away in zone '%s'%s", r.contract, r.flow->name, Distance(at, you) / 1000.0,
        tz ? ZoneName(tz) : "?", r.flow->steps[1].act == Act::Deliver ? (r.dropoff ? ", drop-off at another site" : ", drop-off back here") : "");
    StartStep(r, GetTickCount());
    SendComms(r.contract, r.mission, kCommsAccept);
}

static bool ModuleRunsScript(uintptr_t module, const char*& path) {
    path = "?";
    __try {
        if (const char* p = Rd<const char*>(module + 0x90)) path = p;
        if (!Rd<uint8_t>(module + kModuleHasInstance) && !Rd<uint8_t>(module + kModuleInstanceFailed))
            VCall<void>(module, kCreateInstanceSlot, uintptr_t(0));
        return Rd<uint8_t>(module + kModuleHasInstance) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void StartScriptedContract(uintptr_t module, Plan& plan, const char* path) {
    plan.done = true;
    uint64_t meId = 0;
    const uintptr_t me = MissionEntityOf(module, meId);
    const uint64_t player = MissionPlayer(plan.mission);
    if (me && player && g_addActivePlayer) g_addActivePlayer(me, player, true);
    Log("[contracts] '%s': runs CIG's way on its own script (%s)%s", plan.contract ? plan.contract : "?", path,
        me ? "" : " - but its mission entity wasn't found, so its markers may not reach you");
    SendComms(plan.contract ? plan.contract : "", plan.mission, kCommsAccept);
}

static void StartContractSafe(uintptr_t module, const uint8_t mission[16]) {
    for (Plan& plan : g_plans) {
        if (plan.done || memcmp(plan.mission, mission, 16)) continue;
        __try {
            const char* path = "?";
            if (ModuleRunsScript(module, path)) StartScriptedContract(module, plan, path);
            else StartContract(module, plan);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[contracts] fault while starting our contract (%s)", g_objectiveStep);
        }
        return;
    }
}

using HelperSpawnedFn = int64_t(__fastcall*)(const uint8_t* capture, uintptr_t a2, const int* result);
static HelperSpawnedFn g_helperSpawnedOrig = nullptr;
struct Helper { uint8_t mission[16]; uint64_t entity; };
static Helper  g_helpers[32];
static int     g_helperNext = 0;
static SRWLOCK g_helperLock = SRWLOCK_INIT;

static int64_t __fastcall HelperSpawnedHook(const uint8_t* capture, uintptr_t a2, const int* result) {
    const int64_t r = g_helperSpawnedOrig(capture, a2, result);
    __try {
        Helper h;
        h.entity = *reinterpret_cast<const uint64_t*>(capture);
        memcpy(h.mission, capture + 8, 16);
        if (result && !*result && h.entity && IsOurMission(h.mission)) {
            AcquireSRWLockExclusive(&g_helperLock);
            g_helpers[g_helperNext] = h;
            g_helperNext = (g_helperNext + 1) % 32;
            ReleaseSRWLockExclusive(&g_helperLock);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

static int HelpersOf(const uint8_t mission[16], uint64_t* out, int max) {
    int n = 0;
    AcquireSRWLockShared(&g_helperLock);
    for (const Helper& h : g_helpers)
        if (h.entity && n < max && !memcmp(h.mission, mission, 16)) out[n++] = h.entity;
    ReleaseSRWLockShared(&g_helperLock);
    return n;
}

static void StartDetached(Plan& plan) {
    plan.done = true;
    if (g_runningCount >= kMaxOurs) { Log("[contracts] too many running contracts; this one gets no objectives"); return; }
    uintptr_t actor = 0, player = 0;
    if (!GetLocalPlayer(actor, player)) return;
    uint64_t own = 0;
    const double here[3] = {};
    g_objectiveStep = "spawning our mission entity";
    const char* err = SpawnEntityNearPlayer("MissionEntityStreamable", here, own);
    if (err || !own) { Log("[contracts] '%s': couldn't spawn a mission entity for its objectives (%s)", plan.contract, err ? err : "no entity"); return; }
    Running& r = g_running[g_runningCount++];
    r = Running{};
    memcpy(r.mission, plan.mission, 16);
    r.contract = plan.contract ? plan.contract : "?";
    r.hostiles = &kStandIns[sizeof(kStandIns) / sizeof(kStandIns[0]) - 1];
    for (const StandIn& s : kStandIns)
        if (*s.kind && ContainsNoCase(r.contract, s.kind)) { r.hostiles = &s; break; }
    r.meId = own;
    r.ground = -1;
    r.step = kPending;
    r.since = GetTickCount();
    const uintptr_t zone = VCall<uintptr_t>(player, 0x6B8);
    r.homeZone = zone ? ZoneId(zone) : 0;
    Vec3Out(player, 0x2B8, r.home);
    uint64_t helpers[8];
    const int n = HelpersOf(r.mission, helpers, 8);
    if (n) {
        r.flow = &FlowFor(r.contract);
        r.target = helpers[0];
        double a[3] = {};
        if (const uintptr_t t = EntityById(r.target)) Vec3Out(t, 0x318, a);
        double farthest = kMinDropoffM;
        for (int i = 1; i < n && r.flow->steps[1].act == Act::Deliver; ++i)
            if (const uintptr_t e = EntityById(helpers[i])) {
                double p[3];
                Vec3Out(e, 0x318, p);
                if (Distance(p, a) > farthest) { farthest = Distance(p, a); r.dropoff = helpers[i]; }
            }
    } else {
        r.flow = &kAwayFlows[sizeof(kAwayFlows) / sizeof(kAwayFlows[0]) - 1];
        for (const Flow& f : kAwayFlows)
            if (*f.kinds && MatchesAny(r.contract, f.kinds)) { r.flow = &f; break; }
    }
    Log("[contracts] '%s': no mission module; %s contract with %d site(s) (%s), objectives on our own mission entity %llu", r.contract,
        r.flow->name, n, n ? (r.dropoff ? "pickup + drop-off" : "one site") : "out and back", static_cast<unsigned long long>(own));
}

static void StartPending(Running& r, DWORD now) {
    uintptr_t entity = 0;
    const uintptr_t me = MissionEntityById(r.meId, entity);
    if (!me) {
        if (now - r.since < kOwnEntityWaitMs) return;
        Log("[contracts] '%s': our mission entity %llu has no CMissionEntity; no objectives", r.contract, static_cast<unsigned long long>(r.meId));
        r.step = kDone;
        return;
    }
    if (g_addActivePlayer) g_addActivePlayer(me, LocalPlayerEntityId(), true);
    r.step = 0;
    StartStep(r, now);
}

static void StartDetachedContracts(DWORD now) {
    for (Plan& plan : g_plans) {
        if (plan.done || !plan.builtAt || now - plan.builtAt < kDetachedAfterMs) continue;
        plan.done = true;
        Log("[contracts] '%s': no mission module; left to CIG's own objectives", plan.contract ? plan.contract : "?");
    }
}

static int TestCommand() {
    char path[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("SC_OFFLINE_MOD_LOG", path, MAX_PATH);
    char* slash = n && n < MAX_PATH ? strrchr(path, '\\') : nullptr;
    if (!slash || static_cast<size_t>(slash + 1 - path) + 18 > MAX_PATH) return 0;
    strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), "contract_test.txt");
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) return 0;
    char word[32] = {};
    const bool read = fscanf_s(f, "%31s", word, static_cast<unsigned>(sizeof(word))) == 1;
    fclose(f);
    DeleteFileA(path);
    if (!read) return 0;
    return !_stricmp(word, "next") || !_stricmp(word, "arrive") ? 1 : !_stricmp(word, "complete") ? 2 : !_stricmp(word, "ground") ? 3
         : !_stricmp(word, "drop") ? 4 : !_stricmp(word, "clear") ? 5 : 0;
}

constexpr size_t kWalletUec = 0xC8;
static int64_t g_walletSaved = -1;

static bool WalletPath(char path[MAX_PATH]) {
    const DWORD n = GetEnvironmentVariableA("SC_OFFLINE_MOD_LOG", path, MAX_PATH);
    char* slash = n && n < MAX_PATH ? strrchr(path, '\\') : nullptr;
    if (!slash || static_cast<size_t>(slash + 1 - path) + 11 > MAX_PATH) return false;
    strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), "wallet.txt");
    return true;
}

static uintptr_t PlayerWallet() {
    uintptr_t actor = 0, player = 0;
    return GetLocalPlayer(actor, player) ? EntityComponent(player, "IWallet") : 0;
}

static bool PlayerDataPath(const char* nickname, char path[MAX_PATH]);

static int64_t DataFileAmount(const char* nickname) {
    char path[MAX_PATH];
    if (!PlayerDataPath(nickname, path)) return 0;
    char text[8192] = "";
    size_t n = 0;
    if (FILE* f = _fsopen(path, "rb", _SH_DENYNO)) { n = fread(text, 1, sizeof(text) - 1, f); fclose(f); }
    text[n] = 0;
    static const char kKey[] = "<aUEC amount=\"";
    const char* at = strstr(text, kKey);
    const long long v = at ? _atoi64(at + sizeof(kKey) - 1) : 0;
    return v > 0 && v < 1000000000000LL ? v : 0;
}

static void RestoreWallet() {
    char path[MAX_PATH];
    if (g_walletSaved >= 0 || !WalletPath(path)) return;
    g_walletSaved = 0;
    if (FILE* f = _fsopen(path, "r", _SH_DENYNO)) {
        long long v = 0;
        if (fscanf_s(f, "%lld", &v) == 1 && v > 0 && v < 1000000000000LL) g_walletSaved = v;
        fclose(f);
    }
    if (g_walletSaved <= 0) {
        const int64_t start = DataFileAmount("default_1");
        if (start > 0) {
            g_walletSaved = start;
            Log("[contracts] wallet: no saved balance - %lld aUEC from the offline data file", static_cast<long long>(start));
        }
    }
    const uintptr_t wallet = PlayerWallet();
    if (!wallet || !g_updateBalance) return;
    const int64_t now = Rd<int64_t>(wallet + kWalletUec);
    if (g_walletSaved > 0 && g_walletSaved != now) g_updateBalance(wallet, 1, g_walletSaved - now);
    Log("[contracts] wallet: %lld aUEC from wallet.txt (was %lld, now %lld)", static_cast<long long>(g_walletSaved),
        static_cast<long long>(now), static_cast<long long>(Rd<int64_t>(wallet + kWalletUec)));
}

static void SaveWallet() {
    char path[MAX_PATH];
    const uintptr_t wallet = g_walletSaved >= 0 ? PlayerWallet() : 0;
    if (!wallet || !WalletPath(path)) return;
    const int64_t uec = Rd<int64_t>(wallet + kWalletUec);
    if (uec == g_walletSaved || uec < 0 || uec >= 1000000000000LL) return;
    if (FILE* f = _fsopen(path, "w", _SH_DENYWR)) {
        fprintf(f, "%lld\n", static_cast<long long>(uec));
        fclose(f);
        g_walletSaved = uec;
    }
}

static bool PlayerDataPath(const char* nickname, char path[MAX_PATH]) {
    char dir[MAX_PATH], name[48];
    const DWORD d = GetEnvironmentVariableA("SC_USER", dir, sizeof(dir));
    if (!d || d >= sizeof(dir)) return false;
    sprintf_s(path, MAX_PATH, "%s\\%s.xml", dir, nickname);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return true;
    strncpy_s(name, nickname, _TRUNCATE);
    if (char* cut = strchr(name, '_')) *cut = 0;
    sprintf_s(path, MAX_PATH, "%s\\%s.xml", dir, name);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static EndMissionFn g_endMissionOrig = nullptr;
static uint8_t      g_paid[kMaxOurs][16];
static int          g_paidNext = 0;

static bool TakePayment(const uint8_t* mission) {
    for (const auto& p : g_paid) if (!memcmp(p, mission, 16)) return false;
    memcpy(g_paid[g_paidNext], mission, 16);
    g_paidNext = (g_paidNext + 1) % kMaxOurs;
    return true;
}

static void* __fastcall EndMissionHook(uintptr_t log, const uint8_t* mission, int state, int players, int reason,
                                       const char* text, uint32_t loc) {
    int reward = -1;
    __try {
        if (state == 4 && mission && IsOurMission(mission) && TakePayment(mission)) {
            const uintptr_t entry = g_findEntry ? g_findEntry(log, mission) : 0;
            reward = entry && g_totalReward ? g_totalReward(entry) : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    void* result = g_endMissionOrig(log, mission, state, players, reason, text, loc);
    if (reward >= 0) {
        __try {
            const uintptr_t owner = reward > 0 ? EntityById(EntityIdOfComponent(log)) : 0;
            const uintptr_t wallet = owner ? EntityComponent(owner, "IWallet") : reward > 0 ? PlayerWallet() : 0;
            const bool paid = wallet && g_updateBalance && g_updateBalance(wallet, 1, reward);
            Log("[contracts] a contract's script completed it; reward %d aUEC %s (wallet %lld)", reward,
                paid ? "paid" : "NOT paid", static_cast<long long>(wallet ? Rd<int64_t>(wallet + kWalletUec) : -1));
            SaveWallet();
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return result;
}

using EndPhaseFn = void*(__fastcall*)(uintptr_t service, void* future, const uint8_t* mission, const uint8_t* phase, int a5, int state);
static EndPhaseFn g_endPhaseOrig = nullptr;
struct PhaseEnd { uint8_t mission[16], phase[16]; int state; };
static PhaseEnd g_phaseEnds[16];
static int      g_phaseEndCount = 0;
static SRWLOCK  g_phaseEndLock = SRWLOCK_INIT;

static void NotePhaseEnd(const uint8_t* mission, const uint8_t* phase, int state) {
    AcquireSRWLockExclusive(&g_phaseEndLock);
    if (g_phaseEndCount < 16) {
        PhaseEnd& e = g_phaseEnds[g_phaseEndCount++];
        memcpy(e.mission, mission, 16);
        if (phase) memcpy(e.phase, phase, 16); else memset(e.phase, 0, 16);
        e.state = state;
    }
    ReleaseSRWLockExclusive(&g_phaseEndLock);
}

static void* __fastcall EndPhaseHook(uintptr_t service, void* future, const uint8_t* mission, const uint8_t* phase, int a5, int state) {
    void* result = g_endPhaseOrig(service, future, mission, phase, a5, state);
    __try {
        if (mission && state >= 2 && state <= 5 && IsOurMission(mission)) NotePhaseEnd(mission, phase, state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

static uintptr_t MissionLogOf(const uint8_t* mission) {
    const uint64_t id = MissionPlayer(mission);
    const uintptr_t player = id ? EntityById(id) : 0;
    return player ? EntityComponent(player, "SCPlayerMissionLog") : 0;
}

using FlowContextFn  = void*(__fastcall*)(uintptr_t* ctx, const uint8_t* details);
using UpdateFlowFn   = void(__fastcall*)(uintptr_t* ctx, void* result, bool initial, uintptr_t, uintptr_t);
using TempAllocFn    = uintptr_t(__fastcall*)(void* owner, size_t size);
using FreeMapFn      = void(__fastcall*)(void* map);
using FindPhaseFn    = const uint8_t*(__fastcall*)(uintptr_t phases, const uint8_t* id);
using CreatePhaseFn  = bool(__fastcall*)(uintptr_t* phases, uintptr_t* objectives, const uint8_t* phase, const uint8_t* activeKey,
                                         const uintptr_t* shardId, const uint8_t* details, const uintptr_t* graph, uintptr_t* errors);
static uintptr_t NextNode(uintptr_t n);
static FlowContextFn g_flowContext = nullptr;
static UpdateFlowFn  g_updateFlow = nullptr;
static TempAllocFn   g_tempAlloc = nullptr;
static FreeMapFn     g_freeFlowMap = nullptr;
static FindPhaseFn   g_findPhase = nullptr;
static CreatePhaseFn g_createPhase = nullptr;
static CreatePhaseFn g_createPhaseOrig = nullptr;
constexpr size_t kTokens = 0xB8, kTokenSize = 0x110, kTokenState = 0x20;

static uint8_t* TokenOf(uint8_t* details, const uint8_t id[16]) {
    uint8_t* const b = Rd<uint8_t*>(reinterpret_cast<uintptr_t>(details + kTokens));
    uint8_t* const e = Rd<uint8_t*>(reinterpret_cast<uintptr_t>(details + kTokens + 8));
    for (uint8_t* t = b; t && t + kTokenSize <= e; t += kTokenSize)
        if (!memcmp(t, id, 16)) return t;
    return nullptr;
}

static const char* PhaseName(const uint8_t* record) {
    const char* name = nullptr;
    __try {
        name = reinterpret_cast<const char*>(Rd<uintptr_t>(reinterpret_cast<uintptr_t>(record) + 0x18) & 0xFFFFFFFFFFFFull);
        if (name && !*name) name = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { name = nullptr; }
    return name ? name : "?";
}

using ObjectiveInitFn      = void(__fastcall*)(void* objective);
using ObjectiveCopyFn      = void(__fastcall*)(void* objective, const void* from);
using EmptyLocFn           = const uint32_t*(__fastcall*)();
using TimerInitFn          = void(__fastcall*)(void* timer, uintptr_t);
using PendingToObjectiveFn = void(__fastcall*)(const uint8_t* pending, uint8_t* objective);
static ObjectiveInitFn      g_objectiveInit = nullptr;
static ObjectiveCopyFn      g_objectiveCopy = nullptr;
static EmptyLocFn           g_emptyLoc = nullptr;
static TimerInitFn          g_timerInit = nullptr;
static uintptr_t            g_activeObjectiveVtbl = 0;
static PendingToObjectiveFn g_pendingToObjectiveOrig = nullptr;
constexpr size_t kSmoSize = 0xA0, kPendingSize = 0x88;
static thread_local const uint8_t* t_pendings[64];
static thread_local int t_pendingCount = -1;

static void __fastcall PendingToObjectiveHook(const uint8_t* pending, uint8_t* objective) {
    g_pendingToObjectiveOrig(pending, objective);
    if (t_pendingCount >= 0 && t_pendingCount < 64) t_pendings[t_pendingCount++] = pending;
}

static const uint8_t* TokenOfObjective(uint8_t* details, const char* id) {
    const uintptr_t b = Rd<uintptr_t>(reinterpret_cast<uintptr_t>(details + kTokens)), e = Rd<uintptr_t>(reinterpret_cast<uintptr_t>(details + kTokens + 8));
    for (uintptr_t t = b; t && t + kTokenSize <= e; t += kTokenSize) {
        const char* own = Rd<const char*>(t + 0x28);
        if (own && !strcmp(own, id)) return reinterpret_cast<const uint8_t*>(t);
    }
    return nullptr;
}

static uintptr_t Generator();
static uintptr_t NextNode(uintptr_t n);

static bool UiNotifyOff() {
    static int off = -1;
    if (off < 0) {
        char path[MAX_PATH];
        const DWORD pn = GetEnvironmentVariableA("SC_OFFLINE_MOD_LOG", path, MAX_PATH);
        char* slash = pn && pn < MAX_PATH ? strrchr(path, '\\') : nullptr;
        if (slash) strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), "ui_notify_off.txt");
        off = slash && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
        if (off) Log("[contracts] ui_notify_off.txt: new objectives aren't announced to mobiGlas");
    }
    return off == 1;
}

static void NotifyObjectiveAdded(uintptr_t log, const uint8_t mission[16], const ObjectiveNote& n) {
    if (!g_notifyObjective || !g_emptyLoc || !n.id || !*n.id || UiNotifyOff()) return;
    static int quiet = 0;
    const char* why = nullptr;
    __try {
        if (!(n.flags & 8)) why = "not shown in the log (flags)";
        else if (n.longText == *g_emptyLoc()) why = "no text";
        else if (AlreadyNotified(mission, n.id)) why = "already announced";
        uintptr_t entry = 0;
        if (!why) {
            entry = LogEntry(log, mission);
            bool there = false;
            for (uintptr_t p = entry ? Rd<uintptr_t>(entry + 0x180) : 0; p && p + 0x110 <= Rd<uintptr_t>(entry + 0x188) && !there; p += 0x110) {
                const char* own = Rd<const char*>(p + 8);
                there = own && !strcmp(own, n.id);
            }
            if (!entry) why = "no mission entry";
            else if (!there) why = "not in the mission's entry";
        }
        if (!why && g_objectiveHidden && n.parent && g_objectiveHidden(log, mission, n.parent, false, false)) why = "hidden";
        if (why) {
            if (quiet < 30) { ++quiet; Log("[contracts] objective '%s' not announced: %s", n.id, why); }
            return;
        }
        const bool silent = (Rd<uint8_t>(entry + 0xC0) & 4) || (n.flags & 4);
        uint64_t id = 0;
        if (!MakeCryString(&id, n.id)) { Log("[contracts] objective '%s' not announced: no CryString", n.id); return; }
        Log("[contracts] objective '%s': announcing (flags %x, text %x, silent %d)", n.id, n.flags, n.shortText, silent);
        NotedObjective& slot = g_notedObjectives[g_notedObjectiveNext];
        g_notedObjectiveNext = (g_notedObjectiveNext + 1) % 32;
        memcpy(slot.mission, mission, 16);
        slot.idHash = HashText(n.id);
        slot.at = GetTickCount() | 1;
        t_ourNotify = true;
        g_notifyObjective(log, 0, mission, n.shortText, silent, &id, 0, n.timer > 0 || n.progress > 0.0f);
        t_ourNotify = false;
        static int logged = 0;
        if (logged < 40) { ++logged; Log("[contracts] objective '%s': mobiGlas and the HUD told%s", n.id, silent ? " (silently)" : ""); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { t_ourNotify = false; Log("[contracts] fault announcing objective '%s'", n.id); }
}

static void InitActiveObjective(uint8_t* o, const uint8_t* pending) {
    memset(o, 0, 0x110);
    if (pending && g_objectiveCopy) g_objectiveCopy(o, pending);
    else g_objectiveInit(o);
    *reinterpret_cast<uintptr_t*>(o) = g_activeObjectiveVtbl;
    const uint32_t empty = *g_emptyLoc();
    *reinterpret_cast<uint32_t*>(o + 0xA8) = empty;
    g_timerInit(o + 0xD0, 0);
    *reinterpret_cast<int32_t*>(o + 0xF0) = -1;
    *reinterpret_cast<uint32_t*>(o + 0xF4) = empty;
    *reinterpret_cast<float*>(o + 0x104) = -1.0f;
    *reinterpret_cast<float*>(o + 0x108) = -1.0f;
}

static void DeliverObjective(const uint8_t* smo, const uint8_t* pending, const uint8_t token[16], const uint8_t mission[16]) {
    const uintptr_t ms = g_missionSystem ? *g_missionSystem : 0;
    const uintptr_t service = ms ? Rd<uintptr_t>(ms + 0x68) : 0;
    if (!service || Rd<uintptr_t>(service) != g_offlineServiceVtbl || !g_objectiveInit || !g_emptyLoc || !g_timerInit || !g_activeObjectiveVtbl)
        return;
    uint8_t* o = static_cast<uint8_t*>(_aligned_malloc(0x110, 16));
    if (!o) return;
    InitActiveObjective(o, pending);
    const char* id = Rd<const char*>(reinterpret_cast<uintptr_t>(smo));
    const char* parent = Rd<const char*>(reinterpret_cast<uintptr_t>(smo) + 8);
    MakeCryString(o + 8, id ? id : "");
    MakeCryString(o + 0x10, parent ? parent : "");
    memcpy(o + 0x18, mission, 16);
    memcpy(o + 0x28, smo + 0x34, 8);
    memcpy(o + 0x74, smo + 0x6C, 4);
    memcpy(o + 0xB0, smo + 0x58, 16);
    memcpy(o + 0xF0, smo + 0x70, 8);
    const uintptr_t mb = Rd<uintptr_t>(reinterpret_cast<uintptr_t>(smo) + 0x40), me = Rd<uintptr_t>(reinterpret_cast<uintptr_t>(smo) + 0x48);
    const size_t markers = mb && me > mb ? (me - mb) / 8 : 0;
    if (markers && markers < 64) {
        uint64_t* ids = static_cast<uint64_t*>(malloc(markers * 8));
        if (ids) {
            memcpy(ids, reinterpret_cast<const void*>(mb), markers * 8);
            *reinterpret_cast<uint64_t**>(o + 0x78) = ids;
            *reinterpret_cast<uint64_t**>(o + 0x80) = ids + markers;
            *reinterpret_cast<uint64_t**>(o + 0x88) = ids + markers;
        }
    }
    uintptr_t future[4] = {};
    VCall<void*>(service, 0xA0, static_cast<void*>(future), static_cast<void*>(o), mission, uintptr_t(0), -4098);
    Log("[contracts]   objective '%s' (%s text parameters, %zu marker%s) to the mission's players", id ? id : "?",
        pending ? "with its" : "without", markers, markers == 1 ? "" : "s");
}

using ActivateTokenFn = bool(__fastcall*)(uintptr_t* context, uintptr_t* objectives, uintptr_t* errors);
static ActivateTokenFn g_activateTokenOrig = nullptr;

static bool __fastcall ActivateTokenHook(uintptr_t* context, uintptr_t* objectives, uintptr_t* errors) {
    bool skip = false;
    __try { skip = context && !context[4]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (skip) return false;
    return g_activateTokenOrig(context, objectives, errors);
}

static bool __fastcall CreatePhaseHook(uintptr_t* phases, uintptr_t* objectives, const uint8_t* phase, const uint8_t* activeKey,
                                       const uintptr_t* shardId, const uint8_t* details, const uintptr_t* graph, uintptr_t* errors) {
    static thread_local int depth = 0;
    size_t before = 0;
    __try { before = objectives && objectives[0] ? (objectives[1] - objectives[0]) / kSmoSize : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (depth == 0) t_pendingCount = 0;
    ++depth;
    const bool ok = g_createPhaseOrig(phases, objectives, phase, activeKey, shardId, details, graph, errors);
    --depth;
    const int pendings = depth == 0 ? t_pendingCount : 0;
    if (depth == 0) t_pendingCount = -1;
    __try {
        uint8_t* ours = ok && details && phase ? MissionDetails(details + 0x80) : nullptr;
        uint8_t* token = ours ? TokenOf(ours, phase + 8) : nullptr;
        if (token) {
            *reinterpret_cast<int32_t*>(token + kTokenState) = 1;
            Log("[contracts] phase '%s' of our mission started", PhaseName(phase));
            const size_t after = depth == 0 && objectives && objectives[0] ? (objectives[1] - objectives[0]) / kSmoSize : 0;
            int next = 0;
            for (size_t i = before; i < after && i < before + 32; ++i) {
                const uint8_t* smo = reinterpret_cast<const uint8_t*>(objectives[0] + i * kSmoSize);
                const char* id = Rd<const char*>(reinterpret_cast<uintptr_t>(smo));
                if (!id || !strncmp(id, "phase_", 6)) {
                    const uint32_t flags = Rd<uint32_t>(reinterpret_cast<uintptr_t>(smo) + 0x6C);
                    const uint8_t* own = id ? TokenOfObjective(ours, id) : nullptr;
                    if (own && own[0x108] == 0 && (flags & 1)) DeliverObjective(smo, nullptr, own, ours + 0x80);
                    else Log("[contracts]   its objective '%s' (flags %x) stays with the phase", id ? id : "?", flags);
                    continue;
                }
                const uint8_t* pending = next < pendings ? t_pendings[next++] : nullptr;
                if (!strncmp(id, "pickup_", 7) || !strncmp(id, "dropoff_", 8)) DeliverObjective(smo, pending, token, ours + 0x80);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[contracts] fault while handing a phase's objectives to its players"); }
    return ok;
}

static const uintptr_t* ShardGraph() {
    static uintptr_t graph[2] = {};
    const uintptr_t owner = g_shardStoreOwner ? *g_shardStoreOwner : 0;
    const uintptr_t store = owner ? Rd<uintptr_t>(owner + g_shardStoreOff) : 0;
    if (!graph[0] && store && g_getShardGraph) {
        g_getShardGraph(store, graph, "local_shard");
        if (!graph[0]) g_getShardGraph(store, graph, "");
    }
    return graph[0] ? graph : nullptr;
}

static bool StartPhase(const uint8_t* details, uintptr_t phases, const uint8_t id[16]) {
    const uint8_t* record = phases ? g_findPhase(phases, id) : nullptr;
    const uintptr_t* graph = ShardGraph();
    if (!record || !graph) { Log("[contracts] the next phase can't start (%s)", record ? "no world graph" : "not in the contract's phases"); return false; }
    uintptr_t* list = static_cast<uintptr_t*>(calloc(9, sizeof(uintptr_t)));
    if (!list) return false;
    alignas(16) static const uint8_t key[16] = {};
    uintptr_t shard = 0;
    g_stringInit(&shard);
    const bool ok = g_createPhase(list, list + 3, record, key, &shard, details, graph, list + 6);
    for (uintptr_t e = list[6]; e && e + 72 <= list[7]; e += 72) {
        const uintptr_t cap = Rd<uintptr_t>(e + 0x20);
        const char* what = cap > 15 ? Rd<const char*>(e + 8) : reinterpret_cast<const char*>(e + 8);
        Log("[contracts]   phase '%s': error %d: %s", PhaseName(record), Rd<int>(e), what ? what : "?");
    }
    if (!ok) Log("[contracts] phase '%s' didn't start", PhaseName(record));
    return ok;
}

static int AdvanceMission(const uint8_t mission[16], const uint8_t phase[16], int state) {
    uint8_t* const details = MissionDetails(mission);
    uint8_t* const token = details ? TokenOf(details, phase) : nullptr;
    if (!token || !g_flowContext || !g_updateFlow || !g_tempAlloc || !g_freeFlowMap || !g_findPhase || !g_createPhase || !g_stringInit) {
        Log("[contracts] the contract's flow can't be asked (%s)", !details ? "no details" : !token ? "phase not in them" : "functions missing");
        return -1;
    }
    *reinterpret_cast<int32_t*>(token + kTokenState) = state;
    struct { uintptr_t head, size; int32_t state, pad; } result{};
    const uintptr_t head = g_tempAlloc(&result, 0x38);
    if (!head) return -1;
    *reinterpret_cast<uintptr_t*>(head) = head;
    *reinterpret_cast<uintptr_t*>(head + 8) = head;
    *reinterpret_cast<uintptr_t*>(head + 0x10) = head;
    *reinterpret_cast<uint16_t*>(head + 0x18) = 0x101;
    result.head = head;
    uintptr_t ctx[8] = {};
    g_flowContext(ctx, details);
    g_updateFlow(ctx, &result, false, 0, 0);
    int started = 0;
    for (uintptr_t n = Rd<uintptr_t>(head); n && n != head && !Rd<uint8_t>(n + 0x19); n = NextNode(n)) {
        uint8_t* t = TokenOf(details, reinterpret_cast<const uint8_t*>(n + 0x20));
        const int to = Rd<int>(n + 0x30);
        if (!t) continue;
        if (to == 1) started += StartPhase(details, ctx[1], t);
        else *reinterpret_cast<int32_t*>(t + kTokenState) = to;
    }
    const int missionState = result.state;
    g_freeFlowMap(&result);
    int active = 0;
    for (uint8_t* t = Rd<uint8_t*>(reinterpret_cast<uintptr_t>(details + kTokens));
         t && t + kTokenSize <= Rd<uint8_t*>(reinterpret_cast<uintptr_t>(details + kTokens + 8)); t += kTokenSize)
        active += Rd<int>(reinterpret_cast<uintptr_t>(t + kTokenState)) == 1;
    Log("[contracts] the contract's flow: %d phase%s started, %d running, mission state %d", started, started == 1 ? "" : "s", active, missionState);
    if (missionState == 6 && !active) return -1;
    return missionState;
}

static int AdvanceMissionSafe(const uint8_t mission[16], const uint8_t phase[16], int state) {
    __try { return AdvanceMission(mission, phase, state); } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[contracts] fault while asking the contract's flow");
        return -1;
    }
}

static void HookPhases(const Section& text) {
    int n = 0;
    const uint8_t* p = FindUniquePattern(text,
        "48 8D 53 20 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 94 24 ?? ?? ?? ?? 49 8D 8F 88 00 00 00 48 89 6C 24 38 4D 8D 8F B8 00 00 00 "
        "48 89 54 24 30 4C 8B C0 4C 89 64 24 28 49 8B D6 48 89 4C 24 20 48 8B CE E8", n);
    uint8_t* create = p ? const_cast<uint8_t*>(CallTarget<const uint8_t*>(p + 65)) : nullptr;
    if (create && BytesMatch(create, "48 89 5C 24 08 4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57")
        && HookFunction(create, 15, reinterpret_cast<void*>(&CreatePhaseHook), reinterpret_cast<void**>(&g_createPhaseOrig))) {
        g_createPhase = reinterpret_cast<CreatePhaseFn>(create);
        g_findPhase = CallTarget<FindPhaseFn>(p + 4);
    }
    p = FindUniquePattern(text,
        "48 8D 4C 24 40 BA 38 00 00 00 4C 89 AC 24 68 01 00 00 C5 FA 7F 44 24 40 E8 ?? ?? ?? ?? 49 8D 57 30 48 8D 4D A8 48 89 00 "
        "48 89 40 08 48 89 40 10 66 C7 40 18 01 01 48 89 44 24 40 E8 ?? ?? ?? ?? 45 33 C0 48 8D 54 24 40 48 8D 4D A8 E8", n);
    const uint8_t* q = FindUniquePattern(text, "83 F8 06 74 07 41 89 87 68 01 00 00 48 8D 4C 24 40 E8", n);
    if (p && q) {
        g_tempAlloc = CallTarget<TempAllocFn>(p + 24);
        g_flowContext = CallTarget<FlowContextFn>(p + 59);
        g_updateFlow = CallTarget<UpdateFlowFn>(p + 76);
        g_freeFlowMap = CallTarget<FreeMapFn>(q + 17);
    }
    p = FindUniquePattern(text,
        "48 8D 4D B0 E8 ?? ?? ?? ?? C5 F9 EF C0 C5 F1 EF C9 C5 FA 7F 45 30 C5 FA 7F 4D 40 4C 89 6D B0 4C 89 65 28 4C 89 65 50 E8 ?? ?? ?? ?? "
        "C5 F9 EF C0 33 D2 8B 08 89 4D 58 48 8D 8D 80 00 00 00 C5 FA 11 75 78 C5 FA 11 75 7C C5 FA 7F 45 60 4C 89 65 70 E8", n);
    q = FindUniquePattern(text, "E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8D 53 78 48 89 07 48 8D 4F 78 E8", n);
    if (p && q) {
        g_objectiveInit = CallTarget<ObjectiveInitFn>(p + 4);
        g_emptyLoc = CallTarget<EmptyLocFn>(p + 39);
        g_timerInit = CallTarget<TimerInitFn>(p + 81);
        g_objectiveCopy = CallTarget<ObjectiveCopyFn>(q);
        g_activeObjectiveVtbl = reinterpret_cast<uintptr_t>(q + 12 + Rel32(q + 8));
    }
    uint8_t* fn = FindUniquePattern(text,
        "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 48 8D 51 08 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 10 48 8D 4F 08 E8 ?? ?? ?? ?? "
        "C5 F8 10 43 18 C5 F8 11 47 10 8B 43 2C 48 8B CB 89 47 38 8B 43 28 89", n);
    const bool pendings = fn && HookFunction(fn, 10, reinterpret_cast<void*>(&PendingToObjectiveHook), reinterpret_cast<void**>(&g_pendingToObjectiveOrig));
    fn = FindUniquePattern(text, "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00 4D 8B E0 48 8B F2 48 8B D9 E8", n);
    if (!fn || !HookFunction(fn, 10, reinterpret_cast<void*>(&ActivateTokenHook), reinterpret_cast<void**>(&g_activateTokenOrig)))
        g_activateTokenOrig = nullptr;
    Log("[contracts] phases: start %s, flow %s, hauling objectives %s, their texts %s", g_createPhase ? "ok" : "MISSING",
        g_updateFlow ? "ok" : "MISSING", g_activeObjectiveVtbl ? "ok" : "MISSING", pendings ? "ok" : "MISSING");
}

using StopMissionFn = void(__fastcall*)(uintptr_t module, int reason);
static StopMissionFn g_stopMission = nullptr;

static int StopMissionModules(const uint8_t mission[16], int reason) {
    uintptr_t modules[16];
    int m = 0, stopped = 0;
    AcquireSRWLockShared(&g_moduleLock);
    for (const Module& mod : g_modules) {
        uint8_t id[16];
        if (mod.module && m < 16 && ModuleMission(mod.module, id) && !memcmp(id, mission, 16)) modules[m++] = mod.module;
    }
    ReleaseSRWLockShared(&g_moduleLock);
    for (int k = 0; k < m; ++k)
        if (g_stopMission && ModuleState(modules[k]) == 3) { g_stopMission(modules[k], reason); ++stopped; }
    return stopped;
}

static void PhaseTest() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    char path[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("SC_OFFLINE_MOD_LOG", path, MAX_PATH);
    char* slash = n && n < MAX_PATH ? strrchr(path, '\\') : nullptr;
    if (!slash || static_cast<size_t>(slash + 1 - path) + 15 > MAX_PATH) return;
    strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), "phase_test.txt");
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) return;
    char word[32] = {};
    const bool read = fscanf_s(f, "%31s", word, static_cast<unsigned>(sizeof(word))) == 1;
    fclose(f);
    DeleteFileA(path);
    const int only = read ? atoi(word + strcspn(word, "0123456789")) : 0;
    word[strcspn(word, "0123456789")] = 0;
    const int state = !read ? 0 : !_stricmp(word, "complete") ? 2 : !_stricmp(word, "fail") ? 3 : 0;
    int ended = 0;
    for (int k = 1; state && k <= kMaxOurs && !ended; ++k) {
        uint8_t mission[16];
        AcquireSRWLockShared(&g_joinLock);
        const int i = (g_oursNext - k + kMaxOurs) % kMaxOurs;
        memcpy(mission, g_ours[i], 16);
        const uintptr_t details = reinterpret_cast<uintptr_t>(g_oursDetails[i]);
        ReleaseSRWLockShared(&g_joinLock);
        if (!details) continue;
        __try {
            int running = 0;
            for (uintptr_t t = Rd<uintptr_t>(details + kTokens); t && t + kTokenSize <= Rd<uintptr_t>(details + kTokens + 8); t += kTokenSize)
                if (Rd<int>(t + kTokenState) == 1 && (!only || ++running == only)) {
                    NotePhaseEnd(mission, reinterpret_cast<const uint8_t*>(t), state);
                    ++ended;
                }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    Log("[contracts] test '%s': %d running phase%s ended", word, ended, ended == 1 ? "" : "s");
}

static void EndScriptedContracts() {
    PhaseTest();
    PhaseEnd ends[16];
    AcquireSRWLockExclusive(&g_phaseEndLock);
    const int n = g_phaseEndCount;
    memcpy(ends, g_phaseEnds, sizeof(ends));
    g_phaseEndCount = 0;
    ReleaseSRWLockExclusive(&g_phaseEndLock);
    for (int i = 0; i < n; ++i) {
        static const char* const kPhase[] = { "?", "?", "Completed", "Failed", "Abandoned", "Deactivated" };
        const int state = ends[i].state;
        Log("[contracts] a contract's phase ended %s", kPhase[state]);
        const int flow = AdvanceMissionSafe(ends[i].mission, ends[i].phase, state);
        if (flow == 6) continue;
        const int action = flow == 2 ? 4 : flow == 3 ? 2 : flow == 4 ? 3 : state == 2 ? 4 : state == 3 ? 2 : 3;
        __try {
            const uintptr_t log = g_endMission ? MissionLogOf(ends[i].mission) : 0;
            Log("[contracts] the contract %s", !log ? "can't end (no mission log)" : action == 4 ? "completes" : action == 2 ? "fails" : "is abandoned");
            if (log) g_endMission(log, ends[i].mission, action, 1, 0, "None", 0);
            if (const int stopped = StopMissionModules(ends[i].mission, 2)) Log("[contracts] %d more of its modules stopped", stopped);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[contracts] fault while ending a contract its script finished");
        }
    }
}

using LeaveMissionFn = void*(__fastcall*)(uintptr_t service, void* future, const uint8_t* mission, uint64_t player);
static LeaveMissionFn g_leaveMissionOrig = nullptr;
static uint8_t        g_abandoned[8][16];
static int            g_abandonedCount = 0;

static void* __fastcall LeaveMissionHook(uintptr_t service, void* future, const uint8_t* mission, uint64_t player) {
    void* result = g_leaveMissionOrig(service, future, mission, player);
    __try {
        if (mission && IsOurMission(mission)) {
            AcquireSRWLockExclusive(&g_phaseEndLock);
            if (g_abandonedCount < 8) memcpy(g_abandoned[g_abandonedCount++], mission, 16);
            ReleaseSRWLockExclusive(&g_phaseEndLock);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

static void AbandonContracts() {
    uint8_t missions[8][16];
    AcquireSRWLockExclusive(&g_phaseEndLock);
    const int n = g_abandonedCount;
    memcpy(missions, g_abandoned, sizeof(missions));
    g_abandonedCount = 0;
    ReleaseSRWLockExclusive(&g_phaseEndLock);
    for (int i = 0; i < n; ++i) {
        __try {
            const uintptr_t log = MissionLogOf(missions[i]);
            if (!log || !g_findEntry || !g_findEntry(log, missions[i])) continue;
            int stopped = 0;
            uintptr_t modules[16];
            int m = 0;
            AcquireSRWLockShared(&g_moduleLock);
            for (const Module& mod : g_modules) {
                uint8_t id[16];
                if (mod.module && m < 16 && ModuleMission(mod.module, id) && !memcmp(id, missions[i], 16)) modules[m++] = mod.module;
            }
            ReleaseSRWLockShared(&g_moduleLock);
            for (int r = 0; r < g_runningCount; ++r)
                if (!memcmp(g_running[r].mission, missions[i], 16)) g_running[r].step = kDone;
            bool entity = false;
            uint64_t meId = 0;
            const uintptr_t me = m ? MissionEntityOf(modules[0], meId) : 0;
            if (me && Rd<uintptr_t>(Rd<uintptr_t>(me) + 0x708) == reinterpret_cast<uintptr_t>(g_createObjective)
                && BytesMatch(reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(Rd<uintptr_t>(me) + 0x720)),
                              "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 60 48 8B D9 45 84 C0")) {
                const uint64_t you = MissionPlayer(missions[i]);
                VCall<char>(me, 0x720, &you, false);
                entity = true;
            }
            const bool still = g_findEntry(log, missions[i]) != 0;
            if (still) {
                for (int k = 0; k < m; ++k)
                    if (g_stopMission && ModuleState(modules[k]) == 3) { g_stopMission(modules[k], 4); ++stopped; }
                if (g_endMission) g_endMission(log, missions[i], 3, 1, 0, "Player Abandoned", 0);
            }
            Log("[contracts] you abandoned a contract: %s%s", entity ? "its mission entity let you go" : "no mission entity",
                still ? (stopped ? ", then ended directly (its script stopped)" : ", then ended directly") : "");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[contracts] fault while abandoning a contract");
        }
    }
}

static void HookAbandon(const Section& text, const Section& rdata) {
    g_stopMission = reinterpret_cast<StopMissionFn>(FunctionNaming(text, rdata,
        "StopMission called with reason [$$] subsumptionState [$$] callstack \n $$ $$($$)", "89 54 24 10 48 89 4C 24 08 55 53 56 57 41 56 48 8D AC 24"));
    uint8_t* slot = g_offlineServiceVtbl ? reinterpret_cast<uint8_t*>(g_offlineServiceVtbl + 0x18) : nullptr;
    const uint8_t* fn = slot ? reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(reinterpret_cast<uintptr_t>(slot))) : nullptr;
    if (!fn || !BytesMatch(fn, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24")) {
        Log("[!] contracts: the offline mission service's abandon wasn't found; mobiGlas > Abandon won't end contracts");
        return;
    }
    const uintptr_t hook = reinterpret_cast<uintptr_t>(&LeaveMissionHook);
    DWORD err = 0;
    g_leaveMissionOrig = reinterpret_cast<LeaveMissionFn>(const_cast<uint8_t*>(fn));
    if (!WriteCode(slot, reinterpret_cast<const uint8_t*>(&hook), sizeof(hook), err)) {
        g_leaveMissionOrig = nullptr;
        Log("[!] contracts: couldn't hook the mission service's abandon (%lu)", err);
    }
    Log("[contracts] abandon: service %s, module stop %s", g_leaveMissionOrig ? "ok" : "MISSING", g_stopMission ? "ok" : "MISSING");
}

using EndHaulingFn = void(__fastcall*)(uintptr_t service, const uint8_t* params);
static EndHaulingFn g_endHaulingOrig = nullptr;

static void __fastcall EndHaulingHook(uintptr_t service, const uint8_t* params) {
    __try {
        const uint8_t* mission = params + 8;
        const bool ours = IsOurMission(mission);
        const char* what = *reinterpret_cast<const char* const*>(params);
        Log("[contracts] hauling: end objective + phase asked for %s mission (objective '%s', states %d/%d)", ours ? "our" : "another",
            what ? what : "?", *reinterpret_cast<const int*>(params + 0x28), *reinterpret_cast<const int*>(params + 0x2C));
        if (ours) {
            NotePhaseEnd(mission, params + 0x18, 2);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_endHaulingOrig(service, params);
}

static void HookHaulingEnds() {
    uint8_t* slot = g_offlineServiceVtbl ? reinterpret_cast<uint8_t*>(g_offlineServiceVtbl + 0xC0) : nullptr;
    if (!slot) return;
    const uintptr_t hook = reinterpret_cast<uintptr_t>(&EndHaulingHook);
    DWORD err = 0;
    g_endHaulingOrig = reinterpret_cast<EndHaulingFn>(Rd<uintptr_t>(reinterpret_cast<uintptr_t>(slot)));
    if (!WriteCode(slot, reinterpret_cast<const uint8_t*>(&hook), sizeof(hook), err)) {
        g_endHaulingOrig = nullptr;
        Log("[!] contracts: couldn't hook RequestEndHaulingObjectiveAndPhase (%lu); hauling contracts won't end", err);
    }
}

static void HookPhaseEnds() {
    uint8_t* slot = g_offlineServiceVtbl ? reinterpret_cast<uint8_t*>(g_offlineServiceVtbl + 0x48) : nullptr;
    const uint8_t* fn = slot ? reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(reinterpret_cast<uintptr_t>(slot))) : nullptr;
    if (!fn || !BytesMatch(fn, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 C1 48 81 EC D0 00 00 00")) {
        Log("[!] contracts: the offline mission service's RequestEndMissionPhase wasn't found; contracts run by their scripts won't end");
        return;
    }
    const uintptr_t hook = reinterpret_cast<uintptr_t>(&EndPhaseHook);
    DWORD err = 0;
    g_endPhaseOrig = reinterpret_cast<EndPhaseFn>(const_cast<uint8_t*>(fn));
    if (!WriteCode(slot, reinterpret_cast<const uint8_t*>(&hook), sizeof(hook), err)) {
        g_endPhaseOrig = nullptr;
        Log("[!] contracts: couldn't hook RequestEndMissionPhase (%lu)", err);
    }
}

static void Finish(Running& r) {
    r.step = kDone;
    uintptr_t actor = 0, player = 0;
    const uintptr_t log = g_endMission && GetLocalPlayer(actor, player) ? EntityComponent(player, "SCPlayerMissionLog") : 0;
    const uintptr_t entry = log && g_findEntry ? g_findEntry(log, r.mission) : 0;
    const int reward = entry && g_totalReward ? g_totalReward(entry) : 0;
    g_objectiveStep = "EndMission";
    TakePayment(r.mission);
    if (log) g_endMission(log, r.mission, 4, 1, 0, "None", 0);
    const uintptr_t wallet = reward > 0 && g_updateBalance ? EntityComponent(player, "IWallet") : 0;
    g_objectiveStep = "paying";
    const bool paid = wallet && g_updateBalance(wallet, 1, reward);
    Log("[contracts] '%s': contract complete%s; reward %d aUEC %s (wallet %lld)", r.contract, log ? "" : " (NO mission log - not ended)",
        reward, paid ? "paid" : wallet ? "NOT paid (the wallet refused)" : "NOT paid",
        static_cast<long long>(wallet ? Rd<int64_t>(wallet + kWalletUec) : -1));
    SaveWallet();
}

static void UpdateRunning(DWORD now) {
    static DWORD last = 0;
    if (now - last < 1000 || !g_runningCount) return;
    last = now;
    const int command = TestCommand();
    const LONG ended = InterlockedExchange(&g_emFinished, -1);
    uintptr_t actor = 0, player = 0;
    double you[3] = {};
    const bool live = GetLocalPlayer(actor, player);
    if (live) Vec3Out(player, 0x318, you);
    Running* newest = nullptr;
    Running* fighting = nullptr;
    for (int i = 0; i < g_runningCount; ++i) {
        Running& r = g_running[i];
        if (r.step == kDone) continue;
        g_objectiveStep = "updating a contract";
        if (r.step == kPending) { if (live) StartPending(r, now); continue; }
        newest = &r;
        const Act act = r.flow->steps[r.step].act;
        double at[3];
        uintptr_t zone = 0;
        if (!live || !TargetOf(r, at, zone)) continue;
        const double d = Distance(at, you);
        if (act == Act::Away) {
            if (d > kAwayM) { Log("[contracts] '%s': you're out (%.0f km)", r.contract, d / 1000.0); NextStep(r, now); }
        } else if (act == Act::Deliver && !r.dropoff) {
            if (d < kHomeM) { Log("[contracts] '%s': you're back (%.0f m)", r.contract, d); NextStep(r, now); }
        } else if (act == Act::Travel || act == Act::Deliver) {
            if (r.ground < 0 && d < kProbeM) {
                r.ground = ProbeGround(zone, at, d);
                if (r.ground >= 0) Log("[contracts] '%s': the location is %s", r.contract, r.ground ? "on the ground" : "in space");
            }
            if (r.ground >= 0 && d < (r.ground ? kNearGroundM : kNearSpaceM)) {
                Log("[contracts] '%s': you reached the location (%.0f m)", r.contract, d);
                NextStep(r, now);
            }
        } else if (act == Act::Fight) {
            if (!r.started) { if (d < (r.ground == 1 ? kNearGroundM : kNearSpaceM)) StartFight(r, zone, at); }
            else if (r.ground == 1) {
                const int before = r.npcCount;
                UpdateGroundHostiles(r);
                if (r.npcCount < before) Log("[contracts] '%s': %d hostile(s) left", r.contract, r.npcCount);
                if (!r.npcCount) NextStep(r, now);
            } else if (!fighting) fighting = &r;
        } else if (act == Act::Search) {
            if (d > (r.ground == 1 ? kSearchGroundM : kSearchSpaceM)) r.since = now;
            else if (now - r.since >= kSearchMs) NextStep(r, now);
        }
    }
    if (command == 1 && newest) { Log("[contracts] test: next step for '%s'", newest->contract); NextStep(*newest, now); }
    else if (command == 2 && newest) { Log("[contracts] test: completing '%s'", newest->contract); Finish(*newest); }
    else if (command == 3 && newest && live) {
        const int before = newest->npcCount;
        const int n = SpawnGroundHostiles(*newest, VCall<uintptr_t>(player, 0x6B8), you);
        Log("[contracts] test: %d ground hostile(s) around you for '%s'", n - before, newest->contract);
    } else if (command == 4 && live) {
        Running* c = nullptr;
        for (int i = 0; i < g_runningCount; ++i)
            if (g_running[i].step >= 0 && g_running[i].step != kDone && g_running[i].flow->steps[1].act == Act::Fight) c = &g_running[i];
        const uintptr_t zone = VCall<uintptr_t>(player, 0x6B8);
        const uintptr_t planet = PlanetZoneOf(zone);
        double up[3], high[3];
        if (c && planet && UpAt(planet, you, up)) {
            for (int k = 0; k < 3; ++k) high[k] = you[k] + up[k] * 80.0;
            if (c->step == 0) NextStep(*c, now);
            c->ground = 1;
            c->started = true;
            c->npcCount = 0;
            Log("[contracts] test: %d hostile(s) dropped for '%s'", SpawnGroundHostiles(*c, zone, high), c->contract);
        }
    } else if (command == 5) {
        for (int i = 0; i < g_runningCount; ++i)
            for (int k = 0; k < g_running[i].npcCount; ++k) RemoveEntityById(g_running[i].npcs[k]);
        Log("[contracts] test: ground hostiles removed");
    }
    if (ended < 0 || !fighting) return;
    if (ended == 10) { fighting->started = false; Log("[contracts] '%s': the hostiles left; they come back when you're there", fighting->contract); }
    else NextStep(*fighting, now);
}

static int LogFault(const EXCEPTION_POINTERS* ep, const char* what) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const uintptr_t game = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    char trail[640] = {}, type[96] = {};
    size_t len = 0;
    __try {
        if (er->ExceptionCode == 0xE06D7363 && er->NumberParameters >= 4) {
            const uintptr_t image = er->ExceptionInformation[3];
            const int* info = reinterpret_cast<const int*>(er->ExceptionInformation[2]);
            const int* types = reinterpret_cast<const int*>(image + info[3]);
            const int* first = reinterpret_cast<const int*>(image + types[1]);
            sprintf_s(type, " (%s)", reinterpret_cast<const char*>(image + first[1] + 16));
        }
        CONTEXT ctx = *ep->ContextRecord;
        for (int i = 0; i < 16 && ctx.Rip && len + 48 < sizeof(trail); ++i) {
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(ctx.Rip), &mod) && reinterpret_cast<uintptr_t>(mod) == game) {
                len += sprintf_s(trail + len, sizeof(trail) - len, " %llx", static_cast<unsigned long long>(ctx.Rip - game + 0x140000000ull));
            } else {
                char path[MAX_PATH] = "?";
                if (mod) GetModuleFileNameA(mod, path, MAX_PATH);
                const char* name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
                len += sprintf_s(trail + len, sizeof(trail) - len, " %s+%llx", name,
                                 static_cast<unsigned long long>(ctx.Rip - reinterpret_cast<uintptr_t>(mod)));
            }
            DWORD64 image = 0;
            if (PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr)) {
                PVOID data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &data, &frame, nullptr);
            } else {
                ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Log("[contracts] fault %08lx%s %s (address %llx); through%s", er->ExceptionCode, type, what,
        static_cast<unsigned long long>(er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0), trail);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void UpdateRunningSafe(DWORD now) {
    __try { UpdateRunning(now); } __except (LogFault(GetExceptionInformation(), "while updating our contracts")) {}
}

static int PatchReputationServiceChecks(const Section& text, const Section& rdata) {
    static const char* const kLoad = "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 18 48 8B 08";
    const char* const load = kLoad;
    const uint8_t* msg = FindCString(rdata, "Couldn't access reputation service internal");
    const uint8_t* lea = msg ? FindRipLea(text, 0x4C, 0x8D, 0x05, msg) : nullptr;
    const uint8_t* services = nullptr;
    for (int back = 0; lea && back < 0x120 && !services; ++back)
        if (BytesMatch(lea - back, load)) services = lea - back + 7 + Rel32(lea - back + 3);
    if (!services) return -1;
    static const struct { const char* tail; uint8_t patch[13]; size_t n; } kForms[] = {
        { "48 8B 51 58 48 8B C8 FF D2",    { 0x48, 0x8B, 0xC8, 0xE3, 0x07, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x58, 0x90 }, 12 },
        { "4C 8B 41 58 48 8B C8 41 FF D0", { 0x48, 0x8B, 0xC8, 0xE3, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x58, 0x90, 0x90 }, 13 },
    };
    int patched = 0;
    for (const auto& form : kForms) {
        char pattern[96];
        sprintf_s(pattern, "%s %s", load, form.tail);
        uint8_t* sites[32];
        const int found = FindPattern(text, pattern, sites, 32);
        for (int i = 0; i < found && i < 32; ++i) {
            DWORD err = 0;
            if (sites[i] + 7 + Rel32(sites[i] + 3) == services && WriteCode(sites[i] + 13, form.patch, form.n, err)) ++patched;
        }
    }
    return patched;
}

static bool SkipGameRewards(const Section& text, const Section& rdata) {
    const uint8_t* msg = FindCString(rdata, "CSCPlayerMissionLog::SendRewards No authority");
    uint8_t* lea = msg ? FindRipLea(text, 0x4C, 0x8D, 0x05, msg) : nullptr;
    if (!lea || !BytesMatch(lea - 14, "FF 90 ?? ?? ?? ?? 84 C0 0F 85")) return false;
    static const uint8_t nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    DWORD err = 0;
    return WriteCode(lea - 6, nops, sizeof(nops), err);
}

static void FindPayout(const Section& text, const Section& rdata) {
    int n = 0;
    const uint8_t* walk = FindUniquePattern(text,
        "48 8D 83 40 04 00 00 EB 0C 48 8D 8B 20 04 00 00 E8 ?? ?? ?? ?? 48 8B 08 48 8B 50 08 48 3B CA 74 25 4C 8B 07 "
        "66 0F 1F 44 00 00 4C 39 41 08 75 0A 48 8B 47 08 48 39 41 10 74 1E 48 81 C1 50 02 00 00", n);
    uint8_t* fn = walk ? FunctionStart(walk) : nullptr;
    if (fn && BytesMatch(fn, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 8B FA"))
        g_findEntry = reinterpret_cast<FindEntryFn>(fn);
    const uint8_t* any = FindUniquePattern(text, "B9 28 04 00 00 41 B8 40 04 00 00 44 0F 44 C1 49 8B 04 38 49 8B 54 38 08", n);
    fn = any ? FunctionStart(any) : nullptr;
    if (fn && BytesMatch(fn, "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9"))
        g_findEntryAny = reinterpret_cast<FindEntryFn>(fn);
    g_totalReward = reinterpret_cast<TotalRewardFn>(FunctionNaming(text, rdata, "int __cdecl CMissionLogEntry::GetTotalReward(void) const",
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 60"));
    g_updateBalance = reinterpret_cast<UpdateBalanceFn>(FunctionNaming(text, rdata, "CWallet::UpdateCurrencyBalanceValue",
        "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 C0"));
}

using AsyncUpdateBalanceFn = void*(__fastcall*)(uintptr_t wallet, void* out, uint8_t currency, int64_t amount, void* reason, void* a6);
static AsyncUpdateBalanceFn g_asyncUpdateBalanceOrig = nullptr;

static void* __fastcall AsyncUpdateBalanceHook(uintptr_t wallet, void* out, uint8_t currency, int64_t amount, void* reason, void* a6) {
    if (amount && g_updateBalance) {
        char ok = 0;
        __try { ok = g_updateBalance(wallet, currency, amount); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("[shops] %s of %lld (currency %u) taken through the wallet here: %s", amount < 0 ? "payment" : "deposit",
            static_cast<long long>(amount < 0 ? -amount : amount), currency, ok ? "done" : "FAILED");
        if (ok) return g_asyncUpdateBalanceOrig(wallet, out, currency, 0, reason, a6);
    }
    return g_asyncUpdateBalanceOrig(wallet, out, currency, amount, reason, a6);
}

static void HookShopPayments(const Section& text, const Section& rdata) {
    uint8_t* fn = FunctionNaming(text, rdata, "CWallet::AsyncUpdateCurrencyBalance",
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC C0 00 00 00");
    const bool ok = fn && g_updateBalance && HookFunction(fn, 15, reinterpret_cast<void*>(&AsyncUpdateBalanceHook),
                                                          reinterpret_cast<void**>(&g_asyncUpdateBalanceOrig));
    Log("[shops] wallet payments %s", ok ? "taken here (no ledger service offline)" : "NOT hooked: shops can't take money");
}

static void HookContractSteps(const Section& text, const Section& rdata) {
    const int checks = PatchReputationServiceChecks(text, rdata);
    if (checks < 0) Log("[!] contracts: reputation service checks: the internal services weren't found; finishing a contract may freeze the game");
    else Log("[contracts] reputation service checks: %d site(s) patched (offline a contract's reputation reward fails instead of freezing)", checks);
    const bool skip = SkipGameRewards(text, rdata);
    if (!skip) Log("[!] contracts: the game's reward processing couldn't be switched off; finishing a contract may freeze the game");
    FindPayout(text, rdata);
    Log("[contracts] rewards: game's processing %s, mission log entry %s (client's %s), total reward %s, wallet update %s", skip ? "off" : "ON",
        g_findEntry ? "ok" : "MISSING", g_findEntryAny ? "ok" : "MISSING", g_totalReward ? "ok" : "MISSING", g_updateBalance ? "ok" : "MISSING");
    HookShopPayments(text, rdata);
    int n = 0;
    g_endMission = reinterpret_cast<EndMissionFn>(FindUniquePattern(text,
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 40 48 8B F9 41 8B F1 48 83 C1 08 41 8B E8 4C 8B F2 E8", n));
    const bool payHook = g_endMission && HookFunction(reinterpret_cast<uint8_t*>(g_endMission), 10,
        reinterpret_cast<void*>(&EndMissionHook), reinterpret_cast<void**>(&g_endMissionOrig));
    if (!payHook) Log("[!] contracts: EndMission not hooked; contracts finished by their own scripts won't pay");
    uint8_t* fn = FunctionNaming(text, rdata,
        "void __cdecl CEnvironmentalMissionManager::OnMissionModuleFinished(const class EntityId &,enum EMissionModuleStopReason)",
        "44 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57");
    const bool emHook = fn && HookFunction(fn, 15, reinterpret_cast<void*>(&EmFinishedHook), reinterpret_cast<void**>(&g_emFinishedOrig));
    fn = FunctionNaming(text, rdata, "CActor::Kill", "4C 89 44 24 18 48 89 54 24 10 55 53 57 41 54");
    const bool killHook = fn && HookFunction(fn, 10, reinterpret_cast<void*>(&ActorKillHook), reinterpret_cast<void**>(&g_actorKillOrig));
    g_sendComms = reinterpret_cast<SendCommsFn>(FunctionNaming(text, rdata,
        "SendCommsNotification Record GUID [$$] is invalid - Mission: [$$], Player: $$[$$]", "48 8B C4 4C 89 40 18 48 89 50 10 55 53 56 57 41 56 41 57"));
    if (!g_sendComms) Log("[!] contracts: mission comms (SendCommsNotification) not found; no mission-giver calls");
    fn = FunctionNaming(text, rdata, "Succeeded to spawn delivery mission helper. entityId: $$, missionId: $$", "48 89 5C 24 10 48 89 4C 24 08 55 56 57 41 54");
    const bool helperHook = fn && HookFunction(fn, 10, reinterpret_cast<void*>(&HelperSpawnedHook), reinterpret_cast<void**>(&g_helperSpawnedOrig));
    Log("[contracts] contract steps: end mission %s, environmental mission end %s, actor deaths %s, delivery helpers %s",
        g_endMission ? "ok" : "MISSING", emHook ? "ok" : "MISSING", killHook ? "ok" : "MISSING", helperHook ? "ok" : "MISSING");
}

static void PrepareModule(uintptr_t module) {
    __try {
        bool created = Rd<uint8_t>(module + kModuleHasInstance) != 0, failed = Rd<uint8_t>(module + kModuleInstanceFailed) != 0;
        const bool create = !created && !failed;
        if (create) {
            VCall<void>(module, kCreateInstanceSlot, uintptr_t(0));
            created = Rd<uint8_t>(module + kModuleHasInstance) != 0;
            failed = Rd<uint8_t>(module + kModuleInstanceFailed) != 0;
        }
        Log("[contracts] our mission module %p: mission instance %s%s", reinterpret_cast<void*>(module),
            created ? "ready" : failed ? "FAILED (the game couldn't build the mission)" : "still missing", create ? " (created it now)" : "");
        if (!created || ModuleState(module) != 0) return;
        *reinterpret_cast<int*>(module + kModuleState) = 1;
        const uint8_t gained = 1;
        g_authorityOrig(module, &gained, 0, 0, 0);
        Log("[contracts] our mission module %p: asked the mission service again, state now %d", reinterpret_cast<void*>(module), ModuleState(module));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[contracts] fault while preparing our mission module %p", reinterpret_cast<void*>(module));
    }
}

static void StartWaitingModules() {
    if (!g_startMission) return;
    const DWORD now = GetTickCount();
    uintptr_t due[8], seen[8], prepare[8];
    int n = 0, s = 0, p = 0;
    AcquireSRWLockExclusive(&g_moduleLock);
    for (Module& m : g_modules) {
        if (!m.module || m.done || now - m.since < kStartAfterMs) continue;
        if (now - m.since > kWatchMs) { m.done = true; continue; }
        uint8_t id[16];
        if (!ModuleMission(m.module, id)) { m.done = true; continue; }
        if (!IsOurMission(id)) continue;
        if (!m.seen && s < 8) { m.seen = true; seen[s++] = m.module; }
        const int state = ModuleState(m.module);
        if (state == 0 && !m.prepared && g_authorityOrig && p < 8) {
            m.prepared = true; m.preparedAt = now;
            prepare[p++] = m.module;
            continue;
        }
        if (state != 1 || (m.prepared && now - m.preparedAt < kAnswerWaitMs)) continue;
        m.done = true;
        if (n < 8) due[n++] = m.module;
    }
    ReleaseSRWLockExclusive(&g_moduleLock);
    for (int i = 0; i < s; ++i) {
        Log("[contracts] our mission module %p is here (state %d)", reinterpret_cast<void*>(seen[i]), ModuleState(seen[i]));
        uint8_t id[16];
        if (ModuleMission(seen[i], id)) StartContractSafe(seen[i], id);
    }
    for (int i = 0; i < p; ++i) PrepareModule(prepare[i]);
    for (int i = 0; i < n; ++i) {
        __try {
            g_startMission(due[i], 2);
            Log("[contracts] started our mission module %p (state now %d)", reinterpret_cast<void*>(due[i]), ModuleState(due[i]));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[contracts] fault while starting our mission module %p", reinterpret_cast<void*>(due[i]));
        }
    }
}

static void JoinMissionEntities() {
    Joining joining[kMaxOurs];
    AcquireSRWLockExclusive(&g_joinLock);
    const int n = g_joiningCount;
    memcpy(joining, g_joining, sizeof(joining));
    g_joiningCount = 0;
    ReleaseSRWLockExclusive(&g_joinLock);
    if (!n) return;
    for (int i = 0; i < n; ++i) {
        const uint64_t player = MissionPlayer(joining[i].mission);
        if (!player || !g_addActivePlayer) { Log("[contracts] can't add you to the mission entity (player %d)", player != 0); continue; }
        __try {
            g_addActivePlayer(joining[i].entity, player, true);
            Log("[contracts] you joined mission entity %p", reinterpret_cast<void*>(joining[i].entity));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[contracts] fault while adding you to mission entity %p", reinterpret_cast<void*>(joining[i].entity));
        }
    }
}

static void ResolveMissionCreation(const Section& text, const Section& rdata) {
    HookMissionDiagnostics(text, rdata);
    HookMissionEntity(text, rdata);
    HookMissionModules(text, rdata);
    HookContractSteps(text, rdata);
    FindMissionStreamRadius(text);
    FindOfflineMissionService(text, rdata);
    HookPhaseEnds();
    HookPhases(text);
    HookHaulingEnds();
    HookAbandon(text, rdata);
    FindMissionLogCalls(text, rdata);
    uint8_t* fn = FunctionStart(FindLeaTo(text, FindCString(rdata, "CMissionFactory::CreateMission")));
    if (fn && BytesMatch(fn, "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10"))
        g_factoryCreate = reinterpret_cast<FactoryCreateFn>(fn);
    int matches = 0;
    g_copyPropertyMap = reinterpret_cast<CopyMapFn>(FindUniquePattern(text,
        "48 89 5C 24 20 41 56 48 83 EC 20 4C 8B F2 48 8B D9 48 3B CA 0F 84 ?? ?? ?? ?? 48 89 7C 24 40 48 8B 79 10 48 85 FF 74 ?? "
        "48 89 6C 24 30 48 89 74 24 38 48 8B 17 48 8B CB E8 ?? ?? ?? ?? 0F B6 57 48 48 8D 4F 28 48 8B 77 08", matches));
    if (!g_copyPropertyMap) Log("[!] contracts: property map copy not found; missions get new locations instead of the offer's");

    fn = FunctionStart(FindLeaTo(text, FindCString(rdata,
        "class std::shared_ptr<struct IUniverseHierarchyShardGraph> __cdecl CUniverseHierarchyShardStore::GetUniverseShardGraph(const char *) const")));
    if (fn && BytesMatch(fn, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57"))
        g_getShardGraph = reinterpret_cast<GetShardGraphFn>(fn);

    fn = FunctionStart(FindLeaTo(text, FindCString(rdata, "soc_dumpShardGraph: no shard id supplied and gEnv->GetShardId() is empty")));
    if (const uint8_t* p = fn ? FindInRange(fn, 0x100, "48 8B 0D ?? ?? ?? ?? E8") : nullptr) {
        const uint8_t* get = p + 12 + Rel32(p + 8);
        if (BytesMatch(get, "48 8B 81 ?? ?? ?? ?? C3")) {
            g_shardStoreOwner = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(p + 7 + Rel32(p + 3)));
            g_shardStoreOff = Rel32(get + 3);
        }
    }

    fn = FunctionStart(FindLeaTo(text, FindCString(rdata, "Mission service not accessible")));
    if (fn && BytesMatch(fn, "48 8B C4 55 41 57 48 8D A8")) {
        if (const uint8_t* p = FindInRange(fn, 0x600, "49 8B 57 18 88 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 48 8D 8D ?? ?? ?? ?? E8")) {
            g_urnFromEntity = CallTarget<UrnFromEntityFn>(p + 10);
            g_assignUrn = CallTarget<AssignUrnFn>(p + 25);
        }
        if (const uint8_t* p = FindInRange(fn, 0x600, "C6 85 ?? ?? ?? ?? 05 44 88 A5 ?? ?? ?? ?? E8"))
            g_stringInit = CallTarget<StringInitFn>(p + 14);
    }
}

static void* __fastcall AcceptHook(uintptr_t broker, void* out, const uint8_t* request);

void ResolveContractsApi(const Section& text, const Section& rdata) {
    const uint8_t* s = FindCString(rdata, "DebugAcceptGeneratorContract QueryAvailableContracts Success");
    uint8_t* fn = FunctionStart(FindLeaTo(text, s));
    if (fn && BytesMatch(fn, "48 89 4C 24 08 55 53 56 57 41 55")) {
        g_queryReply = reinterpret_cast<QueryReplyFn>(fn);
        FindMissionSystem(text, fn);
    }
    ResolveMissionCreation(text, rdata);

    s = FindCString(rdata, "DebugAcceptGeneratorContract AcceptContract(after creation) Sent");
    for (const uint8_t* lea = FindLeaTo(text, s); lea && g_autoAcceptCount < 4; lea = FindLeaTo(text, s, lea + 8)) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION rf = FunctionOf(lea, base);
        bool known = !rf;
        for (int i = 0; i < g_autoAcceptCount && !known; ++i) known = g_autoAccept[i] == rf->BeginAddress;
        if (!known) g_autoAccept[g_autoAcceptCount++] = rf->BeginAddress;
    }

    s = FindCString(rdata, "CContractBrokerOffline::AcceptContract is not implemented yet");
    const uint8_t* lea = s ? FindRipLea(text, 0x4C, 0x8D, 0x05, s) : nullptr;
    uint8_t* stub = lea ? const_cast<uint8_t*>(lea - 0xC) : nullptr;
    if (stub && BytesMatch(stub, "40 53 48 81 EC D0 00 00 00 48 8B DA 4C 8D 05")) {
        if (BytesMatch(stub + 0x5E, "E8") && BytesMatch(CallTarget<const uint8_t*>(stub + 0x5E), "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57"))
            g_resolve = CallTarget<ResolveFn>(stub + 0x5E);
        uintptr_t* slot = nullptr;
        int slots = 0;
        auto* q = reinterpret_cast<uintptr_t*>(rdata.base);
        for (size_t i = 0; i < rdata.size / 8; ++i)
            if (q[i] == reinterpret_cast<uintptr_t>(stub)) { slot = q + i; ++slots; }
        if (slots == 1) {
            const uintptr_t hook = reinterpret_cast<uintptr_t>(&AcceptHook);
            DWORD err = 0;
            if (WriteCode(reinterpret_cast<uint8_t*>(slot), reinterpret_cast<const uint8_t*>(&hook), sizeof(hook), err))
                g_acceptStub = reinterpret_cast<AcceptFn>(stub);
            else
                Log("[contracts] couldn't hook AcceptContract (%lu)", err);
        } else {
            Log("[contracts] AcceptContract vtable slot: %d found", slots);
        }
    }

    Log("[contracts] list filler %s, accept hook %s, mission system %s (generator +%Xh), %d auto-accept callers",
        g_queryReply ? "ok" : "MISSING", g_acceptStub ? "ok" : "MISSING",
        g_missionSystem ? "ok" : "MISSING", static_cast<unsigned>(g_generatorOff), g_autoAcceptCount);
    Log("[contracts] mission creation: factory %s, shard graph %s, shard store %s, owner %s/%s, string %s, accept answer %s, offline mission service %s",
        g_factoryCreate ? "ok" : "MISSING", g_getShardGraph ? "ok" : "MISSING", g_shardStoreOwner ? "ok" : "MISSING",
        g_urnFromEntity ? "ok" : "MISSING", g_assignUrn ? "ok" : "MISSING", g_stringInit ? "ok" : "MISSING",
        g_resolve ? "ok" : "MISSING", g_offlineServiceVtbl ? "ok" : "MISSING");
    Log("[contracts] mission log: AddMission %s, add player %s, details copy %s, phase handler %s, phase activation %s",
        g_addMission ? "ok" : "MISSING", g_addPlayer ? "ok" : "MISSING", g_copyDetails ? "ok" : "MISSING",
        g_makePhaseHandler ? "ok" : "MISSING", g_phaseActivate ? "ok" : "MISSING");
}

constexpr int kMaxDefs = 16384;
struct Def { uint8_t id[16]; uint8_t generator[16]; char name[88]; bool listed; };
static Def g_defs[kMaxDefs];
static int g_defCount = 0;

static uintptr_t Generator() {
    const uintptr_t ms = g_missionSystem ? *g_missionSystem : 0;
    return ms ? Rd<uintptr_t>(ms + g_generatorOff) : 0;
}

static uintptr_t NextNode(uintptr_t n) {
    uintptr_t r = Rd<uintptr_t>(n + 0x10);
    if (!Rd<uint8_t>(r + 0x19)) {
        while (!Rd<uint8_t>(Rd<uintptr_t>(r) + 0x19)) r = Rd<uintptr_t>(r);
        return r;
    }
    uintptr_t p = Rd<uintptr_t>(n + 8);
    while (!Rd<uint8_t>(p + 0x19) && n == Rd<uintptr_t>(p + 0x10)) { n = p; p = Rd<uintptr_t>(p + 8); }
    return p;
}

static int ReadDefinitions() {
    const uintptr_t gen = Generator();
    const uintptr_t head = gen ? Rd<uintptr_t>(gen + kContractMap) : 0;
    if (!head || Rd<uint8_t>(head + 0x19) != 1) { Log("[contracts] no contract generator"); return 0; }
    int n = 0;
    for (uintptr_t node = Rd<uintptr_t>(head); node != head && n < kMaxDefs; node = NextNode(node)) {
        Def& d = g_defs[n++];
        memcpy(d.id, reinterpret_cast<const void*>(node + 0x20), 16);
        memcpy(d.generator, reinterpret_cast<const void*>(node + 0x70), 16);
        const uintptr_t data = node + 0x30;
        const char* name = reinterpret_cast<const char*(__fastcall*)(uintptr_t)>(Rd<uintptr_t>(Rd<uintptr_t>(data)))(data);
        strncpy_s(d.name, name ? name : "?", _TRUNCATE);
        d.listed = false;
    }
    Log("[contracts] %d contract definitions", n);
    return n;
}

static bool ContainsNoCase(const char* s, const char* sub) {
    const size_t n = strlen(sub);
    for (; *s; ++s)
        if (_strnicmp(s, sub, n) == 0) return true;
    return false;
}

struct Scripted { uint8_t id[16]; bool runs; };
static Scripted* g_scripted = nullptr;
static int       g_scriptedCount = 0;
static bool      g_scriptedSwapped = false;

static int HexNibble(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void ReadContractScripts() {
    char path[MAX_PATH];
    if (g_scripted || !ShipsFilePath(path, sizeof(path))) return;
    char* slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (!slash) return;
    strcpy_s(slash + 1, sizeof(path) - static_cast<size_t>(slash + 1 - path), "contract_scripts.txt");
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) { Log("[contracts] %s is missing; no contracts listed", path); return; }
    g_scripted = static_cast<Scripted*>(calloc(kMaxDefs, sizeof(Scripted)));
    if (!g_scripted) { fclose(f); return; }
    static char line[8192];
    int runs = 0;
    while (g_scriptedCount < kMaxDefs && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* name = strchr(line, ' ');
        char* scripts = name ? strchr(name + 1, ' ') : nullptr;
        if (line[0] == '#' || !scripts || name - line != 32) continue;
        Scripted& s = g_scripted[g_scriptedCount];
        bool ok = true;
        for (int i = 0; i < 16 && ok; ++i) {
            const int hi = HexNibble(line[2 * i]), lo = HexNibble(line[2 * i + 1]);
            ok = hi >= 0 && lo >= 0;
            s.id[i] = static_cast<uint8_t>(hi << 4 | lo);
        }
        if (!ok) continue;
        s.runs = true;
        for (char* p = scripts + 1; p && *p; ) {
            char* bar = strchr(p, '|');
            if (bar) *bar = 0;
            char full[320];
            if (strcmp(p, "-") && (sprintf_s(full, "Libs/Subsumption/Missions/PU/%s", p) < 0 || !HaveScript(full))) s.runs = false;
            p = bar ? bar + 1 : nullptr;
        }
        runs += s.runs;
        ++g_scriptedCount;
    }
    fclose(f);
    Log("[contracts] %d contracts known; %d run CIG's way here (scripts we have, or hauling)", g_scriptedCount, runs);
}

static const Scripted* ScriptedOf(const uint8_t id[16]) {
    uint8_t swapped[16];
    memcpy(swapped, id + 8, 8);
    memcpy(swapped + 8, id, 8);
    for (int i = 0; i < g_scriptedCount; ++i) {
        if (!memcmp(g_scripted[i].id, id, 16)) return &g_scripted[i];
        if (!memcmp(g_scripted[i].id, swapped, 16)) { g_scriptedSwapped = true; return &g_scripted[i]; }
    }
    return nullptr;
}

static bool Listable(const Def& d) {
    if (ContainsNoCase(d.name, "Pyro") || ContainsNoCase(d.name, "Nyx") || ContainsNoCase(d.name, "NOTFORRELEASE")) return false;
    const Scripted* s = ScriptedOf(d.id);
    return s && s->runs;
}

static bool ListContractById(const uint8_t id[16], uint64_t handle, uint64_t player, int seed) {
    struct { uint8_t id[16]; uint64_t handle; int32_t seed, pad; uint64_t player; } capture{};
    memcpy(capture.id, id, 16);
    capture.handle = handle;
    capture.seed = seed;
    capture.player = player;
    alignas(16) uint8_t reply[64] = {};
    reply[0] = 1;
    __try { g_queryReply(&capture, reply); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ListContract(const Def& d, uint64_t handle, uint64_t player, int seed) {
    return ListContractById(d.id, handle, player, seed);
}

static uint64_t PlayerHandle(uint64_t player) {
    uint64_t handle = 0;
    __try {
        const uint64_t* h = VCall<uint64_t*>(*g_tp.entitySystem, 0x128, &handle, player);
        return h ? *h : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool OfflineBrokerInUse() {
    const uintptr_t ms = g_missionSystem ? *g_missionSystem : 0;
    const uintptr_t broker = ms ? Rd<uintptr_t>(ms + 0x80) : 0;
    return broker && Rd<uintptr_t>(Rd<uintptr_t>(broker) + 0x28) == reinterpret_cast<uintptr_t>(&AcceptHook);
}

static void Fill(int count) {
    const uint64_t player = LocalPlayerEntityId();
    const uint64_t handle = player ? PlayerHandle(player) : 0;
    if (!handle) { Log("[contracts] no player handle; contracts not listed"); return; }
    static int order[kMaxDefs];
    int n = 0;
    for (int i = 0; i < g_defCount; ++i) {
        const Def& d = g_defs[i];
        if (!d.listed && Listable(d) && !ContainsNoCase(d.name, "test") && !ContainsNoCase(d.name, "debug")
            && !ContainsNoCase(d.name, "tutorial"))
            order[n++] = i;
    }
    int known = 0;
    for (int i = 0; i < g_defCount; ++i) known += ScriptedOf(g_defs[i].id) != nullptr;
    Log("[contracts] %d of the generator's %d contracts are in contract_scripts.txt%s", known, g_defCount, g_scriptedSwapped ? " (ids matched swapped)" : "");
    for (int i = n - 1; i > 0; --i) { const int j = rand() % (i + 1); const int t = order[i]; order[i] = order[j]; order[j] = t; }
    char path[MAX_PATH];
    const DWORD pn = GetEnvironmentVariableA("SC_OFFLINE_MOD_LOG", path, MAX_PATH);
    char* slash = pn && pn < MAX_PATH ? strrchr(path, '\\') : nullptr;
    if (slash && static_cast<size_t>(slash + 1 - path) + 15 <= MAX_PATH) {
        strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), "list_first.txt");
        if (FILE* f = _fsopen(path, "r", _SH_DENYNO)) {
            char want[128];
            int front = 0;
            while (fgets(want, sizeof(want), f)) {
                want[strcspn(want, "\r\n")] = 0;
                for (int k = front; *want && k < n; ++k)
                    if (ContainsNoCase(g_defs[order[k]].name, want)) { const int t = order[front]; order[front++] = order[k]; order[k] = t; }
            }
            fclose(f);
            Log("[contracts] test: %d contracts named in list_first.txt go first", front);
        }
    }
    int listed = 0, failed = 0;
    for (int k = 0; k < n && listed < count; ++k) {
        Def& d = g_defs[order[k]];
        int sameGiver = 0;
        for (int i = 0; i < g_defCount; ++i)
            if (g_defs[i].listed && memcmp(g_defs[i].generator, d.generator, 16) == 0) ++sameGiver;
        if (sameGiver >= 3) continue;
        if (ListContract(d, handle, player, static_cast<int>(GetTickCount() ^ (order[k] * 2654435761u)))) {
            d.listed = true;
            ++listed;
        } else {
            ++failed;
        }
    }
    Log("[contracts] sent %d contracts to mobiGlas (%d faulted, %d candidates); Game.log has \"Failed to generate contract\" for any the game rejected",
        listed, failed, n);
}

struct Accepted { uint8_t mission[16], contract[16], definition[16]; uint8_t* details; int step; DWORD due; uint64_t player; uint32_t seed; bool regenerated; };
constexpr int   kMaxPending = 8;
constexpr DWORD kBuildDelayMs = 600;
static Accepted g_incoming[kMaxPending];
static int      g_incomingCount = 0;
static SRWLOCK  g_incomingLock = SRWLOCK_INIT;
static Accepted g_working[kMaxPending];
static int      g_workingCount = 0;

static bool AutoAccept(const void* returnAddress) {
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = FunctionOf(returnAddress, base);
    for (int i = 0; rf && i < g_autoAcceptCount; ++i)
        if (g_autoAccept[i] == rf->BeginAddress) return true;
    return false;
}

static const char* DefName(const uint8_t id[16]) {
    for (int i = 0; i < g_defCount; ++i)
        if (memcmp(g_defs[i].id, id, 16) == 0) return g_defs[i].name;
    return "?";
}

static void NewMissionId(uint8_t id[16]) {
    static volatile LONG counter = 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const uint64_t a = static_cast<uint64_t>(t.QuadPart) ^ (static_cast<uint64_t>(InterlockedIncrement(&counter)) * 0x9E3779B97F4A7C15ull);
    const uint64_t b = (a * 0xBF58476D1CE4E5B9ull) ^ __rdtsc() ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
    memcpy(id, &a, 8);
    memcpy(id + 8, &b, 8);
}

static void RegisterMission(Accepted& a, uint64_t player) {
    NoteOurMission(a.mission, player, a.details);
    uint8_t* const details = a.details;
    if (!details) { Log("[contracts] no contract details copied; the mission won't show in mobiGlas"); return; }
    memcpy(details + 0x80, a.mission, 16);
    NotePlan(a.mission, DefName(a.definition));
    __try {
        const uint64_t* b = *reinterpret_cast<uint64_t* const*>(details + 0xD0);
        const uint64_t* e = *reinterpret_cast<uint64_t* const*>(details + 0xD8);
        const uintptr_t ob = *reinterpret_cast<const uintptr_t*>(details + 0xB8), oe = *reinterpret_cast<const uintptr_t*>(details + 0xC0);
        Log("[contracts] details: %d objective(s), %d entr%s at +D0h", static_cast<int>((oe - ob) / 272), static_cast<int>((e - b) / 6),
            e - b == 6 ? "y" : "ies");
        for (int i = 0; b && b + 6 <= e && i < 6; b += 6, ++i)
            Log("[contracts]   +D0h[%d]: %llx %llx %llx %llx %llx %llx", i, b[0], b[1], b[2], b[3], b[4], b[5]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const uintptr_t ms = g_missionSystem ? *g_missionSystem : 0;
    const uintptr_t service = ms ? Rd<uintptr_t>(ms + 0x68) : 0;
    if (service && g_offlineServiceVtbl && Rd<uintptr_t>(service) == g_offlineServiceVtbl) {
        uint8_t tokens[0x18];
        memcpy(tokens, details + 0xB8, 0x18);
        if (!g_activateTokenOrig) memset(details + 0xB8, 0, 0x18);
        uintptr_t future[4] = {};
        VCall<void*>(service, 0x28, static_cast<void*>(future), static_cast<void*>(details), static_cast<uintptr_t>(0), player);
        memcpy(details + 0xB8, tokens, 0x18);
        Log("[contracts] mission registered with the mission service, you as its player");
        return;
    }
    uintptr_t actor = 0, entity = 0;
    const uintptr_t log = g_addMission && GetLocalPlayer(actor, entity) ? EntityComponent(entity, "SCPlayerMissionLog") : 0;
    if (!log) { Log("[contracts] no mission service and no player mission log; the mission won't show in mobiGlas"); return; }
    g_addMission(log, details + 0x80, details, 1, false, false);
    if (g_addPlayer) g_addPlayer(log, details + 0x80, player);
    Log("[contracts] no mission service (+68h = %p): mission added to your mission log only", reinterpret_cast<void*>(service));
}

static void LogProperties(const uint8_t* map) {
    __try {
        uintptr_t node = Rd<uintptr_t>(reinterpret_cast<uintptr_t>(map) + 8);
        const uintptr_t anchor = reinterpret_cast<uintptr_t>(map);
        for (int i = 0; node && node != anchor && i < 64; ++i) {
            const char* key = reinterpret_cast<const char*>(Rd<uintptr_t>(node + 0x20));
            const uint8_t type = Rd<uint8_t>(node + 0x48);
            const uint64_t* v = reinterpret_cast<const uint64_t*>(node + 0x28);
            if (type == 3) Log("[contracts]   property '%s' = \"%s\"", key ? key : "?", reinterpret_cast<const char*>(v[0]) ? reinterpret_cast<const char*>(v[0]) : "");
            else Log("[contracts]   property '%s' (type %u) = %016llx %016llx %016llx %016llx", key ? key : "?", type, v[0], v[1], v[2], v[3]);
            uintptr_t next = Rd<uintptr_t>(node);
            if (next) { while (Rd<uintptr_t>(next + 8)) next = Rd<uintptr_t>(next + 8); node = next; continue; }
            uintptr_t parent = Rd<uintptr_t>(node + 0x10);
            while (parent && parent != anchor && Rd<uintptr_t>(parent) == node) { node = parent; parent = Rd<uintptr_t>(node + 0x10); }
            node = parent;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[contracts]   (fault reading the properties)"); }
}

static bool CopyOfferProperties(const uint8_t key[16], uint8_t* map, bool byDefinition = false) {
    const uintptr_t ms = g_missionSystem ? *g_missionSystem : 0;
    const uintptr_t broker = ms ? Rd<uintptr_t>(ms + 0x80) : 0;
    if (!broker || !g_copyPropertyMap) return false;
    const uintptr_t b = Rd<uintptr_t>(broker + 0x30), e = Rd<uintptr_t>(broker + 0x38);
    uintptr_t found = 0;
    for (uintptr_t p = b; p && p + 0x188 <= e; p += 0x188)
        if (!memcmp(reinterpret_cast<const void*>(p + (byDefinition ? 0x10 : 0)), key, 16)) {
            found = p;
            if (!byDefinition) break;
        }
    if (!found) return false;
    g_copyPropertyMap(map, reinterpret_cast<const void*>(found + 0x20));
    Log("[contracts] the offer's %llu properties go to the mission build", Rd<unsigned long long>(found + 0x20 + 0x20));
    LogProperties(map);
    return true;
}

static bool BuildMission(const Accepted& a) {
    const uint64_t player = a.player;
    const uintptr_t ms = g_missionSystem ? *g_missionSystem : 0;
    const uintptr_t factory = ms ? Rd<uintptr_t>(ms + 0x78) : 0;
    const uintptr_t owner = g_shardStoreOwner ? *g_shardStoreOwner : 0;
    const uintptr_t store = owner ? Rd<uintptr_t>(owner + g_shardStoreOff) : 0;
    if (!player || !factory || !store || !g_getShardGraph || !g_urnFromEntity || !g_assignUrn || !g_stringInit) {
        Log("[contracts] can't create the mission (player %d, factory %d, shard store %d)", player != 0, factory != 0, store != 0);
        return false;
    }
    if (Rd<uintptr_t>(Rd<uintptr_t>(factory) + 0x10) != reinterpret_cast<uintptr_t>(g_factoryCreate)) {
        Log("[contracts] mission system +78h isn't the mission factory; mission not created");
        return false;
    }
    uintptr_t graph[2] = {};
    const char* shard = "local_shard";
    g_getShardGraph(store, graph, shard);
    if (!graph[0]) { shard = ""; g_getShardGraph(store, graph, shard); }
    if (!graph[0]) { Log("[contracts] no world (shard) graph; mission not created"); return false; }

    alignas(16) uint8_t request[0x100] = {};
    memcpy(request + 0x00, a.mission, 16);
    memcpy(request + 0x10, a.contract, 16);
    memcpy(request + 0x20, a.definition, 16);
    uint8_t* const map = request + 0x30;
    memcpy(map, &map, 8);
    memcpy(map + 8, &map, 8);
    const bool listedHere = CopyOfferProperties(a.contract, map);
    const bool offer = listedHere || (a.regenerated && CopyOfferProperties(a.definition, map, true));
    *reinterpret_cast<int32_t*>(request + 0x60) = static_cast<int32_t>(GetTickCount() ^ (a.mission[0] << 16) ^ a.mission[5]);
    g_stringInit(request + 0x88);
    *reinterpret_cast<uint16_t*>(request + 0x90) = 0x1E11;
    request[0x98] = 5;
    alignas(16) uint8_t urn[0x40] = {};
    g_assignUrn(request + 0x90, g_urnFromEntity(urn, player));

    uintptr_t future[4] = {};
    g_factoryCreate(factory, future, request, graph);
    NoteBuilt(a.mission);
    const uint32_t* m = reinterpret_cast<const uint32_t*>(a.mission);
    Log("[contracts] created mission %08x%08x%08x%08x for '%s' (shard '%s') %s; Game.log has CreateMission errors if the game refused it",
        m[0], m[1], m[2], m[3], DefName(a.definition), shard,
        offer ? "with the offer's locations" : "WITHOUT the offer's properties (the game picks new locations)");
    return true;
}

static void RegisterMissionSafe(Accepted& a) {
    __try { RegisterMission(a, a.player); } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[contracts] fault while registering the mission for '%s'", DefName(a.definition));
    }
}

static void BuildMissionSafe(const Accepted& a) {
    __try { BuildMission(a); } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[contracts] fault while creating the mission for '%s'", DefName(a.definition));
    }
}

static void* __fastcall AcceptHook(uintptr_t broker, void* out, const uint8_t* request) {
    if (AutoAccept(_ReturnAddress()) || !g_factoryCreate || !g_resolve || !g_copyDetails) return g_acceptStub(broker, out, request);
    Accepted a{};
    const uint8_t* listed = nullptr;
    __try {
        listed = *reinterpret_cast<const uint8_t* const*>(request + 0x30);
        if (!listed) return g_acceptStub(broker, out, request);
        memcpy(a.contract, request, 16);
        memcpy(a.definition, listed + 0xF0, 16);
        a.seed = *reinterpret_cast<const uint32_t*>(listed);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return g_acceptStub(broker, out, request); }
    a.player = EntityIdOfHandle(request + 0x10);
    if (!a.player) a.player = LocalPlayerEntityId();
    NewMissionId(a.mission);
    a.details = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x140));
    __try {
        if (a.details) g_copyDetails(a.details, listed);
    } __except (EXCEPTION_EXECUTE_HANDLER) { a.details = nullptr; }
    Log("[contracts] accept for '%s' by player %llu (seed %u) on thread %lu (mod thread %lu), details %s", DefName(a.definition),
        static_cast<unsigned long long>(a.player), a.seed, GetCurrentThreadId(), g_mainThread, a.details ? "copied" : "NOT copied");
    bool started = false;
    AcquireSRWLockExclusive(&g_incomingLock);
    if (g_incomingCount < kMaxPending) { g_incoming[g_incomingCount++] = a; started = true; }
    ReleaseSRWLockExclusive(&g_incomingLock);
    if (!started) return g_acceptStub(broker, out, request);
    alignas(16) uint8_t result[0x60] = {};
    result[0] = 1;
    memcpy(result + 8, a.mission, 16);
    return g_resolve(out, result);
}

constexpr int   kAutoContracts = 40;
constexpr DWORD kFillDelayMs   = 15000;

static void FillOnce() {
    int n = 0;
    __try { n = ReadDefinitions(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[contracts] fault reading contract definitions"); }
    g_defCount = n;
    if (!n) return;
    if (!OfflineBrokerInUse()) {
        Log("[contracts] the game is using the online contract broker (patch 10 missing?); contracts not listed");
        return;
    }
    ReadContractScripts();
    Fill(kAutoContracts);
}

static void ProcessAccepted() {
    AcquireSRWLockExclusive(&g_incomingLock);
    for (int i = 0; i < g_incomingCount && g_workingCount < kMaxPending; ++i) g_working[g_workingCount++] = g_incoming[i];
    g_incomingCount = 0;
    ReleaseSRWLockExclusive(&g_incomingLock);
    const DWORD now = GetTickCount();
    for (int i = 0; i < g_workingCount; ) {
        Accepted& a = g_working[i];
        if (a.step == 0) {
            RegisterMissionSafe(a);
            a.step = 1;
            a.due = now + kBuildDelayMs;
        } else if (static_cast<LONG>(now - a.due) >= 0) {
            BuildMissionSafe(a);
            g_working[i] = g_working[--g_workingCount];
            continue;
        }
        ++i;
    }
}

void ProcessContracts() {
    g_mainThread = GetCurrentThreadId();
    if (!g_queryReply || !g_missionSystem || !g_tp.ok) return;
    static bool  filled = false;
    static DWORD liveSince = 0;
    if (!filled) {
        uintptr_t actor, entity;
        bool live = false;
        __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        const DWORD now = GetTickCount();
        if (!live) liveSince = 0;
        else if (!liveSince) liveSince = now;
        else if (now - liveSince >= kFillDelayMs) {
            filled = true;
            FillOnce();
            {
                __try { RestoreWallet(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[contracts] fault while restoring the wallet"); }
            }
        }
    } else {
        static DWORD saved = 0;
        if (GetTickCount() - saved > 5000) {
            saved = GetTickCount();
            __try { SaveWallet(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    WidenMissionStreamRadius();
    JoinMissionEntities();
    EndScriptedContracts();
    AbandonContracts();
    StartWaitingModules();
    StartDetachedContracts(GetTickCount());
    UpdateRunningSafe(GetTickCount());
    ProcessAccepted();
}
