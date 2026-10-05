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

bool RunConsoleNow(const char* cmd) { return Execute(cmd); }

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
    if (InterlockedExchange(&g_commandPending, 0)) {
        char cmd[256];
        AcquireSRWLockExclusive(&g_lock);
        strcpy_s(cmd, g_command);
        ReleaseSRWLockExclusive(&g_lock);
        if (Execute(cmd)) Log("[cvars] console: %s", cmd);
        else              Log("[cvars] console unavailable: %s", cmd);
    }
}
