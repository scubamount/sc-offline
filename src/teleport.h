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

void LoadSavedSpot(bool startingOverDaymar);
void TeleportTick(DWORD now);
