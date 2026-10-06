#include "cvars.h"
#include "menu.h"

static int32_t* FindCVarStorage(const Section& text, const Section& rdata, const char* cvar, uint8_t registerSlot = 0x40) {
    const uint8_t* name = FindCString(rdata, cvar);
    uint8_t* const end = text.base + text.size - 0x30;
    for (uint8_t* p = text.base + 0x20; name && p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x15 || p + 7 + Rel32(p + 3) != name) continue;
        bool registers = false;
        for (int f = 7; f <= 0x20 && !registers; ++f)
            registers = (BytesMatch(p + f, "FF 50") && p[f + 2] == registerSlot) || (BytesMatch(p + f, "4C 8B 50") && p[f + 3] == registerSlot);
        if (!registers) continue;
        for (int b = 7; b <= 0x20; ++b) {
            const uint8_t* q = p - b;
            if (BytesMatch(q, "4C 8D 05")) return reinterpret_cast<int32_t*>(const_cast<uint8_t*>(q + 7 + Rel32(q + 3)));
        }
    }
    return nullptr;
}

static const struct { const char* cvar; int32_t value; const char* also; } kKeptOn[] = {
    { "v_qdrive2.quantumTravelAllowed", 1, "p_enable_physical_quantum_travel 1" },
    { "v_qdrive2.quantumBoostAllowed", 1, nullptr },
    { "v_qdrive2.setting_ignoreBlockedBoost", 1, nullptr },
    { "v_qdrive2.setting_ignoreBlockedTravel", 1, nullptr },
    { "v_qdrive.logging", 1, nullptr },
};
constexpr int kKeptOnCount = sizeof(kKeptOn) / sizeof(kKeptOn[0]);
static int32_t* g_keptOn[kKeptOnCount];

static const struct { const char* cvar; float value; } kKeptOnFloat[] = {
    { "v_qdrive2.setting_targetLockAngularSpeedThresholdPlayer", 45.0f },
    { "v_qdrive2.setting_targetLockLinearSpeedThresholdPlayer", 1000.0f },
};
constexpr int kKeptOnFloatCount = sizeof(kKeptOnFloat) / sizeof(kKeptOnFloat[0]);
static float* g_keptOnFloat[kKeptOnFloatCount];

static uintptr_t* g_console = nullptr;

static void FindConsole(const Section& text, const Section& rdata) {
    const uint8_t* str = FindCString(rdata, "debugGUI_enable 1");
    uint8_t* const end = text.base + text.size - 0x20;
    for (uint8_t* p = text.base + 7; str && p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x15 || p + 7 + Rel32(p + 3) != str) continue;
        if (BytesMatch(p - 7, "48 8B 0D") && BytesMatch(p + 7, "45 33 C9") && BytesMatch(p + 0x19, "FF 90 30 01 00 00")) {
            g_console = reinterpret_cast<uintptr_t*>(p + Rel32(p - 4));
            return;
        }
    }
    Log("[cvars] console not found; console commands are disabled");
}

static SRWLOCK       g_lock = SRWLOCK_INIT;
static char          g_command[256];
static volatile LONG g_commandPending = 0;

bool Menu_ConsoleReady() { return g_console && *g_console; }

void Menu_RunConsole(const char* cmd) {
    AcquireSRWLockExclusive(&g_lock);
    strncpy_s(g_command, cmd, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_commandPending, 1);
}

static bool Execute(const char* cmd) {
    if (!g_console || !*g_console) return false;
    __try {
        VCall<void>(*g_console, 0x130, cmd, false, false, static_cast<void*>(nullptr));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[cvars] fault while running '%s'", cmd);
        return false;
    }
}

bool SetCVarNow(const char* name, float value) {
    if (!g_console || !*g_console) return false;
    __try {
        const uintptr_t cvar = VCall<uintptr_t>(*g_console, 0xC0, name);
        if (!cvar) return false;
        VCall<void>(cvar, 0x38, value);
        return VCall<float>(cvar, 0x20) == value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GetCVarNow(const char* name, float& value) {
    if (!g_console || !*g_console || !name || !*name) return false;
    __try {
        const uintptr_t cvar = VCall<uintptr_t>(*g_console, 0xC0, name);
        if (!cvar) return false;
        value = VCall<float>(cvar, 0x20);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Squadron 42 tab settings. The shipped mod carries these four cvars XORed with a
// per-blob key (Murmur-style fmix32 over the byte offset); these are the names
// decrypted out of dinput8.dll's label table.
static const struct { const char* label; const char* tip; const char* cvar; } kS42Settings[] = {
    { "SQ42 auto targeting",
      "\"Enables the auto targeting feature for SQ42\" (the game mode can override it).",
      "i_target_selector.targeting2_enabled" },
    { "Visor mini-map",
      "The mini-map on your visor HUD.",
      "pl_lensdisplay.minimap_enabled" },
    { "Visor greebles",
      "The decorative frame pieces on your visor HUD.",
      "pl_lensdisplay.greebles_enabled" },
    { "SQ42 menus (experimental)",
      "\"Enable Squadron 42 Frontend\". Switches the pause menu and loading screens to SQ42's, whose data is "
      "missing; turn it off before traveling or quitting if anything breaks.",
      "g_squadron_frontend" },
};
constexpr int kS42SettingCount = sizeof(kS42Settings) / sizeof(kS42Settings[0]);

static volatile LONG g_s42On[kS42SettingCount] = { -1, -1, -1, -1 };
static volatile LONG g_s42PendingMask = 0;   // bit i = a toggle is waiting
static volatile LONG g_s42PendingVal  = 0;   // bit i = the value it wants

int         Menu_S42SettingCount() { return kS42SettingCount; }
const char* Menu_S42SettingLabel(int i) { return i >= 0 && i < kS42SettingCount ? kS42Settings[i].label : ""; }
const char* Menu_S42SettingTip(int i)   { return i >= 0 && i < kS42SettingCount ? kS42Settings[i].tip   : ""; }
bool        Menu_S42SettingOn(int i)    { return i >= 0 && i < kS42SettingCount && g_s42On[i] == 1; }
bool        Menu_S42SettingKnown(int i) { return i >= 0 && i < kS42SettingCount && g_s42On[i] >= 0; }

// One pending slot per toggle rather than one overall, so two toggles inside the
// same game tick both land. Value first, mask second: a reader that sees the bit
// must already see the value.
void Menu_RequestS42Setting(int i, bool on) {
    if (i < 0 || i >= kS42SettingCount) return;
    if (on) InterlockedOr(&g_s42PendingVal, 1L << i);
    else    InterlockedAnd(&g_s42PendingVal, ~(1L << i));
    InterlockedOr(&g_s42PendingMask, 1L << i);
}

static void ApplyS42Setting(int i, bool on) {
    const int want = on ? 1 : 0;
    if (!SetCVarNow(kS42Settings[i].cvar, static_cast<float>(want))) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "%s %d", kS42Settings[i].cvar, want);
        if (!Execute(cmd)) { Log("[sq42] console unavailable: %s", cmd); return; }
    }
    Log("[sq42] %s = %d", kS42Settings[i].cvar, want);
}

// Game thread only: reads the four cvars back so the menu can show them. Capped at
// 5 Hz — four name lookups every tick buys nothing the menu can see.
static void RefreshS42Settings() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 200) return;
    last = now;
    for (int i = 0; i < kS42SettingCount; ++i) {
        float v = 0.0f;
        const bool ok = GetCVarNow(kS42Settings[i].cvar, v);
        InterlockedExchange(&g_s42On[i], ok && v != 0.0f ? 1 : 0);
    }
}

void ResolveCVarsApi(const Section& text, const Section& rdata) {
    for (int i = 0; i < kKeptOnCount; ++i)
        if (!(g_keptOn[i] = FindCVarStorage(text, rdata, kKeptOn[i].cvar)))
            Log("[+] %s not found; it stays as the game mode sets it", kKeptOn[i].cvar);
    for (int i = 0; i < kKeptOnFloatCount; ++i)
        if (!(g_keptOnFloat[i] = reinterpret_cast<float*>(FindCVarStorage(text, rdata, kKeptOnFloat[i].cvar, 0x48))))
            Log("[+] %s not found; it keeps its default", kKeptOnFloat[i].cvar);
    FindConsole(text, rdata);
}

static void KeepSettingsOn() {
    for (int i = 0; i < kKeptOnCount; ++i) {
        int32_t* const s = g_keptOn[i];
        if (!s) continue;
        __try {
            if (*s == kKeptOn[i].value) continue;
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "%s %d", kKeptOn[i].cvar, kKeptOn[i].value);
            Execute(cmd);
            if (*s != kKeptOn[i].value) *s = kKeptOn[i].value;
            if (kKeptOn[i].also) Execute(kKeptOn[i].also);
            Log("[+] %s = %d%s%s", kKeptOn[i].cvar, *s, kKeptOn[i].also ? ", " : "", kKeptOn[i].also ? kKeptOn[i].also : "");
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    for (int i = 0; i < kKeptOnFloatCount; ++i) {
        float* const s = g_keptOnFloat[i];
        if (!s) continue;
        __try {
            if (*s == kKeptOnFloat[i].value) continue;
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "%s %g", kKeptOnFloat[i].cvar, kKeptOnFloat[i].value);
            Execute(cmd);
            if (*s != kKeptOnFloat[i].value) *s = kKeptOnFloat[i].value;
            Log("[+] %s = %g", kKeptOnFloat[i].cvar, *s);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void ProcessCVars() {
    KeepSettingsOn();
    {
        const LONG mask = InterlockedExchange(&g_s42PendingMask, 0);
        const LONG vals = g_s42PendingVal;
        for (int i = 0; i < kS42SettingCount; ++i)
            if (mask & (1L << i)) ApplyS42Setting(i, (vals >> i) & 1);
        RefreshS42Settings();
    }
    if (InterlockedExchange(&g_commandPending, 0)) {
        char cmd[256];
        AcquireSRWLockExclusive(&g_lock);
        strcpy_s(cmd, g_command);
        ReleaseSRWLockExclusive(&g_lock);
        if (Execute(cmd)) Log("[cvars] console: %s", cmd);
        else              Log("[cvars] console unavailable: %s", cmd);
    }
}
