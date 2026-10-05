#include "patches.h"
#include "hooks.h"

static const uint8_t kIsOnlineStore[]   = { 0x44, 0x88, 0xA0, 0x0E, 0x06, 0x00, 0x00 };
static const uint8_t kIsOnlineCleared[] = { 0xC6, 0x80, 0x0E, 0x06, 0x00, 0x00, 0x00 };
static_assert(sizeof(kIsOnlineStore) == sizeof(kIsOnlineCleared), "patch must be same length");

static PatchStatus PatchIsOnlineStore(const Section& text) {
    PatchStatus st;
    st.expected = 1;
    const size_t n = sizeof(kIsOnlineStore);
    uint8_t* const end = text.base + text.size;
    uint8_t* hit = nullptr;
    for (uint8_t* p = text.base; p + n <= end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, kIsOnlineStore[0], static_cast<size_t>(end - p) - n + 1));
        if (!p) break;
        if (memcmp(p, kIsOnlineStore, n) != 0) continue;
        if (!hit) hit = p;
        ++st.sites;
    }
    if (st.sites != st.expected) { st.result = st.sites ? PatchResult::WrongMatchCount : PatchResult::NotFound; return st; }
    if (!WriteCode(hit, kIsOnlineCleared, n, st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    st.at = hit;
    return st;
}

static const uint8_t kNop6[] = { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 };
const uint8_t* g_isOnlineFlag;

static PatchStatus PatchHandshakeGate(const Section& text, const Section& rdata) {
    PatchStatus st;
    st.expected = 4;
    const uint8_t* guid = FindCString(rdata, "sessionGuid");
    if (!guid) { st.result = PatchResult::NotFound; return st; }

    uint8_t* sites[4] = {};
    const uint8_t* flag = nullptr;
    bool sameFlag = true;
    uint8_t* const end = text.base + text.size - 32;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x44, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x38 || (p[2] & 0xC7) != 0x05 || p[7] != 0x0F || p[8] != 0x84) continue;

        bool loadsGuid = false;
        for (const uint8_t* q = p + 13; q < p + 25; ++q)
            if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x15 && q + 7 + Rel32(q + 3) == guid) { loadsGuid = true; break; }
        if (!loadsGuid) continue;

        const uint8_t* cmpTarget = p + 7 + Rel32(p + 3);
        if (flag && cmpTarget != flag) sameFlag = false;
        flag = cmpTarget;
        if (st.sites < 4) sites[st.sites] = p + 7;
        ++st.sites;
    }
    if (st.sites != st.expected || !sameFlag) { st.result = st.sites ? PatchResult::WrongMatchCount : PatchResult::NotFound; return st; }
    for (uint8_t* s : sites)
        if (!WriteCode(s, kNop6, sizeof(kNop6), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    g_isOnlineFlag = flag;
    st.result = PatchResult::Applied;
    st.at = sites[0];
    return st;
}

static const uint8_t kNop5[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };
static const uint8_t kToLowerPrologue[] = { 0x4C, 0x8B, 0x09, 0x4C, 0x8B, 0xD1, 0x41, 0x0F, 0xB6, 0x01, 0x84, 0xC0 };

static PatchStatus PatchMegamapCase(const Section& text, const Section& rdata) {
    PatchStatus st;
    st.expected = 1;
    const uint8_t* help = FindCString(rdata, "Load a map, same usage as 'megamap' cvar.");
    uint8_t* reg = help ? FindRipLea(text, 0x48, 0x8D, 0x15, help) : nullptr;
    if (!reg) { st.result = PatchResult::NotFound; return st; }

    const uint8_t* handler = nullptr;
    for (uint8_t* q = reg + 7; q < reg + 7 + 32; ++q)
        if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x05) { handler = q + 7 + Rel32(q + 3); break; }
    if (!handler || handler < text.base || handler >= text.base + text.size - 0x80) { st.result = PatchResult::NotFound; return st; }

    static const uint8_t leaRcx[] = { 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8 };
    uint8_t* call = nullptr;
    for (const uint8_t* h = handler; h < handler + 0x60; ++h) {
        if (memcmp(h, leaRcx, 6) != 0 || memcmp(h + 10, leaRcx, 6) != 0) continue;
        const uint8_t* callee = h + 20 + Rel32(h + 16);
        if (callee < text.base || callee + sizeof(kToLowerPrologue) > text.base + text.size) continue;
        if (memcmp(callee, kToLowerPrologue, sizeof(kToLowerPrologue)) != 0) continue;
        call = const_cast<uint8_t*>(h + 15);
        ++st.sites;
    }
    if (st.sites != st.expected) { st.result = st.sites ? PatchResult::WrongMatchCount : PatchResult::NotFound; return st; }
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

static PatchStatus PatchBootIntoPU(const Section& text, const Section& rdata) {
    PatchStatus st;
    st.expected = 1;
    const uint8_t* frontend   = FindCString(rdata, "Frontend_Main");
    const uint8_t* scFrontend = FindCString(rdata, "SC_Frontend");
    const uint8_t* allMap     = BootIntoAllSystems() ? FindCString(rdata, "MegaMap.PU_All") : nullptr;
    const uint8_t* pu         = allMap ? allMap + 8 : FindCString(rdata, "PU");
    const uint8_t* scDefault  = FindCString(rdata, "SC_Default");
    if (!frontend || !scFrontend || !pu || !scDefault) { st.result = PatchResult::NotFound; return st; }

    uint8_t* site = nullptr;
    uint8_t* const end = text.base + text.size - 14;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x4C, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x05 || p + 7 + Rel32(p + 3) != scFrontend) continue;
        const uint8_t* q = p + 7;
        if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x15 || q + 7 + Rel32(q + 3) != frontend) continue;
        if (!site) site = p;
        ++st.sites;
    }
    if (st.sites != st.expected) { st.result = st.sites ? PatchResult::WrongMatchCount : PatchResult::NotFound; return st; }

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

static PatchStatus PatchOfflineDbPath(const Section& text, const Section& rdata) {
    PatchStatus st;
    st.expected = 2;
    const uint8_t* offlineDb = FindCString(rdata, "Libs/OfflineDB");
    const uint8_t* user      = FindCString(rdata, "%USER%");
    if (!offlineDb || !user) { st.result = PatchResult::NotFound; return st; }

    uint8_t* sites[2] = {};
    uint8_t* const end = text.base + text.size - 7;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x4C, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x05 || p + 7 + Rel32(p + 3) != offlineDb) continue;
        if (st.sites < 2) sites[st.sites] = p;
        ++st.sites;
    }
    if (st.sites != st.expected) { st.result = st.sites ? PatchResult::WrongMatchCount : PatchResult::NotFound; return st; }
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

static PatchStatus PatchOrLoopBound(const Section& text) {
    PatchStatus st;
    st.expected = 4;
    static const char* const kPattern = 
        "44 8B A4 24 E8 00 00 00 8B 8C 24 D8 00 00 00 FF C3 48 FF C6 49 81 C7 90 00 00 00 "
        "48 3B 74 24 68 0F 8C ?? ?? ?? ?? 4C 8D 77 78 89 6F 08 41 8B 45 18";
    static const uint8_t kFixed[] = { 0x41, 0x3B, 0x5D, 0x18, 0x90 };
    uint8_t* sites[4] = {};
    st.sites = FindPattern(text, kPattern, sites, 4);
    if (st.sites != st.expected) { st.result = st.sites ? PatchResult::WrongMatchCount : PatchResult::NotFound; return st; }
    for (uint8_t* s : sites)
        if (!WriteCode(s + 27, kFixed, sizeof(kFixed), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    st.at = sites[0] + 27;
    return st;
}

static PatchStatus PatchAsopShardGate(const Section& text, const Section& rdata) {
    PatchStatus st;
    st.expected = 2;
    if (!g_isOnlineFlag) { st.result = PatchResult::NotFound; return st; }
    const uint8_t* persisted = g_isOnlineFlag + 1;

    int n = 0;
    uint8_t* open = FindUniquePattern(text,
        "44 89 AD D0 00 00 00 44 38 2D ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 48 8B 51 08 48 8D 8D D0 00 00 00 E8", n);
    st.sites += n;
    uint8_t* valid = FindUniquePattern(text,
        "80 3D ?? ?? ?? ?? 00 75 ?? 48 8D 44 24 60 C7 44 24 60 E3 00 00 00 48 89 44 24 70 4C 8D 0D", n);
    st.sites += n;
    const uint8_t* msg = FindCString(rdata, "Can only perform ASOP operations in the PU.");
    if (!open || !valid || !msg
        || open + 14 + Rel32(open + 10) != persisted
        || valid + 7 + Rel32(valid + 2) != persisted
        || valid + 34 + Rel32(valid + 30) != msg) {
        st.result = st.sites > st.expected ? PatchResult::WrongMatchCount : PatchResult::NotFound;
        return st;
    }

    uint8_t jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    const int32_t rel = Rel32(open + 16) + 1;
    memcpy(jmp + 1, &rel, sizeof(rel));
    static const uint8_t kJmpShort[] = { 0xEB };
    if (!WriteCode(open + 14, jmp, sizeof(jmp), st.err)
        || !WriteCode(valid + 7, kJmpShort, sizeof(kJmpShort), st.err)) {
        st.result = PatchResult::ProtectFailed;
        return st;
    }
    st.result = PatchResult::Applied;
    st.at = open + 14;
    return st;
}

static PatchStatus PatchNoRestrictedAreaImpound(const Section& text, const Section& rdata) {
    PatchStatus st;
    st.expected = 1;
    uint8_t* f = FindUniquePattern(text,
        "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 38 FE FF FF 48 81 EC C8 02 00 00 33 D2 48 8B F1 E8", st.sites);
    const uint8_t* msg = FindCString(rdata, "$$: Vehicle '$$' [$$] impounded (or destroyed) by restricted area '$$' [$$]");
    const uint8_t* tag = FindCString(rdata, "Boundary Violation");
    if (!f || !msg || !tag || !BytesMatch(f + 0x59, "4C 8D 0D") || f + 0x60 + Rel32(f + 0x5C) != msg
        || !BytesMatch(f + 0x79, "4C 8D 05") || f + 0x80 + Rel32(f + 0x7C) != tag) {
        st.result = st.sites > 1 ? PatchResult::WrongMatchCount : PatchResult::NotFound;
        return st;
    }
    static const uint8_t kRet[] = { 0xC3 };
    if (!WriteCode(f, kRet, sizeof(kRet), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    st.at = f;
    return st;
}

static PatchStatus PatchOfflineMissionServices(const Section& text, const Section& rdata) {
    PatchStatus st;
    st.expected = 2;
    const char* const names[] = { "contract_broker.use_service", "contract_broker.use_online_mission_service" };
    uint8_t* defaults[2] = {};
    for (int i = 0; i < 2; ++i) {
        const uint8_t* name = FindCString(rdata, names[i]);
        uint8_t* lea = name ? FindRipLea(text, 0x48, 0x8D, 0x15, name) : nullptr;
        if (!lea || !BytesMatch(lea - 6, "41 B9 01 00 00 00") || !BytesMatch(lea - 0xF, "4C 8D 43")) {
            st.result = PatchResult::NotFound;
            return st;
        }
        defaults[i] = lea - 6;
        ++st.sites;
    }
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
        if (const uintptr_t services = VCall<uintptr_t>(env, 0x18)) return services;
    return reinterpret_cast<uintptr_t>(&g_stubServices);
}

static PatchStatus PatchSocialGroupQueries(const Section& text) {
    PatchStatus st;
    static const uint8_t kSlotB0[4] = { 0xB0, 0x00, 0x00, 0x00 };
    uint8_t* sites[64];
    const uint8_t* global = nullptr;
    const uint8_t* const end = text.base + text.size - 0xA0;
    for (uint8_t* p = text.base; p < end; ++p) {
        if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x0D) continue;
        const uint8_t* call = nullptr;
        for (const uint8_t* q = p + 7; q < p + 7 + 0x30 && !call; ++q)
            if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x01 && q[3] == 0xFF && q[4] == 0x50 && q[5] == 0x18) call = q + 6;
        if (!call) continue;
        const uint8_t* social = nullptr;
        for (const uint8_t* q = call; q < call + 0x18 && !social; ++q) {
            if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8B && (q[2] & 0xC0) == 0x40 && q[3] == 0x70) social = q + 4;
            else if (q[0] == 0xFF && (q[1] & 0xF8) == 0x50 && q[2] == 0x70) social = q + 3;
        }
        if (!social) continue;
        bool sameGroup = false;
        for (const uint8_t* q = social; q < social + 0x50 && !sameGroup; ++q)
            sameGroup = ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8B && (q[2] & 0xC0) == 0x80 && !memcmp(q + 3, kSlotB0, 4))
                     || (q[0] == 0xFF && (q[1] & 0xF8) == 0x90 && !memcmp(q + 2, kSlotB0, 4));
        if (!sameGroup) continue;
        const uint8_t* g = p + 7 + Rel32(p + 3);
        if (global && g != global) { st.result = PatchResult::WrongMatchCount; return st; }
        global = g;
        if (st.sites < 64) sites[st.sites] = p;
        ++st.sites;
    }
    st.expected = st.sites;
    if (!st.sites) { st.result = PatchResult::NotFound; return st; }
    if (st.sites > 64) { st.result = PatchResult::WrongMatchCount; return st; }
    uint8_t* slot = NearData(8);
    if (!slot) { st.result = PatchResult::ProtectFailed; return st; }
    g_servicesGlobal = global;
    for (void*& f : g_envVtable) f = reinterpret_cast<void*>(&StubReturnZero);
    for (void*& f : g_servicesVtable) f = reinterpret_cast<void*>(&StubReturnZero);
    for (void*& f : g_socialVtable) f = reinterpret_cast<void*>(&StubReturnZero);
    g_envVtable[0x18 / 8] = reinterpret_cast<void*>(&ServicesOrStub);
    g_servicesVtable[0x70 / 8] = reinterpret_cast<void*>(&StubSocialApi);
    g_socialVtable[0xB0 / 8] = reinterpret_cast<void*>(&StubSameGroup);
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

static const uint8_t* FindLeaFrom(const Section& text, const uint8_t* target, const uint8_t* from) {
    const uint8_t* const end = text.base + text.size - 7;
    for (const uint8_t* p = from; target && p < end; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, 0x8D, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p > text.base && (p[-1] == 0x48 || p[-1] == 0x4C) && (p[1] & 0xC7) == 0x05 && p + 6 + Rel32(p + 2) == target)
            return p - 1;
    }
    return nullptr;
}

static PatchStatus PatchServiceStreams(const Section& text, const Section& rdata) {
    static const char* const kServices[] = { "presence::v1::PresenceService", "analytics::v1::AnalyticsService",
                                           "trace::v1::TraceService", "echo::v1::EchoService" };
    PatchStatus st;
    st.expected = sizeof(kServices) / sizeof(kServices[0]);
    int services = 0;
    for (const char* service : kServices) {
        char name[256];
        sprintf_s(name, "auto __cdecl CAsyncClient<class sc::external::services::%s>::CreateClientStream::<lambda_3>::operator ()(const char *) const", service);
        const uint8_t* s = FindCString(rdata, name);
        bool found = false;
        for (const uint8_t* lea = FindLeaFrom(text, s, text.base); lea; lea = FindLeaFrom(text, s, lea + 8)) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(lea), &base, nullptr);
            uint8_t* fn = rf ? reinterpret_cast<uint8_t*>(base + rf->BeginAddress) : nullptr;
            if (!fn || lea - fn > 0x200) continue;
            if (fn[0] == 0xC3) { found = true; continue; }
            if (!BytesMatch(fn, "48 89 5C 24 18 55 56 57 41 56 41 57")) continue;
            static const uint8_t kRet[] = { 0xC3 };
            if (!WriteCode(fn, kRet, sizeof(kRet), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
            if (!st.at) st.at = fn;
            found = true;
        }
        services += found;
    }
    st.sites = services;
    st.result = services ? PatchResult::Applied : PatchResult::NotFound;
    return st;
}

static PatchStatus PatchRemoteConsoleLocal(const Section& text, const Section& rdata) {
    PatchStatus st;
    st.expected = 1;
    const uint8_t* lea = FindLeaFrom(text, FindCString(rdata, "Remote console listening on: %u\n"), text.base);
    for (const uint8_t* p = lea ? lea - 0x100 : nullptr; p && p < lea; ++p)
        if (BytesMatch(p, "33 C9 FF 15 ?? ?? ?? ?? 0F B7 CB 66 89 7C 24 48 89 44 24 4C")) {
            if (!st.at) st.at = const_cast<uint8_t*>(p);
            ++st.sites;
        }
    if (st.sites != st.expected) { st.result = st.sites ? PatchResult::WrongMatchCount : PatchResult::NotFound; return st; }
    static const uint8_t kLocalhost[] = { 0xB8, 0x7F, 0x00, 0x00, 0x01, 0x0F, 0x1F, 0x00 };
    if (!WriteCode(static_cast<uint8_t*>(st.at), kLocalhost, sizeof(kLocalhost), st.err)) { st.result = PatchResult::ProtectFailed; return st; }
    st.result = PatchResult::Applied;
    return st;
}

static PatchStatus PatchNoProfilerServer(const Section& text) {
    PatchStatus st;
    st.expected = 1;
    uint8_t* site = FindUniquePattern(text, "73 ?? 0F 1F 40 00 0F 1F 84 00 00 00 00 00 48 8B BD ?? ?? ?? ?? 0F B7 CE 66 44 89 67 10 44 89 7F 14 FF 15", st.sites);
    if (!site) { st.result = st.sites ? PatchResult::WrongMatchCount : PatchResult::NotFound; return st; }
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
    g_isOnlinePatch = PatchIsOnlineStore(text);
    if (g_isOnlinePatch.result == PatchResult::Applied)
        g_handshakePatch = PatchHandshakeGate(text, rdata);
    g_megamapCasePatch = PatchMegamapCase(text, rdata);
    if (g_isOnlinePatch.result != PatchResult::Applied) return false;
    if (BootIntoPURequested())
        g_bootIntoPUPatch = PatchBootIntoPU(text, rdata);
    g_offlineDbPatch = PatchOfflineDbPath(text, rdata);
    g_orLoopPatch = PatchOrLoopBound(text);
    g_asopPatch = PatchAsopShardGate(text, rdata);
    g_noImpoundPatch = PatchNoRestrictedAreaImpound(text, rdata);
    g_missionServicesPatch = PatchOfflineMissionServices(text, rdata);
    g_socialGroupPatch = PatchSocialGroupQueries(text);
    g_serviceStreamsPatch = PatchServiceStreams(text, rdata);
    g_remoteConsolePatch = PatchRemoteConsoleLocal(text, rdata);
    g_profilerServerPatch = PatchNoProfilerServer(text);
    return true;
}

void LogOfflinePatches() {
    LogPatch("offline-force (IsOnline = 0)", g_isOnlinePatch);
    LogPatch("local handshake (accept frontend connection)", g_handshakePatch);
    LogPatch("megamap keeps record-name case", g_megamapCasePatch);
    if (g_bootIntoPUPatch.result == PatchResult::NotRun)
        Log("[-] boot into PU: off (set SC_OFFLINE_BOOT_MAP=PU to enable)");
    else if (BootIntoAllSystems())
        LogPatch("boot into PU, every system (frontend request -> PU_All/SC_Default)", g_bootIntoPUPatch);
    else
        LogPatch("boot into PU (frontend request -> PU/SC_Default)", g_bootIntoPUPatch);
    LogPatch("offline player data from %USER%\\default_1.xml", g_offlineDbPatch);
    LogPatch("query OR-loop bound fix", g_orLoopPatch);
    LogPatch("ASOP / fleet manager allowed on offline shard", g_asopPatch);
    LogPatch("landing zones don't destroy spawned ships", g_noImpoundPatch);
    LogPatch("offline contract broker + mission service (mobiGlas contracts)", g_missionServicesPatch);
    LogPatch("party/group checks without the social service (ramming, hostility, law)", g_socialGroupPatch);
    LogPatch("no reconnecting service streams (presence, analytics, trace, echo)", g_serviceStreamsPatch);
    LogPatch("remote console only on this PC (127.0.0.1)", g_remoteConsolePatch);
    LogPatch("no Optick profiler server (TCP 31318 closed)", g_profilerServerPatch);
}
