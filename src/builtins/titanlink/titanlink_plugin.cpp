// titanlink: Titanfall 2 inside Star Citizen, an optional built-in (CMake SCO_BRIDGE_TITANLINK).
//
// Pilot mode (F9, titanlink.pilot) opens the channel Local\SCO_titanlink.link through sco.ipc
// (layout sc_titanlink.h, docs/bridges.md) and, when Titanfall 2 isn't answering yet, starts the
// EA app and NorthstarLauncher.exe (only on that key press; the command line goes to mod.log). The
// Titanfall side, a Northstar plugin that opens the channel by name, loads the local match the
// built-in asks for. From then on, ten times a second, the built-in writes where you are and where
// you look, converted into the match's frame from an anchor taken where you stood when pilot mode
// began, and the Titanfall side draws its match from your eyes; the overlay
// (titanlink_overlay.cpp) shows that picture over the game. V calls your Titan down in front of
// you and E gets in and out (titanlink.titan, titanlink.embark); the mouse buttons and a few keys
// go across as held buttons.
//
// Game access is teleport.spatial (your pose, zone conversions) and the cl_fov read the product
// already has; nothing else of the game is touched. Every value the Titanfall side writes is
// checked before use (ValidTf, DrainPeer, PlaceTitanfallWindow), and the channel closes with the
// built-in, so the Titanfall side sees it drop at once.
#include "titanlink.h"
#include "../bridge_common.h"
#include "../builtins.h"
#include "../tabs.h"
#include "../../common.h"
#include "../../cvars.h"
#include <sc_spatial.h>
#include <tlhelp32.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const sco_api*       g_api = nullptr;
sco_plugin*          g_self = nullptr;
const sco_ipc_v1*    g_ipc = nullptr;
const sc_spatial_v1* g_sp = nullptr;
uint64_t             g_ch = 0;   // the channel; 0 while closed

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "titanlink", SCO_VERSION, "sc-offline",
};

constexpr const char* kCap = "titanlink";
constexpr uint32_t    kLinkTimeoutMs = 4000;
constexpr double      kEyeHeight = 1.62;     // metres above your feet
constexpr double      kMaxUnits = 1.0e6;     // any coordinate of a Titanfall map is well inside this
constexpr double      kEmbarkRange = 9.0;    // metres from the Titan
constexpr double      kTitanDropAhead = 8.0; // metres in front of you
constexpr double      kPi = 3.14159265358979323846;

#define LOGI(...) BridgeLog(g_api, g_self, SCO_LOG_INFO, __VA_ARGS__)
#define LOGW(...) BridgeLog(g_api, g_self, SCO_LOG_WARN, __VA_ARGS__)

TlStatus    g_st;
tl_tf_state g_in = {};   // the last valid state from the Titanfall side; zero while unlinked

struct Config {
    char   game[MAX_PATH] = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Titanfall2";
    char   map[64] = "mp_coliseum";
    char   mode[32] = "tdm";
    char   extra[256] = "";
    double fov = 0;   // vertical degrees; 0 = Star Citizen's cl_fov
};
Config g_cfg;

// Where pilot mode began: a zone and a frame in it (your feet, facing, up). The match's floor_pt
// is this point; metres along r, f, u are the match's x, y, z.
struct Anchor {
    bool     ok = false;
    uint64_t zone = 0;
    double   o[3] = {}, r[3] = {}, f[3] = {}, u[3] = {};
};
Anchor g_anchor;

bool     g_startSent = false;   // TL_MSG_START_MATCH went out for this link
bool     g_ringBad = false;     // the peer's ring failed validation once (logged)
bool     g_stateBad = false;    // a tl_tf_state failed validation once (logged)
DWORD    g_launchAt = 0, g_launchDue = 0, g_callAt = 0, g_placedAt = 0;
uint32_t g_buttons = 0;
bool     g_titanKeyWas = false, g_embarkKeyWas = false;
float    g_lastFeet[3] = {};
uint64_t g_lastFeetMs = 0;
DWORD    g_fovAt = 0;
float    g_fovScale = 1.0f;
DWORD    g_checkedPid = 0;
bool     g_pidIsTitanfall = false;

// ---- config: data\titanlink.txt ----------------------------------------------------------------

bool ValidName(const char* s, size_t max) {
    size_t n = 0;
    for (; s[n]; ++n)
        if (n >= max || !((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '_')) return false;
    return n > 0;
}

void LoadConfig() {
    g_cfg = Config();
    char path[MAX_PATH];
    FILE* f = nullptr;
    if (DataFilePath(path, sizeof(path), "titanlink.txt") && fopen_s(&f, path, "r") == 0 && f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') continue;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char* key = line;
            while (*key == ' ') ++key;
            for (char* e = key + strlen(key); e > key && e[-1] == ' ';) *--e = 0;
            char* value = eq + 1;
            value[strcspn(value, "\r\n")] = 0;
            while (*value == ' ') ++value;
            if (!*value) continue;
            if (!_stricmp(key, "game")) strncpy_s(g_cfg.game, value, _TRUNCATE);
            else if (!_stricmp(key, "extra")) strncpy_s(g_cfg.extra, value, _TRUNCATE);
            else if (!_stricmp(key, "fov")) g_cfg.fov = strtod(value, nullptr);
            else if (!_stricmp(key, "map") || !_stricmp(key, "mode")) {
                const bool map = !_stricmp(key, "map");
                if (ValidName(value, map ? sizeof(g_cfg.map) - 1 : sizeof(g_cfg.mode) - 1))
                    strncpy_s(map ? g_cfg.map : g_cfg.mode, map ? sizeof(g_cfg.map) : sizeof(g_cfg.mode), value, _TRUNCATE);
                else
                    LOGW("[titanlink] %s: %s = %s isn't a map or mode name ([a-z0-9_]); keeping %s", path, key, value,
                         map ? g_cfg.map : g_cfg.mode);
            }
        }
        fclose(f);
        LOGI("[titanlink] %s: Titanfall 2 in %s, %s on %s", path, g_cfg.game, g_cfg.mode, g_cfg.map);
    } else {
        LOGI("[titanlink] no data\\titanlink.txt; Titanfall 2 is looked for in %s", g_cfg.game);
    }
    if (!(g_cfg.fov >= 0 && g_cfg.fov < 170)) g_cfg.fov = 0;
    strncpy_s(g_st.game, g_cfg.game, _TRUNCATE);
    strncpy_s(g_st.map, g_cfg.map, _TRUNCATE);
    strncpy_s(g_st.mode, g_cfg.mode, _TRUNCATE);
}

// ---- starting Titanfall 2 (only from pilot mode's key or button) --------------------------------

bool ProcessRunning(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) found = !_wcsicmp(pe.szExeFile, exe);
    CloseHandle(snap);
    return found;
}

bool IsFile(const wchar_t* path) {
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// True when the EA app (or Origin) runs; otherwise starts it, minimized, and answers false.
bool StartEaApp() {
    if (ProcessRunning(L"EADesktop.exe") || ProcessRunning(L"Origin.exe")) return true;
    wchar_t path[MAX_PATH] = L"C:\\Program Files\\Electronic Arts\\EA Desktop\\EA Desktop\\EADesktop.exe";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Origin", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t found[MAX_PATH] = L"";
        DWORD bytes = sizeof(found) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(key, L"ClientPath", nullptr, &type, reinterpret_cast<LPBYTE>(found), &bytes) == ERROR_SUCCESS &&
            type == REG_SZ) {
            found[MAX_PATH - 1] = 0;
            const size_t n = wcslen(found);
            if (n > 4 && !_wcsicmp(found + n - 4, L".exe") && IsFile(found)) wcscpy_s(path, found);
        }
        RegCloseKey(key);
    }
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(path, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LOGI("[titanlink] started the EA app (%ls); Titanfall 2 starts in 25 seconds", path);
    } else {
        LOGW("[titanlink] couldn't start the EA app (%ls, error %lu): open it and sign in, then press F9 again", path, GetLastError());
    }
    return false;
}

void Launch() {
    g_st.launched = true;
    g_launchAt = GetTickCount() | 1;
    if (!StartEaApp()) {
        g_launchDue = GetTickCount() + 25000;
        return;
    }
    uint32_t w = 0, h = 0;
    BridgeViewSize(w, h);
    if (w < 64 || h < 64) { w = 1920; h = 1080; }
    wchar_t dir[MAX_PATH], exe[MAX_PATH], extra[256], cmd[1200];
    if (!MultiByteToWideChar(CP_ACP, 0, g_cfg.game, -1, dir, MAX_PATH) ||
        !MultiByteToWideChar(CP_ACP, 0, g_cfg.extra, -1, extra, 256) ||
        swprintf_s(exe, L"%s\\NorthstarLauncher.exe", dir) < 0 ||
        swprintf_s(cmd, L"\"%s\" -novid -windowed -noborder -w %u -h %u %s", exe, w, h, extra) < 0) {
        LOGW("[titanlink] the Titanfall 2 folder or extra arguments in titanlink.txt are too long");
        return;
    }
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi = {};
    LOGI("[titanlink] starting Titanfall 2: %ls", cmd);
    if (CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        LOGW("[titanlink] couldn't start %ls (error %lu): set game= in data\\titanlink.txt to your Titanfall 2 folder", exe, GetLastError());
    }
}

// The pid the peer names is used only once it is a NorthstarLauncher.exe or Titanfall2.exe of
// this user; then its window is kept behind the game, at the game's size, so it renders at it.
bool IsTitanfallProcess(DWORD pid) {
    if (!pid || pid == GetCurrentProcessId()) return false;
    if (pid == g_checkedPid) return g_pidIsTitanfall;
    g_checkedPid = pid;
    g_pidIsTitanfall = false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(h, 0, path, &n) != 0;
    CloseHandle(h);
    if (!ok) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    g_pidIsTitanfall = !_wcsicmp(name, L"NorthstarLauncher.exe") || !_wcsicmp(name, L"Titanfall2.exe");
    if (!g_pidIsTitanfall) LOGW("[titanlink] the Titanfall side named process %lu (%ls), which isn't Titanfall 2; its window is left alone", pid, name);
    return g_pidIsTitanfall;
}

struct WndFind { DWORD pid; HWND wnd; };

BOOL CALLBACK FindPidWindow(HWND h, LPARAM p) {
    auto* s = reinterpret_cast<WndFind*>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r = {};
    GetWindowRect(h, &r);
    if (r.right - r.left < 200) return TRUE;
    s->wnd = h;
    return FALSE;
}

void PlaceTitanfallWindow(DWORD now) {
    if (g_placedAt && now - g_placedAt < 2000) return;
    g_placedAt = now;
    if (!IsTitanfallProcess(g_in.pid)) return;
    WndFind s = { g_in.pid, nullptr };
    EnumWindows(FindPidWindow, reinterpret_cast<LPARAM>(&s));
    HWND game = BridgeGameWindow();
    if (!s.wnd || !game) return;
    RECT rc = {}, cur = {};
    POINT org = { 0, 0 };
    GetClientRect(game, &rc);
    ClientToScreen(game, &org);
    GetWindowRect(s.wnd, &cur);
    const bool same = cur.left == org.x && cur.top == org.y && cur.right - cur.left == rc.right && cur.bottom - cur.top == rc.bottom;
    SetWindowPos(s.wnd, HWND_BOTTOM, org.x, org.y, rc.right, rc.bottom,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | (same ? SWP_NOMOVE | SWP_NOSIZE : 0));
}

// ---- poses: teleport.spatial, converted into the match's frame ----------------------------------

// Your feet, facing and up in the anchor's zone (the zone you're in, converted when it differs).
bool PoseInAnchor(double pos[3], double fwd[3], double up[3]) {
    double p[3], q[4], r[3], f[3], u[3];
    uint64_t zone = 0;
    if (!g_sp || !g_anchor.ok || !g_sp->player_pose(p, q, &zone)) return false;
    BridgeAxes(q, r, f, u);
    if (zone == g_anchor.zone) {
        memcpy(pos, p, sizeof(p));
        memcpy(fwd, f, sizeof(f));
        memcpy(up, u, sizeof(u));
        return true;
    }
    const double pf[3] = { p[0] + f[0], p[1] + f[1], p[2] + f[2] };
    const double pu[3] = { p[0] + u[0], p[1] + u[1], p[2] + u[2] };
    double a[3], b[3], c[3];
    if (!g_sp->zone_to_zone(zone, g_anchor.zone, p, a) || !g_sp->zone_to_zone(zone, g_anchor.zone, pf, b) ||
        !g_sp->zone_to_zone(zone, g_anchor.zone, pu, c))
        return false;
    for (int i = 0; i < 3; ++i) { pos[i] = a[i]; fwd[i] = b[i] - a[i]; up[i] = c[i] - a[i]; }
    return BridgeNormalize(fwd) && BridgeNormalize(up);
}

bool SetAnchor() {
    double p[3], q[4], r[3], f[3], u[3];
    uint64_t zone = 0;
    g_anchor.ok = false;
    if (!g_sp || !g_sp->player_pose(p, q, &zone) || !zone) return false;
    BridgeAxes(q, r, f, u);
    g_anchor.zone = zone;
    memcpy(g_anchor.o, p, sizeof(p));
    memcpy(g_anchor.f, f, sizeof(f));
    memcpy(g_anchor.u, u, sizeof(u));
    BridgeCross(f, u, g_anchor.r);
    if (!BridgeNormalize(g_anchor.r) || !BridgeNormalize(g_anchor.f) || !BridgeNormalize(g_anchor.u)) return false;
    g_anchor.ok = true;
    g_lastFeetMs = 0;
    return true;
}

void ToMatch(const double local[3], float out[3]) {
    const double d[3] = { local[0] - g_anchor.o[0], local[1] - g_anchor.o[1], local[2] - g_anchor.o[2] };
    out[0] = static_cast<float>(g_in.floor_pt[0] + TL_UNITS_PER_METRE * BridgeDot(d, g_anchor.r));
    out[1] = static_cast<float>(g_in.floor_pt[1] + TL_UNITS_PER_METRE * BridgeDot(d, g_anchor.f));
    out[2] = static_cast<float>(g_in.floor_pt[2] + TL_UNITS_PER_METRE * BridgeDot(d, g_anchor.u));
}

void DirToMatch(const double v[3], double out[3]) {
    out[0] = BridgeDot(v, g_anchor.r);
    out[1] = BridgeDot(v, g_anchor.f);
    out[2] = BridgeDot(v, g_anchor.u);
}

void FromMatch(const float m[3], double local[3]) {
    const double d[3] = { (m[0] - g_in.floor_pt[0]) / TL_UNITS_PER_METRE, (m[1] - g_in.floor_pt[1]) / TL_UNITS_PER_METRE,
                          (m[2] - g_in.floor_pt[2]) / TL_UNITS_PER_METRE };
    for (int i = 0; i < 3; ++i) local[i] = g_anchor.o[i] + g_anchor.r[i] * d[0] + g_anchor.f[i] * d[1] + g_anchor.u[i] * d[2];
}

// Metres from you to the Titan, or -1.
double TitanDistance() {
    if (!(g_in.flags & (TL_TF_PARKED | TL_TF_IN_TITAN))) return -1;
    double pos[3], f[3], u[3], titan[3];
    if (!PoseInAnchor(pos, f, u)) return -1;
    FromMatch(g_in.titan_origin, titan);
    const double d[3] = { titan[0] - pos[0], titan[1] - pos[1], titan[2] - pos[2] };
    return std::sqrt(BridgeDot(d, d));
}

float FovScale(DWORD now) {
    if (g_fovAt && now - g_fovAt < 1000) return g_fovScale;
    g_fovAt = now;
    double vfov = g_cfg.fov;
    float cl = 0;
    if (vfov <= 0 && GetCVarNow("cl_fov", cl) && cl > 10 && cl < 170) vfov = cl;
    if (vfov <= 0) vfov = 60.0;
    const double deg = kPi / 180.0;
    const double hor43 = 2.0 * std::atan(std::tan(vfov * deg * 0.5) * 4.0 / 3.0) / deg;
    double s = hor43 / 70.0;
    s = s < 0.6 ? 0.6 : s > 2.0 ? 2.0 : s;
    g_fovScale = static_cast<float>(s);
    return g_fovScale;
}

// Writes tl_sc_state. Ten times a second (the host tick): the Titanfall side interpolates with
// vel and time_ms between writes.
void Publish(uint64_t nowMs) {
    if (!g_ch) return;
    tl_sc_state s = {};
    s.time_ms = nowMs;
    const bool focused = GameHasFocus();
    if (g_st.pilot) s.flags |= TL_SC_PILOT;
    if (focused) s.flags |= TL_SC_FOCUSED;
    double pos[3], f[3], u[3];
    if (g_anchor.ok && PoseInAnchor(pos, f, u)) {
        s.flags |= TL_SC_ANCHORED;
        ToMatch(pos, s.feet);
        const double eye[3] = { pos[0] + u[0] * kEyeHeight, pos[1] + u[1] * kEyeHeight, pos[2] + u[2] * kEyeHeight };
        ToMatch(eye, s.eye);
        double d[3];
        DirToMatch(f, d);
        const double z = d[2] < -1.0 ? -1.0 : d[2] > 1.0 ? 1.0 : d[2];
        s.ang[0] = static_cast<float>(-std::asin(z) * 180.0 / kPi);
        s.ang[1] = static_cast<float>(std::atan2(d[1], d[0]) * 180.0 / kPi);
        const uint64_t dt = nowMs - g_lastFeetMs;
        if (g_lastFeetMs && dt > 0 && dt <= 1000)
            for (int i = 0; i < 3; ++i) s.vel[i] = static_cast<float>((s.feet[i] - g_lastFeet[i]) * 1000.0 / static_cast<double>(dt));
        memcpy(g_lastFeet, s.feet, sizeof(g_lastFeet));
        g_lastFeetMs = nowMs;
    } else {
        g_lastFeetMs = 0;
    }
    s.fov_scale = FovScale(static_cast<DWORD>(nowMs));
    BridgeViewSize(s.view_w, s.view_h);
    s.buttons = g_st.pilot && focused ? g_buttons : 0;
    g_ipc->block_write(g_self, g_ch, TL_OFF_SC_STATE, &s, sizeof(s));
}

// ---- the link ----------------------------------------------------------------------------------

bool Push(uint32_t type, const void* data, uint32_t size) {
    return g_ch && g_ipc->ring_push(g_self, g_ch, TL_OFF_TO_PEER, type, data, size) == SCO_OK;
}

// Everything the Titanfall side wrote is checked here before any of it is used: flags are masked,
// every coordinate must be finite and inside a map, counts and fractions in range, text printable.
bool ValidTf(tl_tf_state& s) {
    s.flags &= TL_TF_FLAGS;
    if (!BridgeFinite3(s.origin, kMaxUnits) || !BridgeFinite3(s.titan_origin, kMaxUnits) || !BridgeFinite3(s.floor_pt, kMaxUnits) ||
        !BridgeFinite(s.eye_z, kMaxUnits) || !BridgeFinite(s.titan_yaw, 1.0e4))
        return false;
    if (!BridgeFinite(s.damage, 1.0e5) || s.damage < 0) s.damage = 0;
    if (!BridgeFinite(s.titan_health, 1.0)) s.titan_health = 0;
    if (s.titan_health < 0) s.titan_health = 0;
    if (!BridgeFinite(s.zoom_frac, 1.0) || s.zoom_frac < 0) s.zoom_frac = 0;
    if (!BridgeFinite(s.zoom_fov, 180.0) || s.zoom_fov < 0) s.zoom_fov = 0;
    if (s.clip < -1 || s.clip > 100000) s.clip = -1;
    BridgeSanitize(s.weapon, sizeof(s.weapon));
    return true;
}

// Takes what the Titanfall side queued: TL_MSG_LOG lines; other types (a newer peer) are skipped.
void DrainPeer() {
    static uint8_t buf[TL_RING_FROM_PEER];   // a record never exceeds the ring
    for (int i = 0; i < 16; ++i) {
        uint32_t type = 0, size = sizeof(buf);
        const sco_result r = g_ipc->ring_pop(g_self, g_ch, TL_OFF_FROM_PEER, &type, buf, &size);
        if (r == SCO_NOT_FOUND) return;
        if (r != SCO_OK) {
            if (!g_ringBad) LOGW("[titanlink] the Titanfall side's message ring failed validation (%d); its messages are ignored", static_cast<int>(r));
            g_ringBad = true;
            return;
        }
        if (type != TL_MSG_LOG || size == 0 || size > TL_MSG_LOG_MAX) continue;
        memcpy(g_st.peerLog, buf, size);
        g_st.peerLog[size] = 0;
        BridgeSanitize(g_st.peerLog, sizeof(g_st.peerLog));
        LOGI("[titanlink] Titanfall: %s", g_st.peerLog);
    }
}

void PilotOff(bool tell) {
    if (!g_st.pilot && !g_st.wantPilot) return;
    const bool was = g_st.pilot;
    g_st.pilot = false;
    g_st.wantPilot = false;
    g_buttons = 0;
    TlOverlayShow(false);
    if (was && tell) Push(TL_MSG_PILOT_OFF, nullptr, 0);
    Publish(GetTickCount64());
    if (was) LOGI("[titanlink] pilot mode off");
}

void PilotOn() {
    // A parked Titan keeps its place: re-anchor only when there is none (or no anchor yet).
    if (!g_anchor.ok || !(g_in.flags & (TL_TF_PARKED | TL_TF_IN_TITAN))) SetAnchor();
    g_st.pilot = true;
    g_callAt = 0;
    g_titanKeyWas = g_embarkKeyWas = true;   // a key already held doesn't fire
    TlOverlayShow(true);
    LOGI("[titanlink] pilot mode on%s: V calls your Titan, E gets in and out, F9 ends it",
         g_anchor.ok ? "" : " (waiting for your position to anchor the match)");
}

bool OpenLink(char* reply, uint32_t size) {
    if (g_ch) return true;
    if (!g_ipc || !g_sp) {
        snprintf(reply, size, "TitanLink needs sco.ipc and teleport.spatial, and this build has %s", !g_ipc ? "no sco.ipc" : "no teleport.spatial");
        return false;
    }
    LoadConfig();
    uint64_t ch = 0;
    sco_result r = g_ipc->create(g_self, TL_CHANNEL_NAME, TL_CHANNEL_BYTES, TL_LAYOUT_ID, TL_LAYOUT_VERSION, &ch);
    if (r == SCO_OK) r = g_ipc->ring_init(g_self, ch, TL_OFF_TO_PEER, TL_RING_TO_PEER, SCO_IPC_TO_PEER);
    if (r == SCO_OK) r = g_ipc->ring_init(g_self, ch, TL_OFF_FROM_PEER, TL_RING_FROM_PEER, SCO_IPC_FROM_PEER);
    if (r != SCO_OK) {
        if (ch) g_ipc->close(g_self, ch);
        snprintf(reply, size, "Couldn't open the TitanLink channel (sco.ipc %d); see mod.log", static_cast<int>(r));
        LOGW("[titanlink] couldn't open Local\\SCO_titanlink.link: sco.ipc answered %d", static_cast<int>(r));
        return false;
    }
    g_ch = ch;
    g_st.linkOpen = true;
    g_startSent = g_ringBad = g_stateBad = false;
    g_checkedPid = 0;
    Publish(GetTickCount64());
    TlOverlayStart(g_ipc, g_self, ch);
    LOGI("[titanlink] link open: Local\\SCO_titanlink.link, waiting for Titanfall 2's TitanLink plugin");
    return true;
}

void CloseLink() {
    if (!g_ch) return;
    PilotOff(true);
    TlOverlayStop();
    g_ipc->close(g_self, g_ch);
    g_ch = 0;
    g_in = {};
    g_anchor.ok = false;
    const TlStatus keep = g_st;
    g_st = TlStatus();
    strncpy_s(g_st.game, keep.game, _TRUNCATE);
    strncpy_s(g_st.map, keep.map, _TRUNCATE);
    strncpy_s(g_st.mode, keep.mode, _TRUNCATE);
    g_st.launched = keep.launched;
    LOGI("[titanlink] link closed");
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Pilot mode's keys, read while the game is in front: V and E as presses, the rest as held
// buttons. Star Citizen sees these keys too (follow-up: an input capture in sco.ui).
void PilotKeys() {
    const bool focused = GameHasFocus();
    const bool tk = focused && KeyDown('V'), ek = focused && KeyDown('E');
    char reply[160];
    if (tk && !g_titanKeyWas && !(g_in.flags & TL_TF_IN_TITAN)) { TlCallTitan(reply, sizeof(reply)); LOGI("[titanlink] V: %s", reply); }
    if (ek && !g_embarkKeyWas) { TlEmbark(reply, sizeof(reply)); LOGI("[titanlink] E: %s", reply); }
    g_titanKeyWas = tk;
    g_embarkKeyWas = ek;
    uint32_t b = 0;
    if (focused && !(g_in.flags & TL_TF_BUSY)) {
        const bool titan = (g_in.flags & TL_TF_IN_TITAN) != 0;
        if (KeyDown(VK_LBUTTON)) b |= TL_BTN_ATTACK;
        if (KeyDown(VK_RBUTTON)) b |= TL_BTN_ZOOM;
        if (KeyDown('R')) b |= TL_BTN_RELOAD;
        if (KeyDown('F')) b |= TL_BTN_MELEE;
        if (KeyDown('G')) b |= TL_BTN_OFFHAND;
        if (KeyDown('Q')) b |= TL_BTN_ABILITY;
        if (titan && KeyDown('C')) b |= TL_BTN_TITAN_C;
        if (titan && KeyDown('V')) b |= TL_BTN_TITAN_V;
        if (KeyDown('Y')) b |= TL_BTN_CYCLE;
        if (KeyDown('X')) b |= TL_BTN_X;
    }
    g_buttons = b;
}

void OnTick(const char*, const void*, void*) {
    if (!g_ch) return;
    const uint64_t nowMs = GetTickCount64();
    const DWORD now = static_cast<DWORD>(nowMs);
    uint32_t age = 0;
    const bool linked = g_ipc->peer_age_ms(g_self, g_ch, &age) == SCO_OK && age < kLinkTimeoutMs;
    if (linked != g_st.linked) LOGI(linked ? "[titanlink] Titanfall 2 linked" : "[titanlink] Titanfall 2 stopped answering");
    g_st.linked = linked;
    g_st.peerAgeMs = linked ? age : 0;
    if (linked) {
        tl_tf_state in = {};
        if (g_ipc->block_read(g_self, g_ch, TL_OFF_TF_STATE, &in, sizeof(in)) == SCO_OK) {
            if (ValidTf(in)) {
                g_in = in;
            } else if (!g_stateBad) {
                g_stateBad = true;
                LOGW("[titanlink] the Titanfall side wrote an impossible state (a coordinate out of range); ignored");
            }
        }
        DrainPeer();
        tl_frame fr = {};
        if (g_ipc->block_read(g_self, g_ch, TL_OFF_FRAME, &fr, sizeof(fr)) == SCO_OK) {
            BridgeSanitize(fr.status, sizeof(fr.status));
            strncpy_s(g_st.frameStatus, fr.status, _TRUNCATE);
        }
    } else {
        g_in = {};
        g_startSent = false;
    }
    if (linked && !g_startSent) {
        tl_msg_start_match m = {};
        strncpy_s(m.map, g_cfg.map, _TRUNCATE);
        strncpy_s(m.mode, g_cfg.mode, _TRUNCATE);
        if (Push(TL_MSG_START_MATCH, &m, sizeof(m))) {
            g_startSent = true;
            LOGI("[titanlink] asked Titanfall 2 for a local match: %s on %s", m.mode, m.map);
        }
    }
    g_st.inMatch = linked && (g_in.flags & TL_TF_IN_MATCH);
    g_st.inTitan = (g_in.flags & TL_TF_IN_TITAN) != 0;
    g_st.titanParked = (g_in.flags & TL_TF_PARKED) != 0;
    g_st.titanHealth = g_in.titan_health;
    g_st.clip = g_in.clip;
    strncpy_s(g_st.weapon, g_in.weapon, _TRUNCATE);
    if (g_launchDue && static_cast<LONG>(now - g_launchDue) >= 0) {
        g_launchDue = 0;
        Launch();
    }
    if (g_st.wantPilot && !g_st.pilot && g_st.inMatch) PilotOn();
    if (g_st.pilot && !linked) {
        LOGW("[titanlink] Titanfall 2 stopped answering; pilot mode off");
        PilotOff(false);
    }
    if (g_st.pilot) {
        if (!g_anchor.ok) SetAnchor();
        PilotKeys();
    }
    if (linked) PlaceTitanfallWindow(now);
    g_st.titanDistance = TitanDistance();
    Publish(nowMs);
}

// ---- commands ----------------------------------------------------------------------------------

sco_result CmdPilot(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    TlTogglePilot(reply, size);
    return SCO_OK;
}
sco_result CmdTitan(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    TlCallTitan(reply, size);
    return SCO_OK;
}
sco_result CmdEmbark(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    TlEmbark(reply, size);
    return SCO_OK;
}
sco_result CmdUnlink(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    TlUnlink(reply, size);
    return SCO_OK;
}
sco_result CmdStatus(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    snprintf(reply, size, "TitanLink: link %s, Titanfall %s%s, pilot mode %s, %s",
             g_st.linkOpen ? "open" : "closed", g_st.linked ? "linked" : "not answering", g_st.inMatch ? " in a match" : "",
             g_st.pilot ? "on" : g_st.wantPilot ? "waiting" : "off",
             g_st.inTitan ? "in the Titan" : g_st.titanParked ? "Titan parked" : "no Titan");
    return SCO_OK;
}

const sco_plugin_info* TlQuery() { return &kInfo; }

sco_result TlLoad(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    if (api->size > offsetof(sco_api, query_service)) {
        const void* t = nullptr;
        if (api->query_service(SCO_IPC_NAME, SCO_IPC_VERSION_1_0, &t) == SCO_OK) g_ipc = static_cast<const sco_ipc_v1*>(t);
        t = nullptr;
        if (api->query_service(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION, &t) == SCO_OK) g_sp = static_cast<const sc_spatial_v1*>(t);
    }
    if (!g_ipc || !g_sp) LOGW("[titanlink] %s isn't published; pilot mode can't start", !g_ipc ? "sco.ipc" : "teleport.spatial");
    sco_result r = RegisterBuiltinCommand(api, self, kCap, "titanlink.pilot", "Titanfall pilot mode",
        "Turns pilot mode on or off (F9); starts Titanfall 2 if it isn't running", CmdPilot);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "titanlink.titan", "Call Titan",
        "Calls your Titan down in front of you (V in pilot mode)", CmdTitan);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "titanlink.embark", "Embark / disembark",
        "Gets into your Titan when you're next to it, or out of it (E in pilot mode)", CmdEmbark);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "titanlink.unlink", "Close TitanLink",
        "Ends pilot mode and closes the link; Titanfall 2 sees it drop", CmdUnlink);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "titanlink.status", "TitanLink status",
        "The link, the match and your Titan in one line", CmdStatus);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r != SCO_OK) return r;   // the host releases what was registered
    RegisterBuiltinTab(api, self, "titanlink.titanfall", "Titanfall", kTabTitanLink, DrawTitanLinkTab);
    BindBuiltinHotkey(api, self, "f9", "titanlink.pilot");
    return SCO_OK;
}

void TlUnload() {
    CloseLink();
    g_ipc = nullptr;
    g_sp = nullptr;
    g_api = nullptr;
    g_self = nullptr;
}

}  // namespace

// ---- the tab's view of it (titanlink.h) ---------------------------------------------------------

const TlStatus& TlGetStatus() { return g_st; }

void TlTogglePilot(char* reply, uint32_t size) {
    if (g_st.pilot || g_st.wantPilot) {
        PilotOff(true);
        snprintf(reply, size, "Pilot mode off");
        return;
    }
    if (!OpenLink(reply, size)) return;
    g_st.wantPilot = true;
    const DWORD now = GetTickCount();
    if (!g_st.linked) {
        if (!g_launchAt || now - g_launchAt > 90000) Launch();
        snprintf(reply, size, "Waiting for Titanfall 2 to load its match; pilot mode starts by itself");
    } else if (!g_st.inMatch) {
        snprintf(reply, size, "Titanfall 2 is running but not in a match yet; pilot mode starts when it is");
    } else {
        PilotOn();
        snprintf(reply, size, "Pilot mode on");
    }
}

void TlCallTitan(char* reply, uint32_t size) {
    const DWORD now = GetTickCount();
    double pos[3], f[3], u[3];
    if (!g_st.pilot) { snprintf(reply, size, "Start pilot mode first (F9)"); return; }
    if (!g_st.inMatch) { snprintf(reply, size, "Titanfall's match is still loading"); return; }
    if (g_in.flags & TL_TF_IN_TITAN) { snprintf(reply, size, "You're in your Titan"); return; }
    if (g_in.flags & TL_TF_BUSY) { snprintf(reply, size, "Wait until you're out of the Titan"); return; }
    if (g_callAt && now - g_callAt < 8000) { snprintf(reply, size, "Your Titan is still on its way down"); return; }
    if (!PoseInAnchor(pos, f, u)) { snprintf(reply, size, "Your position isn't known yet"); return; }
    double d[3];
    DirToMatch(f, d);
    d[2] = 0;
    if (!BridgeNormalize(d)) { d[0] = 0; d[1] = 1; }
    tl_msg_call_titan m = {};
    ToMatch(pos, m.drop);
    for (int i = 0; i < 2; ++i) m.drop[i] += static_cast<float>(d[i] * kTitanDropAhead * TL_UNITS_PER_METRE);
    m.yaw = static_cast<float>(std::atan2(d[1], d[0]) * 180.0 / kPi);
    if (!Push(TL_MSG_CALL_TITAN, &m, sizeof(m))) { snprintf(reply, size, "Titanfall's message ring is full; try again"); return; }
    g_callAt = now;
    snprintf(reply, size, "Titan called %.0f m in front of you", kTitanDropAhead);
}

void TlEmbark(char* reply, uint32_t size) {
    if (!g_st.pilot) { snprintf(reply, size, "Start pilot mode first (F9)"); return; }
    if (g_in.flags & TL_TF_BUSY) { snprintf(reply, size, "Wait until the Titan is done"); return; }
    if (g_in.flags & TL_TF_IN_TITAN) {
        snprintf(reply, size, Push(TL_MSG_DISEMBARK, nullptr, 0) ? "Getting out of your Titan" : "Titanfall's message ring is full; try again");
        return;
    }
    if (!(g_in.flags & TL_TF_PARKED)) { snprintf(reply, size, "No Titan yet: call one (V)"); return; }
    const double dist = TitanDistance();
    if (dist < 0) { snprintf(reply, size, "Your position isn't known yet"); return; }
    if (dist >= kEmbarkRange) { snprintf(reply, size, "Walk closer to your Titan to get in (%.0f m away)", dist); return; }
    snprintf(reply, size, Push(TL_MSG_EMBARK, nullptr, 0) ? "Climbing into your Titan" : "Titanfall's message ring is full; try again");
}

void TlUnlink(char* reply, uint32_t size) {
    if (!g_ch) { snprintf(reply, size, "TitanLink isn't open"); return; }
    CloseLink();
    snprintf(reply, size, "TitanLink closed");
}

const sco::plugins::Builtin kTitanLinkBuiltin = { "titanlink", TlQuery, TlLoad, TlUnload };
