#include "ammo.h"
#include "hooks.h"
#include "menu.h"
#include "spawner.h"
#include "teleport.h"

using SetAmmoFn = void(__fastcall*)(uintptr_t container, int count, uint8_t notify);
static SetAmmoFn         g_setAmmoOrig = nullptr;
static volatile LONG     g_infinite = 0;
static volatile LONG     g_shipInfinite = 0;
static volatile uint64_t g_you = 0;
static volatile uint64_t g_myShip = 0;      // the ship you're aboard
static volatile uint64_t g_targetShip = 0;  // the ship in the Crew & seats panel

void Menu_SetInfiniteAmmo(bool on) {
    InterlockedExchange(&g_infinite, on ? 1 : 0);
    Log("[ammo] infinite ammo %s", on ? "on" : "off");
}

void Menu_SetInfiniteShipAmmo(bool on) {
    InterlockedExchange(&g_shipInfinite, on ? 1 : 0);
    Log("[ammo] infinite ship ammo %s", on ? "on" : "off");
}

static uint64_t ParentId(uintptr_t entity) {
    uint64_t port = 0;
    VCall<void>(entity, 0x150, &port, 0ull);
    if (!(port & kPtrMask)) return 0;
    uint64_t id = 0;
    const uint64_t* owner = VCall<const uint64_t*>(port & kPtrMask, 0x8, &id);
    return owner ? *owner : 0;
}

static uintptr_t EntityById(uint64_t id) {
    uint64_t handle = 0;
    const uint64_t* h = VCall<const uint64_t*>(*g_tp.entitySystem, 0x128, &handle, id);
    return h ? (*h & kPtrMask) : 0;
}

// Ship weapons sit deeper than hand-held ones (weapon -> gimbal/mount -> turret -> ship), so walk
// further up. True if any ancestor is the ship you're aboard or the Crew & seats target ship.
static bool OnYourShip(uintptr_t container) {
    const uint64_t a = g_myShip, b = g_targetShip;
    if (!a && !b) return false;
    uintptr_t entity = Rd<uint64_t>(container + 8) & kPtrMask;
    for (int up = 0; entity && up < 8; ++up) {
        const uint64_t parent = ParentId(entity);
        if (!parent) return false;
        if (parent == a || parent == b) return true;
        entity = EntityById(parent);
    }
    return false;
}

static bool YouCarry(uintptr_t container) {
    const uint64_t you = g_you;
    uintptr_t entity = Rd<uint64_t>(container + 8) & kPtrMask;
    for (int up = 0; you && entity && up < 4; ++up) {
        const uint64_t parent = ParentId(entity);
        if (!parent) return false;
        if (parent == you) return true;
        entity = EntityById(parent);
    }
    return false;
}

static volatile LONG g_lastNotify = 1;

static void __fastcall SetAmmoHook(uintptr_t container, int count, uint8_t notify) {
    g_lastNotify = notify;
    if (g_infinite || g_shipInfinite) {
        __try {
            const int32_t key = Rd<int32_t>(container + 0xC4);
            const int32_t now = key ? Rd<int32_t>(container + 0xC0) ^ key : 0;
            if (count < now) {
                if (g_infinite && YouCarry(container)) count = Rd<int32_t>(container + 0xB8);
                else if (g_shipInfinite && OnYourShip(container)) count = Rd<int32_t>(container + 0xB8);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_setAmmoOrig(container, count, notify);
}

void ResolveAmmoApi(const Section& text) {
    int n = 0;
    uint8_t* fn = FindUniquePattern(text, "40 56 57 41 54 41 57 48 81 EC 88 00 00 00 8B 81 C4 00 00 00 45 33 FF 45 0F B6 E0 48 8B F9", n);
    if (fn && g_tp.entitySystem && HookFunction(fn, 14, reinterpret_cast<void*>(&SetAmmoHook), reinterpret_cast<void**>(&g_setAmmoOrig)))
        Log("[+] infinite ammo: ready (M menu)");
    else
        Log("[!] infinite ammo: the magazine setter wasn't found (%d matches)", n);
}

// Energy weapons can drain their magazine without going through the setter above, so with ship
// ammo on, every ammo container on your ship is also topped up directly, twice a second. A
// container is only touched if it reads like a magazine (key set, count between 0 and its maximum).
static bool ReadMagazine(uintptr_t c, int32_t& count, int32_t& max) {
    __try {
        const int32_t key = Rd<int32_t>(c + 0xC4);
        max = Rd<int32_t>(c + 0xB8);
        if (!key || max <= 0 || max > 1000000) return false;
        count = Rd<int32_t>(c + 0xC0) ^ key;
        return count >= 0 && count <= max;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void Refill(uintptr_t c, int32_t max) {
    __try { g_setAmmoOrig(c, max, static_cast<uint8_t>(g_lastNotify)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void TopUpShip(uint64_t ship) {
    uintptr_t comps[96];
    char names[96][96];
    const int n = ShipPartComponents(ship, "AmmoContainerComponent", comps, names, 96);
    for (int i = 0; i < n; ++i) {
        int32_t count = 0, max = 0;
        if (ReadMagazine(comps[i], count, max) && count < max) Refill(comps[i], max);
    }
}

void ProcessAmmo() {
    if (g_infinite) g_you = LocalPlayerEntityId();
    if (g_shipInfinite) {
        g_myShip = PlayerShipId();
        g_targetShip = TargetShipId();
        static DWORD last = 0;
        const DWORD now = GetTickCount();
        if (g_setAmmoOrig && now - last >= 500) {
            last = now;
            if (g_myShip) TopUpShip(g_myShip);
            if (g_targetShip && g_targetShip != g_myShip) TopUpShip(g_targetShip);
        }
    }
}
