#include "ammo.h"
#include "hooks.h"
#include "menu.h"
#include "spawner.h"
#include "teleport.h"

using SetAmmoFn = void(__fastcall*)(uintptr_t container, int count, uint8_t notify);
static SetAmmoFn         g_setAmmoOrig = nullptr;
static volatile LONG     g_infinite = 0;
static volatile uint64_t g_you = 0;

void Menu_SetInfiniteAmmo(bool on) {
    InterlockedExchange(&g_infinite, on ? 1 : 0);
    Log("[ammo] infinite ammo %s", on ? "on" : "off");
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

static void __fastcall SetAmmoHook(uintptr_t container, int count, uint8_t notify) {
    if (g_infinite) {
        __try {
            const int32_t key = Rd<int32_t>(container + 0xC4);
            const int32_t now = key ? Rd<int32_t>(container + 0xC0) ^ key : 0;
            if (count < now && YouCarry(container)) count = Rd<int32_t>(container + 0xB8);
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

void ProcessAmmo() {
    if (g_infinite) g_you = LocalPlayerEntityId();
}
