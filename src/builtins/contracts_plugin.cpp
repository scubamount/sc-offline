// contracts: offline contracts as a built-in plugin.
//
// The built-in's tick subscription runs ProcessContracts: listing contracts in the mobiGlas 15 s
// after you spawn, building and running the missions you accept, paying rewards, and restoring
// and saving the wallet (the built-in's storage, data/storage/contracts.db, opened here; and
// data/wallet.txt, which players may edit). contracts.status reports where that stands, gated on
// the "contracts" capability (the mission system found). The mechanics, the mission detours, the
// wallet and its file stay in contracts.cpp.
#include "builtins.h"
#include "builtin_store.h"
#include "../contracts.h"
#include "../teleport.h"
#include <cstdio>

namespace {

bool g_ticking = false;   // the tick subscription owns ProcessContracts

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "contracts", SCO_VERSION, "sc-offline",
};

constexpr const char* kCap = "contracts";

sco_result Status(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    const ContractsStatus s = ReadContractsStatus();
    char wallet[48];
    if (s.wallet >= 0) snprintf(wallet, sizeof(wallet), "wallet %lld aUEC", s.wallet);
    else               snprintf(wallet, sizeof(wallet), "wallet not readable yet");
    if (!s.known)
        snprintf(reply, size, "No contracts listed yet (they're listed 15 s after you spawn); %s", wallet);
    else
        snprintf(reply, size, "%d contracts known, %d in the mobiGlas, %d running, %d starting; %s", s.known, s.listed,
                 s.running, s.starting, wallet);
    return SCO_OK;
}

void OnTick(const char*, const void*, void*) {
    if (g_tp.ok) ProcessContracts();
}

const sco_plugin_info* ContractsQuery() { return &kInfo; }

sco_result ContractsLoad(const sco_api* api, sco_plugin* self) {
    sco_result r = RegisterBuiltinCommand(api, self, kCap, "contracts.status", "Contracts status",
        "How many contracts are known, offered and running, and your wallet's balance", Status);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r != SCO_OK) return r;   // the host releases what was registered
    g_contractsStore.Open(api, self);   // before the first tick, which restores the wallet
    g_ticking = true;
    return SCO_OK;
}

// A crash leaves g_ticking set, so dllmain doesn't take ProcessContracts back.
void ContractsUnload() {
    g_contractsStore.Close();
    g_ticking = false;
}

}  // namespace

bool ContractsBuiltinOwnsTick() { return g_ticking; }

const sco::plugins::Builtin kContractsBuiltin = { "contracts", ContractsQuery, ContractsLoad, ContractsUnload };
