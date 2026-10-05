#pragma once
#include "common.h"

struct TeleportApi {
    bool       ok = false;
    uintptr_t* clientMgr = nullptr;
    uintptr_t* entitySystem = nullptr;
    void*      handleFromId = nullptr;
};
extern TeleportApi g_tp;

bool ResolveTeleportApi(const Section& text, const Section& rdata);

bool        GetLocalPlayer(uintptr_t& actor, uintptr_t& entity);
const char* ZoneName(uintptr_t zone);
uintptr_t   ZoneParent(uintptr_t zone);
uint64_t    ZoneId(uintptr_t zone);
uintptr_t   ZoneFromId(uint64_t zoneId);
void        Vec3Out(uintptr_t obj, size_t off, double out[3]);
void        LocalToWorld(uintptr_t zone, const double local[3], double world[3]);
bool        WorldToLocal(uintptr_t zone, const double world[3], double local[3]);

const char* TeleportToEntity(uint64_t entityId, double up);

// A saved position: where you were in each zone, innermost first.
constexpr int kMaxZoneDepth = 12;
struct ZoneSpot { char name[96]; double local[3]; };
struct Spot { int n = 0; ZoneSpot z[kMaxZoneDepth] = {}; };

const char* CaptureCurrentSpot(Spot& s);                          // where you're standing now
const char* GoToSpot(const Spot& s, DWORD now, const char* why);   // teleport, then refine as zones stream in
const char* TeleportIntoZone(uintptr_t zone, const double local[3]);
void        SpotSystemName(const Spot& s, char* out, size_t n);   // "Stanton", from the OOC_<system>_ zone names
void        CurrentSystemName(char* out, size_t n);               // the system you're in now, or ""
uintptr_t   SystemZoneOf(uintptr_t zone);
void        CurrentSystemZoneName(char* out, size_t n);           // "SolarSystem_<id>" you're in, or ""                         // the SolarSystem_* zone above a zone, or 0

void LoadSavedSpot(bool startingOverDaymar);
void TeleportTick(DWORD now);
