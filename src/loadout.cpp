#include "loadout.h"
#include "teleport.h"
#include "menu.h"
#include <share.h>
#include <string>

static struct {
    bool       ok = false;
    uintptr_t* game = nullptr;
    int32_t    frameworkSlot = 0;
    int32_t    actorSlot = 0;
    int32_t    loadSlot = 0;
} g_lo;

bool ResolveLoadoutApi(const Section& text, const Section& rdata) {
    const uint8_t* folder = FindCString(rdata, "Scripts/Loadouts/Player");
    uint8_t* const end = text.base + text.size - 7;
    for (uint8_t* p = text.base + 0x44; folder && p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x15 || p + 7 + Rel32(p + 3) != folder) continue;
        const uint8_t* h = p - 0x44;
        if (!BytesMatch(h, "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 50 08 83 F8 01")
            || !BytesMatch(h + 0x18, "48 8B 0D") || !BytesMatch(h + 0x27, "FF 90") || !BytesMatch(h + 0x30, "48 8B 91")
            || !BytesMatch(h + 0x88, "41 B1 01") || !BytesMatch(h + 0x90, "41 B8 08 00 00 00") || !BytesMatch(h + 0x99, "FF 90"))
            continue;
        g_lo.game          = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(h + 0x1F + Rel32(h + 0x1B)));
        g_lo.frameworkSlot = Rel32(h + 0x29);
        g_lo.actorSlot     = Rel32(h + 0x33);
        g_lo.loadSlot      = Rel32(h + 0x9B);
        g_lo.ok = true;
        break;
    }
    if (!g_lo.ok) Log("[gear] loadout loader not found; gear menu disabled");
    return g_lo.ok;
}

static const char* const kSlotNames[Gear_SlotCount] = {
    "undersuit", "helmet", "torso", "arms", "legs", "backpack", "primary", "sidearm", "ammo", "grenade" };

constexpr int        kMaxGear = 3000;
static char          g_gear[kMaxGear][64];
static int           g_gearStart[Gear_SlotCount];
static int           g_gearCount[Gear_SlotCount];
static volatile LONG g_gearState = 0;
static SRWLOCK       g_gearLock = SRWLOCK_INIT;
static struct { bool pending; int picks[Gear_SlotCount]; } g_equipRequest;

int Menu_GearCount(int slot) {
    if (g_gearState != 2) { InterlockedCompareExchange(&g_gearState, 1, 0); return -1; }
    return slot >= 0 && slot < Gear_SlotCount ? g_gearCount[slot] : 0;
}

const char* Menu_GearName(int slot, int index) {
    if (g_gearState != 2 || slot < 0 || slot >= Gear_SlotCount || index < 0 || index >= g_gearCount[slot]) return "";
    return g_gear[g_gearStart[slot] + index];
}

void Menu_RequestEquip(const int picks[Gear_SlotCount]) {
    AcquireSRWLockExclusive(&g_gearLock);
    g_equipRequest.pending = true;
    memcpy(g_equipRequest.picks, picks, sizeof(g_equipRequest.picks));
    ReleaseSRWLockExclusive(&g_gearLock);
}

static bool DataFilePath(char* path, DWORD n, const char* file) {
    if (!ShipsFilePath(path, n)) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (!slash || static_cast<DWORD>(slash + 1 - path) + strlen(file) + 1 > n) return false;
    strcpy_s(slash + 1, n - static_cast<DWORD>(slash + 1 - path), file);
    return true;
}
static bool ItemsFilePath(char* path, DWORD n) { return DataFilePath(path, n, "items.txt"); }

static int BuildGearLists() {
    char path[MAX_PATH];
    if (!ItemsFilePath(path, sizeof(path))) return 0;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) { Log("[gear] can't open %s", path); return 0; }
    const uintptr_t registry = VCall<uintptr_t>(*g_tp.entitySystem, 0xC0);
    int slot = -1, total = 0, unknown = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char* name = line + strspn(line, " \t");
        if (!*name) continue;
        if (*name == '[') {
            slot = -1;
            for (int s = 0; s < Gear_SlotCount; ++s)
                if (_strnicmp(name + 1, kSlotNames[s], strlen(kSlotNames[s])) == 0 && name[1 + strlen(kSlotNames[s])] == ']') slot = s;
            if (slot >= 0) { g_gearStart[slot] = total; g_gearCount[slot] = 0; }
            continue;
        }
        if (slot < 0 || total >= kMaxGear) continue;
        if (!VCall<uintptr_t>(registry, 0x20, static_cast<const char*>(name))) { ++unknown; continue; }
        strncpy_s(g_gear[total++], name, _TRUNCATE);
        ++g_gearCount[slot];
    }
    fclose(f);
    Log("[gear] %d items in the gear menu (%d unknown names skipped)", total, unknown);
    return total;
}

static const char* Pick(const int picks[Gear_SlotCount], int slot) {
    const int i = picks[slot];
    return i >= 0 && i < g_gearCount[slot] ? g_gear[g_gearStart[slot] + i] : nullptr;
}

static const char* MatchingMag(const char* gun) {
    if (!gun) return nullptr;
    char base[64];
    strncpy_s(base, gun, _TRUNCATE);
    for (;;) {
        char want[72];
        snprintf(want, sizeof(want), "%s_mag", base);
        for (int i = 0; i < g_gearCount[Gear_Ammo]; ++i)
            if (_stricmp(g_gear[g_gearStart[Gear_Ammo] + i], want) == 0) return g_gear[g_gearStart[Gear_Ammo] + i];
        char* cut = strrchr(base, '_');
        if (!cut) return nullptr;
        *cut = 0;
    }
}

static std::string Item(const char* port, const char* name, const std::string& children = std::string()) {
    std::string s = std::string("<Item portName=\"") + port + "\" itemName=\"" + name + "\"";
    return children.empty() ? s + "/>" : s + "><Items>" + children + "</Items></Item>";
}

static std::string Gun(const char* port, const char* gun) {
    const char* mag = MatchingMag(gun);
    return Item(port, gun, mag ? Item("magazine_attach", mag) : std::string());
}

static std::string HeadAndMobiGlas();

static std::string LoadoutXml(const int picks[Gear_SlotCount]) {
    const char* torso   = Pick(picks, Gear_Torso);
    const char* legs    = Pick(picks, Gear_Legs);
    const char* primary = Pick(picks, Gear_Primary);
    const char* sidearm = Pick(picks, Gear_Sidearm);
    const char* ammo    = Pick(picks, Gear_Ammo);
    if (!ammo) ammo = MatchingMag(primary);

    std::string onTorso, onLegs;
    if (const char* b = Pick(picks, Gear_Backpack)) onTorso += Item("backpack", b);
    if (primary) onTorso += Gun("wep_stocked_3", primary);
    for (int i = 1; ammo && i <= (torso ? 4 : 2); ++i) onTorso += Item(("magazine_attach_" + std::to_string(i)).c_str(), ammo);
    if (const char* g = Pick(picks, Gear_Grenade); g && torso)
        for (int i = 1; i <= 2; ++i) onTorso += Item(("grenade_attach_" + std::to_string(i)).c_str(), g);
    if (sidearm) onLegs += Gun("wep_sidearm", sidearm);
    onLegs += Item("medPen_attach_1", "crlf_consumable_healing_01");

    std::string onSuit;
    if (const char* h = Pick(picks, Gear_Helmet)) onSuit += Item("Armor_Helmet", h);
    if (torso) onSuit += Item("Armor_Torso", torso, onTorso); else onSuit += onTorso;
    if (const char* a = Pick(picks, Gear_Arms)) onSuit += Item("Armor_Arms", a);
    if (legs) onSuit += Item("Armor_Legs", legs, onLegs); else onSuit += onLegs;

    const char* suit = Pick(picks, Gear_Undersuit);
    return "<Loadout><Items>" + Item("Body_ItemPort", "body_01",
        Item("Armor_Undersuit", suit ? suit : "rsi_odyssey_undersuit_01_01_01", onSuit) + HeadAndMobiGlas())
        + "</Items></Loadout>\n";
}

static std::string HeadAndMobiGlas() {
    return Item("Head_ItemPort", "PU_Protos_Head",
                Item("Eyes_ItemPort", "Head_Eyes_Blue_01", Item("Lens_ItemPort", "Default_LensDisplay_PU"))
                + "<Item portName=\"Teeth_ItemPort\" itemName=\"Head_Teeth\" tag=\"Char_Accessory_Head\"/>"
                + "<Item portName=\"Hair_ItemPort\" itemName=\"hair_37\" tag=\"Char_Head_Hair Male\"><Items>"
                + Item("Material_Variant", "Hair_Var_Brown") + "</Items></Item>")
        + Item("mobiglas_attach", "MobiGlas",
               "<Item portName=\"mobiglas_screen_attach\" itemName=\"PersonalMobiGlas_PU\" tag=\"MobiGlas\"/>"
               "<Item portName=\"legacy_mobiglas_screen_attach\" itemName=\"LegacyMobiGlas\" tag=\"MobiGlas\"/>");
}

static const char* LoadLoadout(const char* gamePath) {
    __try {
        const uintptr_t framework = VCall<uintptr_t>(*g_lo.game, g_lo.frameworkSlot);
        const uintptr_t actor = framework ? VCall<uintptr_t>(framework, g_lo.actorSlot) : 0;
        if (!actor) return "you're not spawned yet";
        VCall<void>(actor, g_lo.loadSlot, gamePath, 8u, true);
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while loading the loadout";
    }
}

static void Equip(const std::string& xml) {
    static int  counter = 0;
    static char lastFile[MAX_PATH] = "";
    char dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("SC_USER", dir, sizeof(dir));
    if (n == 0 || n >= sizeof(dir)) { Log("[gear] SC_USER isn't set (start the game with launch_offline.bat)"); return; }
    char name[64], file[MAX_PATH], gamePath[96];
    snprintf(name, sizeof(name), "offline_loadout_%lu_%d.xml", GetCurrentProcessId(), ++counter);
    snprintf(file, sizeof(file), "%s\\%s", dir, name);
    snprintf(gamePath, sizeof(gamePath), "%%USER%%/%s", name);
    FILE* f = _fsopen(file, "w", _SH_DENYNO);
    if (!f) { Log("[gear] can't write %s", file); return; }
    fwrite(xml.data(), 1, xml.size(), f);
    fclose(f);
    if (const char* err = LoadLoadout(gamePath)) { Log("[gear] equip failed: %s", err); return; }
    Log("[gear] equipped (%s)", name);
    if (lastFile[0]) DeleteFileA(lastFile);
    strcpy_s(lastFile, file);
}

static void EquipPicks(const int picks[Gear_SlotCount]) { Equip(LoadoutXml(picks)); }

void ProcessLoadout() {
    if (!g_lo.ok || !g_tp.ok) return;
    if (g_gearState == 1) {
        uintptr_t actor, entity;
        bool live = false;
        __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (live) {
            __try { BuildGearLists(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[gear] fault while reading items.txt"); }
            InterlockedExchange(&g_gearState, 2);
        }
    }
    AcquireSRWLockExclusive(&g_gearLock);
    const auto req = g_equipRequest;
    g_equipRequest.pending = false;
    ReleaseSRWLockExclusive(&g_gearLock);
    if (req.pending && g_gearState == 2) EquipPicks(req.picks);
}
