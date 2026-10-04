#include "quantum.h"
#include "hooks.h"
#include "teleport.h"
#include "dcb_patch.h"

using LoadDataCoreFn = uintptr_t(__fastcall*)(uintptr_t loader, uintptr_t path, uintptr_t a3, uintptr_t a4, uintptr_t a5);
using PakOpenFn  = uintptr_t(__fastcall*)(uintptr_t pak, const char* path, const char* mode, uint32_t flags);
using PakReadFn  = size_t(__fastcall*)(uintptr_t pak, void* data, size_t length, size_t elems, uintptr_t file, const void* tag);
using PakSeekFn  = int(__fastcall*)(uintptr_t pak, uintptr_t file, int offset, int mode);
using PakCloseFn = int(__fastcall*)(uintptr_t pak, uintptr_t file);
constexpr size_t kPakOpen = 0x148, kPakRead = 0x160, kPakSeek = 0x1D0, kPakClose = 0x1E0;

static LoadDataCoreFn g_loadDataCoreOrig = nullptr;
static uintptr_t*     g_cryPak = nullptr;
static bool           g_pakCalls = false, g_hooked = false;

static struct {
    PakOpenFn  open;  PakReadFn  read;
    PakSeekFn  seek;  PakCloseFn close;
    DWORD      thread;
    uintptr_t  file;
    int        state;
    uint64_t   pos, real;
} g_dcb;

struct DcbRun { uint64_t start, len, orig; const uint8_t* bytes; };
static DcbRun g_runs[2 * _countof(dcbpatch::kEdits) + 1];
static int    g_runCount = 0;

static void BuildRuns() {
    uint64_t o = 0, p = 0;
    g_runCount = 0;
    for (const dcbpatch::Edit& e : dcbpatch::kEdits) {
        if (e.at > o) { g_runs[g_runCount++] = { p, e.at - o, o, nullptr }; p += e.at - o; o = e.at; }
        if (e.added)  { g_runs[g_runCount++] = { p, e.added, 0, e.bytes }; p += e.added; }
        o += e.removed;
    }
    g_runs[g_runCount++] = { p, dcbpatch::kOrigSize - o, o, nullptr };
}

static const void* ReadTag() {
    static const char* const sealed = "bool __cdecl CDataCoreLoader::InitializeBinary(const class CryStringT<char> &,bool)";
    static const char* const open = sealed;
    static const char* const name = open;
    return &name;
}

static size_t ReadPatched(uintptr_t pak, uint8_t* out, size_t n, const void* name) {
    size_t done = 0;
    for (int i = 0; i < g_runCount && done < n; ++i) {
        const DcbRun& r = g_runs[i];
        if (g_dcb.pos >= r.start + r.len) continue;
        const uint64_t into = g_dcb.pos - r.start;
        const size_t k = r.len - into < n - done ? static_cast<size_t>(r.len - into) : n - done;
        if (r.bytes) {
            memcpy(out + done, r.bytes + into, k);
        } else {
            const uint64_t at = r.orig + into;
            if (g_dcb.real != at) g_dcb.seek(pak, g_dcb.file, static_cast<int>(at), SEEK_SET);
            const size_t got = g_dcb.read(pak, out + done, 1, k, g_dcb.file, name);
            g_dcb.real = at + got;
            if (got != k) { done += got; g_dcb.pos += got; break; }
        }
        done += k;
        g_dcb.pos += k;
    }
    return done;
}

static bool IsPatchable(uintptr_t pak, uintptr_t file) {
    uint8_t buf[sizeof(dcbpatch::kOrigHeader)];
    bool same = g_dcb.read(pak, buf, 1, sizeof(buf), file, ReadTag()) == sizeof(buf)
             && !memcmp(buf, dcbpatch::kOrigHeader, sizeof(buf));
    for (const dcbpatch::Edit& e : dcbpatch::kEdits)
        if (same && e.removed)
            same = e.removed <= sizeof(buf) && g_dcb.seek(pak, file, static_cast<int>(e.at), SEEK_SET) == 0
                && g_dcb.read(pak, buf, 1, e.removed, file, ReadTag()) == e.removed && !memcmp(buf, e.old, e.removed);
    g_dcb.seek(pak, file, 0, SEEK_SET);
    return same;
}

static uintptr_t __fastcall PakOpenHook(uintptr_t pak, const char* path, const char* mode, uint32_t flags) {
    const uintptr_t file = g_dcb.open(pak, path, mode, flags);
    const size_t n = path ? strlen(path) : 0;
    if (file && !g_dcb.file && GetCurrentThreadId() == g_dcb.thread && n > 4 && !_stricmp(path + n - 4, ".dcb")) {
        g_dcb.file = file;
        g_dcb.pos = g_dcb.real = 0;
        g_dcb.state = IsPatchable(pak, file) ? 1 : -1;
    }
    return file;
}

static size_t __fastcall PakReadHook(uintptr_t pak, void* data, size_t length, size_t elems, uintptr_t file, const void* tag) {
    if (!file || file != g_dcb.file || g_dcb.state != 1) return g_dcb.read(pak, data, length, elems, file, tag);
    return length ? ReadPatched(pak, static_cast<uint8_t*>(data), length * elems, tag ? tag : ReadTag()) / length : 0;
}

static int __fastcall PakSeekHook(uintptr_t pak, uintptr_t file, int offset, int mode) {
    if (!file || file != g_dcb.file || g_dcb.state != 1) return g_dcb.seek(pak, file, offset, mode);
    const int64_t base = mode == SEEK_SET ? 0 : mode == SEEK_CUR ? static_cast<int64_t>(g_dcb.pos) : dcbpatch::kPatchedSize;
    if (mode < SEEK_SET || mode > SEEK_END || base + offset < 0) return -1;
    g_dcb.pos = static_cast<uint64_t>(base + offset);
    return 0;
}

static int __fastcall PakCloseHook(uintptr_t pak, uintptr_t file) {
    if (file && file == g_dcb.file) g_dcb.file = 0;
    return g_dcb.close(pak, file);
}

static bool SwapPakSlots(bool on) {
    const uintptr_t pak = g_cryPak ? *g_cryPak : 0;
    if (!pak) return false;
    void** const vt = reinterpret_cast<void**>(Rd<uintptr_t>(pak));
    if (on && !g_dcb.open) {
        g_dcb.open  = reinterpret_cast<PakOpenFn>(vt[kPakOpen / 8]);
        g_dcb.read  = reinterpret_cast<PakReadFn>(vt[kPakRead / 8]);
        g_dcb.seek  = reinterpret_cast<PakSeekFn>(vt[kPakSeek / 8]);
        g_dcb.close = reinterpret_cast<PakCloseFn>(vt[kPakClose / 8]);
    }
    DWORD old;
    if (!VirtualProtect(vt + kPakOpen / 8, kPakClose - kPakOpen + 8, PAGE_READWRITE, &old)) return false;
    vt[kPakOpen / 8]  = on ? reinterpret_cast<void*>(&PakOpenHook)  : reinterpret_cast<void*>(g_dcb.open);
    vt[kPakRead / 8]  = on ? reinterpret_cast<void*>(&PakReadHook)  : reinterpret_cast<void*>(g_dcb.read);
    vt[kPakSeek / 8]  = on ? reinterpret_cast<void*>(&PakSeekHook)  : reinterpret_cast<void*>(g_dcb.seek);
    vt[kPakClose / 8] = on ? reinterpret_cast<void*>(&PakCloseHook) : reinterpret_cast<void*>(g_dcb.close);
    VirtualProtect(vt + kPakOpen / 8, kPakClose - kPakOpen + 8, old, &old);
    return true;
}

static uintptr_t __fastcall LoadDataCoreHook(uintptr_t loader, uintptr_t path, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
    g_dcb.thread = GetCurrentThreadId();
    g_dcb.state = 0;
    const bool swapped = SwapPakSlots(true);
    const uintptr_t ok = g_loadDataCoreOrig(loader, path, a3, a4, a5);
    if (swapped) SwapPakSlots(false);
    g_dcb.thread = 0;
    g_dcb.file = 0;
    const char* const result = (ok & 0xFF) ? "ok" : "FAILED";
    if (!swapped)
        Log("[!] new quantum drive: CryPak not found; game data loaded %s without the new drive", result);
    else if (g_dcb.state == 1)
        Log("[+] new quantum drive: game data patched as it loaded (%d edits, %u -> %u bytes; the Gladius gets the new drive), load %s",
            static_cast<int>(_countof(dcbpatch::kEdits)), dcbpatch::kOrigSize, dcbpatch::kPatchedSize, result);
    else if (g_dcb.state == -1)
        Log("[!] new quantum drive: Game2.dcb isn't the 4.10.0 one the patch was made for (game updated?); loaded %s without the new drive", result);
    else
        Log("[!] new quantum drive: the loader opened no .dcb; game data loaded %s without the new drive", result);
    return ok;
}

using OnActionFn = uintptr_t(__fastcall*)(uintptr_t handler, int action, int mode, float value, void* functor);
using StartUseFn = void(__fastcall*)(uintptr_t usable, uintptr_t user);
struct QuantumInput { int32_t action, mode; float value; };
using DriveInputFn = void(__fastcall*)(uintptr_t drive, const QuantumInput* input);
static OnActionFn   g_onActionOrig = nullptr;
static StartUseFn   g_startUseOrig = nullptr;
static DriveInputFn g_driveInput = nullptr;
static bool         g_inputHooked = false, g_startUseHooked = false, g_capsHeld = false;

constexpr int kEngage = 579;
constexpr int kEnableHold = 582;
constexpr int kPress = 1, kRelease = 2;

static uintptr_t g_handler = 0, g_handlerVt = 0;
static bool      g_held = false, g_byMouse = false;
static uintptr_t g_drive = 0, g_driveVt = 0;

static const char* StateName(int s) {
    static const char* const names[] = { "Off", "WaitForInputDelay", "TargetLocking", "Charging", "BoostingSlow",
                                         "Travelling", "CancelingTravel", "CancelingBoost", "Cooldown", "Done" };
    return s >= 0 && s < 10 ? names[s] : "?";
}

static const char* ReasonName(int r) {
    static const char* const names[] = { "None", "TargetTooClose", "TargetTooFar", "SolarObstruction", "QuantumObstruction",
                                         "MovingBackwards", "OutOfFuel", "GroupNotReady", "GroupHasObstruction", "NavpointBlocked",
                                         "RotationNotAligned", "RotationUnstable", "BlockedByEvent", "LandingGear", "Unknown" };
    return r >= 0 && r < 15 ? names[r] : "none";
}

static void NoteAction(int action) {
    static int seen[32], count = 0;
    for (int i = 0; i < count; ++i) if (seen[i] == action) return;
    if (count == 32) return;
    seen[count++] = action;
    Log("[qt] new drive input handler got action %d", action);
}

static uintptr_t __fastcall OnActionHook(uintptr_t handler, int action, int mode, float value, void* functor) {
    NoteAction(action);
    if (action != kEngage) return g_onActionOrig(handler, action, mode, value, functor);
    if (g_held && !g_byMouse) {
        g_held = false;
        Log("[qt] engage again -> quantum hold released");
        return g_onActionOrig(handler, kEnableHold, kRelease, 0.0f, functor);
    }
    g_handler = handler;
    g_handlerVt = Rd<uintptr_t>(handler);
    g_byMouse = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    g_held = true;
    Log("[qt] engage -> quantum hold pressed (%s)", g_byMouse ? "let go of the mouse button to stop" : "engage again to stop");
    return g_onActionOrig(handler, kEnableHold, kPress, 1.0f, functor);
}

static void __fastcall StartUseHook(uintptr_t usable, uintptr_t user) {
    g_startUseOrig(usable, user);
    const uintptr_t drive = usable - 0x1C0;
    __try {
        if (!Rd<uint8_t>(drive + 0xB60)) return;
        g_driveVt = Rd<uintptr_t>(drive);
        g_drive = drive;
        if (float* disableCharge = reinterpret_cast<float*>(Rd<uintptr_t>(drive + 0x18) + 0xC0); *disableCharge < 1e9f) {
            Log("[qt] drive data: disableCharge %.1f -> 1e9 (it blocked the drive at any charge)", *disableCharge);
            *disableCharge = 1e9f;
        }
        Log("[qt] you control a new quantum drive: hold Caps Lock, or B for NAV mode then hold left mouse "
            "(no quantum target = boost, target = travel)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

using EffectUpdateFn = uintptr_t(__fastcall*)(uintptr_t handler, uint64_t state, double a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8);
static EffectUpdateFn g_effectUpdateOrig = nullptr;
static bool           g_effectGuarded = false;

static uintptr_t __fastcall EffectUpdateHook(uintptr_t handler, uint64_t state, double a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8) {
    if (handler && (Rd<uint64_t>(handler) & kPtrMask)) return g_effectUpdateOrig(handler, state, a3, a4, a5, a6, a7, a8);
    static bool logged = false;
    if (!logged) { logged = true; Log("[qt] quantum effects aren't linked to the ship; skipped them (no crash)"); }
    return 0;
}

using ChargeFn = uint8_t(__fastcall*)(uintptr_t drive, float alignment, float dt, uint8_t blocked);
using SplineGetYFn = float(__fastcall*)(uintptr_t spline, float x);
static ChargeFn     g_chargeOrig = nullptr;
static SplineGetYFn g_splineGetY = nullptr;
static bool         g_chargeHooked = false;

static uint8_t __fastcall ChargeHook(uintptr_t drive, float alignment, float dt, uint8_t blocked) {
    const uint8_t result = g_chargeOrig(drive, alignment, dt, blocked);
    if (drive != g_drive) return result;
    static DWORD chargingSince = 0, lastLog = 0;
    const DWORD now = GetTickCount();
    const int32_t state = Rd<int32_t>(drive + 0xC48);
    if (state != 3) chargingSince = 0;
    else if (!chargingSince) chargingSince = now;
    const bool full = state == 4 || state == 5 || (state == 3 && now - chargingSince > 3000);
    if (now - lastLog >= 1000) {
        lastLog = now;
        __try {
            const uintptr_t params = Rd<uintptr_t>(drive + 0x18);
            const float* rate = reinterpret_cast<const float*>(params + 0x560);
            const float curve = g_splineGetY ? g_splineGetY(params + 0x320 + 0x20, 1.0f) : -1.0f;
            Log("[qt] %s: charge %.2f (game: %d), alignment %.2f, blocked %d, rates %.2f/%.2f/%.2f, curve(1) %.2f%s",
                StateName(state), Rd<float>(drive + 0xCA8), result, alignment, blocked, rate[0], rate[1], rate[2], curve,
                full ? " -> kept full" : "");
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!full) return result;
    *reinterpret_cast<float*>(drive + 0xCA8) = 1.0f;
    return 3;
}

using HandleValidFn = bool(__fastcall*)(const uint64_t* handle);
static HandleValidFn g_handleValid = nullptr;
static uintptr_t*    g_audioSystem = nullptr;
static uint32_t      g_lightballId = 0, g_portalId = 0;
static int           g_soundStage = 0;

static uint32_t SoundId(const char* name) {
    uint32_t id = 0;
    if (g_audioSystem && *g_audioSystem) VCall<void>(*g_audioSystem, 0x58, name, &id);
    return id;
}

static void ShipSound(uint32_t id, bool play) {
    const uintptr_t handler = Rd<uintptr_t>(g_drive + 0x800);
    if (!handler || !id || !g_handleValid || !g_handleValid(reinterpret_cast<const uint64_t*>(handler))) return;
    const uintptr_t audio = Rd<uintptr_t>(handler) & kPtrMask;
    if (play) VCall<uint8_t>(audio, 0x700, id, 1u, 0u);
    else      VCall<uint8_t>(audio, 0x718, id, false, 1u);
}

static void UpdateBoostSounds(int32_t state) {
    __try {
        if (state >= 1 && state <= 3 && g_soundStage == 0) {
            if (!g_lightballId) {
                g_lightballId = SoundId("Play_SSQT_Stage_2_Lightball");
                g_portalId = SoundId("Play_SSQT_Stage_3_Portal");
                Log("[qt] sound ids: Lightball %08X, Portal %08X", g_lightballId, g_portalId);
            }
            ShipSound(g_lightballId, true);
            g_soundStage = 1;
        }
        if (state == 3 && g_soundStage == 1) { ShipSound(g_portalId, true); g_soundStage = 2; }
        if ((state == 4 || state == 5) && g_soundStage) {
            if (g_soundStage == 1) ShipSound(g_lightballId, false);
            g_soundStage = 3;
        }
        if ((state == 0 || state >= 8) && g_soundStage) {
            if (g_soundStage < 3) { ShipSound(g_lightballId, false); ShipSound(g_portalId, false); }
            g_soundStage = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

using SendEffectTagFn = uintptr_t(__fastcall*)(uintptr_t entitySystem, uintptr_t entity, const void* evt);
static SendEffectTagFn g_sendEffectTag = nullptr;
static uintptr_t       g_warmedDrive = 0;
static DWORD           g_warmUpSince = 0;

static void WarmUpEffects(DWORD now) {
    if (!g_sendEffectTag || !g_tp.entitySystem || !*g_tp.entitySystem || g_warmedDrive == g_drive) return;
    if (!g_warmUpSince) g_warmUpSince = now;
    __try {
        const uintptr_t handler = Rd<uintptr_t>(g_drive + 0x7F0);
        if (Rd<int32_t>(g_drive + 0xC48) != 0 || !handler || !(Rd<uint64_t>(handler) & kPtrMask)
            || !g_handleValid || !g_handleValid(reinterpret_cast<const uint64_t*>(handler + 8))) {
            if (now - g_warmUpSince > 15000) {
                g_warmedDrive = g_drive;
                Log("[qt] quantum effects never linked to the ship; not warmed up");
            }
            return;
        }
        const uintptr_t entity = Rd<uint64_t>(handler + 8) & kPtrMask;
        static const size_t kTags[] = { 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xC8, 0xE0, 0xE8, 0xF0, 0xF8, 0x100 };
        int sent = 0;
        for (int on = 1; on >= 0; --on)
            for (size_t off : kTags) {
                struct { uint64_t tag; uint8_t on; uint8_t pad[7]; } evt = { Rd<uint64_t>(handler + off), static_cast<uint8_t>(on), {} };
                if (static_cast<uint16_t>(evt.tag) == 0xFFFF) continue;
                g_sendEffectTag(*g_tp.entitySystem, entity, &evt);
                sent += on;
            }
        g_warmedDrive = g_drive;
        g_warmUpSince = 0;
        Log("[qt] quantum effects warmed up (%d tags on and off) - the first boost plays in full", sent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_warmedDrive = g_drive;
        Log("[qt] fault warming up the quantum effects");
    }
}

void ProcessQuantum() {
    if (g_held && g_byMouse && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        g_held = false;
        __try {
            if (Rd<uintptr_t>(g_handler) == g_handlerVt) {
                uint64_t functor[4] = {};
                g_onActionOrig(g_handler, kEnableHold, kRelease, 0.0f, functor);
                Log("[qt] mouse button up -> quantum hold released");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (!g_drive) return;
    const bool caps = GameHasFocus() && (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
    if (caps != g_capsHeld && g_driveInput) {
        g_capsHeld = caps;
        const QuantumInput input = { kEnableHold, caps ? kPress : kRelease, caps ? 1.0f : 0.0f };
        __try {
            if (Rd<uintptr_t>(g_drive) == g_driveVt) g_driveInput(g_drive, &input);
            Log("[qt] Caps Lock %s -> quantum hold %s", caps ? "down" : "up", caps ? "pressed" : "released");
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    struct { int32_t state, reason; uint8_t powered, travelBlocked, boostBlocked, input; } now{};
    __try {
        if (Rd<uintptr_t>(g_drive) != g_driveVt) { g_drive = 0; return; }
        now.state = Rd<int32_t>(g_drive + 0xC48);
        now.reason = Rd<int32_t>(g_drive + 0xC3C);
        now.powered = Rd<uint8_t>(g_drive + 0xC68);
        now.travelBlocked = Rd<uint8_t>(g_drive + 0xC69);
        now.boostBlocked = Rd<uint8_t>(g_drive + 0xC6A);
        now.input = Rd<uint8_t>(g_drive + 0xBB0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_drive = 0; return; }
    UpdateBoostSounds(now.state);
    WarmUpEffects(GetTickCount());
    static decltype(now) last{ -1, -1 };
    if (!memcmp(&now, &last, sizeof(now))) return;
    last = now;
    Log("[qt] drive: %s, powered %d, input %d, boost blocked %d (reason %s), travel blocked %d",
        StateName(now.state), now.powered, now.input, now.boostBlocked, ReasonName(now.reason), now.travelBlocked);
}

static void HookBoostInput(const Section& text) {
    int n = 0;
    if (uint8_t* fn = FindUniquePattern(text, "40 53 41 56 48 83 EC 68 8D 82 2F FE FF FF 45 33 F6 83 F8 77 0F 87", n))
        g_inputHooked = HookFunction(fn, 8, reinterpret_cast<void*>(&OnActionHook), reinterpret_cast<void**>(&g_onActionOrig));
    uint8_t* fn = FindUniquePattern(text, "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 48 48 8B 02 4C 8D B9 40 FE FF FF", n);
    g_startUseHooked = fn && HookFunction(fn, 5, reinterpret_cast<void*>(&StartUseHook), reinterpret_cast<void**>(&g_startUseOrig));
    g_driveInput = reinterpret_cast<DriveInputFn>(FindUniquePattern(text, "48 83 EC 28 8B 02 05 2F FE FF FF 83 F8 77 0F 87", n));
    fn = FindUniquePattern(text, "48 8B C4 C5 FA 11 50 18 55 53 56 57 41 57 48 8D A8 B8 FC FF FF 48 81 EC 20 04 00 00", n);
    g_effectGuarded = fn && HookFunction(fn, 8, reinterpret_cast<void*>(&EffectUpdateHook), reinterpret_cast<void**>(&g_effectUpdateOrig));
    fn = FindUniquePattern(text, "48 8B C4 53 57 48 81 EC B8 00 00 00 48 89 70 E8 41 0F B6 F9 C5 F8 29 70 D8 C5 F8 29 78 C8", n);
    g_chargeHooked = fn && HookFunction(fn, 5, reinterpret_cast<void*>(&ChargeHook), reinterpret_cast<void**>(&g_chargeOrig));
    g_splineGetY = reinterpret_cast<SplineGetYFn>(FindUniquePattern(text,
        "48 89 5C 24 10 55 48 8D 6C 24 A9 48 81 EC 90 00 00 00 48 8B D9 C7 45 F7 00 29 00 00 33 C9", n));
    if (uint8_t* p = FindUniquePattern(text, "83 79 10 00 4C 8D 41 10 75 33 48 8B 41 08 48 B9 FF FF FF FF FF FF 00 00 "
                                             "48 8B D0 48 23 D1 74 1D 48 B9 00 00 00 00 00 00 FF 3F 48 85 C1 74 0E 48 8B 0D", n))
        g_audioSystem = reinterpret_cast<uintptr_t*>(p + 0x36 + Rel32(p + 0x32));
    g_handleValid = reinterpret_cast<HandleValidFn>(FindUniquePattern(text,
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 19 48 8B F9 48 85 DB 74 ?? "
        "48 B8 FF FF FF FF FF FF 00 00 48 8B F3 48 23 F0 48 8B CE E8 ?? ?? ?? ?? 48 8B E8 0F B7 40 04 "
        "66 83 F8 04 74 ?? 48 C1 EB 30 B9 FF 0F 00 00 66 23 D9 66 39 5D 02 75 ?? 66 83 F8 02", n));
}

void ResolveQuantumApi(const Section& text, const Section& rdata) {
    HookBoostInput(text);
    const uint8_t* header = FindCString(rdata, "C:\\workspace\\CryEngine\\Code\\CryEngine\\CryCommon\\Events/VFX/EntityEffectSystem.h");
    uint8_t* senders[8];
    const int found = FindPattern(text, "48 89 5C 24 08 57 48 83 EC 50 8B 05 ?? ?? ?? ?? 48 8B FA 4C 89 44 24 20 48 8B D9 85 C0 75 19 "
                                        "41 B8 17 00 00 00 48 8D 15", senders, 8);
    for (int i = 0; header && i < found && i < 8; ++i)
        if (senders[i] + 0x2C + Rel32(senders[i] + 0x28) == header) g_sendEffectTag = reinterpret_cast<SendEffectTagFn>(senders[i]);
    const uint8_t* msg = FindCString(rdata, "DCB file is smaller than expected");
    const uint8_t* site = msg ? FindRipLea(text, 0x4C, 0x8D, 0x0D, msg) : nullptr;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = site ? RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(site), &base, nullptr) : nullptr;
    for (int i = 0; rf && i < 8; ++i) {
        const uint8_t* info = reinterpret_cast<const uint8_t*>(base + rf->UnwindData);
        if (!((info[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(info + 4 + ((info[2] + 1) & ~1) * 2));
    }
    uint8_t* loader = rf ? reinterpret_cast<uint8_t*>(base + rf->BeginAddress) : nullptr;
    if (loader && !BytesMatch(loader, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57")) loader = nullptr;
    for (int i = 0; loader && !g_cryPak && i < 0x400; ++i)
        if (BytesMatch(loader + i, "48 8B 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 48 8B 55 ?? 45 33 C9 48 8B 01 FF 90 48 01 00 00"))
            g_cryPak = reinterpret_cast<uintptr_t*>(loader + i + 7 + Rel32(loader + i + 3));
    static const size_t kSlots[] = { kPakRead, kPakSeek, kPakClose };
    int calls = 0;
    for (size_t slot : kSlots)
        for (int i = 0; loader && i < 0x2400; ++i)
            if (loader[i] == 0xFF && loader[i + 1] == 0x90 && Rel32(loader + i + 2) == static_cast<int32_t>(slot)) { ++calls; break; }
    g_pakCalls = calls == 3;
    BuildRuns();
    g_hooked = loader && g_cryPak && g_pakCalls
        && HookFunction(loader, 15, reinterpret_cast<void*>(&LoadDataCoreHook), reinterpret_cast<void**>(&g_loadDataCoreOrig));
}

void LogQuantum() {
    if (g_hooked) Log("[+] new quantum drive: game data loader hooked (the Gladius' drive data is patched in as it loads)");
    else          Log("[!] new quantum drive: game data loader not hooked (CryPak %s, its calls %s)",
                      g_cryPak ? "ok" : "MISSING", g_pakCalls ? "ok" : "MISSING");
    const bool audio = g_audioSystem && g_handleValid;
    if (g_inputHooked && g_startUseHooked && g_driveInput && g_effectGuarded && g_chargeHooked && audio && g_sendEffectTag)
        Log("[+] quantum boost: hold Caps Lock, or NAV mode + hold left mouse");
    else
        Log("[!] quantum boost: input hook %s, drive hook %s, drive input %s, effects guard %s, charge hook %s, audio %s, effect warm-up %s",
            g_inputHooked ? "ok" : "MISSING", g_startUseHooked ? "ok" : "MISSING", g_driveInput ? "ok" : "MISSING",
            g_effectGuarded ? "ok" : "MISSING", g_chargeHooked ? "ok" : "MISSING", audio ? "ok" : "MISSING",
            g_sendEffectTag ? "ok" : "MISSING");
}
