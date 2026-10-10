#include "quantum.h"
#include "hooks.h"
#include "teleport.h"
#include "version.h"
#include "sco/caps.h"
#include "sco/datacore.h"
#include "sco/datacore_service.h"
#include "sco/game/features.h"
#include "sco/game/pak.h"
#include "world_caps.h"
#include "sco/plugins.h"
#include "sco/vfs.h"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <vector>

// The new quantum drive's game data: sc-offline's built-in data pack,
// data/builtin/quantum/datacore/quantum_drive.toml. It names records and fields, not bytes, so it
// keeps applying across game patches while they exist (design decision 11: made once with
// `sco-dcb diff`, edited by hand since). A sco::vfs mount on Game2.dcb runs sco-core's pack loader
// (sco::datacore::service::Load) when the DataCore loader opens the file: the file is read through
// the engine's own CryPak reads, parsed, the pack applied, and the splices go back to sco::vfs.
// sco::game::pak serves the result to the loader. A pack that doesn't apply leaves the mount
// inert and the game loads its own bytes.
static constexpr const char* kPackOwner = "quantum";       // the built-in the pack belongs to
static constexpr const char* kPackName = "quantum_drive";  // datacore/quantum_drive.toml
static sco::vfs::MountTable  g_dcbMounts;
static std::filesystem::path g_packDir;   // data/builtin/quantum; set before sco::game::pak::Enable
static bool                  g_pakEnabled = false;
static const char*           g_notHooked = "see [pak] above";
static std::atomic<int>      g_dataPatched{ -1 };  // -1 no load yet, 0 loaded without the drive, 1 patched

// What the pack did at the last load: written by ApplyDrivePack, read by ReportDataLoad (both on
// the loader's thread, one after the other; the lock keeps it simple).
struct PackOutcome {
    bool        ran = false, applied = false;
    size_t      ops = 0;       // patcher operations applied
    std::string reason;        // why it didn't apply: the pack loader's first failure
};
static std::mutex  g_packLock;
static PackOutcome g_pack;

// Records the outcome; the mount's result is its reason (empty when the pack applied).
static sco::vfs::Result Finish(PackOutcome&& o) {
    sco::vfs::Result r{ o.applied ? std::string() : o.reason };
    std::lock_guard<std::mutex> hold(g_packLock);
    g_pack = std::move(o);
    return r;
}

// The mount's producer (sco::vfs::Transform), run once when the loader opens Game2.dcb.
static sco::vfs::Result ApplyDrivePack(sco::vfs::BaseIo& base, uint64_t baseSize, std::vector<sco::vfs::Splice>& out) {
    PackOutcome o;
    o.ran = true;
    std::vector<uint8_t> file;
    try {
        file.resize(static_cast<size_t>(baseSize));
    } catch (const std::bad_alloc&) {
        o.reason = "no memory to read Game2.dcb";
        return Finish(std::move(o));
    }
    size_t got = 0;
    if (base.Seek(0))
        while (got < file.size()) {
            const size_t n = base.Read(file.data() + got, file.size() - got);
            if (n == 0) break;
            got += n;
        }
    if (got != file.size()) {
        o.reason = "Game2.dcb couldn't be read";
        return Finish(std::move(o));
    }
    sco::datacore::Schema schema;
    if (!schema.Parse(file)) {
        o.reason = "Game2.dcb's layout was refused: " + schema.error;
        return Finish(std::move(o));
    }
    // Built-ins have no plugin folder, so the pack loader gets the quantum built-in's pack as a
    // data-pack entry of its own, first in plugin order. No dataRoot: sc-offline doesn't run the
    // sco.datacore service, so there are no saved patches to read.
    std::vector<sco::plugins::Plugin> list(1);
    sco::plugins::Plugin& p = list[0];
    p.dir = g_packDir;
    p.folder = kPackOwner;
    p.manifest.id = kPackOwner;
    p.manifest.name = "quantum drive data";
    p.manifest.version = SCO_VERSION;
    p.manifest.kind = sco::plugins::Kind::Data;
    p.manifestOk = true;
    p.state = sco::plugins::State::Ready;
    sco::plugins::ContentIndex index;
    index.Build(list);
    const sco::datacore::service::LoadResult loaded = sco::datacore::service::Load(schema, list, index, {});
    const sco::datacore::PackReport* rep = nullptr;
    for (const sco::datacore::PackReport& r : loaded.result.packs)
        if (r.plugin == kPackOwner) rep = &r;   // the built-in's one file
    if (!rep) {
        o.reason = list[0].state == sco::plugins::State::Refused
                       ? list[0].reason
                       : (g_packDir / "datacore" / (std::string(kPackName) + ".toml")).string() + " is missing";
        return Finish(std::move(o));
    }
    if (rep->state != sco::datacore::PackState::Applied || !loaded.result.status) {
        o.reason = !rep->reason.empty() ? rep->reason : loaded.result.status.message;
        for (const sco::datacore::PackOpReport& op : rep->ops)   // the first refusal names the record and field
            if (!op.status) {
                o.reason = "line " + std::to_string(op.line) + ": " + op.status.message + " (" +
                           sco::datacore::RefusalName(op.status.category) + ")";
                break;
            }
        return Finish(std::move(o));
    }
    o.applied = true;
    o.ops = rep->applied;
    out = loaded.result.splices;
    return Finish(std::move(o));
}

static std::shared_ptr<const sco::vfs::Table> BuildDriveMount() {
    sco::vfs::Mount m;
    m.path = "Data/Game2.dcb";
    m.source = kPackName;
    m.producer = sco::vfs::Transform(ApplyDrivePack);
    std::vector<sco::vfs::Mount> mounts;
    mounts.push_back(std::move(m));
    return sco::vfs::Table::Build(std::move(mounts));
}

static std::shared_ptr<const sco::vfs::Table> DriveMounts() { return g_dcbMounts.Current(); }

// After each DataCore load: sco::game::pak's report and the pack's, in the feature's words.
// sco-core calls it on the loader's thread after releasing its lock.
static void ReportDataLoad(const sco::game::pak::LoadReport& r) {
    const char* ok = r.loaderOk ? "ok" : "FAILED";
    const unsigned long long ms = static_cast<unsigned long long>(r.durationMs);
    PackOutcome pack;
    {
        std::lock_guard<std::mutex> hold(g_packLock);
        pack = std::move(g_pack);
        g_pack = PackOutcome();
    }
    const bool patched = r.outcome == sco::game::pak::Outcome::Applied && pack.applied;
    switch (r.outcome) {
    case sco::game::pak::Outcome::Applied:
        Log("[+] new quantum drive: game data patched as it loaded (pack %s: %zu operations), load %s in %llu ms",
            kPackName, pack.ops, ok, ms);
        break;
    case sco::game::pak::Outcome::Passed:
        if (pack.ran && !pack.applied)
            Log("[!] new quantum drive: pack %s not applied (%s); game data loaded %s in %llu ms without the new drive",
                kPackName, pack.reason.c_str(), ok, ms);
        else
            Log("[!] new quantum drive: %s passed through (%s); game data loaded %s in %llu ms without the new drive",
                r.path.c_str(), r.reason.c_str(), ok, ms);
        break;
    case sco::game::pak::Outcome::NoDcb:
        Log("[!] new quantum drive: the loader opened no .dcb; game data loaded %s without the new drive", ok);
        break;
    case sco::game::pak::Outcome::NoCryPak:
        Log("[!] new quantum drive: CryPak not found; game data loaded %s without the new drive", ok);
        break;
    case sco::game::pak::Outcome::SwapFailed:
        Log("[!] new quantum drive: %s; game data loaded %s without the new drive", r.reason.c_str(), ok);
        break;
    case sco::game::pak::Outcome::None:
        return;
    }
    g_dataPatched.store(patched ? 1 : 0, std::memory_order_release);
    const sco::Result cr = sco::caps::Set("quantum.drive", patched, patched ? nullptr : "the game data wasn't patched (see mod.log)");
    if (cr != sco::Result::Ok) Log("[!] capability quantum.drive: %s", sco::ResultName(cr));
}

void EnableQuantumDrive() {
    char dir[MAX_PATH];
    if (!DataFilePath(dir, sizeof(dir), "builtin")) {   // data\builtin, beside ships.txt
        g_notHooked = "SC_OFFLINE_SHIPS_FILE is unset, so there is no data folder for the pack";
        return;
    }
    std::error_code ec;
    std::filesystem::path root = std::filesystem::absolute(dir, ec);
    if (ec) root = dir;
    g_packDir = root / kPackOwner;
    std::shared_ptr<const sco::vfs::Table> table = BuildDriveMount();
    const std::vector<sco::vfs::MountInfo> info = table->Mounts();
    if (info.empty() || info[0].state == sco::vfs::MountState::Refused) {
        Log("[!] new quantum drive: the pack's mount was refused (%s)", info.empty() ? "no mount" : info[0].reason.c_str());
        g_notHooked = "the pack's mount was refused";
        return;
    }
    g_dcbMounts.Publish(std::move(table));
    sco::game::pak::Options o;
    o.mounts = DriveMounts;
    o.onLoad = ReportDataLoad;
    g_pakEnabled = sco::game::pak::Enable(o) == sco::Result::Ok;
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

// The functions come from sco-core's quantum.* rows (sco/game/world.h); hooking stays here.
static void HookBoostInput() {
    if (!WorldCapability("quantum.boost")) {
        Log("[!] quantum boost: functions not found (see the [core] lines in mod.log)");
        return;
    }
    g_inputHooked = HookFunction(sco::Sig("quantum.on_action"), 8, reinterpret_cast<void*>(&OnActionHook), reinterpret_cast<void**>(&g_onActionOrig));
    g_startUseHooked = HookFunction(sco::Sig("quantum.start_use"), 5, reinterpret_cast<void*>(&StartUseHook), reinterpret_cast<void**>(&g_startUseOrig));
    g_driveInput = reinterpret_cast<DriveInputFn>(sco::Sig("quantum.drive_input"));
    g_effectGuarded = HookFunction(sco::Sig("quantum.effect_update"), 8, reinterpret_cast<void*>(&EffectUpdateHook), reinterpret_cast<void**>(&g_effectUpdateOrig));
    g_chargeHooked = HookFunction(sco::Sig("quantum.charge"), 5, reinterpret_cast<void*>(&ChargeHook), reinterpret_cast<void**>(&g_chargeOrig));
    g_splineGetY = reinterpret_cast<SplineGetYFn>(sco::Sig("quantum.spline_get_y"));
    g_audioSystem = reinterpret_cast<uintptr_t*>(sco::Sig("quantum.audio_system"));
    g_handleValid = reinterpret_cast<HandleValidFn>(sco::Sig("quantum.handle_valid"));
}

// The effect tag sender is sco-core's quantum.send_effect_tag row (capability quantum.effect_tag,
// sco/game/features.h).
static void FindEffectTagSender() {
    size_t n = 0;
    const sco::game::features::Capability* caps = sco::game::features::Capabilities(n);
    for (size_t i = 0; i < n; ++i)
        if (strcmp(caps[i].name, "quantum.effect_tag") == 0) {
            sco::caps::SetFromSignatures(caps[i].name, caps[i].rows, caps[i].count);
            if (sco::caps::Has(caps[i].name)) g_sendEffectTag = reinterpret_cast<SendEffectTagFn>(sco::Sig("quantum.send_effect_tag"));
        }
}

void ResolveQuantumApi(const Section&, const Section&) {
    HookBoostInput();
    FindEffectTagSender();
}

bool QuantumDriveReady() {
    const int patched = g_dataPatched.load(std::memory_order_acquire);
    return patched < 0 ? g_pakEnabled : patched == 1;
}

bool QuantumBoostReady() {
    return g_inputHooked && g_startUseHooked && g_driveInput && g_effectGuarded && g_chargeHooked
        && g_audioSystem && g_handleValid && g_sendEffectTag;
}

void LogQuantum() {
    if (g_pakEnabled) Log("[+] new quantum drive: game data loader hooked (pack %s applies as Game2.dcb loads)", kPackName);
    else              Log("[!] new quantum drive: game data loader not hooked (%s)", g_notHooked);
    const bool audio = g_audioSystem && g_handleValid;
    if (QuantumBoostReady())
        Log("[+] quantum boost: hold Caps Lock, or NAV mode + hold left mouse");
    else
        Log("[!] quantum boost: input hook %s, drive hook %s, drive input %s, effects guard %s, charge hook %s, audio %s, effect warm-up %s",
            g_inputHooked ? "ok" : "MISSING", g_startUseHooked ? "ok" : "MISSING", g_driveInput ? "ok" : "MISSING",
            g_effectGuarded ? "ok" : "MISSING", g_chargeHooked ? "ok" : "MISSING", audio ? "ok" : "MISSING",
            g_sendEffectTag ? "ok" : "MISSING");
}
