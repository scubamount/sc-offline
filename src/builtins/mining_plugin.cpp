// mining: natural mining as a built-in plugin.
//
// The game builds each large-scale ecosystem cell with CBiomeBuilder::BuildLargeScaleEcoSystem. A
// cell that has only built its physics (rule flags 0x6) never gets its harvestables offline; this
// built-in detours that function and, for those cells only, ORs the spawning bit 0x8 into the flags
// (0x6 -> 0xE) before the native function runs once, unchanged otherwise. The native code keeps its
// own provider, location and depletion checks, transforms and batch submission; nothing is
// spawned or edited here, and no game state, context flag or CVar is written.
//
// Ported from src/mining.cpp of junikka/sc-offline-mining (GPL-3.0, like this repository: see
// LICENSE and the ChrisWareOffline note in README.md), whose manual experiment was confirmed in
// game on 9 October 2026 on an earlier game build. What changed in the port: the address is
// sco-core's `mining.cell` row (capability `game.mining`, include/sco/game/mining.h), found in any
// build and pinned by byte checks on every field read here; the detour goes through sco::hook;
// the drop's executable SHA-256 check, its fixed RVAs and its server and editor context bytes are
// gone. The only context test is sc-offline's own: the offline patches applied (g_tp, the spawner)
// and the game's online flag still 0.
//
// Off by default: sc-offline.ini `mining = on` (SC_OFFLINE_MINING) installs the hook at startup.
// `mining_debug = on` (SC_OFFLINE_MINING_DEBUG) adds the drop's counters to mod.log as
// "[mining/trace]" lines, once a second while they change. mining.status reports the state.
#include "builtins.h"
#include "../common.h"
#include "../patches.h"
#include "../spawner.h"
#include "../teleport.h"
#include "../version.h"
#include "sco/caps.h"
#include "sco/game/mining.h"
#include "sco/hook.h"
#include "sco/signatures.h"
#include <atomic>
#include <cstdio>

namespace {

namespace mining = sco::game::mining;

constexpr const char* kCap = "game.mining";
// The first instruction of mining.cell, mov [rsp+0x20], r9d: five whole bytes with no relative
// operand, pinned by the row's check at +0x000.
constexpr size_t kStolen = 5;

// void cell(builder, cell, component, lod, flags, page, option): mining.h has the layout.
using CellFn = void(__fastcall*)(uintptr_t builder, uintptr_t cell, uintptr_t component, uint32_t lod,
                                 uint32_t flags, uint64_t page, uint8_t option);

enum class State { Off, NoRows, HookFailed, Hooked };

CellFn g_original = nullptr;
State  g_state = State::Off;
char   g_why[96] = "";
bool   g_debug = false;
int    g_lastShown = -1;
DWORD  g_lastTraceAt = 0;
uint64_t g_lastTraceCells = 0;

std::atomic<bool> g_enabled{false};   // the promotion runs; cleared on unload
std::atomic<bool> g_faulted{false};   // a read faulted: no promotion until restart
std::atomic<uint64_t> g_cells{0}, g_promoted{0}, g_faults{0};
std::atomic<uint64_t> g_skipContext{0}, g_skipFlags{0}, g_skipLod{0}, g_skipBuilder{0}, g_skipCell{0};

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "mining", SCO_VERSION, "sc-offline",
};

bool ReadSwitch(const char* env, bool whenUnset) {
    char v[16];
    const DWORD n = GetEnvironmentVariableA(env, v, sizeof(v));
    if (!n || n >= sizeof(v)) return whenUnset;
    if (!_stricmp(v, "on") || !_stricmp(v, "1") || !_stricmp(v, "yes") || !_stricmp(v, "true")) return true;
    if (!_stricmp(v, "off") || !_stricmp(v, "0") || !_stricmp(v, "no") || !_stricmp(v, "false")) return false;
    return whenUnset;
}

void Fault() {
    g_faults.fetch_add(1, std::memory_order_relaxed);
    g_faulted.store(true);
}

// sc-offline's offline mode is up (the teleport and spawner rows resolved, as for the other
// built-ins) and the game's own online flag, read-only, is still 0.
bool ContextAllowed() {
    if (!g_enabled.load() || g_faulted.load()) return false;
    __try {
        return g_tp.ok && SpawnerReady() && g_isOnlineFlag && *g_isOnlineFlag == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { Fault(); return false; }
}

// The drop's rule: flags exactly 0x6, a requested LOD of 0 or 1, builder type 0 (the cell path;
// 1, 2 and 3 are the planet-side entity spawning), the builder's spawn gate open (the native
// code skips the spawn branch unless that byte is 0) and a harvestable LOD of 2..100 already
// built. Everything else passes through unchanged.
uint32_t PromotedFlags(uintptr_t builder, uintptr_t cell, uint32_t lod, uint32_t flags) {
    if (flags != mining::kRuleFlags) { g_skipFlags.fetch_add(1, std::memory_order_relaxed); return flags; }
    if (lod > 1) { g_skipLod.fetch_add(1, std::memory_order_relaxed); return flags; }
    if (!ContextAllowed()) { g_skipContext.fetch_add(1, std::memory_order_relaxed); return flags; }
    __try {
        if (!builder || !cell) { g_skipCell.fetch_add(1, std::memory_order_relaxed); return flags; }
        if (Rd<int>(builder + mining::kCellBuilderType) != 0 || Rd<uint8_t>(builder + mining::kCellSpawnGate) != 0) {
            g_skipBuilder.fetch_add(1, std::memory_order_relaxed);
            return flags;
        }
        const int previous = Rd<int>(cell + mining::kCellHarvestLod);
        if (previous < 2 || previous > 100) { g_skipCell.fetch_add(1, std::memory_order_relaxed); return flags; }
        return flags | mining::kRuleSpawnBit;
    } __except (EXCEPTION_EXECUTE_HANDLER) { Fault(); return flags; }
}

void __fastcall CellHook(uintptr_t builder, uintptr_t cell, uintptr_t component, uint32_t lod, uint32_t flags,
                         uint64_t page, uint8_t option) {
    g_cells.fetch_add(1, std::memory_order_relaxed);
    const uint32_t forwarded = PromotedFlags(builder, cell, lod, flags);
    if (forwarded != flags) g_promoted.fetch_add(1, std::memory_order_relaxed);
    // The native function does the rest, once: provider checks, depletion, batch submission and
    // the completed-LOD write. Exceptions from it propagate; nothing is retried here.
    g_original(builder, cell, component, lod, forwarded, page, option);
}

bool InstallHook() {
    uint8_t* at = sco::Sig("mining.cell");
    if (!at) { snprintf(g_why, sizeof(g_why), "the mining.cell row isn't OK"); return false; }
    const sco::hook::Error e = sco::hook::InstallDetour(at, kStolen, reinterpret_cast<void*>(&CellHook),
                                                        reinterpret_cast<void**>(&g_original));
    if (e != sco::hook::Error::None) {
        snprintf(g_why, sizeof(g_why), "the detour wasn't installed (%s)", sco::hook::ErrorName(e));
        return false;
    }
    return true;
}

int Shown() {   // what mining.status and the log say
    if (g_state == State::Off) return 0;
    if (g_state == State::NoRows) return 1;
    if (g_state == State::HookFailed) return 2;
    if (g_faulted.load()) return 3;
    return ContextAllowed() ? 5 : 4;
}

const char* StateText(int s) {
    static const char* const text[] = {
        "off (mining = off in sc-offline.ini)",
        "unavailable: this game build's addresses weren't found (see the [core] lines)",
        "unavailable: the hook couldn't be installed",
        "disabled after a read fault; restart the game",
        "waiting for the offline session",
        "active",
    };
    return text[s];
}

sco_result Status(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    const int s = Shown();
    snprintf(reply, size, "Natural mining %s%s%s; %llu cells seen, %llu promoted, %llu read faults", StateText(s),
             s == 2 ? ": " : "", s == 2 ? g_why : "", static_cast<unsigned long long>(g_cells.load()),
             static_cast<unsigned long long>(g_promoted.load()), static_cast<unsigned long long>(g_faults.load()));
    return SCO_OK;
}

void OnTick(const char*, const void*, void*) {
    const int s = Shown();
    if (s != g_lastShown) {
        Log("[mining] natural mining %s", StateText(s));
        g_lastShown = s;
    }
    if (!g_debug || g_state != State::Hooked) return;
    const DWORD now = GetTickCount();
    const uint64_t cells = g_cells.load(std::memory_order_relaxed);
    if (now - g_lastTraceAt < 1000 || cells == g_lastTraceCells) return;
    g_lastTraceAt = now;
    g_lastTraceCells = cells;
    // A promoted cell means the spawning bit was forwarded, not that every candidate produced an
    // entity: native provider, location and depletion checks still apply.
    Log("[mining/trace] active=%d cells=%llu promoted=%llu skipped(context=%llu flags=%llu lod=%llu builder=%llu cell=%llu) readFaults=%llu",
        s == 5, static_cast<unsigned long long>(cells), static_cast<unsigned long long>(g_promoted.load()),
        static_cast<unsigned long long>(g_skipContext.load()), static_cast<unsigned long long>(g_skipFlags.load()),
        static_cast<unsigned long long>(g_skipLod.load()), static_cast<unsigned long long>(g_skipBuilder.load()),
        static_cast<unsigned long long>(g_skipCell.load()), static_cast<unsigned long long>(g_faults.load()));
}

const sco_plugin_info* MiningQuery() { return &kInfo; }

sco_result MiningLoad(const sco_api* api, sco_plugin* self) {
    sco_result r = RegisterBuiltinCommand(api, self, kCap, "mining.status", "Mining status",
        "Whether natural mining is on, and how many ecosystem cells it has seen and promoted", Status);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r != SCO_OK) return r;   // the host releases what was registered
    g_debug = ReadSwitch("SC_OFFLINE_MINING_DEBUG", false);
    if (!ReadSwitch("SC_OFFLINE_MINING", false)) {
        g_state = State::Off;
    } else if (!sco::caps::Has(kCap)) {
        g_state = State::NoRows;
    } else if (!InstallHook()) {
        g_state = State::HookFailed;
        Log("[!] mining: %s", g_why);
    } else {
        g_state = State::Hooked;
        g_enabled.store(true);
    }
    return SCO_OK;
}

// The detour stays in place until the process ends (a worker thread may be inside it); it only
// passes calls through once the promotion is off.
void MiningUnload() { g_enabled.store(false); }

}  // namespace

void SetMiningCaps() {
    size_t n = 0;
    const mining::Capability* caps = mining::Capabilities(n);
    for (size_t i = 0; i < n; ++i) {
        const sco::Result r = sco::caps::SetFromSignatures(caps[i].name, caps[i].rows, caps[i].count);
        if (r != sco::Result::Ok) Log("[!] capability %s: %s", caps[i].name, sco::ResultName(r));
    }
}

const sco::plugins::Builtin kMiningBuiltin = { "mining", MiningQuery, MiningLoad, MiningUnload };
