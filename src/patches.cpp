#include "patches.h"
#include "hooks.h"
#include "sco/caps.h"
#include "sco/signatures.h"
#include "sco/game/features.h"
#include "sco/game/offline.h"

// Every address below is a sco-core signature row (sco/game/offline.h, plus features'
// offline.or_loop_bound); StartOffline resolves them before these patches change any byte. A patch
// runs only when its capability is ready.
namespace off = sco::game::offline;

template <typename Cap>
static const Cap* FindCap(const Cap* caps, size_t n, const char* name) {
    for (size_t i = 0; i < n; ++i)
        if (strcmp(caps[i].name, name) == 0) return &caps[i];
    return nullptr;
}

bool OfflineCapReady(const char* name) {
    size_t n = 0;
    const off::Capability* caps = off::Capabilities(n);
    if (const off::Capability* c = FindCap(caps, n, name))
        sco::caps::SetFromSignatures(c->name, c->rows, c->count);
    else {
        const sco::game::features::Capability* fcaps = sco::game::features::Capabilities(n);
        const sco::game::features::Capability* f = FindCap(fcaps, n, name);
        if (!f) return false;
        sco::caps::SetFromSignatures(f->name, f->rows, f->count);
    }
    return sco::caps::Has(name);
}

void LogOfflinePatch(const char* name, const PatchStatus& st, const char* cap) {
    if (st.result == PatchResult::NotFound && !sco::caps::Has(cap))
        Log("[!] %s: not patched, %s isn't ready (see the [core] lines in mod.log)", name, cap);
    else
        LogPatch(name, st);
}

// Row "<base>.<k>" (numbered rows).
static uint8_t* SigN(const char* base, int k) {
    char id[64];
    snprintf(id, sizeof(id), "%s.%d", base, k);
    return sco::Sig(id);
}

static const uint8_t kIsOnlineCleared[] = { 0xC6, 0x80, 0x0E, 0x06, 0x00, 0x00, 0x00 };   // over offline.is_online_store (7 bytes)

static PatchStatus PatchIsOnlineStore() {
    PatchStatus st;
    st.expected = 1;
    if (!OfflineCapReady("offline.force_offline")) { st.result = PatchResult::NotFound; return st; }
    uint8_t* hit = sco::Sig("offline.is_online_store");
    st.sites = 1;
    if (!WriteCode(hit, kIsOnlineCleared, sizeof(kIsOnlineCleared), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    st.at = hit;
    return st;
}

static const uint8_t kNop6[] = { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 };
const uint8_t* g_isOnlineFlag;

static PatchStatus PatchHandshakeGate() {
    PatchStatus st;
    st.expected = off::kHandshakeSites;
    if (!OfflineCapReady("offline.handshake")) { st.result = PatchResult::NotFound; return st; }

    uint8_t* sites[off::kHandshakeSites] = {};
    for (int k = 1; k <= off::kHandshakeSites; ++k) sites[k - 1] = SigN("offline.handshake_gate", k) + off::kHandshakeJe;
    st.sites = off::kHandshakeSites;
    for (uint8_t* s : sites)
        if (!WriteCode(s, kNop6, sizeof(kNop6), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    g_isOnlineFlag = sco::Sig("offline.is_online_flag");
    st.result = PatchResult::Applied;
    st.at = sites[0];
    return st;
}

static const uint8_t kNop5[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };

static PatchStatus PatchMegamapCase() {
    PatchStatus st;
    st.expected = 1;
    if (!OfflineCapReady("offline.megamap_case")) { st.result = PatchResult::NotFound; return st; }
    uint8_t* call = sco::Sig("offline.megamap_tolower_call");
    st.sites = 1;
    if (!WriteCode(call, kNop5, sizeof(kNop5), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    st.at = call;
    return st;
}

static bool BootIntoAllSystems() {
    char value[16] = {};
    const DWORD n = GetEnvironmentVariableA("SC_OFFLINE_BOOT_MAP", value, sizeof(value));
    return n > 0 && n < sizeof(value) && _stricmp(value, "PU_All") == 0;
}

static const char* BootCap() { return BootIntoAllSystems() ? "offline.boot_pu_all" : "offline.boot_pu"; }

static PatchStatus PatchBootIntoPU() {
    PatchStatus st;
    st.expected = 1;
    if (!OfflineCapReady(BootCap())) { st.result = PatchResult::NotFound; return st; }
    uint8_t* site = sco::Sig("offline.boot_frontend_request");
    const uint8_t* pu = BootIntoAllSystems() ? sco::Sig("offline.str_megamap_pu_all") + off::kMegaMapPuAllName
                                             : sco::Sig("offline.str_pu");
    const uint8_t* scDefault = sco::Sig("offline.str_sc_default");
    st.sites = 1;

    const int32_t relRules = static_cast<int32_t>(scDefault - (site + 7));
    const int32_t relMap   = static_cast<int32_t>(pu - (site + 14));
    uint8_t patched[14];
    memcpy(patched, site, sizeof(patched));
    memcpy(patched + 3, &relRules, 4);
    memcpy(patched + 10, &relMap, 4);
    if (!WriteCode(site, patched, sizeof(patched), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    st.at = site;
    return st;
}

static bool BootIntoPURequested() {
    char value[16] = {};
    DWORD n = GetEnvironmentVariableA("SC_OFFLINE_BOOT_MAP", value, sizeof(value));
    return n > 0 && n < sizeof(value) && (_stricmp(value, "PU") == 0 || _stricmp(value, "PU_All") == 0);
}

static PatchStatus PatchOfflineDbPath() {
    PatchStatus st;
    st.expected = off::kOfflineDbSites;
    if (!OfflineCapReady("offline.db_path")) { st.result = PatchResult::NotFound; return st; }
    const uint8_t* user = sco::Sig("offline.str_user");
    uint8_t* sites[off::kOfflineDbSites] = {};
    for (int k = 1; k <= off::kOfflineDbSites; ++k) sites[k - 1] = SigN("offline.offline_db_path", k);
    st.sites = off::kOfflineDbSites;
    for (uint8_t* s : sites) {
        const int32_t rel = static_cast<int32_t>(user - (s + 7));
        uint8_t patched[7];
        memcpy(patched, s, 3);
        memcpy(patched + 3, &rel, 4);
        if (!WriteCode(s, patched, sizeof(patched), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    }
    st.result = PatchResult::Applied;
    st.at = sites[0];
    return st;
}

static PatchStatus PatchOrLoopBound() {
    namespace feat = sco::game::features;
    PatchStatus st;
    st.expected = feat::kOrLoopSites;
    if (!OfflineCapReady("offline.or_loop_bound")) { st.result = PatchResult::NotFound; return st; }
    static const uint8_t kFixed[] = { 0x41, 0x3B, 0x5D, 0x18, 0x90 };
    uint8_t* sites[feat::kOrLoopSites] = {};
    for (int k = 1; k <= feat::kOrLoopSites; ++k) sites[k - 1] = SigN("offline.or_loop_bound", k);
    st.sites = feat::kOrLoopSites;
    for (uint8_t* s : sites)
        if (!WriteCode(s + feat::kOrLoopPatch, kFixed, sizeof(kFixed), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    st.at = sites[0] + feat::kOrLoopPatch;
    return st;
}

// Needs the handshake patch (g_isOnlineFlag); the rows check that both gates read the byte after it.
static PatchStatus PatchAsopShardGate() {
    PatchStatus st;
    st.expected = 2;
    if (!g_isOnlineFlag || !OfflineCapReady("offline.asop_shard_gate")) { st.result = PatchResult::NotFound; return st; }
    uint8_t* open  = sco::Sig("offline.asop_gate_open") + off::kAsopGateOpenJne;          // jne rel32 (6 bytes)
    uint8_t* valid = sco::Sig("offline.asop_gate_validation") + off::kAsopGateValidationJne;   // jne rel8
    st.sites = 2;

    uint8_t jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    const int32_t rel = Rel32(open + 2) + 1;
    memcpy(jmp + 1, &rel, sizeof(rel));
    static const uint8_t kJmpShort[] = { 0xEB };
    if (!WriteCode(open, jmp, sizeof(jmp), st.err)
        || !WriteCode(valid, kJmpShort, sizeof(kJmpShort), st.err)) {
        st.result = PatchResult::ProtectFailed;
        return st;
    }
    st.result = PatchResult::Applied;
    st.at = open;
    return st;
}

static PatchStatus PatchNoRestrictedAreaImpound() {
    PatchStatus st;
    st.expected = 1;
    if (!OfflineCapReady("offline.no_impound")) { st.result = PatchResult::NotFound; return st; }
    uint8_t* f = sco::Sig("offline.restricted_area_impound");
    st.sites = 1;
    static const uint8_t kRet[] = { 0xC3 };
    if (!WriteCode(f, kRet, sizeof(kRet), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    st.at = f;
    return st;
}

static PatchStatus PatchOfflineMissionServices() {
    PatchStatus st;
    st.expected = 2;
    if (!OfflineCapReady("offline.mission_services")) { st.result = PatchResult::NotFound; return st; }
    uint8_t* defaults[2] = { sco::Sig("offline.use_service_default"), sco::Sig("offline.use_online_mission_service_default") };
    st.sites = 2;
    static const uint8_t kOff[] = { 0x41, 0xB9, 0x00, 0x00, 0x00, 0x00 };
    for (uint8_t* d : defaults)
        if (!WriteCode(d, kOff, sizeof(kOff), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    st.at = defaults[0];
    return st;
}

struct StubObject { void* const* vtable; };
static const uint8_t* g_servicesGlobal = nullptr;
static void*      g_envVtable[4];
static void*      g_servicesVtable[64];
static void*      g_socialVtable[64];
static StubObject g_stubEnv      = { g_envVtable };
static StubObject g_stubServices = { g_servicesVtable };
static StubObject g_stubSocial   = { g_socialVtable };

static uintptr_t __fastcall StubReturnZero(uintptr_t) { return 0; }
static uintptr_t __fastcall StubSocialApi(uintptr_t) { return reinterpret_cast<uintptr_t>(&g_stubSocial); }
static bool __fastcall StubSameGroup(uintptr_t, const uint64_t* a, const uint64_t* b) { return a && b && *a == *b; }
static uintptr_t __fastcall ServicesOrStub(uintptr_t) {
    const uintptr_t env = *reinterpret_cast<const uintptr_t*>(g_servicesGlobal);
    if (env)
        if (const uintptr_t services = VCall<uintptr_t>(env, off::kEnvServicesSlot)) return services;
    return reinterpret_cast<uintptr_t>(&g_stubServices);
}

static PatchStatus PatchSocialGroupQueries() {
    PatchStatus st;
    st.expected = off::kSocialGroupSites;
    if (!OfflineCapReady("offline.social_group")) { st.result = PatchResult::NotFound; return st; }
    uint8_t* sites[off::kSocialGroupSites];
    for (int k = 1; k <= off::kSocialGroupSites; ++k) sites[k - 1] = SigN("offline.social_group", k);
    const uint8_t* global = sco::Sig("offline.services_env");
    st.sites = off::kSocialGroupSites;
    uint8_t* slot = NearData(8);
    if (!slot) { st.result = PatchResult::ProtectFailed; return st; }
    g_servicesGlobal = global;
    for (void*& f : g_envVtable) f = reinterpret_cast<void*>(&StubReturnZero);
    for (void*& f : g_servicesVtable) f = reinterpret_cast<void*>(&StubReturnZero);
    for (void*& f : g_socialVtable) f = reinterpret_cast<void*>(&StubReturnZero);
    g_envVtable[off::kEnvServicesSlot / 8] = reinterpret_cast<void*>(&ServicesOrStub);
    g_servicesVtable[off::kSocialApiSlot / 8] = reinterpret_cast<void*>(&StubSocialApi);
    g_socialVtable[off::kSameGroupSlot / 8] = reinterpret_cast<void*>(&StubSameGroup);
    *reinterpret_cast<StubObject**>(slot) = &g_stubEnv;
    for (int i = 0; i < st.sites; ++i) {
        const int64_t rel = slot - (sites[i] + 7);
        if (rel < INT32_MIN || rel > INT32_MAX) { st.result = PatchResult::ProtectFailed; return st; }
        const int32_t rel32 = static_cast<int32_t>(rel);
        if (!WriteCode(sites[i] + 3, reinterpret_cast<const uint8_t*>(&rel32), 4, st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    }
    st.result = PatchResult::Applied;
    st.at = sites[0];
    return st;
}

static PatchStatus PatchServiceStreams() {
    // presence, analytics, trace, echo: echo has two stream functions.
    static const char* const kStreams[] = { "offline.service_stream.presence", "offline.service_stream.analytics",
                                            "offline.service_stream.trace", "offline.service_stream.echo.1",
                                            "offline.service_stream.echo.2" };
    PatchStatus st;
    st.expected = 4;
    if (!OfflineCapReady("offline.service_streams")) { st.result = PatchResult::NotFound; return st; }
    static const uint8_t kRet[] = { 0xC3 };
    for (const char* row : kStreams) {
        uint8_t* fn = sco::Sig(row);
        if (!WriteCode(fn, kRet, sizeof(kRet), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
        if (!st.at) st.at = fn;
    }
    st.sites = 4;
    st.result = PatchResult::Applied;
    return st;
}

static PatchStatus PatchRemoteConsoleLocal() {
    PatchStatus st;
    st.expected = 1;
    if (!OfflineCapReady("offline.remote_console")) { st.result = PatchResult::NotFound; return st; }
    st.at = sco::Sig("offline.remote_console_bind");
    st.sites = 1;
    static const uint8_t kLocalhost[] = { 0xB8, 0x7F, 0x00, 0x00, 0x01, 0x0F, 0x1F, 0x00 };
    if (!WriteCode(static_cast<uint8_t*>(st.at), kLocalhost, sizeof(kLocalhost), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    return st;
}

static PatchStatus PatchNoProfilerServer() {
    PatchStatus st;
    st.expected = 1;
    if (!OfflineCapReady("offline.profiler_server")) { st.result = PatchResult::NotFound; return st; }
    uint8_t* site = sco::Sig("offline.profiler_listen_branch");
    st.sites = 1;
    static const uint8_t kJmp[] = { 0xEB };
    if (!WriteCode(site, kJmp, sizeof(kJmp), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    st.at = site;
    return st;
}

static PatchStatus g_isOnlinePatch;
static PatchStatus g_handshakePatch;
static PatchStatus g_megamapCasePatch;
static PatchStatus g_bootIntoPUPatch;
static PatchStatus g_offlineDbPatch;
static PatchStatus g_orLoopPatch;
static PatchStatus g_asopPatch;
static PatchStatus g_noImpoundPatch;
static PatchStatus g_missionServicesPatch;
static PatchStatus g_socialGroupPatch;
static PatchStatus g_serviceStreamsPatch;
static PatchStatus g_remoteConsolePatch;
static PatchStatus g_profilerServerPatch;

bool ApplyOfflinePatches() {
    const Section text = FindSection(".text"), rdata = FindSection(".rdata");
    if (!text.base || !rdata.base) { g_isOnlinePatch.result = PatchResult::NotFound; return false; }
    g_text = text;
    g_rdata = rdata;
    g_isOnlinePatch = PatchIsOnlineStore();
    if (g_isOnlinePatch.result == PatchResult::Applied)
        g_handshakePatch = PatchHandshakeGate();
    g_megamapCasePatch = PatchMegamapCase();
    if (g_isOnlinePatch.result != PatchResult::Applied) return false;
    if (BootIntoPURequested())
        g_bootIntoPUPatch = PatchBootIntoPU();
    g_offlineDbPatch = PatchOfflineDbPath();
    g_orLoopPatch = PatchOrLoopBound();
    g_asopPatch = PatchAsopShardGate();
    g_noImpoundPatch = PatchNoRestrictedAreaImpound();
    g_missionServicesPatch = PatchOfflineMissionServices();
    g_socialGroupPatch = PatchSocialGroupQueries();
    g_serviceStreamsPatch = PatchServiceStreams();
    g_remoteConsolePatch = PatchRemoteConsoleLocal();
    g_profilerServerPatch = PatchNoProfilerServer();
    return true;
}

void LogOfflinePatches() {
    LogOfflinePatch("offline-force (IsOnline = 0)", g_isOnlinePatch, "offline.force_offline");
    LogOfflinePatch("local handshake (accept frontend connection)", g_handshakePatch, "offline.handshake");
    LogOfflinePatch("megamap keeps record-name case", g_megamapCasePatch, "offline.megamap_case");
    if (g_bootIntoPUPatch.result == PatchResult::NotRun)
        Log("[-] boot into PU: off (set SC_OFFLINE_BOOT_MAP=PU to enable)");
    else if (BootIntoAllSystems())
        LogOfflinePatch("boot into PU, every system (frontend request -> PU_All/SC_Default)", g_bootIntoPUPatch, BootCap());
    else
        LogOfflinePatch("boot into PU (frontend request -> PU/SC_Default)", g_bootIntoPUPatch, BootCap());
    LogOfflinePatch("offline player data from %USER%\\default_1.xml", g_offlineDbPatch, "offline.db_path");
    LogOfflinePatch("query OR-loop bound fix", g_orLoopPatch, "offline.or_loop_bound");
    LogOfflinePatch("ASOP / fleet manager allowed on offline shard", g_asopPatch, "offline.asop_shard_gate");
    LogOfflinePatch("landing zones don't destroy spawned ships", g_noImpoundPatch, "offline.no_impound");
    LogOfflinePatch("offline contract broker + mission service (mobiGlas contracts)", g_missionServicesPatch, "offline.mission_services");
    LogOfflinePatch("party/group checks without the social service (ramming, hostility, law)", g_socialGroupPatch, "offline.social_group");
    LogOfflinePatch("no reconnecting service streams (presence, analytics, trace, echo)", g_serviceStreamsPatch, "offline.service_streams");
    LogOfflinePatch("remote console only on this PC (127.0.0.1)", g_remoteConsolePatch, "offline.remote_console");
    LogOfflinePatch("no Optick profiler server (TCP 31318 closed)", g_profilerServerPatch, "offline.profiler_server");
}
