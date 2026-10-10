#pragma once
// What the optional bridge built-ins (titanlink/, voxel_bridge/) share: finding the game window,
// orientation math over teleport.spatial poses, and checks for values read from the other program
// (docs/bridges.md). Only compiled into a build with SCO_BRIDGE_TITANLINK or SCO_BRIDGE_VOXEL.
#include "sco_api.h"
#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

// Logs one formatted line as the built-in (mod.log shows the built-in's id).
inline void BridgeLog(const sco_api* api, sco_plugin* self, sco_log_level level, const char* fmt, ...) {
    if (!api) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    api->log(self, level, line);
}

// Text from the other program: cut at n - 1 and every byte outside printable ASCII made '?', so a
// line can't forge mod.log lines or reach ImGui as markup.
inline void BridgeSanitize(char* s, size_t n) {
    if (!n) return;
    s[n - 1] = 0;
    for (char* p = s; *p; ++p)
        if (*p < 0x20 || *p > 0x7e) *p = '?';
}

// A float from the other program is finite and within +-limit.
inline bool BridgeFinite(float v, double limit) { return std::isfinite(v) && std::fabs(v) <= limit; }
inline bool BridgeFinite3(const float v[3], double limit) {
    return BridgeFinite(v[0], limit) && BridgeFinite(v[1], limit) && BridgeFinite(v[2], limit);
}

inline double BridgeDot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

inline void BridgeCross(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

inline bool BridgeNormalize(double v[3]) {
    const double l = std::sqrt(BridgeDot(v, v));
    if (!(l > 1e-9)) return false;
    for (int i = 0; i < 3; ++i) v[i] /= l;
    return true;
}

// The local axes of a rotation in the game's (x, y, z, w) order: right (+x), forward (+y) and up
// (+z), the way the game's entities face.
inline void BridgeAxes(const double q[4], double right[3], double fwd[3], double up[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    right[0] = 1 - 2 * (y * y + z * z); right[1] = 2 * (x * y + w * z);     right[2] = 2 * (x * z - w * y);
    fwd[0]   = 2 * (x * y - w * z);     fwd[1]   = 1 - 2 * (x * x + z * z); fwd[2]   = 2 * (y * z + w * x);
    up[0]    = 2 * (x * z + w * y);     up[1]    = 2 * (y * z - w * x);     up[2]    = 1 - 2 * (x * x + y * y);
}

namespace bridge_detail {
inline BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    wchar_t title[128];
    if (GetWindowTextW(hwnd, title, 128) && wcsstr(title, L"Star Citizen")) {
        *reinterpret_cast<HWND*>(out) = hwnd;
        return FALSE;
    }
    return TRUE;
}
}  // namespace bridge_detail

// Star Citizen's own top-level window, or nullptr. Any thread.
inline HWND BridgeGameWindow() {
    HWND w = nullptr;
    EnumWindows(bridge_detail::FindGameWindow, reinterpret_cast<LPARAM>(&w));
    return w;
}

// The game window's client area in pixels (0 x 0 when there is none).
inline void BridgeViewSize(uint32_t& w, uint32_t& h) {
    w = h = 0;
    RECT rc = {};
    if (HWND gw = BridgeGameWindow(); gw && GetClientRect(gw, &rc)) {
        w = static_cast<uint32_t>(rc.right > rc.left ? rc.right - rc.left : 0);
        h = static_cast<uint32_t>(rc.bottom > rc.top ? rc.bottom - rc.top : 0);
    }
}
