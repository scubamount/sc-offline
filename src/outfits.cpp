#include "outfits.h"
#include "loadout.h"
#include "teleport.h"
#include "menu.h"
#include <share.h>
#include <string>

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

static int BuildOutfitList() {
    char path[MAX_PATH];
    if (!DataFilePath(path, sizeof(path), "outfits.txt")) return 0;
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
    if (clothTorso0) onTorso += LoadoutItem("Clothing_Torso_0", clothTorso0);
    if (clothTorso1) onTorso += LoadoutItem("Clothing_Torso_1", clothTorso1);
    if (clothTorso2) onTorso += LoadoutItem("Clothing_Torso2", clothTorso2);

    std::string onArms;
    if (clothHands) onArms += LoadoutItem("Clothing_Hands", clothHands);

    std::string onLegs;
    if (clothLegs) onLegs += LoadoutItem("Clothing_Legs", clothLegs);
    if (clothFeet) onLegs += LoadoutItem("Clothing_Feet", clothFeet);

    std::string onSuit;
    if (helmet) onSuit += LoadoutItem("Armor_Helmet", helmet);
    if (torso) onSuit += LoadoutItem("Armor_Torso", torso, onTorso);
    else if (!onTorso.empty()) onSuit += onTorso;
    if (arms) onSuit += LoadoutItem("Armor_Arms", arms, onArms);
    if (legs) onSuit += LoadoutItem("Armor_Legs", legs, onLegs);
    else if (!onLegs.empty()) onSuit += onLegs;

    std::string suitItem;
    if (undersuit) suitItem = LoadoutItem("Armor_Undersuit", undersuit, onSuit);
    else if (!onSuit.empty()) suitItem = onSuit;

    std::string headChildren;
    if (hair) {
        std::string hairChildren;
        if (hairColor) hairChildren = LoadoutItem("Material_Variant", hairColor);
        headChildren += LoadoutItem("Hair_ItemPort", hair, hairChildren);
    }
    if (hat) headChildren += LoadoutItem("Hat_ItemPort", hat);
    if (eyebrow) headChildren += LoadoutItem("Eyebrow_ItemPort", eyebrow);
    if (teeth) headChildren += LoadoutItem("Teeth_ItemPort", teeth);
    // The lens display sits under the eyes. PU is the plain one, the bare
    // Default_LensDisplay is the SQ42 visor HUD the Settings checkbox picks.
    const char* lens = InterlockedCompareExchange(&g_s42VisorHud, 0, 0) != 0
                     ? "Default_LensDisplay" : "Default_LensDisplay_PU";
    if (eyes) headChildren += LoadoutItem("Eyes_ItemPort", eyes, LoadoutItem("Lens_ItemPort", lens));
    else      headChildren += LoadoutItem("Lens_ItemPort", lens);

    std::string headItem;
    if (head) headItem = LoadoutItem("Head_ItemPort", head, headChildren);
    else if (!headChildren.empty()) headItem = headChildren;

    return "<Loadout><Items>" + LoadoutItem("Body_ItemPort", body, suitItem + headItem)
        + LoadoutItem("mobiglas_attach", "MobiGlas",
               "<Item portName=\"mobiglas_screen_attach\" itemName=\"PersonalMobiGlas_PU\" tag=\"MobiGlas\"/>"
               "<Item portName=\"legacy_mobiglas_screen_attach\" itemName=\"LegacyMobiGlas\" tag=\"MobiGlas\"/>")
        + "</Items></Loadout>\n";
}

static void EquipOutfit(int index) {
    if (index < 0 || index >= g_outfitCount) return;
    EquipLoadoutXml(OutfitXml(g_outfits[index]), "outfit");
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

// Menu-thread side only: EquipLoadoutXml() writes the loadout file and calls into the game's
// actor, so the request is queued and honoured by ProcessOutfits on the game thread.
void Menu_RequestWearSq42() {
    AcquireSRWLockExclusive(&g_outfitLock);
    g_wearRequest = { true, true, -1 };
    ReleaseSRWLockExclusive(&g_outfitLock);
}

// Deliberately not inlined into ProcessOutfits: OutfitXml returns a std::string, and
// a function that owns an object needing unwinding may not contain __try (C2712).
static void WearRequested(bool sq42, int index) {
    if (sq42) EquipLoadoutXml(OutfitXml(kSq42Outfit), "outfit");
    else      EquipOutfit(index);
}

void ProcessOutfits() {
    if (!LoadoutApiReady() || !g_tp.ok) return;
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
