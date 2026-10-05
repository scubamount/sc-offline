#include "outfits.h"
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
} g_of;

bool ResolveOutfitApi(const Section& text, const Section& rdata) {
    const uint8_t* folder = FindCString(rdata, "Scripts/Loadouts/Player");
    // The scan below reads as far as h + 0x9A, i.e. p + 0x56 past the LEA it
    // started from, so the loop bound has to leave that much room inside .text.
    const uint8_t* const end = text.base + text.size - 0x5F;
    for (uint8_t* p = text.base + 0x44; folder && p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x15 || p + 7 + Rel32(p + 3) != folder) continue;
        const uint8_t* h = p - 0x44;
        if (!BytesMatch(h, "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 50 08 83 F8 01")
            || !BytesMatch(h + 0x18, "48 8B 0D") || !BytesMatch(h + 0x27, "FF 90") || !BytesMatch(h + 0x30, "48 8B 91")
            || !BytesMatch(h + 0x88, "41 B1 01") || !BytesMatch(h + 0x90, "41 B8 08 00 00 00") || !BytesMatch(h + 0x99, "FF 90"))
            continue;
        g_of.game          = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(h + 0x1F + Rel32(h + 0x1B)));
        g_of.frameworkSlot = Rel32(h + 0x29);
        g_of.actorSlot     = Rel32(h + 0x33);
        g_of.loadSlot      = Rel32(h + 0x9B);
        g_of.ok = true;
        break;
    }
    if (!g_of.ok) Log("[outfit] loadout loader not found; outfit menu disabled");
    return g_of.ok;
}

struct OutfitPiece {
    char port[64];
    char item[64];
};

struct Outfit {
    char name[128];
    OutfitPiece pieces[32];
    int pieceCount = 0;
};

static const int kMaxOutfits = 256;
static Outfit       g_outfits[kMaxOutfits];
static int          g_outfitCount = 0;
static volatile LONG g_outfitState = 0;
static volatile LONG g_s42VisorHud = 0;
static SRWLOCK       g_outfitLock = SRWLOCK_INIT;
static struct { bool pending; bool sq42; int index; } g_wearRequest;

static const Outfit kSq42Outfit = {
    "SQ42 outfit",
    {
        {"Body_ItemPort", "m_body_01"},
        {"Head_ItemPort", "sq42_pilot_head_01"},
        {"Hair_ItemPort", "hair_37"},
        {"Hair_Color", "Hair_Var_Brown"},
        {"Clothing_Torso_0", "sq42_pilot_shirt_01_01_01"},
        {"Clothing_Torso_1", "sq42_pilot_jacket_01_01_01"},
        {"Clothing_Legs", "sq42_pilot_pants_01_01_01"},
        {"Clothing_Feet", "sq42_pilot_boots_01_01_01"},
        {"Armor_Helmet", "sq42_pilot_helmet_01_01_01"},
    },
    9
};

void Menu_SetS42VisorHud(bool on) { InterlockedExchange(&g_s42VisorHud, on ? 1 : 0); }

static bool OutfitsFilePath(char* path, DWORD n) {
    if (!ShipsFilePath(path, n)) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (!slash || static_cast<DWORD>(slash + 1 - path) + 12 + 1 > n) return false;
    strcpy_s(slash + 1, n - static_cast<DWORD>(slash + 1 - path), "outfits.txt");
    return true;
}

static int BuildOutfitList() {
    char path[MAX_PATH];
    if (!OutfitsFilePath(path, sizeof(path))) return 0;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) { Log("[outfit] can't open %s", path); return 0; }
    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char* name = line + strspn(line, " \t");
        if (!*name) continue;
        if (*name == '[') {
            if (count >= kMaxOutfits) break;
            char* end = strchr(name, ']');
            if (!end) continue;
            *end = 0;
            strncpy_s(g_outfits[count].name, name + 1, _TRUNCATE);
            g_outfits[count].pieceCount = 0;
            ++count;
            continue;
        }
        if (count == 0) continue;
        Outfit& o = g_outfits[count - 1];
        if (o.pieceCount >= 32) continue;
        char* port = name;                              // first token: the port
        char* sep  = port + strcspn(port, " \t");
        if (!*sep) continue;                            // no item on this line
        *sep = 0;
        char* item = sep + 1 + strspn(sep + 1, " \t");  // second token: the item
        if (!*item) continue;
        item[strcspn(item, " \t\r\n")] = 0;
        strncpy_s(o.pieces[o.pieceCount].port, port, _TRUNCATE);
        strncpy_s(o.pieces[o.pieceCount].item, item, _TRUNCATE);
        ++o.pieceCount;
    }
    fclose(f);
    Log("[outfit] %d outfits loaded", count);
    return count;
}

static const char* FindPiece(const Outfit& o, const char* port) {
    for (int i = 0; i < o.pieceCount; ++i)
        if (_stricmp(o.pieces[i].port, port) == 0) return o.pieces[i].item;
    return nullptr;
}

static std::string Item(const char* port, const char* name, const std::string& children = std::string()) {
    std::string s = std::string("<Item portName=\"") + port + "\" itemName=\"" + name + "\"";
    return children.empty() ? s + "/>" : s + "><Items>" + children + "</Items></Item>";
}

static std::string OutfitXml(const Outfit& o) {
    const char* body = FindPiece(o, "Body_ItemPort");
    if (!body) body = "m_body_01";

    const char* undersuit = FindPiece(o, "Armor_Undersuit");
    const char* helmet = FindPiece(o, "Armor_Helmet");
    const char* torso = FindPiece(o, "Armor_Torso");
    const char* arms = FindPiece(o, "Armor_Arms");
    const char* legs = FindPiece(o, "Armor_Legs");

    const char* clothTorso0 = FindPiece(o, "Clothing_Torso_0");
    const char* clothTorso1 = FindPiece(o, "Clothing_Torso_1");
    const char* clothTorso2 = FindPiece(o, "Clothing_Torso2");
    const char* clothLegs = FindPiece(o, "Clothing_Legs");
    const char* clothFeet = FindPiece(o, "Clothing_Feet");
    const char* clothHands = FindPiece(o, "Clothing_Hands");

    const char* head = FindPiece(o, "Head_ItemPort");
    const char* hair = FindPiece(o, "Hair_ItemPort");
    const char* hairColor = FindPiece(o, "Hair_Color");
    const char* hat = FindPiece(o, "Hat_ItemPort");
    const char* eyebrow = FindPiece(o, "Eyebrow_ItemPort");
    const char* teeth = FindPiece(o, "Teeth_ItemPort");
    const char* eyes = FindPiece(o, "Eyes_ItemPort");

    std::string onTorso;
    if (clothTorso0) onTorso += Item("Clothing_Torso_0", clothTorso0);
    if (clothTorso1) onTorso += Item("Clothing_Torso_1", clothTorso1);
    if (clothTorso2) onTorso += Item("Clothing_Torso2", clothTorso2);

    std::string onArms;
    if (clothHands) onArms += Item("Clothing_Hands", clothHands);

    std::string onLegs;
    if (clothLegs) onLegs += Item("Clothing_Legs", clothLegs);
    if (clothFeet) onLegs += Item("Clothing_Feet", clothFeet);

    std::string onSuit;
    if (helmet) onSuit += Item("Armor_Helmet", helmet);
    if (torso) onSuit += Item("Armor_Torso", torso, onTorso);
    else if (!onTorso.empty()) onSuit += onTorso;
    if (arms) onSuit += Item("Armor_Arms", arms, onArms);
    if (legs) onSuit += Item("Armor_Legs", legs, onLegs);
    else if (!onLegs.empty()) onSuit += onLegs;

    std::string suitItem;
    if (undersuit) suitItem = Item("Armor_Undersuit", undersuit, onSuit);
    else if (!onSuit.empty()) suitItem = onSuit;

    std::string headChildren;
    if (hair) {
        std::string hairChildren;
        if (hairColor) hairChildren = Item("Material_Variant", hairColor);
        headChildren += Item("Hair_ItemPort", hair, hairChildren);
    }
    if (hat) headChildren += Item("Hat_ItemPort", hat);
    if (eyebrow) headChildren += Item("Eyebrow_ItemPort", eyebrow);
    if (teeth) headChildren += Item("Teeth_ItemPort", teeth);
    // The lens display sits under the eyes. PU is the plain one, the bare
    // Default_LensDisplay is the SQ42 visor HUD the Settings checkbox picks.
    const char* lens = InterlockedCompareExchange(&g_s42VisorHud, 0, 0) != 0
                     ? "Default_LensDisplay" : "Default_LensDisplay_PU";
    if (eyes) headChildren += Item("Eyes_ItemPort", eyes, Item("Lens_ItemPort", lens));
    else      headChildren += Item("Lens_ItemPort", lens);

    std::string headItem;
    if (head) headItem = Item("Head_ItemPort", head, headChildren);
    else if (!headChildren.empty()) headItem = headChildren;

    return "<Loadout><Items>" + Item("Body_ItemPort", body, suitItem + headItem)
        + Item("mobiglas_attach", "MobiGlas",
               "<Item portName=\"mobiglas_screen_attach\" itemName=\"PersonalMobiGlas_PU\" tag=\"MobiGlas\"/>"
               "<Item portName=\"legacy_mobiglas_screen_attach\" itemName=\"LegacyMobiGlas\" tag=\"MobiGlas\"/>")
        + "</Items></Loadout>\n";
}

static const char* LoadLoadout(const char* gamePath) {
    __try {
        const uintptr_t framework = VCall<uintptr_t>(*g_of.game, g_of.frameworkSlot);
        const uintptr_t actor = framework ? VCall<uintptr_t>(framework, g_of.actorSlot) : 0;
        if (!actor) return "you're not spawned yet";
        VCall<void>(actor, g_of.loadSlot, gamePath, 8u, true);
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
    if (n == 0 || n >= sizeof(dir)) { Log("[outfit] SC_USER isn't set (start the game with launch_offline.bat)"); return; }
    char name[64], file[MAX_PATH], gamePath[96];
    snprintf(name, sizeof(name), "offline_loadout_%lu_%d.xml", GetCurrentProcessId(), ++counter);
    snprintf(file, sizeof(file), "%s\\%s", dir, name);
    snprintf(gamePath, sizeof(gamePath), "%%USER%%/%s", name);
    FILE* f = _fsopen(file, "w", _SH_DENYNO);
    if (!f) { Log("[outfit] can't write %s", file); return; }
    fwrite(xml.data(), 1, xml.size(), f);
    fclose(f);
    if (const char* err = LoadLoadout(gamePath)) { Log("[outfit] equip failed: %s", err); return; }
    Log("[outfit] equipped (%s)", name);
    if (lastFile[0]) DeleteFileA(lastFile);
    strcpy_s(lastFile, file);
}

static void EquipOutfit(int index) {
    if (index < 0 || index >= g_outfitCount) return;
    Equip(OutfitXml(g_outfits[index]));
}

int Menu_OutfitCount() {
    if (g_outfitState != 2) { InterlockedCompareExchange(&g_outfitState, 1, 0); return -1; }
    return g_outfitCount;
}

const char* Menu_OutfitName(int index) {
    if (g_outfitState != 2 || index < 0 || index >= g_outfitCount) return "";
    return g_outfits[index].name;
}

void Menu_RequestWearOutfit(int index) {
    AcquireSRWLockExclusive(&g_outfitLock);
    g_wearRequest = { true, false, index };
    ReleaseSRWLockExclusive(&g_outfitLock);
}

// Menu-thread side only: Equip() writes the loadout file and calls into the game's
// actor, so the request is queued and honoured by ProcessOutfits on the game thread.
void Menu_RequestWearSq42() {
    AcquireSRWLockExclusive(&g_outfitLock);
    g_wearRequest = { true, true, -1 };
    ReleaseSRWLockExclusive(&g_outfitLock);
}

// Deliberately not inlined into ProcessOutfits: OutfitXml returns a std::string, and
// a function that owns an object needing unwinding may not contain __try (C2712).
static void WearRequested(bool sq42, int index) {
    if (sq42) Equip(OutfitXml(kSq42Outfit));
    else      EquipOutfit(index);
}

void ProcessOutfits() {
    if (!g_of.ok || !g_tp.ok) return;
    if (g_outfitState == 1) {
        uintptr_t actor, entity;
        bool live = false;
        __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (live) {
            __try { g_outfitCount = BuildOutfitList(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[outfit] fault while reading outfits.txt"); }
            InterlockedExchange(&g_outfitState, 2);
        }
    }
    AcquireSRWLockExclusive(&g_outfitLock);
    const auto req = g_wearRequest;
    g_wearRequest.pending = false;
    ReleaseSRWLockExclusive(&g_outfitLock);
    if (req.pending && g_outfitState == 2) WearRequested(req.sq42, req.index);
}
