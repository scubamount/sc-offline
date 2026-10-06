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
static SRWLOCK       g_outfitLock = SRWLOCK_INIT;
static struct { bool pending; int index; } g_wearRequest;

static int BuildOutfitList() {
    char path[MAX_PATH];
    if (!DataFilePath(path, sizeof(path), "outfits.txt")) return 0;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) { Log("[outfit] can't open %s", path); return 0; }
    // Like the original (orig 1800247b0): every item is checked against the game's entity
    // class registry, unknown ones are skipped, and an outfit left with no pieces is dropped
    // (its slot is reused by the next [section]).
    const uintptr_t registry = VCall<uintptr_t>(*g_tp.entitySystem, 0xC0);
    int count = 0, unknown = 0;
    bool open = false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char* name = line + strspn(line, " \t");
        if (!*name) continue;
        if (*name == '[') {
            if (open && g_outfits[count - 1].pieceCount == 0) --count;
            open = false;
            if (count >= kMaxOutfits) break;
            name[1 + strcspn(name + 1, "]")] = 0;
            strncpy_s(g_outfits[count].name, name + 1, _TRUNCATE);
            g_outfits[count].pieceCount = 0;
            ++count;
            open = true;
            continue;
        }
        if (!open) continue;
        Outfit& o = g_outfits[count - 1];
        if (o.pieceCount >= 32) continue;
        char* port = name;                              // first token: the port
        char* sep  = port + strcspn(port, " \t");
        if (!*sep) continue;                            // no item on this line
        *sep = 0;
        char* item = sep + 1 + strspn(sep + 1, " \t");  // second token: the item
        if (!*item) continue;
        item[strcspn(item, " \t\r\n")] = 0;
        // Hair_Color names a material variant, not an entity class.
        if (_stricmp(port, "Hair_Color") != 0 && !VCall<uintptr_t>(registry, 0x20, static_cast<const char*>(item))) {
            ++unknown;
            continue;
        }
        strncpy_s(o.pieces[o.pieceCount].port, port, _TRUNCATE);
        strncpy_s(o.pieces[o.pieceCount].item, item, _TRUNCATE);
        ++o.pieceCount;
    }
    if (open && g_outfits[count - 1].pieceCount == 0) --count;
    fclose(f);
    Log("[outfit] %d outfits loaded (%d unknown items skipped)", count, unknown);
    return count;
}

static const char* FindPiece(const Outfit& o, const char* port) {
    for (int i = 0; i < o.pieceCount; ++i)
        if (_stricmp(o.pieces[i].port, port) == 0) return o.pieces[i].item;
    return nullptr;
}

// The outfit's loadout, shaped like the original's (orig 180026f20): clothing flat under
// the body with Clothing_Torso2 (belt / vest layer) inside Clothing_Torso_1; armor under the
// undersuit, which defaults to the Odyssey suit when any armor piece is named; then the
// head (always present, see HeadItem) and MobiGlas.
static std::string OutfitXml(const Outfit& o) {
    const char* body = FindPiece(o, "Body_ItemPort");
    if (!body) body = "body_01";

    std::string clothing;
    for (const char* port : { "Clothing_Feet", "Clothing_Legs", "Clothing_Torso_0", "Clothing_Hands" })
        if (const char* item = FindPiece(o, port)) clothing += LoadoutItem(port, item);
    if (const char* torso1 = FindPiece(o, "Clothing_Torso_1")) {
        const char* torso2 = FindPiece(o, "Clothing_Torso2");
        clothing += LoadoutItem("Clothing_Torso_1", torso1, torso2 ? LoadoutItem("Clothing_Torso2", torso2) : std::string());
    }

    std::string armor;
    for (const char* port : { "Armor_Helmet", "Armor_Torso", "Armor_Arms", "Armor_Legs" })
        if (const char* item = FindPiece(o, port)) armor += LoadoutItem(port, item);
    const char* undersuit = FindPiece(o, "Armor_Undersuit");
    if (undersuit || !armor.empty())
        clothing += LoadoutItem("Armor_Undersuit", undersuit ? undersuit : "rsi_odyssey_undersuit_01_01_01", armor);

    HeadParts head;
    head.head      = FindPiece(o, "Head_ItemPort");
    head.eyes      = FindPiece(o, "Eyes_ItemPort");
    head.teeth     = FindPiece(o, "Teeth_ItemPort");
    head.eyebrow   = FindPiece(o, "Eyebrow_ItemPort");
    head.hair      = FindPiece(o, "Hair_ItemPort");
    head.hairColor = FindPiece(o, "Hair_Color");
    for (const char* port : { "Hat_ItemPort", "Eye_Accessories_ItemPort", "Head_Accessory_ItemPort",
                              "Head_Horn_ItemPort", "Jewellery_ItemPort" })
        if (const char* item = FindPiece(o, port)) head.accessories += LoadoutItem(port, item);

    return "<Loadout><Items>" + LoadoutItem("Body_ItemPort", body, clothing + HeadItem(head) + MobiGlasItem())
        + "</Items></Loadout>\n";
}

int Menu_OutfitCount() {
    if (g_outfitState != 2) { InterlockedCompareExchange(&g_outfitState, 1, 0); return -1; }
    return g_outfitCount;
}

const char* Menu_OutfitName(int index) {
    if (g_outfitState != 2 || index < 0 || index >= g_outfitCount) return "";
    return g_outfits[index].name;
}

// Menu-thread side only: EquipLoadoutXml() writes the loadout file and calls into the game's
// actor, so the request is queued and honoured by ProcessOutfits on the game thread.
void Menu_RequestWearOutfit(int index) {
    AcquireSRWLockExclusive(&g_outfitLock);
    g_wearRequest = { true, index };
    ReleaseSRWLockExclusive(&g_outfitLock);
}

// Deliberately not inlined into ProcessOutfits: OutfitXml returns a std::string, and
// a function that owns an object needing unwinding may not contain __try (C2712).
static void EquipOutfit(int index) {
    if (index < 0 || index >= g_outfitCount) return;
    EquipLoadoutXml(OutfitXml(g_outfits[index]), "outfit");
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
    if (req.pending && g_outfitState == 2) EquipOutfit(req.index);
}
