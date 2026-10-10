// Ship terminals (ASOP) offline: open, list, Deliver, Retrieve to a hangar pad on the ship lift, and
// Store. The research doc "Offline ship terminals (ASOP), ship lifts, personal hangars and ATC"
// (4.10.196.36804), sections 5-10; "rcN" below is a root cause from its section 2.
//
// Threading (doc 3.5): the hooks run on whichever game thread calls them. They read their
// arguments, look at the views the game thread publishes and queue work, all under g_lock. Every
// call into the game is made from ProcessFleet on the game thread (the window-message thread,
// every 100 ms), outside g_lock: a lift or ATC call may re-enter one of the hooks on this thread.
// Nothing here sleeps or waits; a step that needs the game to catch up returns and runs again on
// a later tick. Every read of game memory is inside __try and a fault drops that step.
#include "fleet.h"
#include "cvars.h"
#include "hooks.h"
#include "spawner.h"
#include "teleport.h"
#include "builtins/mover.h"
#include "sco/caps.h"
#include "sco/hook.h"
#include "sco/signatures.h"
#include "sco/game/asop.h"
#include <algorithm>
#include <vector>

namespace asop = sco::game::asop;

// A stored id this module took the unstow of, with no pad spawn of it pending (rc17).
bool Fleet_IsSettlingStoredId(uint64_t id);

namespace {

// ---- tunables (doc Appendix D) -----------------------------------------------------------------

constexpr double  kDeliverySpawnHeight = 1000.0;   // metres above you (zone-local +z, your zone)
constexpr DWORD   kDeliverySettleMs    = 3000;     // the delivered ship settles before it's stored
constexpr DWORD   kDeliveryGiveUpMs    = 30000;    // it never appeared
constexpr DWORD   kRefreshAfterStoreMs = 1500;
constexpr DWORD   kRefreshAfterClaimMs = 1500;
constexpr int32_t kClaimTimeoutSeconds = 8;        // SInsuranceComponentConfig +0x48 (default 120)
constexpr DWORD   kRetrieveGiveUpMs    = 45000;    // cancel a Retrieve the ATC can't finish
constexpr DWORD   kRetrieveWatchGapMs  = 60000;    // a longer gap between sightings starts a new watch
constexpr DWORD   kRetrieveRecentMs    = 20000;    // cancel only while the ATC is still trying
constexpr DWORD   kPadSpawnGiveUpMs    = 15000;    // the landing area never streamed in
constexpr DWORD   kLiftLowerMs         = 45000;    // longest wait for ClosedIdle
constexpr DWORD   kLiftUnseenMs        = 3000;     // the close request was never handled
constexpr DWORD   kLiftOpenFallbackMs  = 4000;     // Open, if the spawn's own event didn't raise the lift
constexpr DWORD   kLiftWatchMs         = 90000;
constexpr DWORD   kRetrievedSpawnMs    = 90000;    // OnVehicleSpawned may match a retrieved ship this long
constexpr DWORD   kRaiseAfterStoreMs   = 2000;
constexpr DWORD   kStoreCheckMs        = 15000;    // a stored ship still in the world after this: the store failed

constexpr size_t kVehicleDataSize = 0x138;   // SVehicleProviderData
constexpr size_t kVehicleUrn      = 0xA8;    // CSCURN in a list entry
constexpr size_t kVehicleUrnSize  = 0x28;
constexpr size_t kUrnId           = 0x10;    // the 16-byte id in a CSCURN
constexpr size_t kProviderBegin   = 0x78, kProviderEnd = 0x80;
constexpr size_t kRequestClass    = 0x98;    // SATCActionRequest: IEntityClass*
constexpr size_t kLiftStateField  = 0x60;    // CSCLoadingPlatformManager: int32 state

// Ship lift states (doc 10.1), by name.
enum LiftState : int32_t {
    kOpeningFrontGate = 0, kOpeningLoadingGate = 1, kRaisingPlatform = 2, kOpenIdle = 3, kClosingFrontGate = 4,
    kClosingLoadingGate = 5, kLoweringPlatform = 6, kClosedIdle = 7, kObstructedAtLoadingPlatformLower = 8,
    kObstructedAtFrontGateClose = 9,
};
const char* LiftStateName(int32_t s) {
    static const char* const kNames[] = { "OpeningFrontGate", "OpeningLoadingGate", "RaisingPlatform", "OpenIdle",
                                          "ClosingFrontGate", "ClosingLoadingGate", "LoweringPlatform", "ClosedIdle",
                                          "ObstructedAtLoadingPlatformLower", "ObstructedAtFrontGateClose" };
    return s >= 0 && s < 10 ? kNames[s] : "unknown (not seen yet)";
}

// ---- features: one per capability group ---------------------------------------------------------

enum Feature { kTerminal, kCaller, kList, kDeliver, kClaim, kRetrieve, kLift, kStore, kFeatureCount };
struct FeatureState {
    const char* cap;     // sco::game::asop capability (and sco::caps name)
    const char* title;   // mod.log
    const char* ready;   // what "[+]" says
    bool        on;
    char        why[160];
};
FeatureState g_features[kFeatureCount] = {
    { "asop.terminal", "ship terminal open (rc1)", "the terminal loads its ship list", false, "" },
    { "asop.caller", "ship terminal requests (rc2)", "the terminal's remote methods find you", false, "" },
    { "asop.list", "ship terminal list (rc4, rc15)", "stored and retrieved ships are listed as such", false, "" },
    { "asop.deliver", "ship terminal Deliver (rc3)", "Deliver stores the ship at the station", false, "" },
    { "asop.claim_timeout", "ship terminal claim timeout (rc5)", "\"Awaiting Delivery\" lasts 8 s", false, "" },
    { "asop.retrieve", "ship terminal Retrieve (rc6, rc12, rc14-rc18)", "Retrieve brings the ship up on the hangar lift", false, "" },
    { "hangar.lift", "hangar ship lift (rc13)", "the lift lowers, takes the ship and raises it", false, "" },
    { "atc.store", "hangar terminal Store (rc19)", "Store lowers the lift and stores the ship", false, "" },
};
bool g_enabled = false;    // sc-offline.ini asop
bool g_resolved = false;   // ResolveFleetApi ran

bool On(Feature f) { return g_features[f].on; }
void Off(Feature f, const char* why) {
    g_features[f].on = false;
    strncpy_s(g_features[f].why, why, _TRUNCATE);
}

// ---- log budget (doc 14: hot hooks must not flood mod.log) -----------------------------------

volatile LONG g_asopLines = 0;
bool Budget() { return InterlockedIncrement(&g_asopLines) <= 200; }

// ---- game functions, all from rows -------------------------------------------------------------

using OpenFn            = uintptr_t(__fastcall*)(uintptr_t kiosk, uintptr_t a2, uintptr_t a3, uintptr_t a4);
using FetchFn           = void(__fastcall*)(uintptr_t provider, const void* event, uintptr_t a3, uintptr_t a4);
using SetDataListFn     = void(__fastcall*)(uintptr_t provider, void* list, uintptr_t a3, uintptr_t a4);
using SetDeliveredFn    = void(__fastcall*)(uintptr_t provider, void* out, const void* urn, const void* inventory, uint64_t vehicle,
                                         const void* extra);
using SetRetrievedFn    = void(__fastcall*)(uintptr_t provider, const uint64_t* vehicle, const void* urn, uint32_t location);
using SetSpawnedFn      = void(__fastcall*)(uintptr_t provider, const uint64_t* vehicle, uint64_t landingArea, uint64_t landingAtc);
using DeliverFn         = uintptr_t(__fastcall*)(uintptr_t kiosk, uintptr_t context, const void* requestId, const uint8_t* urn);
using FindCallerFn      = uintptr_t(__fastcall*)(uintptr_t game, const uint64_t* id);
using RequestVehicleFn  = uintptr_t(__fastcall*)(uintptr_t kiosk, uintptr_t a2, uintptr_t a3, uintptr_t a4);
using StoreVehicleFn    = void(__fastcall*)(uintptr_t atc, uint64_t vehicle, uint64_t player, uint32_t location, const void* inventory,
                                         uint64_t other);
using GetAtcFn          = uint64_t*(__fastcall*)(uint64_t* out, uint64_t atcEntity);
using AtcProcessFn      = uintptr_t(__fastcall*)(uintptr_t atc, uint64_t* request, uintptr_t cls, void* out, uint64_t mode, uint64_t extra);
using QueueSpawnFn      = uintptr_t(__fastcall*)(uintptr_t atc, uint64_t* request, const void* token);
using RequestCancelFn   = void(__fastcall*)(uintptr_t atc, uint64_t player);
using SpawnCancelledFn  = void(__fastcall*)(uintptr_t atc, uint64_t vehicle);
using StoreTokenFn      = bool(__fastcall*)(uintptr_t capture, const uintptr_t* token);
using UnstowResultFn    = void(__fastcall*)(uintptr_t context, const uint8_t* result, uintptr_t a3, uintptr_t a4);
using VehicleSpawnedFn  = void(__fastcall*)(uintptr_t manager, uintptr_t vehicle, uintptr_t a3, uintptr_t a4);
using PlatformSpawnFn   = void(__fastcall*)(uintptr_t self, uint64_t requester, const char* const* cls, uint64_t landingArea,
                                          uint64_t trackedByPlayer);
using LiftRequestFn     = void(__fastcall*)(uintptr_t self, uint64_t manager);
using LiftHandlerFn     = void(__fastcall*)(uintptr_t component, uintptr_t event, uintptr_t a3, uintptr_t a4);

OpenFn           g_openOrig = nullptr;
FetchFn          g_fetchOrig = nullptr;
SetDataListFn    g_setDataListOrig = nullptr;
SetDeliveredFn   g_setDelivered = nullptr;
SetRetrievedFn   g_setRetrieved = nullptr;
SetSpawnedFn     g_setSpawned = nullptr;
DeliverFn        g_deliverOrig = nullptr;
DeliverFn        g_deliver2Orig = nullptr;
FindCallerFn     g_findCallerOrig = nullptr;
RequestVehicleFn g_requestVehicleOrig = nullptr;
StoreVehicleFn   g_storeVehicle = nullptr;
GetAtcFn         g_getAtc = nullptr;
AtcProcessFn     g_atcProcessOrig = nullptr;
QueueSpawnFn     g_queueSpawnOrig = nullptr;
RequestCancelFn  g_requestCancel = nullptr;
SpawnCancelledFn g_spawnCancelled = nullptr;
StoreTokenFn     g_storeTokenOrig = nullptr;
UnstowResultFn   g_unstowResultOrig = nullptr;
VehicleSpawnedFn g_vehicleSpawnedOrig = nullptr;
PlatformSpawnFn  g_platformSpawn = nullptr;
LiftRequestFn    g_liftClose = nullptr;
LiftRequestFn    g_liftOpen = nullptr;
LiftHandlerFn    g_liftHandlerOrig = nullptr;

uint8_t*   g_open = nullptr;              // OnRequestOpen
volatile uintptr_t g_openKiosk = 0;       // the last opened kiosk (diagnostics)
volatile uintptr_t g_openKioskVtable = 0;
volatile DWORD     g_openKioskAt = 0;
uintptr_t* g_gameSlot = nullptr;          // &gEnv->pGame
size_t     g_callerSlot = 0;              // pGame vtable offset of the caller lookup
uint8_t    g_inventoryKind = 0;
uintptr_t* g_insuranceConfig = nullptr;   // &cached SInsuranceComponentConfig*
volatile LONG64 g_player = 0;             // the player's entity id, refreshed every tick (doc 3.4)

thread_local bool t_ourFetch = false;     // our own replay: don't capture it

// ---- state (doc section 4) ---------------------------------------------------------------------

struct FetchEvent {
    uintptr_t provider = 0, vtable = 0;
    alignas(16) uint8_t event[0x20] = {};
};
struct DeliverRequest { uint64_t atc = 0; uint32_t location = 0; uint64_t urn[2] = {}; DWORD at = 0; };
struct Delivery { int ship = -1; uint64_t atc = 0; uint32_t location = 0; uint64_t vehicle = 0; DWORD requestedAt = 0, spawnedAt = 0; };
struct StoredShip {
    int ship = -1; uint64_t vehicle = 0; uint64_t inventory[3] = {}; uintptr_t cls = 0;
    DWORD storedAt = 0; uint32_t location = 0; uint64_t atc = 0; bool checked = false;
};
struct RetrievedShip {
    int ship = -1; uint64_t vehicle = 0, atc = 0, pad = 0, stored = 0; uint64_t inventory[3] = {}; uint32_t location = 0;
    DWORD at = 0;
};
struct PadSpawnRequest { uint64_t stored = 0, pad = 0; DWORD at = 0; };
struct PadSpawn {
    int ship = -1; uint64_t stored = 0, pad = 0, manager = 0; DWORD queuedAt = 0, liftAt = 0;
    bool resolved = false; int32_t lastState = -2;
};
struct LiftWatch { uint64_t manager = 0; DWORD spawnedAt = 0; bool opened = false; int32_t lastState = -2; };
struct HangarStore {
    uint64_t vehicle = 0, manager = 0; DWORD liftAt = 0, storedAt = 0; bool started = false, stored = false;
    int32_t lastState = -2;
};
struct RetrieveWatch { uint64_t vehicle = 0; uintptr_t atc = 0; DWORD first = 0, last = 0; bool cancelled = false; };
struct SpawnCancel { uint64_t atc = 0, vehicle = 0; };
struct StationLocation { uint64_t atc = 0; uint32_t location = 0; };
struct LiftComponent { uint64_t manager = 0; uintptr_t component = 0; };
struct SpawnedVehicle { uint64_t id = 0; char name[96] = {}; DWORD at = 0; };
struct PadMove { uint64_t atc = 0, pad = 0, vehicle = 0; };
// What the hooks may look at, published by the game thread after each tick.
struct StoredView { int ship; uint64_t vehicle; uintptr_t cls; uint64_t inventory[3]; };
struct RetrievedView { int ship; uint64_t vehicle, stored, atc, pad; uint64_t inventory[3]; uint32_t location; bool alive; };

// Under g_lock (hooks and the game thread).
SRWLOCK g_lock = SRWLOCK_INIT;
FetchEvent                   g_lastFetch;
std::vector<DeliverRequest>  g_deliverQueue;
std::vector<PadSpawnRequest> g_padSpawnQueue;
std::vector<uint64_t>        g_retrieving;     // stored ids whose unstow this module took over (rc14)
std::vector<uint64_t>        g_hangarStoreQueue;
std::vector<RetrieveWatch>   g_watches;
std::vector<StationLocation> g_stations;
std::vector<LiftComponent>   g_liftComponents;
std::vector<SpawnedVehicle>  g_spawnedVehicles;
std::vector<PadMove>         g_padMoves;
std::vector<StoredView>      g_storedView;
std::vector<RetrievedView>   g_retrievedView;
std::vector<uint64_t>        g_pendingPadSpawnIds;     // published: stored ids with a pad spawn not yet done
bool                         g_waitingForSpawn = false; // published: a retrieved ship's new id isn't known

// Game thread only.
std::vector<Delivery>      g_deliveries;
std::vector<StoredShip>    g_stored;
std::vector<RetrievedShip> g_retrieved;
std::vector<PadSpawn>      g_padSpawns;
std::vector<LiftWatch>     g_liftWatches;
std::vector<HangarStore>   g_hangarStores;
std::vector<SpawnCancel>   g_spawnCancels;
std::vector<DWORD>         g_refreshDue;

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

uint64_t Player() { return static_cast<uint64_t>(InterlockedCompareExchange64(&g_player, 0, 0)); }

template <typename V, typename P> bool Contains(const V& v, P pred) { return std::find_if(v.begin(), v.end(), pred) != v.end(); }

// ---- small game reads (each guarded; no C++ objects in these functions) -------------------------

uintptr_t GetEntitySafe(uint64_t id) {
    if (!id || !g_tp.entitySystem) return 0;
    __try { return *g_tp.entitySystem ? VCall<uintptr_t>(*g_tp.entitySystem, 0x120, id) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uintptr_t LocalActorSafe() {
    __try {
        uintptr_t actor = 0, entity = 0;
        return GetLocalPlayer(actor, entity) ? actor : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The ATC data manager (CSCAirTrafficControllerDataManager*) of an ATC entity, through the game's
// own getter (the call at asop.fleet_retrieve +0xDD).
uintptr_t AtcDataManager(uint64_t atcEntity) {
    if (!atcEntity || !g_getAtc) return 0;
    __try {
        uint64_t out[2] = {};
        g_getAtc(out, atcEntity);
        return out[0] & kPtrMask;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// GetPersonalLocationInventoryConfiguration, called exactly as the Deliver handler calls it (doc 7.3).
bool PersonalInventory(uint64_t player, uint32_t location, uint64_t inventory[3]) {
    __try {
        alignas(16) uint8_t result[0x100] = {};
        const uintptr_t game = *g_gameSlot;
        const uintptr_t configs = game ? VCall<uintptr_t>(game, asop::kInventoryConfigSlot) : 0;
        if (!configs) return false;
        VCall<void>(configs, asop::kInventoryLookupSlot, static_cast<void*>(result), player, location, g_inventoryKind);
        if (!result[asop::kInventoryResultOk]) return false;
        memcpy(inventory, result, 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* CallStoreVehicle(uintptr_t atc, uint64_t vehicle, uint64_t player, uint32_t location, const uint64_t inventory[3]) {
    __try {
        alignas(16) uint64_t inv[3] = { inventory[0], inventory[1], inventory[2] };
        g_storeVehicle(atc, vehicle, player, location, inv, 0);
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault in the ATC's StoreVehicle";
    }
}

uintptr_t ClassByName(const char* name) {
    __try {
        const uintptr_t registry = *g_tp.entitySystem ? VCall<uintptr_t>(*g_tp.entitySystem, 0xC0) : 0;
        return registry ? VCall<uintptr_t>(registry, 0x20, name) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// An entity's name, zone and that zone's id.
bool ReadEntityPlace(uint64_t id, char* name, size_t n, uint64_t& zoneId) {
    name[0] = 0;
    zoneId = 0;
    __try {
        const uintptr_t e = GetEntitySafe(id);
        if (!e) return false;
        const char* s = VCall<const char*>(e, 0x78);
        if (s) strncpy_s(name, n, s, _TRUNCATE);
        const uintptr_t zone = VCall<uintptr_t>(e, 0x6B8);
        zoneId = zone ? ZoneId(zone) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int32_t ReadInt32(uintptr_t p) {
    __try { return Rd<int32_t>(p); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

bool CallLift(LiftRequestFn fn, uint64_t manager) {
    __try { fn(0, manager); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SvRequestSpawnVehicleInLoadingPlatform (doc 8.4). The class goes in as an engine string: the
// int32 length and capacity sit just before the text. A fault after the ship was queued has been
// seen; it counts as "requested" (the caller doesn't spawn again).
bool CallPlatformSpawn(uint64_t manager, const char* cls, uint64_t landingArea, uint64_t player) {
    struct { int32_t length, capacity; char text[128]; } name = {};
    const size_t len = strnlen(cls, sizeof(name.text) - 1);
    name.length = name.capacity = static_cast<int32_t>(len);
    memcpy(name.text, cls, len);
    const char* data = name.text;
    __try {
        g_platformSpawn(0, manager, &data, landingArea, player);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSpawnCancelled(uintptr_t atc, uint64_t vehicle) {
    __try { g_spawnCancelled(atc, vehicle); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallRequestCancel(uintptr_t atc, uint64_t player) {
    __try { g_requestCancel(atc, player); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Replays a captured fetch on its provider (doc 5.3): the terminal fetches its list again.
bool SafeFetch(const FetchEvent& f) {
    __try {
        if (!f.provider || Rd<uintptr_t>(f.provider) != f.vtable) return false;   // the provider is gone
        t_ourFetch = true;
        g_fetchOrig(f.provider, f.event, 0, 0);
        t_ourFetch = false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_ourFetch = false;
        return false;
    }
}

// ---- list entries (doc 5.4) --------------------------------------------------------------------

uint8_t* FindListEntry(uintptr_t provider, int ship) {
    uint8_t* begin = Rd<uint8_t*>(provider + kProviderBegin);
    uint8_t* end = Rd<uint8_t*>(provider + kProviderEnd);
    if (!begin || end < begin || static_cast<size_t>(end - begin) > kVehicleDataSize * 4096) return nullptr;
    for (uint8_t* e = begin; e + kVehicleDataSize <= end; e += kVehicleDataSize)
        if (FleetShipIndexOfUrnId(reinterpret_cast<const uint64_t*>(e + kVehicleUrn + kUrnId)) == ship) return e;
    return nullptr;
}

bool MarkStored(uintptr_t provider, const StoredView& s) {
    __try {
        uint8_t* entry = FindListEntry(provider, s.ship);
        if (!entry) return false;
        alignas(16) uint8_t urn[0x40] = {};
        memcpy(urn, entry + kVehicleUrn, kVehicleUrnSize);
        alignas(16) uint64_t inventory[3] = { s.inventory[0], s.inventory[1], s.inventory[2] };
        uint64_t added[3] = {}, extra[3] = {};   // empty std::vectors; `added` is left to leak (doc 5.4)
        g_setDelivered(provider, added, urn, inventory, s.vehicle, extra);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The online order on a retrieved ship's entry: stow context with the new id, entry +0x00 = the
// new id (the spawned setter searches by it), retrieved, spawned (doc 5.4 step 2).
bool MarkRetrieved(uintptr_t provider, const RetrievedView& r) {
    __try {
        uint8_t* entry = FindListEntry(provider, r.ship);
        if (!entry) return false;
        alignas(16) uint8_t urn[0x40] = {};
        memcpy(urn, entry + kVehicleUrn, kVehicleUrnSize);
        alignas(16) uint64_t inventory[3] = { r.inventory[0], r.inventory[1], r.inventory[2] };
        uint64_t added[3] = {}, extra[3] = {};
        uint64_t vehicle = r.vehicle;
        g_setDelivered(provider, added, urn, inventory, vehicle, extra);
        *reinterpret_cast<uint64_t*>(entry) = vehicle;
        g_setRetrieved(provider, &vehicle, urn, r.location);
        g_setSpawned(provider, &vehicle, r.pad, r.atc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void MarkEntries(uintptr_t provider) {
    std::vector<StoredView> stored;
    std::vector<RetrievedView> retrieved;
    {
        Lock l;
        stored = g_storedView;
        retrieved = g_retrievedView;
    }
    int marked = 0;
    for (const StoredView& s : stored) marked += MarkStored(provider, s);
    for (const RetrievedView& r : retrieved)
        if (r.vehicle && r.alive) marked += MarkRetrieved(provider, r);   // destroyed: left "Deliverable"
    if (marked && Budget()) Log("[asop] ship list: %d entr%s marked stored or on pad", marked, marked == 1 ? "y" : "ies");
}

// ---- hooks -------------------------------------------------------------------------------------

void RememberStation(uint64_t atc, uint32_t location) {
    bool fresh = false;
    {
        Lock l;
        auto it = std::find_if(g_stations.begin(), g_stations.end(), [atc](const StationLocation& s) { return s.atc == atc; });
        if (it == g_stations.end()) { g_stations.push_back({ atc, location }); fresh = true; }
        else if (it->location != location) { it->location = location; fresh = true; }
    }
    if (fresh) Log("[asop] terminal opened: ATC 0x%llX, location %u", static_cast<unsigned long long>(atc), location);
}

bool ReadKioskStation(uintptr_t kiosk, uint64_t& atc, uint32_t& location) {
    __try {
        atc = Rd<uint64_t>(kiosk + asop::kKioskAtc);
        location = Rd<uint32_t>(kiosk + asop::kKioskLocation);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// OnRequestOpen: after it, the kiosk holds its ATC and the player's location there (doc 5.2, 12.5).
uintptr_t ReadVtable(uintptr_t object);

uintptr_t __fastcall Hook_OnRequestOpen(uintptr_t kiosk, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    const uintptr_t r = g_openOrig(kiosk, a2, a3, a4);
    uint64_t atc = 0;
    uint32_t location = 0;
    if (On(kTerminal) && ReadKioskStation(kiosk, atc, location) && atc && location) RememberStation(atc, location);
    if (On(kTerminal)) {
        g_openKioskVtable = ReadVtable(kiosk);
        g_openKiosk = kiosk;
        g_openKioskAt = GetTickCount();
    }
    return r;
}

// pGame->vfunc[0xA0]: the caller of a remote method. Offline the call context id is zeros and the
// original finds nobody; the caller is the local player (doc 6).
uintptr_t __fastcall Hook_FindCaller(uintptr_t game, const uint64_t* id) {
    const uintptr_t found = g_findCallerOrig(game, id);
    if (found || !On(kCaller)) return found;
    return LocalActorSafe();
}

bool CopyEvent(const void* event, uint8_t* out) {
    __try { memcpy(out, event, 0x20); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t ReadVtable(uintptr_t object) {
    __try { return Rd<uintptr_t>(object); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// OnRequestFetchVehicles: keep each genuine fetch, so the list can be fetched again later.
void __fastcall Hook_Fetch(uintptr_t provider, const void* event, uintptr_t a3, uintptr_t a4) {
    if (!t_ourFetch && provider && event && (On(kList) || On(kClaim) || On(kDeliver))) {
        FetchEvent f;
        f.provider = provider;
        f.vtable = ReadVtable(provider);
        if (f.vtable && CopyEvent(event, f.event)) {
            Lock l;
            g_lastFetch = f;
        }
    }
    g_fetchOrig(provider, event, a3, a4);
}

void __fastcall Hook_SetDataList(uintptr_t provider, void* list, uintptr_t a3, uintptr_t a4) {
    g_setDataListOrig(provider, list, a3, a4);
    if (On(kList) && provider) MarkEntries(provider);
}

bool ReadDeliverRequest(uintptr_t kiosk, const uint8_t* urn, DeliverRequest& r) {
    __try {
        r.atc = Rd<uint64_t>(kiosk + asop::kKioskAtc);
        r.location = Rd<uint32_t>(kiosk + asop::kKioskLocation);
        memcpy(r.urn, urn + kUrnId, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Both RmAuthorityRequestDeliver handlers. Their continuation calls the internal delivery service,
// which is null offline (rc3), so the originals never run while Deliver is on: the request is
// queued and done on the game thread.
uintptr_t DeliverHook(DeliverFn orig, uintptr_t kiosk, uintptr_t context, const void* requestId, const uint8_t* urn) {
    if (!On(kDeliver)) return orig(kiosk, context, requestId, urn);
    DeliverRequest r;
    r.at = GetTickCount();
    if (!urn || !ReadDeliverRequest(kiosk, urn, r)) {
        Log("[asop] Deliver: couldn't read the request (ignored)");
        return 0;
    }
    {
        Lock l;
        g_deliverQueue.push_back(r);
    }
    return 0;
}
uintptr_t __fastcall Hook_Deliver(uintptr_t kiosk, uintptr_t context, const void* requestId, const uint8_t* urn) {
    return DeliverHook(g_deliverOrig, kiosk, context, requestId, urn);
}
uintptr_t __fastcall Hook_Deliver2(uintptr_t kiosk, uintptr_t context, const void* requestId, const uint8_t* urn) {
    return DeliverHook(g_deliver2Orig, kiosk, context, requestId, urn);
}

bool ClearKioskRetrieving(uintptr_t kiosk, uint64_t& was) {
    __try {
        uint64_t& field = *reinterpret_cast<uint64_t*>(kiosk + asop::kKioskRetrieving);
        was = field;
        if (!was || !Fleet_IsSettlingStoredId(was)) return false;
        field = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// RequestVehicle: the kiosk's "vehicle being retrieved" still holds the stored id of a retrieve
// this module finished, and the kiosk refuses another one (rc17).
uintptr_t __fastcall Hook_RequestVehicle(uintptr_t kiosk, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    uint64_t was = 0;
    if (On(kRetrieve) && ClearKioskRetrieving(kiosk, was))
        Log("[asop] Retrieve: cleared the terminal's retrieving vehicle 0x%llX (rc17)", static_cast<unsigned long long>(was));
    return g_requestVehicleOrig(kiosk, a2, a3, a4);
}

bool ReadQwords(const uint64_t* p, uint64_t* out, int n) {
    __try { for (int i = 0; i < n; ++i) out[i] = p[i]; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool FillRequestClass(uint64_t* request, uintptr_t cls) {
    __try {
        if (!request[kRequestClass / 8]) request[kRequestClass / 8] = cls;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The stored ship among the request's first six qwords: its class, or 0 (rc6).
uintptr_t StoredClassFor(const uint64_t* request, uint64_t& vehicle) {
    uint64_t q[6] = {};
    vehicle = 0;
    if (!request || !ReadQwords(request, q, 6)) return 0;
    Lock l;
    for (uint64_t v : q)
        for (const StoredView& s : g_storedView)
            if (v && s.vehicle == v && s.cls) { vehicle = v; return s.cls; }
    return 0;
}

void NoteRetrieveSighting(uint64_t vehicle, uintptr_t atc) {
    const DWORD now = GetTickCount();
    Lock l;
    for (RetrieveWatch& w : g_watches)
        if (w.vehicle == vehicle) {
            if (now - w.last > kRetrieveWatchGapMs) { w.first = now; w.cancelled = false; }
            w.last = now;
            w.atc = atc;
            return;
        }
    g_watches.push_back({ vehicle, atc, now, now, false });
}

// ATC request processing: a stowed ship has no entity, so the class its callers read is null and
// the ATC asks for a size-0 hangar forever (rc6). Supply the stored ship's class, here and in
// SATCActionRequest +0x98 (which QueueSpawnShip reads).
uintptr_t __fastcall Hook_AtcProcess(uintptr_t atc, uint64_t* request, uintptr_t cls, void* out, uint64_t mode, uint64_t extra) {
    if (On(kRetrieve) && !cls) {
        uint64_t vehicle = 0;
        if (const uintptr_t found = StoredClassFor(request, vehicle)) {
            cls = found;
            FillRequestClass(request, found);
            NoteRetrieveSighting(vehicle, atc);
            if (Budget()) Log("[asop] Retrieve: ATC request for stored 0x%llX: class supplied (rc6)", static_cast<unsigned long long>(vehicle));
        }
    }
    return g_atcProcessOrig(atc, request, cls, out, mode, extra);
}

uintptr_t __fastcall Hook_QueueSpawnShip(uintptr_t atc, uint64_t* request, const void* token) {
    if (On(kRetrieve)) {
        uint64_t vehicle = 0;
        if (const uintptr_t found = StoredClassFor(request, vehicle)) FillRequestClass(request, found);
    }
    return g_queueSpawnOrig(atc, request, token);
}

bool ReadUnstowResult(uintptr_t context, const uint8_t* result, bool& ok, uint64_t& vehicle, uint64_t& pad) {
    __try {
        ok = result[0] == 1;
        vehicle = Rd<uint64_t>(context + 0x10);
        pad = EntityIdOfHandle(reinterpret_cast<const void*>(context + 0x00));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Landing area +0x318 keeps a deferred unstow that every lift close reruns (rc18).
bool DropDeferredUnstow(uintptr_t context, uint64_t vehicle) {
    __try {
        const uintptr_t landingArea = Rd<uintptr_t>(context + 0x18);
        if (!landingArea) return false;
        uint64_t& deferred = *reinterpret_cast<uint64_t*>(landingArea + asop::kDeferredUnstow);
        if (deferred != vehicle) return false;
        deferred = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The landing area's unstow result. Offline the in-memory entity database can't unstow and the
// failure path calls the null internal services hub (rc12). For a stored ship of this module the
// failure handler doesn't run: the ship is spawned new on the pad instead (8.4). A second failed
// unstow of the same ship (after the lift closed) is already on its way (rc14).
void __fastcall Hook_UnstowResult(uintptr_t context, const uint8_t* result, uintptr_t a3, uintptr_t a4) {
    bool ok = true;
    uint64_t vehicle = 0, pad = 0;
    if (On(kRetrieve) && context && result && ReadUnstowResult(context, result, ok, vehicle, pad) && !ok && vehicle) {
        bool stored = false, again = false;
        {
            Lock l;
            stored = Contains(g_storedView, [vehicle](const StoredView& s) { return s.vehicle == vehicle; });
            again = std::find(g_retrieving.begin(), g_retrieving.end(), vehicle) != g_retrieving.end();
            if (stored && !again) {
                g_padSpawnQueue.push_back({ vehicle, pad, GetTickCount() });
                g_retrieving.push_back(vehicle);
            }
        }
        if (again || stored) {
            const bool dropped = DropDeferredUnstow(context, vehicle);
            Log("[asop] Retrieve: unstow of 0x%llX taken over%s%s", static_cast<unsigned long long>(vehicle),
                again ? " (second unstow after the lift closed, rc14)" : ", the ship is spawned on the pad (rc12)",
                dropped ? "; deferred unstow cleared (rc18)" : "");
            return;
        }
    }
    g_unstowResultOrig(context, result, a3, a4);
}

bool ReadSpawnedVehicle(uintptr_t entity, uint64_t& id, char* name, size_t n) {
    __try {
        uint64_t tmp = 0;
        const uint64_t* p = VCall<const uint64_t*>(entity, 0x08, &tmp);
        id = p ? *p : 0;
        const char* s = VCall<const char*>(entity, 0x78);
        name[0] = 0;
        if (s) strncpy_s(name, n, s, _TRUNCATE);
        return id != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// CPlayerShipRespawnManager::OnVehicleSpawned: the platform spawn returns no id; this learns it (rc15).
void __fastcall Hook_VehicleSpawned(uintptr_t manager, uintptr_t vehicle, uintptr_t a3, uintptr_t a4) {
    g_vehicleSpawnedOrig(manager, vehicle, a3, a4);
    if (!On(kRetrieve)) return;
    bool waiting = false;
    {
        Lock l;
        waiting = g_waitingForSpawn;
    }
    SpawnedVehicle v;
    if (!waiting || !ReadSpawnedVehicle(vehicle & kPtrMask, v.id, v.name, sizeof(v.name))) return;
    v.at = GetTickCount();
    Lock l;
    g_spawnedVehicles.push_back(v);
}

bool ReadStoreToken(uintptr_t capture, const uintptr_t* token, uint64_t& vehicle, uint64_t& tokenVehicle) {
    __try {
        vehicle = Rd<uint64_t>(capture + 0x08);
        tokenVehicle = Rd<uint64_t>(token[0] + 0x10);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SetFound(uintptr_t capture) {
    __try { *Rd<uint8_t*>(capture + 0x30) = 1; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The Store handler's per-token lambda. Offline the despawn it queues for owner 0 never runs
// (rc19): for a retrieved ship this module stores it itself (lift down, StoreVehicle, lift up).
bool __fastcall Hook_StoreToken(uintptr_t capture, const uintptr_t* token) {
    uint64_t vehicle = 0, tokenVehicle = 0;
    if (On(kStore) && capture && token && ReadStoreToken(capture, token, vehicle, tokenVehicle) && vehicle && tokenVehicle == vehicle) {
        bool ours = false, queued = false;
        {
            Lock l;
            ours = Contains(g_retrievedView, [vehicle](const RetrievedView& r) { return r.vehicle == vehicle; });
            if (ours && std::find(g_hangarStoreQueue.begin(), g_hangarStoreQueue.end(), vehicle) == g_hangarStoreQueue.end()) {
                g_hangarStoreQueue.push_back(vehicle);
                queued = true;
            }
        }
        if (ours && SetFound(capture)) {
            if (queued) Log("[asop] Store: 0x%llX queued (lift down, store, lift up)", static_cast<unsigned long long>(vehicle));
            return false;   // stop iterating; the handler neither despawns nor stores it
        }
    }
    return g_storeTokenOrig(capture, token);
}

// The lift manager's close handler, only to learn each manager's component (doc 10.2).
void __fastcall Hook_LiftHandler(uintptr_t component, uintptr_t event, uintptr_t a3, uintptr_t a4) {
    const uint64_t manager = component ? EntityIdOfComponent(component) : 0;
    if (manager) {
        Lock l;
        auto it = std::find_if(g_liftComponents.begin(), g_liftComponents.end(), [manager](const LiftComponent& c) { return c.manager == manager; });
        if (it == g_liftComponents.end()) g_liftComponents.push_back({ manager, component });
        else it->component = component;
    }
    g_liftHandlerOrig(component, event, a3, a4);
}

// ---- game thread -------------------------------------------------------------------------------

int32_t LiftStateOf(uint64_t manager) {
    uintptr_t component = 0;
    {
        Lock l;
        for (const LiftComponent& c : g_liftComponents)
            if (c.manager == manager) component = c.component;
    }
    return component ? ReadInt32(component + kLiftStateField) : -1;
}

// The lift that goes with a landing area (doc 10.3): LandingArea_X -> LoadingPlatformManager_X,
// accepted only in the landing area's zone.
uint64_t FindLiftManager(uint64_t pad, char* padName, size_t n) {
    uint64_t padZone = 0;
    if (!ReadEntityPlace(pad, padName, n, padZone) || !padName[0]) return 0;
    const char* rest = strncmp(padName, "LandingArea_", 12) == 0 ? padName + 12 : padName;
    char name[160];
    snprintf(name, sizeof(name), "LoadingPlatformManager_%s", rest);
    uintptr_t entity = 0;
    uint64_t id = 0;
    if (!FindEntityByNameEx(name, entity, id) || !id) {
        Log("[asop] lift: no %s for %s", name, padName);
        return 0;
    }
    char found[96];
    uint64_t zone = 0;
    if (!ReadEntityPlace(id, found, sizeof(found), zone) || zone != padZone) {
        Log("[asop] lift: %s is in another zone than %s (another hangar of the same class); not used", name, padName);
        return 0;
    }
    return id;
}

void RequestRefresh(DWORD at) { g_refreshDue.push_back(at); }

void PublishViews(DWORD now) {
    std::vector<StoredView> stored;
    std::vector<RetrievedView> retrieved;
    std::vector<uint64_t> pending;
    bool waiting = false;
    for (const PadSpawn& p : g_padSpawns) pending.push_back(p.stored);
    for (const StoredShip& s : g_stored) stored.push_back({ s.ship, s.vehicle, s.cls, { s.inventory[0], s.inventory[1], s.inventory[2] } });
    for (const RetrievedShip& r : g_retrieved) {
        retrieved.push_back({ r.ship, r.vehicle, r.stored, r.atc, r.pad, { r.inventory[0], r.inventory[1], r.inventory[2] }, r.location,
                              r.vehicle && EntityAlive(r.vehicle) });
        waiting |= !r.vehicle && now - r.at <= kRetrievedSpawnMs;
    }
    Lock l;
    g_storedView.swap(stored);
    g_retrievedView.swap(retrieved);
    g_pendingPadSpawnIds.swap(pending);
    g_waitingForSpawn = waiting || !g_padSpawns.empty();
}

bool ShipIsBusy(int ship) {
    return Contains(g_stored, [ship](const StoredShip& s) { return s.ship == ship; })
        || Contains(g_retrieved, [ship](const RetrievedShip& r) { return r.ship == ship; })
        || Contains(g_deliveries, [ship](const Delivery& d) { return d.ship == ship; })
        || Contains(g_padSpawns, [ship](const PadSpawn& p) { return p.ship == ship; });
}

// StoreVehicle with the player's personal inventory at the station (doc 7.3).
const char* StoreShip(uint64_t atcEntity, uint64_t vehicle, uint32_t location, uint64_t inventory[3]) {
    const uint64_t player = Player();
    if (!player) return "you're not spawned";
    if (!location) return "no location id (open a terminal at this station first)";
    const uintptr_t atc = AtcDataManager(atcEntity);
    if (!atc) return "the station's ATC isn't streamed in";
    if (!PersonalInventory(player, location, inventory)) return "no personal inventory at this location";
    return CallStoreVehicle(atc, vehicle, player, location, inventory);
}

void StartDelivery(const DeliverRequest& r, DWORD now) {
    const int ship = FleetShipIndexOfUrnId(r.urn);
    const char* cls = ship >= 0 ? FleetShipClass(ship) : nullptr;
    if (!cls) {
        Log("[asop] Deliver: not a ship of the fleet list (id %016llX %016llX); ignored", static_cast<unsigned long long>(r.urn[0]),
            static_cast<unsigned long long>(r.urn[1]));
        return;
    }
    if (ShipIsBusy(ship)) {
        Log("[asop] Deliver %s: already stored, out or being delivered this session; ignored", cls);
        return;
    }
    const double offset[3] = { 0, 0, kDeliverySpawnHeight };
    uint64_t id = 0;
    if (const char* err = SpawnEntityNearPlayer(cls, offset, id)) {
        Log("[asop] Deliver %s: spawn failed: %s", cls, err);
        return;
    }
    RegisterPlayerVehicle(id);
    g_deliveries.push_back({ ship, r.atc, r.location, id, r.at, now });
    Log("[asop] Deliver %s: spawned 0x%llX %.0f m above you, storing it at location %u in %lu ms", cls,
        static_cast<unsigned long long>(id), kDeliverySpawnHeight, r.location, kDeliverySettleMs);
}

void RunDeliveries(DWORD now) {
    for (size_t i = 0; i < g_deliveries.size();) {
        Delivery& d = g_deliveries[i];
        const char* cls = FleetShipClass(d.ship);
        if (now - d.spawnedAt < kDeliverySettleMs) { ++i; continue; }
        if (!EntityAlive(d.vehicle)) {
            if (now - d.spawnedAt < kDeliveryGiveUpMs) { ++i; continue; }
            Log("[!] [asop] Deliver %s: the ship never appeared; given up", cls ? cls : "?");
            UnregisterPlayerVehicle(d.vehicle);
            g_deliveries.erase(g_deliveries.begin() + i);
            continue;
        }
        StoredShip s;
        s.ship = d.ship;
        s.vehicle = d.vehicle;
        s.location = d.location;
        s.atc = d.atc;
        s.storedAt = now;
        s.cls = cls ? ClassByName(cls) : 0;
        if (const char* err = StoreShip(d.atc, d.vehicle, d.location, s.inventory)) {
            Log("[!] [asop] Deliver %s: store failed: %s", cls ? cls : "?", err);
        } else {
            g_stored.push_back(s);
            Log("[asop] Deliver %s: stored 0x%llX at location %u (ATC 0x%llX)", cls ? cls : "?", static_cast<unsigned long long>(d.vehicle),
                d.location, static_cast<unsigned long long>(d.atc));
            // The terminal shows the station and "Awaiting Delivery", then "Stored" once its own
            // claim request has timed out (doc 7.4).
            RequestRefresh(d.requestedAt + kDeliverySettleMs + kRefreshAfterStoreMs);
            RequestRefresh(d.requestedAt + kClaimTimeoutSeconds * 1000 + kRefreshAfterClaimMs);
        }
        UnregisterPlayerVehicle(d.vehicle);
        g_deliveries.erase(g_deliveries.begin() + i);
    }
}

// Cancel an ATC request that can't finish (rc6), and drop stored ships whose store didn't take.
void WatchRetrieves(DWORD now) {
    std::vector<RetrieveWatch> cancel;
    {
        Lock l;
        for (RetrieveWatch& w : g_watches)
            if (!w.cancelled && now - w.first > kRetrieveGiveUpMs && now - w.last < kRetrieveRecentMs) {
                w.cancelled = true;
                cancel.push_back(w);
            }
    }
    for (const RetrieveWatch& w : cancel) {
        const bool ok = CallRequestCancel(w.atc, Player());
        Log("[!] [asop] Retrieve: the ATC couldn't finish 0x%llX in %lu s; request %s", static_cast<unsigned long long>(w.vehicle),
            kRetrieveGiveUpMs / 1000, ok ? "cancelled" : "cancel faulted");
    }
    for (size_t i = 0; i < g_stored.size();) {
        StoredShip& s = g_stored[i];
        if (!s.checked && now - s.storedAt > kStoreCheckMs) {
            s.checked = true;
            if (EntityAlive(s.vehicle)) {
                const char* cls = FleetShipClass(s.ship);
                Log("[!] [asop] %s 0x%llX is still in the world %lu s after StoreVehicle: not stored", cls ? cls : "?",
                    static_cast<unsigned long long>(s.vehicle), kStoreCheckMs / 1000);
                g_stored.erase(g_stored.begin() + i);
                continue;
            }
        }
        ++i;
    }
}

void LogLiftState(const char* what, uint64_t manager, int32_t state, int32_t& last) {
    if (state == last) return;
    last = state;
    Log("[asop] %s: lift 0x%llX is %s", what, static_cast<unsigned long long>(manager), LiftStateName(state));
}

// Lower the lift, spawn the ship through the game's loading-platform spawn, raise the lift (rc13).
void RunPadSpawns(DWORD now) {
    for (size_t i = 0; i < g_padSpawns.size();) {
        PadSpawn& p = g_padSpawns[i];
        const char* cls = FleetShipClass(p.ship);
        char padName[96] = "";
        if (!p.resolved) {
            uint64_t zone = 0;
            if (!ReadEntityPlace(p.pad, padName, sizeof(padName), zone)) {
                if (now - p.queuedAt > kPadSpawnGiveUpMs) {
                    Log("[!] [asop] Retrieve %s: the pad 0x%llX never streamed in; given up", cls ? cls : "?", static_cast<unsigned long long>(p.pad));
                    g_padSpawns.erase(g_padSpawns.begin() + i);
                    continue;
                }
                ++i;
                continue;
            }
            p.resolved = true;
            p.manager = On(kLift) ? FindLiftManager(p.pad, padName, sizeof(padName)) : 0;
            Log("[asop] Retrieve %s: pad %s, lift 0x%llX", cls ? cls : "?", padName[0] ? padName : "?", static_cast<unsigned long long>(p.manager));
        }
        if (p.manager) {
            const int32_t state = LiftStateOf(p.manager);
            LogLiftState("Retrieve", p.manager, state, p.lastState);
            if (state != kClosedIdle) {
                if (!p.liftAt) {
                    p.liftAt = now;
                    if (!CallLift(g_liftClose, p.manager)) Log("[asop] Retrieve: lift close request faulted");
                    ++i;
                    continue;
                }
                const bool unseen = state < 0 && now - p.liftAt > kLiftUnseenMs;
                if (!unseen && now - p.liftAt <= kLiftLowerMs) { ++i; continue; }
                Log("[asop] Retrieve: lift not down (%s) after %lu ms; spawning anyway", LiftStateName(state), now - p.liftAt);
            }
        } else {
            Log("[!] [asop] Retrieve %s: no lift for this pad; the ship is spawned on the pad without lowering it", cls ? cls : "?");
        }
        if (!cls || !CallPlatformSpawn(p.manager, cls, p.pad, Player()))
            Log("[asop] Retrieve %s: the platform spawn faulted; treated as requested", cls ? cls : "?");
        else
            Log("[asop] Retrieve %s: spawn requested on the lift platform", cls);
        // The stored record becomes a retrieved one; the new entity id comes from OnVehicleSpawned.
        auto s = std::find_if(g_stored.begin(), g_stored.end(), [&p](const StoredShip& x) { return x.vehicle == p.stored; });
        if (s != g_stored.end()) {
            RetrievedShip r;
            r.ship = s->ship;
            r.atc = s->atc;
            r.pad = p.pad;
            r.stored = s->vehicle;
            memcpy(r.inventory, s->inventory, sizeof(r.inventory));
            r.location = s->location;
            r.at = now;
            g_retrieved.push_back(r);
            g_stored.erase(s);
        }
        if (p.manager) g_liftWatches.push_back({ p.manager, now, false, -2 });
        g_padSpawns.erase(g_padSpawns.begin() + i);
    }
}

// Raise the lift once the ship is on it: the spawn's own open event does it; Open after 4 s if not.
void RunLiftWatches(DWORD now) {
    for (size_t i = 0; i < g_liftWatches.size();) {
        LiftWatch& w = g_liftWatches[i];
        const int32_t state = LiftStateOf(w.manager);
        LogLiftState("Retrieve", w.manager, state, w.lastState);
        if (state == kOpenIdle || now - w.spawnedAt > kLiftWatchMs) { g_liftWatches.erase(g_liftWatches.begin() + i); continue; }
        if (state == kClosedIdle && !w.opened && now - w.spawnedAt > kLiftOpenFallbackMs) {
            w.opened = true;
            Log("[asop] Retrieve: the lift is still down %lu ms after the spawn; raising it", now - w.spawnedAt);
            if (!CallLift(g_liftOpen, w.manager)) Log("[asop] Retrieve: lift open request faulted");
        }
        ++i;
    }
}

void MatchSpawnedVehicles(const std::vector<SpawnedVehicle>& spawned, DWORD now) {
    for (const SpawnedVehicle& v : spawned)
        for (RetrievedShip& r : g_retrieved) {
            const char* cls = FleetShipClass(r.ship);
            if (r.vehicle || !cls || now - r.at > kRetrievedSpawnMs) continue;
            const size_t n = strlen(cls);
            if (strncmp(v.name, cls, n) != 0 || v.name[n] != '_') continue;   // "<class>_<n>"
            r.vehicle = v.id;
            RegisterPlayerVehicle(v.id);
            if (r.stored) g_spawnCancels.push_back({ r.atc, r.stored });   // rc16
            RequestRefresh(now);
            Log("[asop] Retrieve %s: new ship %s 0x%llX (was stored as 0x%llX)", cls, v.name, static_cast<unsigned long long>(v.id),
                static_cast<unsigned long long>(r.stored));
            break;
        }
}

// The ATC keeps the stored id in m_spawnVehicleList and ignores the player's next requests (rc16).
void RunSpawnCancels() {
    for (const SpawnCancel& c : g_spawnCancels) {
        const uintptr_t atc = AtcDataManager(c.atc);
        const bool ok = atc && CallSpawnCancelled(atc, c.vehicle);
        Log("[asop] Retrieve: ATC spawn list entry for 0x%llX %s (rc16)", static_cast<unsigned long long>(c.vehicle),
            ok ? "removed" : "not removed (ATC not found or fault)");
    }
    g_spawnCancels.clear();
}

void ApplyPadMoves(const std::vector<PadMove>& moves, DWORD now) {
    for (const PadMove& m : moves)
        for (RetrievedShip& r : g_retrieved) {
            if (r.vehicle != m.vehicle || (r.pad == m.pad && r.atc == m.atc)) continue;
            const bool newAtc = r.atc != m.atc;
            r.pad = m.pad;
            r.atc = m.atc;
            if (newAtc) {
                Lock l;
                for (const StationLocation& s : g_stations)
                    if (s.atc == m.atc) r.location = s.location;
            }
            Log("[asop] 0x%llX is on pad 0x%llX of ATC 0x%llX, location %u (rc22)", static_cast<unsigned long long>(r.vehicle),
                static_cast<unsigned long long>(m.pad), static_cast<unsigned long long>(m.atc), r.location);
            RequestRefresh(now);
        }
}

void ApplyStations() {
    std::vector<StationLocation> stations;
    {
        Lock l;
        stations = g_stations;
    }
    for (RetrievedShip& r : g_retrieved)
        for (const StationLocation& s : stations)
            if (s.atc == r.atc && s.location != r.location) r.location = s.location;
}

// Store from the hangar terminal (rc19): lift down with the ship, StoreVehicle, the empty lift up.
void RunHangarStores(DWORD now) {
    for (size_t i = 0; i < g_hangarStores.size();) {
        HangarStore& h = g_hangarStores[i];
        auto r = std::find_if(g_retrieved.begin(), g_retrieved.end(), [&h](const RetrievedShip& x) { return x.vehicle == h.vehicle; });
        if (!h.stored && r == g_retrieved.end()) {
            const uint64_t vehicle = h.vehicle;
            g_hangarStores.erase(g_hangarStores.begin() + i);
            Lock l;
            g_hangarStoreQueue.erase(std::remove(g_hangarStoreQueue.begin(), g_hangarStoreQueue.end(), vehicle), g_hangarStoreQueue.end());
            continue;
        }
        if (!h.started) {
            h.started = true;
            char padName[96];
            h.manager = On(kLift) ? FindLiftManager(r->pad, padName, sizeof(padName)) : 0;
            const int32_t state = h.manager ? LiftStateOf(h.manager) : -1;
            if (h.manager && state != kClosedIdle) {
                h.liftAt = now;
                if (!CallLift(g_liftClose, h.manager)) Log("[asop] Store: lift close request faulted");
            }
            Log("[asop] Store 0x%llX: lift 0x%llX %s", static_cast<unsigned long long>(h.vehicle), static_cast<unsigned long long>(h.manager),
                h.manager ? "lowering" : "not found, storing in place");
            ++i;
            continue;
        }
        if (!h.stored) {
            const int32_t state = h.manager ? LiftStateOf(h.manager) : kClosedIdle;
            if (h.manager) LogLiftState("Store", h.manager, state, h.lastState);
            const bool unseen = state < 0 && now - h.liftAt > kLiftUnseenMs;
            if (state != kClosedIdle && !unseen && now - h.liftAt <= kLiftLowerMs) { ++i; continue; }
            StoredShip s;
            s.ship = r->ship;
            s.vehicle = r->vehicle;
            s.location = r->location;
            s.atc = r->atc;
            s.storedAt = now;
            const char* cls = FleetShipClass(r->ship);
            s.cls = cls ? ClassByName(cls) : 0;
            if (const char* err = StoreShip(r->atc, r->vehicle, r->location, s.inventory)) {
                Log("[!] [asop] Store 0x%llX failed: %s", static_cast<unsigned long long>(h.vehicle), err);
            } else {
                Log("[asop] Store %s 0x%llX: stored at location %u", cls ? cls : "?", static_cast<unsigned long long>(h.vehicle), r->location);
                UnregisterPlayerVehicle(r->vehicle);
                g_stored.push_back(s);
                g_retrieved.erase(r);
                RequestRefresh(now + kRefreshAfterStoreMs);
            }
            h.stored = true;
            h.storedAt = now;
            ++i;
            continue;
        }
        if (now - h.storedAt < kRaiseAfterStoreMs) { ++i; continue; }
        if (h.manager && LiftStateOf(h.manager) == kClosedIdle && !CallLift(g_liftOpen, h.manager)) Log("[asop] Store: lift open request faulted");
        if (h.manager) Log("[asop] Store: raising the empty lift");
        {
            const uint64_t vehicle = h.vehicle;
            Lock l;
            g_hangarStoreQueue.erase(std::remove(g_hangarStoreQueue.begin(), g_hangarStoreQueue.end(), vehicle), g_hangarStoreQueue.end());
        }
        g_hangarStores.erase(g_hangarStores.begin() + i);
    }
}

void RunTerminalRefreshes(DWORD now) {
    bool due = false;
    for (size_t i = 0; i < g_refreshDue.size();) {
        if (static_cast<LONG>(now - g_refreshDue[i]) >= 0) { due = true; g_refreshDue.erase(g_refreshDue.begin() + i); }
        else ++i;
    }
    if (!due) return;
    FetchEvent f;
    {
        Lock l;
        f = g_lastFetch;
    }
    if (f.provider && !SafeFetch(f)) Log("[asop] terminal refresh: the last terminal's list is gone (open it again)");
}

// The claim request of the terminal waits 120 s for an answer that never comes (rc5).
void ShortenClaimTimeout() {
    static bool logged = false;
    __try {
        const uintptr_t config = *g_insuranceConfig;
        if (!config) return;
        int32_t& timeout = *reinterpret_cast<int32_t*>(config + asop::kInsuranceTimeout);
        if (timeout == kClaimTimeoutSeconds) return;
        const int32_t was = timeout;
        timeout = kClaimTimeoutSeconds;
        if (!logged) { logged = true; Log("[asop] insurance request timeout %d s -> %d s (rc5)", was, kClaimTimeoutSeconds); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!logged) { logged = true; Log("[asop] fault while shortening the claim timeout"); }
    }
}

// The terminal offers Deliver only with the item-recovery delivery system set up. The kiosk copies
// g_itemRecovery.deliverySystemSetup into its DeliverySystemSetup binding on every list update
// ("0: Disabled / 1: delivery times applied, can't be expedited / 2: ... can be expedited"). Online
// the server's config sets it; offline it keeps the cvar block's default, 0, and every row shows
// "Deliverable" with no action. 1 keeps the expedite path (DGS pricing) off; Deliver itself is
// this module's (rc3). Checked every 2 s: the console may not exist yet, and a config may reset it.
void EnsureDeliverySystem(DWORD now) {
    static const char kCVar[] = "g_itemRecovery.deliverySystemSetup";
    static DWORD last = 0;
    static bool logged = false;
    if (now - last < 2000) return;
    last = now;
    float value = 0;
    if (!GetCVarNow(kCVar, value) || value >= 1) return;
    const bool ok = SetCVarNow(kCVar, 1);
    if (!logged || !ok) Log("[asop] %s %.0f -> 1 %s (the terminal's Deliver action needs it)", kCVar, value, ok ? "set" : "NOT set");
    logged |= ok;
}

// ---- terminal diagnostics ----------------------------------------------------------------------
//
// What the open terminal's screen is bound to, read from CEntityComponentShipInsuranceProvider on
// 4.10.196.36804 (log only; a fault or a moved field just stops the lines). Kiosk bindings: a field
// object per binding, bool value at +0x41, int at +0x48. Rows: vector at kiosk +0xF0 / +0xF8,
// 0x16C0 bytes per row (UpdateBindingsElement), same field layout (InsuredVehicle table).
struct TerminalSnapshot {
    int64_t rows, selected, deliverySystem, capacity, used;
    int64_t deliverable, deliverOk;           // rows with CanBeDelivered, and with it not disabled
    int64_t info, canDeliver, deliverDisabled, canRetrieve, retrieveDisabled, canClaim, claimDisabled,
            claimedThisPatch, sameLocation, matchesFilter;   // the selected row (or row 0)
};

bool ReadTerminal(uintptr_t kiosk, TerminalSnapshot& s) {
    __try {
        const uintptr_t begin = Rd<uintptr_t>(kiosk + 0xF0), end = Rd<uintptr_t>(kiosk + 0xF8);
        if (end < begin || (end - begin) % 0x16C0 || (end - begin) / 0x16C0 > 8192) return false;
        s.rows = static_cast<int64_t>((end - begin) / 0x16C0);
        s.selected = Rd<int64_t>(kiosk + 0x278 + 0x48);        // CurrentVehicleIndex
        s.deliverySystem = Rd<int64_t>(kiosk + 0x198 + 0x48);  // DeliverySystemSetup
        s.capacity = Rd<int64_t>(kiosk + 0x400 + 0x48);        // StorageCapacity
        s.used = Rd<int64_t>(kiosk + 0x450 + 0x48);            // StorageUsed
        s.deliverable = s.deliverOk = 0;
        for (uintptr_t e = begin; e < end; e += 0x16C0) {
            const bool can = Rd<uint8_t>(e + 0x4D0 + 0x41) != 0;
            s.deliverable += can;
            s.deliverOk += can && !Rd<uint8_t>(e + 0x518 + 0x41);
        }
        const int64_t pick = s.selected >= 0 && s.selected < s.rows ? s.selected : 0;
        const uintptr_t e = begin + static_cast<uintptr_t>(pick) * 0x16C0;
        if (!s.rows) return true;
        s.info = Rd<int64_t>(e + 0x138 + 0x48);                // Information (13 = Deliverable)
        s.canDeliver = Rd<uint8_t>(e + 0x4D0 + 0x41);
        s.deliverDisabled = Rd<uint8_t>(e + 0x518 + 0x41);
        s.canRetrieve = Rd<uint8_t>(e + 0x368 + 0x41);
        s.retrieveDisabled = Rd<uint8_t>(e + 0x3B0 + 0x41);
        s.canClaim = Rd<uint8_t>(e + 0x560 + 0x41);
        s.claimDisabled = Rd<uint8_t>(e + 0x5A8 + 0x41);
        s.claimedThisPatch = Rd<uint8_t>(e + 0x7E8 + 0x41);
        s.sameLocation = Rd<uint8_t>(e + 0xAC0 + 0x41);
        s.matchesFilter = Rd<uint8_t>(e + 0xC30 + 0x41);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every 500 ms for 3 minutes after a terminal opens: one line whenever what it shows changes
// (a row click changes "selected").
void WatchTerminal(DWORD now) {
    static DWORD last = 0;
    static TerminalSnapshot shown = {};
    static uintptr_t shownKiosk = 0;
    static int lines = 0;
    const uintptr_t kiosk = g_openKiosk;
    if (!kiosk || now - g_openKioskAt > 180000 || now - last < 500 || lines > 300) return;
    last = now;
    if (ReadVtable(kiosk) != g_openKioskVtable) return;   // the terminal streamed out
    TerminalSnapshot s = {};
    if (!ReadTerminal(kiosk, s)) return;
    if (kiosk == shownKiosk && !memcmp(&s, &shown, sizeof(s))) return;
    shown = s;
    shownKiosk = kiosk;
    ++lines;
    Log("[asop] terminal: %lld rows, %lld CanBeDelivered (%lld not disabled), selected %lld, DeliverySystemSetup %lld, storage %lld/%lld",
        s.rows, s.deliverable, s.deliverOk, s.selected, s.deliverySystem, s.used, s.capacity);
    if (s.rows)
        Log("[asop] terminal row %lld: info %lld, CanBeDelivered %lld IsDeliverDisabled %lld, CanBeRetrieved %lld IsRetrieveDisabled %lld, "
            "CanBeClaimed %lld IsClaimDisabled %lld, HasBeenClaimedInCurrentPatch %lld, IsOnSameLocation %lld, matchesFilter %lld",
            s.selected >= 0 && s.selected < s.rows ? s.selected : 0, s.info, s.canDeliver, s.deliverDisabled, s.canRetrieve,
            s.retrieveDisabled, s.canClaim, s.claimDisabled, s.claimedThisPatch, s.sameLocation, s.matchesFilter);
}

// Swaps pGame's caller-lookup slot once pGame exists (doc 6).
void InstallCallerLookup() {
    static bool done = false;
    if (done) return;
    uintptr_t game = 0;
    __try { game = *g_gameSlot; } __except (EXCEPTION_EXECUTE_HANDLER) { game = 0; }
    if (!game) return;
    done = true;
    uintptr_t vtable = ReadVtable(game);
    if (!vtable) { Off(kCaller, "pGame has no vtable"); Log("[!] %s: pGame has no vtable", g_features[kCaller].title); return; }
    void** slot = reinterpret_cast<void**>(vtable + g_callerSlot);
    const sco::hook::Error e = sco::hook::SwapSlot(slot, reinterpret_cast<void*>(&Hook_FindCaller), reinterpret_cast<void**>(&g_findCallerOrig));
    if (e != sco::hook::Error::None) {
        char why[96];
        snprintf(why, sizeof(why), "vtable slot not swapped (%s)", sco::hook::ErrorName(e));
        Off(kCaller, why);
        Log("[!] %s: %s", g_features[kCaller].title, why);
        return;
    }
    Log("[asop] remote-method caller lookup: falls back to you (rc2)");
}

void RefreshPlayer() {
    const uint64_t id = LocalPlayerEntityId();
    InterlockedExchange64(&g_player, static_cast<LONG64>(id));
}

// ---- install -----------------------------------------------------------------------------------

bool EnsureHook(const char* row, size_t stolen, void* detour, void** original) {
    uint8_t* target = sco::Sig(row);
    if (!target) return false;
    if (sco::hook::IsHooked(target)) return *original != nullptr;
    return HookFunction(target, stolen, detour, original);
}

void InstallFeature(Feature f, bool ok) {
    if (!ok && g_features[f].on) Off(f, "a hook couldn't be installed (see the [!] detour line above)");
}

// rc1: the server half's jmp at +0x95E goes to a stub that reloads rdi (the ATC handle) and
// continues in the client half at +0x963 (doc 5.2).
bool PatchOnRequestOpen(char* why, size_t n) {
    uint8_t* open = g_open;
    if (open[asop::kOpenPatchSite] != 0xE9 || open + asop::kOpenClientHalf + Rel32(open + asop::kOpenPatchSite + 1) != open + asop::kOpenExit
        || open[asop::kOpenClientHalf] != 0x80 || open[asop::kOpenClientHalf + 1] != 0x3D
        || !BytesMatch(open + asop::kOpenAtcReload, "48 8B BD D8 00 00 00")) {
        snprintf(why, n, "OnRequestOpen +0x%X isn't the expected jmp", asop::kOpenPatchSite);
        return false;
    }
    uint8_t* stub = sco::hook::AllocateNear(open, 16);
    if (!stub) { snprintf(why, n, "no executable memory near OnRequestOpen"); return false; }
    static const uint8_t kReloadRdi[] = { 0x48, 0x8B, 0xBD, 0xD8, 0x00, 0x00, 0x00 };   // mov rdi, [rbp+0xD8]
    memcpy(stub, kReloadRdi, sizeof(kReloadRdi));
    stub[7] = 0xE9;                                                                    // jmp OnRequestOpen+0x963
    const int32_t back = static_cast<int32_t>((open + asop::kOpenClientHalf) - (stub + 12));
    memcpy(stub + 8, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), stub, 12);
    uint8_t jmp[5] = { 0xE9 };
    const int32_t to = static_cast<int32_t>(stub - (open + asop::kOpenPatchSite + 5));
    memcpy(jmp + 1, &to, 4);
    if (!sco::hook::WriteCode(open + asop::kOpenPatchSite, jmp, sizeof(jmp))) {
        snprintf(why, n, "couldn't write OnRequestOpen +0x%X (error %u)", asop::kOpenPatchSite, sco::hook::LastOsError());
        return false;
    }
    g_hooksInstalled = true;
    return true;
}

bool ReadAsopSwitch() {
    char v[16];
    const DWORD n = GetEnvironmentVariableA("SC_OFFLINE_ASOP", v, sizeof(v));
    if (!n || n >= sizeof(v)) return true;   // unset: on
    return !(_stricmp(v, "off") == 0 || _stricmp(v, "0") == 0 || _stricmp(v, "no") == 0 || _stricmp(v, "false") == 0);
}

}  // namespace

// For ClearKioskRetrieving: a stored id this module took the unstow of, with no pad spawn pending.
bool Fleet_IsSettlingStoredId(uint64_t id) {
    Lock l;
    if (std::find(g_retrieving.begin(), g_retrieving.end(), id) == g_retrieving.end()) return false;
    return !Contains(g_padSpawnQueue, [id](const PadSpawnRequest& p) { return p.stored == id; })
        && std::find(g_pendingPadSpawnIds.begin(), g_pendingPadSpawnIds.end(), id) == g_pendingPadSpawnIds.end();
}

bool AsopEnabled() { return g_enabled; }

bool Fleet_UseGameList() {
    // asop_fleet_list = game|ships in sc-offline.ini (SC_OFFLINE_ASOP_FLEET_LIST); unset = game.
    static int cached = -1;
    if (cached < 0) {
        char v[16];
        const DWORD n = GetEnvironmentVariableA("SC_OFFLINE_ASOP_FLEET_LIST", v, sizeof(v));
        cached = n && n < sizeof(v) && _stricmp(v, "ships") == 0 ? 0 : 1;
    }
    return g_enabled && cached == 1;
}

bool AsopCapabilityRows(const char* capability, char* why, size_t n) {
    size_t count = 0;
    const asop::Capability* caps = asop::Capabilities(count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(caps[i].name, capability) != 0) continue;
        for (size_t r = 0; r < caps[i].count; ++r) {
            const sco::SigResult* s = sco::SigLookup(caps[i].rows[r]);
            if (!s) { snprintf(why, n, "unknown signature %s", caps[i].rows[r]); return false; }
            if (s->state != sco::SigState::Ok) {
                snprintf(why, n, "needs %s (%s)", caps[i].rows[r], sco::SigStateName(s->state));
                return false;
            }
        }
        return true;
    }
    snprintf(why, n, "sco-core has no capability %s (sco-core too old?)", capability);
    return false;
}

void ResolveFleetApi() {
    g_resolved = true;
    g_enabled = ReadAsopSwitch();
    if (!g_enabled) return;
    for (FeatureState& f : g_features) f.on = AsopCapabilityRows(f.cap, f.why, sizeof(f.why));
    // A Retrieve uses the lift (docs/game/asop.md): it needs both groups.
    if (g_features[kRetrieve].on && !g_features[kLift].on) Off(kRetrieve, "needs the hangar ship lift (hangar.lift isn't ready)");

    // Function pointers first (the hooks below may run as soon as they are installed).
    g_open            = sco::Sig("asop.on_request_open");
    g_gameSlot        = reinterpret_cast<uintptr_t*>(sco::Sig("asop.game"));
    g_setDelivered    = reinterpret_cast<SetDeliveredFn>(sco::Sig("asop.set_delivered_or_claimed"));
    g_setRetrieved    = reinterpret_cast<SetRetrievedFn>(sco::Sig("asop.set_retrieved"));
    g_setSpawned      = reinterpret_cast<SetSpawnedFn>(sco::Sig("asop.set_spawned"));
    g_storeVehicle    = reinterpret_cast<StoreVehicleFn>(sco::Sig("atc.store_vehicle"));
    g_getAtc          = reinterpret_cast<GetAtcFn>(sco::Sig("atc.get_component"));
    g_requestCancel   = reinterpret_cast<RequestCancelFn>(sco::Sig("atc.request_cancel"));
    g_spawnCancelled  = reinterpret_cast<SpawnCancelledFn>(sco::Sig("atc.on_vehicle_spawn_cancelled"));
    g_platformSpawn   = reinterpret_cast<PlatformSpawnFn>(sco::Sig("lift.spawn_on_platform"));
    g_liftClose       = reinterpret_cast<LiftRequestFn>(sco::Sig("lift.close_request"));
    g_liftOpen        = reinterpret_cast<LiftRequestFn>(sco::Sig("lift.open_request"));
    g_insuranceConfig = reinterpret_cast<uintptr_t*>(sco::Sig("insurance.config"));
    if (const uint8_t* deliver = sco::Sig("asop.rm_request_deliver")) {
        g_callerSlot = static_cast<size_t>(Rel32(deliver + 0x44));   // call [rax+disp32] at +0x3F (pinned 0xA0)
    }
    if (const uint8_t* kind = sco::Sig("asop.inventory_kind")) g_inventoryKind = *kind;

    // The fetch capture serves the list, the claim refreshes and Deliver's refreshes.
    bool fetchOk = true;
    if (On(kList) || On(kClaim) || On(kDeliver))
        fetchOk = sco::SigReady("asop.fetch_vehicles")
               && EnsureHook("asop.fetch_vehicles", 5, reinterpret_cast<void*>(&Hook_Fetch), reinterpret_cast<void**>(&g_fetchOrig));
    if (On(kList))
        InstallFeature(kList, fetchOk && EnsureHook("asop.set_vehicle_data_list", 5, reinterpret_cast<void*>(&Hook_SetDataList),
                                                    reinterpret_cast<void**>(&g_setDataListOrig)));
    if (On(kClaim)) InstallFeature(kClaim, fetchOk);
    if (On(kDeliver))
        InstallFeature(kDeliver, EnsureHook("asop.rm_request_deliver", 5, reinterpret_cast<void*>(&Hook_Deliver), reinterpret_cast<void**>(&g_deliverOrig))
                                 && EnsureHook("asop.rm_request_deliver_2", 5, reinterpret_cast<void*>(&Hook_Deliver2),
                                               reinterpret_cast<void**>(&g_deliver2Orig)));
    // The lift's component is learned from its close handler: the lift, Retrieve and Store use it.
    bool liftHandlerOk = true;
    if (On(kLift) || On(kStore))
        liftHandlerOk = sco::SigReady("lift.manager_close_handler")
                     && EnsureHook("lift.manager_close_handler", 5, reinterpret_cast<void*>(&Hook_LiftHandler),
                                   reinterpret_cast<void**>(&g_liftHandlerOrig));
    if (On(kLift)) InstallFeature(kLift, liftHandlerOk);
    if (On(kStore))
        InstallFeature(kStore, liftHandlerOk && EnsureHook("atc.store_token_lambda", 6, reinterpret_cast<void*>(&Hook_StoreToken),
                                                           reinterpret_cast<void**>(&g_storeTokenOrig)));
    if (On(kRetrieve) && !On(kLift)) Off(kRetrieve, "needs the hangar ship lift, which isn't installed");
    if (On(kRetrieve))
        InstallFeature(kRetrieve,
            EnsureHook("asop.request_vehicle", 5, reinterpret_cast<void*>(&Hook_RequestVehicle), reinterpret_cast<void**>(&g_requestVehicleOrig))
            && EnsureHook("atc.process_request", 7, reinterpret_cast<void*>(&Hook_AtcProcess), reinterpret_cast<void**>(&g_atcProcessOrig))
            && EnsureHook("atc.queue_spawn_ship", 5, reinterpret_cast<void*>(&Hook_QueueSpawnShip), reinterpret_cast<void**>(&g_queueSpawnOrig))
            && EnsureHook("landing.unstow_result", 13, reinterpret_cast<void*>(&Hook_UnstowResult), reinterpret_cast<void**>(&g_unstowResultOrig))
            && EnsureHook("respawn.on_vehicle_spawned", 7, reinterpret_cast<void*>(&Hook_VehicleSpawned),
                          reinterpret_cast<void**>(&g_vehicleSpawnedOrig)));
    if (On(kTerminal)) {
        char why[160];
        if (!PatchOnRequestOpen(why, sizeof(why))) Off(kTerminal, why);
        else if (!EnsureHook("asop.on_request_open", 5, reinterpret_cast<void*>(&Hook_OnRequestOpen), reinterpret_cast<void**>(&g_openOrig)))
            Log("[!] [asop] OnRequestOpen hook not installed: Store after landing at another station keeps the first station (rc22)");
    }
    // The caller lookup is swapped on the game thread once pGame exists (InstallCallerLookup).
}

void LogFleet() {
    if (!g_resolved) return;
    if (!g_enabled) {
        Log("[-] ship terminals, hangars and ATC (ASOP): off (asop = off in sc-offline.ini)");
        return;
    }
    for (const FeatureState& f : g_features)
        if (f.on) Log("[+] %s: ready (%s)", f.title, f.ready);
        else      Log("[!] %s: %s", f.title, f.why);
}

void SetFleetCaps() {
    for (const FeatureState& f : g_features) {
        const char* why = !g_resolved ? "the game's addresses weren't resolved (teleport unavailable)"
                        : !g_enabled  ? "asop = off in sc-offline.ini"
                                      : f.why;
        const sco::Result r = sco::caps::Set(f.cap, g_resolved && g_enabled && f.on, why);
        if (r != sco::Result::Ok) Log("[!] capability %s: %s", f.cap, sco::ResultName(r));
    }
}

bool Fleet_RetrieveActive() { return g_enabled && On(kRetrieve); }

uint64_t Fleet_RetrievedShipOwner(uint64_t vehicle) {
    if (!g_enabled || !vehicle) return 0;
    {
        Lock l;
        if (!Contains(g_retrievedView, [vehicle](const RetrievedView& r) { return r.vehicle == vehicle; })) return 0;
    }
    return Player();
}

void Fleet_OnPadVehicle(uint64_t atcEntity, uint64_t padEntity, uint64_t vehicle) {
    if (!g_enabled || !atcEntity || !padEntity || !vehicle) return;
    Lock l;
    if (Contains(g_retrievedView, [vehicle](const RetrievedView& r) { return r.vehicle == vehicle; }))
        g_padMoves.push_back({ atcEntity, padEntity, vehicle });
}

void ProcessFleet(DWORD now) {
    if (!g_enabled) return;
    RefreshPlayer();
    if (On(kCaller)) InstallCallerLookup();
    if (On(kClaim)) ShortenClaimTimeout();
    if (On(kDeliver)) EnsureDeliverySystem(now);
    if (On(kTerminal)) WatchTerminal(now);

    // What the hooks queued.
    std::vector<DeliverRequest> delivers;
    std::vector<PadSpawnRequest> padSpawns;
    std::vector<SpawnedVehicle> spawned;
    std::vector<PadMove> moves;
    std::vector<uint64_t> stores;
    {
        Lock l;
        delivers.swap(g_deliverQueue);
        padSpawns.swap(g_padSpawnQueue);
        spawned.swap(g_spawnedVehicles);
        moves.swap(g_padMoves);
        for (uint64_t v : g_hangarStoreQueue)
            if (!Contains(g_hangarStores, [v](const HangarStore& h) { return h.vehicle == v; })) stores.push_back(v);
    }
    for (const DeliverRequest& r : delivers) StartDelivery(r, now);
    for (const PadSpawnRequest& p : padSpawns) {
        auto s = std::find_if(g_stored.begin(), g_stored.end(), [&p](const StoredShip& x) { return x.vehicle == p.stored; });
        if (s == g_stored.end()) continue;
        PadSpawn ps;
        ps.ship = s->ship;
        ps.stored = p.stored;
        ps.pad = p.pad;
        ps.queuedAt = now;
        g_padSpawns.push_back(ps);
    }
    for (uint64_t v : stores) {
        HangarStore h;
        h.vehicle = v;
        g_hangarStores.push_back(h);
    }
    MatchSpawnedVehicles(spawned, now);
    ApplyStations();
    ApplyPadMoves(moves, now);

    RunDeliveries(now);
    WatchRetrieves(now);
    RunPadSpawns(now);
    RunLiftWatches(now);
    RunSpawnCancels();
    RunHangarStores(now);
    PublishViews(now);   // before the refreshes: a replayed list is marked from these views
    RunTerminalRefreshes(now);
}
