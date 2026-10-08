#include "menu.h"
#include "common.h"
#include "travel.h"
#include "version.h"
#include <algorithm>
#include <d3d11.h>
#include <wincodec.h>
#include <vector>
#include <shellapi.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "spawner.h"
#include "cvars.h"
#include "build.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_impl_win32.h"
#include "third_party/imgui/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND                    g_game;
static HWND                    g_wnd;
static ID3D11Device*           g_device;
static ID3D11DeviceContext*    g_context;
static IDXGISwapChain*         g_swap;
static ID3D11RenderTargetView* g_rtv;
static UINT                    g_resizeW, g_resizeH;

constexpr int kMenuW = 820, kMenuH = 740;

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void ReleaseRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool CreateDevice() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static LRESULT CALLBACK MenuWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) { g_resizeW = LOWORD(l); g_resizeH = HIWORD(l); }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}

static bool OurProcessHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool g_clipped = false;
static double g_openSince = -10.0;

static void KeepCursorInMenu() {
    if (GetForegroundWindow() != g_wnd) {
        if (g_clipped) { ClipCursor(nullptr); g_clipped = false; }
        return;
    }
    RECT r = {};
    GetWindowRect(g_wnd, &r);
    ClipCursor(&r);
    g_clipped = true;
}

static void ShowMenu(bool show) {
    if (show) {
        RECT r = {};
        GetWindowRect(g_game, &r);
        SetWindowPos(g_wnd, HWND_TOPMOST, r.left + 60, r.top + 60, kMenuW, kMenuH, SWP_SHOWWINDOW);
        SetForegroundWindow(g_wnd);
        SetCursorPos(r.left + 60 + kMenuW / 2, r.top + 60 + kMenuH / 2);
        KeepCursorInMenu();
        g_openSince = ImGui::GetTime();
    } else {
        ClipCursor(nullptr);
        g_clipped = false;
        ShowWindow(g_wnd, SW_HIDE);
        SetForegroundWindow(g_game);
    }
}

static bool ContainsNoCase(const char* s, const char* needle, size_t n) {
    for (; *s; ++s) {
        size_t i = 0;
        while (i < n && s[i] && tolower(static_cast<unsigned char>(s[i])) == tolower(static_cast<unsigned char>(needle[i]))) ++i;
        if (i == n) return true;
    }
    return false;
}

static bool MatchesFilter(const char* name, const char* filter) {
    for (const char* p = filter; *p; ) {
        p += strspn(p, " _");
        const size_t n = strcspn(p, " _");
        if (n && !ContainsNoCase(name, p, n)) return false;
        p += n;
    }
    return true;
}

static ImVec4 Hex(uint32_t rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

static const ImVec4 kBg         = Hex(0x151517);
static const ImVec4 kCard       = Hex(0x1B1B1E);
static const ImVec4 kRow        = Hex(0x242428);
static const ImVec4 kRowUp      = Hex(0x2E2E33);
static const ImVec4 kRowDown    = Hex(0x38383E);
static const ImVec4 kLine       = Hex(0x28282C);
static const ImVec4 kText       = Hex(0xECECF0);
static const ImVec4 kBody       = Hex(0xBEC0C6);
static const ImVec4 kMuted      = Hex(0x76767E);
static const ImVec4 kFaint      = Hex(0x4A4A52);
static const ImVec4 kBlue       = Hex(0x6088EC);
static const ImVec4 kAction     = Hex(0x182852);
static const ImVec4 kActionUp   = Hex(0x20366C);
static const ImVec4 kActionDown = Hex(0x2A4486);
static const ImVec4 kActionText = Hex(0x80A4FF);
static const ImVec4 kRed        = Hex(0xC43434);

constexpr float kBodySize = 16.0f;

static void ApplyTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(0, 0);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(8, 8);
    st.ItemInnerSpacing = ImVec2(8, 4);
    st.CellPadding = ImVec2(8, 5);
    st.IndentSpacing = 16;
    st.ScrollbarSize = 8;
    st.GrabMinSize = 12;
    st.WindowRounding = 0;
    st.ChildRounding = 10;
    st.FrameRounding = 8;
    st.PopupRounding = 8;
    st.GrabRounding = 8;
    st.TabRounding = 8;
    st.ScrollbarRounding = 8;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 0;
    st.TabBorderSize = 0;

    ImVec4* c = st.Colors;
    const ImVec4 none(0, 0, 0, 0);
    c[ImGuiCol_Text]                      = kText;
    c[ImGuiCol_TextDisabled]              = kMuted;
    c[ImGuiCol_WindowBg]                  = none;
    c[ImGuiCol_ChildBg]                   = none;
    c[ImGuiCol_PopupBg]                   = Hex(0x1B1B1E, 0.98f);
    c[ImGuiCol_Border]                    = kLine;
    c[ImGuiCol_BorderShadow]              = none;
    c[ImGuiCol_FrameBg]                   = kRow;
    c[ImGuiCol_FrameBgHovered]            = kRowUp;
    c[ImGuiCol_FrameBgActive]             = kRowDown;
    c[ImGuiCol_TitleBg]                   = kBg;
    c[ImGuiCol_TitleBgActive]             = kBg;
    c[ImGuiCol_TitleBgCollapsed]          = kBg;
    c[ImGuiCol_ScrollbarBg]               = none;
    c[ImGuiCol_ScrollbarGrab]             = kRow;
    c[ImGuiCol_ScrollbarGrabHovered]      = kRowUp;
    c[ImGuiCol_ScrollbarGrabActive]       = kAction;
    c[ImGuiCol_CheckMark]                 = kBlue;
    c[ImGuiCol_SliderGrab]                = kBlue;
    c[ImGuiCol_SliderGrabActive]          = kActionText;
    c[ImGuiCol_Button]                    = kRow;
    c[ImGuiCol_ButtonHovered]             = kRowUp;
    c[ImGuiCol_ButtonActive]              = kRowDown;
    c[ImGuiCol_Header]                    = kRow;
    c[ImGuiCol_HeaderHovered]             = kRowUp;
    c[ImGuiCol_HeaderActive]              = kRowDown;
    c[ImGuiCol_Separator]                 = kLine;
    c[ImGuiCol_SeparatorHovered]          = kAction;
    c[ImGuiCol_SeparatorActive]           = kBlue;
    c[ImGuiCol_ResizeGrip]                = none;
    c[ImGuiCol_ResizeGripHovered]         = none;
    c[ImGuiCol_ResizeGripActive]          = none;
    c[ImGuiCol_Tab]                       = none;
    c[ImGuiCol_TabHovered]                = kRow;
    c[ImGuiCol_TabSelected]               = kRow;
    c[ImGuiCol_TabSelectedOverline]       = kBlue;
    c[ImGuiCol_TabDimmed]                 = none;
    c[ImGuiCol_TabDimmedSelected]         = kRow;
    c[ImGuiCol_TabDimmedSelectedOverline] = kBlue;
    c[ImGuiCol_TableHeaderBg]             = Hex(0x111113);
    c[ImGuiCol_TableBorderStrong]         = kLine;
    c[ImGuiCol_TableBorderLight]          = kLine;
    c[ImGuiCol_TableRowBg]                = none;
    c[ImGuiCol_TableRowBgAlt]             = Hex(0x242428, 0.35f);
    c[ImGuiCol_PlotHistogram]             = kBlue;
    c[ImGuiCol_PlotHistogramHovered]      = kActionText;
    c[ImGuiCol_TextSelectedBg]            = Hex(0x20366C, 0.8f);
    c[ImGuiCol_DragDropTarget]            = kBlue;
    c[ImGuiCol_NavCursor]                 = kBlue;
}

static ImFont* g_bold = nullptr;

static void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    char dir[MAX_PATH] = "C:\\Windows";
    GetWindowsDirectoryA(dir, sizeof(dir));
    auto load = [&](const char* name) -> ImFont* {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\Fonts\\%s", dir, name);
        return GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES ? nullptr : io.Fonts->AddFontFromFileTTF(path, kBodySize);
    };
    ImFont* font = load("segoeui.ttf");
    if (!font) font = load("bahnschrift.ttf");
    g_bold = load("seguisb.ttf");
    if (!g_bold) g_bold = font;
    if (font) io.FontDefault = font;
    ImGuiStyle& st = ImGui::GetStyle();
    st.FontSizeBase = font ? kBodySize : 13.0f;
    st.FontScaleMain = font ? 1.0f : 1.3f;
}

static ImU32 Col(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

static ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

static float Anim(ImGuiID id, float target, float speed = 16.0f) {
    float* v = ImGui::GetStateStorage()->GetFloatRef(id, target);
    *v += (target - *v) * (1.0f - expf(-speed * ImGui::GetIO().DeltaTime));
    return *v;
}

static float EaseOut(float t) {
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

enum Icon { kIcoPlayer, kIcoTravel, kIcoShip, kIcoCrew, kIcoNpc, kIcoBuild, kIcoStar, kIcoMark };

static void DrawIcon(Icon icon, ImVec2 c, float s, ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = 1.6f, pi = 3.14159265f;
    auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
    auto line = [&](std::initializer_list<ImVec2> pts, bool closed = false) {
        ImVec2 v[12];
        int n = 0;
        for (const ImVec2& p : pts) if (n < 12) v[n++] = p;
        dl->AddPolyline(v, n, col, closed ? ImDrawFlags_Closed : ImDrawFlags_None, t);
    };
    auto person = [&](float dx, float k) {
        dl->AddCircle(P(dx, -0.45f * k), 0.38f * s * k, col, 20, t);
        dl->PathArcTo(P(dx, 0.95f), 0.8f * s * k, pi * 1.05f, pi * 1.95f, 20);
        dl->PathStroke(col, ImDrawFlags_None, t);
    };
    switch (icon) {
    case kIcoPlayer: person(0.0f, 1.0f); break;
    case kIcoCrew: person(-0.38f, 0.8f); person(0.42f, 0.8f); break;
    case kIcoNpc:
        person(-0.15f, 0.9f);
        line({ P(0.62f, -0.78f), P(0.62f, -0.18f) });
        line({ P(0.32f, -0.48f), P(0.92f, -0.48f) });
        break;
    case kIcoTravel:
        dl->PathArcTo(P(0, -0.25f), 0.62f * s, pi * 0.82f, pi * 2.18f, 24);
        dl->PathLineTo(P(0, 0.95f));
        dl->PathStroke(col, ImDrawFlags_Closed, t);
        dl->AddCircleFilled(P(0, -0.25f), 0.2f * s, col, 12);
        break;
    case kIcoShip: line({ P(0, -0.95f), P(0.75f, 0.8f), P(0, 0.4f), P(-0.75f, 0.8f) }, true); break;
    case kIcoBuild:
        line({ P(0, -0.95f), P(0.85f, -0.48f), P(0.85f, 0.48f), P(0, 0.95f), P(-0.85f, 0.48f), P(-0.85f, -0.48f) }, true);
        line({ P(-0.85f, -0.48f), P(0, 0), P(0.85f, -0.48f) });
        line({ P(0, 0), P(0, 0.95f) });
        break;
    case kIcoStar: {
        ImVec2 v[10];
        for (int i = 0; i < 10; ++i) {
            const float a = -pi / 2 + float(i) * pi / 5, r = (i % 2 ? 0.42f : 0.95f) * s;
            v[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
        }
        dl->AddPolyline(v, 10, col, ImDrawFlags_Closed, t);
        break;
    }
    case kIcoMark:
        dl->AddCircle(c, 0.92f * s, col, 40, 2.2f);
        dl->AddTriangleFilled(P(-0.28f, -0.45f), P(0.5f, 0.0f), P(-0.28f, 0.45f), col);
        break;
    }
}

struct Page { const char* name; Icon icon; };
static const Page kPages[] = {
    { "Player", kIcoPlayer }, { "Travel", kIcoTravel }, { "Vehicles", kIcoShip }, { "Crew", kIcoCrew },
    { "NPCs", kIcoNpc }, { "Build", kIcoBuild }, { "Squadron 42", kIcoStar },
};
constexpr int kPageCount = int(sizeof(kPages) / sizeof(kPages[0]));
static int    g_page = 0;
static double g_pageSince = -10.0;

static const char*    kDiscordText = "discord.gg/KPt5VBWeJ";
static const wchar_t* kDiscordUrl = L"https://discord.gg/KPt5VBWeJ";
static bool           g_openDiscord = false;

// --- background image: data\menu_background.png / .jpg, decoded with Windows' own WIC ---------

static ID3D11ShaderResourceView* g_bgSrv = nullptr;
static UINT  g_bgW = 0, g_bgH = 0;
static bool  g_bgShow = true;
static int   g_bgDarkness = 74;       // percent of the green wash over the image
static float g_bgPosition = 0.5f;     // which part of a wide image shows in the tall menu
static char  g_bgPath[MAX_PATH] = "";

static bool DecodeImage(const char* file, std::vector<uint8_t>& pixels, UINT& w, UINT& h) {
    wchar_t wide[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, file, -1, wide, MAX_PATH)) return false;
    IWICImagingFactory*    factory = nullptr;
    IWICBitmapDecoder*     decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter*   rgba = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
           && SUCCEEDED(factory->CreateDecoderFromFilename(wide, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
           && SUCCEEDED(decoder->GetFrame(0, &frame))
           && SUCCEEDED(factory->CreateFormatConverter(&rgba))
           && SUCCEEDED(rgba->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))
           && SUCCEEDED(rgba->GetSize(&w, &h))
           && w > 0 && h > 0 && w <= 8192 && h <= 8192;
    if (ok) {
        pixels.resize(static_cast<size_t>(w) * h * 4);
        ok = SUCCEEDED(rgba->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    }
    if (rgba) rgba->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    return ok;
}

static void LoadBackground() {
    static const char* const kNames[] = { "menu_background.png", "menu_background.jpg", "menu_background.jpeg" };
    for (const char* name : kNames) {
        char path[MAX_PATH];
        if (!DataFilePath(path, sizeof(path), name)) return;
        if (!g_bgPath[0]) strcpy_s(g_bgPath, path);           // shown in the Menu tab if nothing loads
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        std::vector<uint8_t> pixels;
        UINT w = 0, h = 0;
        if (!DecodeImage(path, pixels, w, h)) { Log("[menu] couldn't read %s", path); continue; }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA init = { pixels.data(), w * 4, 0 };
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(g_device->CreateTexture2D(&desc, &init, &tex)) && tex) {
            g_device->CreateShaderResourceView(tex, nullptr, &g_bgSrv);
            tex->Release();
        }
        if (g_bgSrv) {
            g_bgW = w;
            g_bgH = h;
            strcpy_s(g_bgPath, path);
            Log("[menu] background: %s (%ux%u)", path, w, h);
        }
        return;
    }
}

static void DrawBackdrop() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    dl->AddRectFilled(ImVec2(0, 0), size, ImGui::GetColorU32(kBg));
    if (!g_bgSrv || !g_bgShow || size.x <= 0 || size.y <= 0) return;
    // Cover the window, cropping whichever direction overflows.
    const float imageAspect = static_cast<float>(g_bgW) / static_cast<float>(g_bgH);
    const float viewAspect = size.x / size.y;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (imageAspect > viewAspect) {
        const float visible = viewAspect / imageAspect;
        uv0.x = g_bgPosition * (1 - visible);
        uv1.x = uv0.x + visible;
    } else {
        const float visible = imageAspect / viewAspect;
        uv0.y = (1 - visible) * 0.5f;
        uv1.y = uv0.y + visible;
    }
    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_bgSrv)), ImVec2(0, 0), size, uv0, uv1);
    dl->AddRectFilled(ImVec2(0, 0), size, ImGui::GetColorU32(ImVec4(kBg.x, kBg.y, kBg.z, g_bgDarkness / 100.0f)));
}

// --- small building blocks ------------------------------------------------------------------

static bool g_cardOpen = false;
static int  g_cardIndex = 0;

static void CloseCard() {
    if (!g_cardOpen) return;
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 4));
    g_cardOpen = false;
}

static void Section(const char* title) {
    CloseCard();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild(ImGui::GetID(g_page * 100 + g_cardIndex++), ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    g_cardOpen = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(g_bold, 0.0f);
    const float fs = ImGui::GetFontSize();
    DrawIcon(kPages[g_page].icon, ImVec2(p.x + 8, p.y + fs * 0.5f), 7, Col(kBlue));
    dl->AddText(ImVec2(p.x + 26, p.y), Col(kText), title);
    ImGui::PopFont();
    dl->AddLine(ImVec2(p.x - 2, p.y + fs + 12), ImVec2(p.x + w + 2, p.y + fs + 12), Col(kLine), 1.0f);
    ImGui::Dummy(ImVec2(0, fs + 16));
}

static void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static void Tip(const char* text) { ImGui::SetItemTooltip("%s", text); }

static bool UiButton(const char* label, ImVec2 size = ImVec2(0, 0), bool primary = false) {
    const char* end = label;
    while (*end && !(end[0] == '#' && end[1] == '#')) ++end;
    ImGui::PushFont(primary ? g_bold : nullptr, 0.0f);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const float avail = ImGui::GetContentRegionAvail().x;
    if (size.x < 0) size.x = (std::max)(4.0f, avail + size.x + 1);
    if (size.x == 0) size.x = ts.x + ImGui::GetStyle().FramePadding.x * 2;
    if (size.y == 0) size.y = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const ImGuiID id = ImGui::GetItemID();
    const bool hot = ImGui::IsItemHovered(), down = ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float h = Anim(id, hot ? 1.0f : 0.0f, 12.0f), d = Anim(id + 1, down ? 1.0f : 0.0f, 20.0f);
    const ImVec4 bg = primary ? Mix(Mix(kAction, kActionUp, h), kActionDown, d) : Mix(Mix(kRow, kRowUp, h), kRowDown, d);
    const ImVec4 fg = primary ? Mix(kActionText, Hex(0xB4CAFF), h) : Mix(kBody, kText, h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 in(d, d);
    dl->AddRectFilled(pos + in, pos + size - in, Col(bg), 8);
    const ImVec2 at(pos.x + (std::max)(ImGui::GetStyle().FramePadding.x, (size.x - ts.x) * 0.5f), pos.y + (size.y - ts.y) * 0.5f + d * 0.5f);
    dl->PushClipRect(pos, pos + size, true);
    dl->AddText(at, Col(fg), label, end);
    dl->PopClipRect();
    ImGui::PopFont();
    return clicked;
}

static bool PrimaryButton(const char* label, float height = 40.0f) {
    return UiButton(label, ImVec2(-1, height), true);
}

static bool UiToggle(const char* label, bool* v) {
    const char* end = label;
    while (*end && !(end[0] == '#' && end[1] == '#')) ++end;
    const float h = ImGui::GetFrameHeight(), sw = 38, sh = 22, gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(sw + (ts.x > 0 ? gap + ts.x : 0), h));
    if (clicked) *v = !*v;
    const ImGuiID id = ImGui::GetItemID();
    const bool hot = ImGui::IsItemHovered();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float on = Anim(id, *v ? 1.0f : 0.0f, 14.0f), hv = Anim(id + 1, hot ? 1.0f : 0.0f, 12.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(pos.x, pos.y + (h - sh) * 0.5f), b(a.x + sw, a.y + sh);
    dl->AddRectFilled(a, b, Col(Mix(Mix(kRow, kRowUp, hv), Mix(kAction, kActionUp, hv), on)), sh * 0.5f);
    const ImVec2 c(a.x + sh * 0.5f + (sw - sh) * on, a.y + sh * 0.5f);
    dl->AddCircleFilled(c, sh * 0.5f - 4 + hv * 0.5f, Col(Mix(kMuted, kActionText, on)), 24);
    if (ts.x > 0) dl->AddText(ImVec2(b.x + gap, pos.y + (h - ts.y) * 0.5f), Col(Mix(kBody, kText, (std::max)(on, hv))), label, end);
    return clicked;
}

static ImGuiStorage* g_comboStore = nullptr;
static ImGuiID       g_comboKey = 0;

static bool UiBeginCombo(const char* id, const char* preview, ImGuiComboFlags flags = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiStorage* store = ImGui::GetStateStorage();
    char list[96];
    snprintf(list, sizeof(list), "%s##list", id);
    const ImGuiID key = ImGui::GetID(id);
    const float w = ImGui::CalcItemWidth(), fh = ImGui::GetFrameHeight();
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + fh);
    const bool wasOpen = store->GetBool(key + 7);
    if (ImGui::InvisibleButton(id, ImVec2(w, fh)) && !wasOpen && !ImGui::IsPopupOpen(list)) {
        ImGui::OpenPopup(list);
        store->SetFloat(key + 5, float(ImGui::GetTime()));
    }
    const bool hot = ImGui::IsItemHovered();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const bool isOpen = ImGui::IsPopupOpen(list);
    store->SetBool(key + 7, isOpen);
    const float h = Anim(key, hot ? 1.0f : 0.0f, 12.0f), o = Anim(key + 1, isOpen ? 1.0f : 0.0f, 16.0f);
    dl->AddRectFilled(a, b, Col(Mix(Mix(kRow, kRowUp, h), kRowDown, o * 0.5f)), 8);
    if (o > 0.0f) dl->AddRect(a, b, Col(kBlue, 0.55f * o), 8, 0, 1.0f);
    const ImVec2 ts = ImGui::CalcTextSize(preview);
    const float tx = a.x + ImGui::GetStyle().FramePadding.x, room = b.x - 30 - tx;
    if (ts.x <= room) {
        dl->AddText(ImVec2(tx, a.y + (fh - ts.y) * 0.5f), Col(kText), preview);
    } else {
        dl->PushClipRect(a, ImVec2(b.x - 30 - ImGui::CalcTextSize("...").x, b.y), true);
        dl->AddText(ImVec2(tx, a.y + (fh - ts.y) * 0.5f), Col(kText), preview);
        dl->PopClipRect();
        dl->AddText(ImVec2(b.x - 30 - ImGui::CalcTextSize("...").x, a.y + (fh - ts.y) * 0.5f), Col(kText), "...");
    }
    const ImVec2 c(b.x - 16, (a.y + b.y) * 0.5f);
    const float k = 1.0f - 2.0f * o;
    const ImVec2 pts[3] = { ImVec2(c.x - 4.5f, c.y - 2.25f * k), ImVec2(c.x, c.y + 2.25f * k), ImVec2(c.x + 4.5f, c.y - 2.25f * k) };
    dl->AddPolyline(pts, 3, Col(Mix(kMuted, kText, (std::max)(h, o))), ImDrawFlags_None, 1.6f);
    if (!isOpen) return false;

    const float maxH = (flags & ImGuiComboFlags_HeightLargest) ? 420.0f : (flags & ImGuiComboFlags_HeightLarge) ? 320.0f : 240.0f;
    const float lastH = (std::min)(store->GetFloat(key + 6, 0.0f), maxH);
    const float t = EaseOut((float(ImGui::GetTime()) - store->GetFloat(key + 5, 0.0f)) / 0.16f);
    const float slide = (1.0f - t) * 6.0f;
    const bool above = b.y + 6 + lastH > ImGui::GetIO().DisplaySize.y - 8 && a.y - 6 - lastH > 8;
    ImGui::SetNextWindowPos(ImVec2(a.x, above ? a.y - 6 - lastH + slide : b.y + 6 - slide));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, maxH));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * t);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Hex(0x202024));
    if (!ImGui::BeginPopup(list)) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(4);
        return false;
    }
    g_comboStore = store;
    g_comboKey = key;
    return true;
}

static void UiEndCombo() {
    if (g_comboStore) g_comboStore->SetFloat(g_comboKey + 6, ImGui::GetWindowHeight());
    ImGui::EndPopup();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

static bool UiOption(const char* label, bool selected, bool enabled = true) {
    const char* end = label;
    while (*end && !(end[0] == '#' && end[1] == '#')) ++end;
    const float w = ImGui::GetContentRegionAvail().x, rh = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, rh));
    const bool hot = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float h = Anim(ImGui::GetItemID(), hot ? 1.0f : 0.0f, 18.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) dl->AddRectFilled(pos, pos + ImVec2(w, rh), Col(Mix(Hex(0x1C2A4C), kActionUp, h)), 6);
    else if (h > 0.0f) dl->AddRectFilled(pos, pos + ImVec2(w, rh), Col(kRow, h), 6);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const ImVec4 fg = !enabled ? kFaint : selected ? kActionText : Mix(kBody, kText, h);
    dl->PushClipRect(pos, pos + ImVec2(w - 26, rh), true);
    dl->AddText(ImVec2(pos.x + 10, pos.y + (rh - ts.y) * 0.5f), Col(fg), label, end);
    dl->PopClipRect();
    if (selected) {
        const ImVec2 c(pos.x + w - 16, pos.y + rh * 0.5f);
        const ImVec2 pts[3] = { ImVec2(c.x - 4.5f, c.y), ImVec2(c.x - 1.5f, c.y + 3.0f), ImVec2(c.x + 4.5f, c.y - 3.5f) };
        dl->AddPolyline(pts, 3, Col(kActionText), ImDrawFlags_None, 1.6f);
    }
    if (clicked && enabled) ImGui::CloseCurrentPopup();
    return clicked && enabled;
}

static bool UiGroup(const char* label, bool defaultOpen, bool forceOpen) {
    const char* end = label;
    while (*end && !(end[0] == '#' && end[1] == '#')) ++end;
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID(label);
    bool open = store->GetBool(key + 3, defaultOpen);
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(label, ImVec2(w, h))) { open = !open; store->SetBool(key + 3, open); }
    if (forceOpen) open = true;
    const bool hot = ImGui::IsItemHovered();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float hv = Anim(key, hot ? 1.0f : 0.0f, 12.0f), o = Anim(key + 1, open ? 1.0f : 0.0f, 14.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + ImVec2(w, h), Col(Mix(kRow, kRowUp, hv)), 8);
    const ImVec2 c(pos.x + 18, pos.y + h * 0.5f);
    const float ang = (1.0f - o) * -1.5707963f, cs = cosf(ang), sn = sinf(ang);
    auto rot = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
    const ImVec2 pts[3] = { rot(-4.5f, -2.25f), rot(0.0f, 2.25f), rot(4.5f, -2.25f) };
    dl->AddPolyline(pts, 3, Col(Mix(kMuted, kBlue, o)), ImDrawFlags_None, 1.6f);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    dl->AddText(ImVec2(pos.x + 34, pos.y + (h - ts.y) * 0.5f), Col(Mix(kBody, kText, (std::max)(hv, o))), label, end);
    return open;
}

static float SliderFraction(float v, float lo, float hi, bool logarithmic) {
    if (hi <= lo) return 0.0f;
    const float t = logarithmic && lo > 0 ? logf(v / lo) / logf(hi / lo) : (v - lo) / (hi - lo);
    return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

static void PushHiddenSlider() {
    const ImVec4 none(0, 0, 0, 0);
    for (ImGuiCol c : { ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive, ImGuiCol_Text })
        ImGui::PushStyleColor(c, none);
}

static void DrawSlider(float t, const char* text) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImGuiID id = ImGui::GetItemID();
    const bool hot = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    if (hot || held) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float shown = Anim(id + 7, t, 22.0f), hv = Anim(id + 8, hot || held ? 1.0f : 0.0f, 12.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = b.y - a.y, r = h * 0.5f;
    dl->AddRectFilled(a, b, Col(Mix(kRow, kRowUp, hv)), r);
    const float fx = a.x + h + (b.x - a.x - h) * shown;
    dl->AddRectFilled(a, ImVec2(fx, b.y), Col(Mix(kAction, kActionUp, hv)), r);
    const float kx = fx - r, kr = r - 5 + hv * 1.5f;
    dl->AddCircleFilled(ImVec2(kx, a.y + r), kr, Col(Mix(kBlue, kActionText, hv)), 24);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const bool inside = kx - kr - 10 - ts.x >= a.x + 12;
    const float side = Anim(id + 9, inside ? 1.0f : 0.0f, 16.0f);
    const float tx = (kx + kr + 10) + ((kx - kr - 10 - ts.x) - (kx + kr + 10)) * side;
    dl->PushClipRect(a, b, true);
    dl->AddText(ImVec2(tx, a.y + (h - ts.y) * 0.5f), Col(Mix(kBody, kText, side)), text);
    dl->PopClipRect();
}

static bool UiSliderFloat(const char* id, float* v, float lo, float hi, const char* fmt, ImGuiSliderFlags flags = 0) {
    PushHiddenSlider();
    const bool changed = ImGui::SliderFloat(id, v, lo, hi, fmt, flags | ImGuiSliderFlags_NoInput);
    ImGui::PopStyleColor(6);
    char text[96];
    snprintf(text, sizeof(text), fmt, *v);
    DrawSlider(SliderFraction(*v, lo, hi, (flags & ImGuiSliderFlags_Logarithmic) != 0), text);
    return changed;
}

static bool UiSliderInt(const char* id, int* v, int lo, int hi, const char* fmt) {
    PushHiddenSlider();
    const bool changed = ImGui::SliderInt(id, v, lo, hi, fmt, ImGuiSliderFlags_NoInput);
    ImGui::PopStyleColor(6);
    char text[96];
    snprintf(text, sizeof(text), fmt, *v);
    DrawSlider(SliderFraction(float(*v), float(lo), float(hi), false), text);
    return changed;
}

static float Columns(int n) {
    return (ImGui::GetContentRegionAvail().x - (n - 1) * ImGui::GetStyle().ItemSpacing.x) / n;
}

static bool SearchBox(const char* id, const char* hint, char* buf, size_t n) {
    ImGui::SetNextItemWidth(-1);
    return ImGui::InputTextWithHint(id, hint, buf, n);
}

static void PrettyBuildName(char* out, size_t n, const char* name) {
    static const char* const kPrefixes[] = { "PlayerDeco_", "Turret_Automated_", "BaseBuilding_Interactables_", "BaseBuilding_",
                                             "PU_Human_Enemy_GroundCombat_NPC_", "PU_Human-", "NPC_Archetypes-", "AIShip_CrewProfiles-" };
    if (const char* slash = strrchr(name, '/')) name = slash + 1;
    for (const char* p : kPrefixes)
        if (_strnicmp(name, p, strlen(p)) == 0) { name += strlen(p); break; }
    strncpy_s(out, n, name, _TRUNCATE);
    if (char* ext = strstr(out, ".socpak")) *ext = 0;
    if (char* guid = strstr(out, "_{")) {
        char tag[8];
        snprintf(tag, sizeof(tag), " (%.4s)", guid + 2);
        *guid = 0;
        strncat_s(out, n, tag, _TRUNCATE);
    }
    for (char* c = out; *c; ++c) if (*c == '_' || *c == '-') *c = ' ';
}

// --- NPC picker, shared by the NPCs and Crew tabs -------------------------------------------

static int g_npcPick = 0;

static bool NpcPicker() {
    static char filter[64] = "";
    const int npcs = Menu_NpcCount();
    if (npcs < 0) { Hint("Loading NPCs (you need to be in the universe)..."); return false; }
    if (npcs == 0) { Hint("No NPCs found. Check data\\npcs.txt."); return false; }
    if (g_npcPick >= npcs) g_npcPick = 0;
    if (SearchBox("##npcFilter", "Search NPCs", filter, sizeof(filter)))
        for (int i = 0; i < npcs; ++i)
            if (MatchesFilter(Menu_NpcName(i), filter)) { g_npcPick = i; break; }
    char preview[128];
    PrettyBuildName(preview, sizeof(preview), Menu_NpcName(g_npcPick));
    ImGui::SetNextItemWidth(-1);
    if (UiBeginCombo("##npc", preview, ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < npcs; ++i) {
            const char* name = Menu_NpcName(i);
            if (!MatchesFilter(name, filter)) continue;
            char label[160];
            PrettyBuildName(label, sizeof(label) - 16, name);
            snprintf(label + strlen(label), 16, "##n%d", i);
            if (UiOption(label, i == g_npcPick)) g_npcPick = i;
            ImGui::SetItemTooltip("%s", name);
            if (i == g_npcPick) ImGui::SetItemDefaultFocus();
        }
        UiEndCombo();
    }
    return true;
}

// =============================================================================================
// Player
// =============================================================================================

static void GearCombo(int slot, const char* label, const char* none, int& pick, const char* filter, float width) {
    const int n = Menu_GearCount(slot);
    if (pick >= n) pick = -1;
    char preview[112], id[32];
    snprintf(preview, sizeof(preview), "%s: %s", label, pick >= 0 ? Menu_GearName(slot, pick) : none);
    snprintf(id, sizeof(id), "##gear%d", slot);
    ImGui::SetNextItemWidth(width);
    if (!UiBeginCombo(id, preview, ImGuiComboFlags_HeightLarge)) return;
    if (UiOption(none, pick < 0)) pick = -1;
    for (int i = 0; i < n; ++i) {
        const char* name = Menu_GearName(slot, i);
        if (!MatchesFilter(name, filter)) continue;
        ImGui::PushID(i);
        if (UiOption(name, i == pick)) pick = i;
        if (i == pick) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
    }
    UiEndCombo();
}

static void DrawPlayerTab(bool& keepOpen) {
    Section("Movement");
    static bool  noclip = false;
    static float speed = 30.0f;
    if (UiToggle("Noclip", &noclip)) Menu_SetNoclip(noclip, speed);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (UiSliderFloat("##noclipSpeed", &speed, 1.0f, 500.0f, "Speed %.0f", ImGuiSliderFlags_Logarithmic))
        Menu_SetNoclipSpeed(speed);

    Section("Protection");
    static bool god = true, ammo = false;
    if (UiToggle("God mode", &god)) Menu_SetGodMode(god);
    ImGui::SameLine(0, 24);
    if (UiToggle("Infinite ammo", &ammo)) Menu_SetInfiniteAmmo(ammo);

    Section("Gear");
    static int  gear[Gear_SlotCount] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    static char gearFilter[64] = "";
    static const struct { const char* label; const char* none; } kGear[Gear_SlotCount] = {
        { "Undersuit", "Default" }, { "Helmet", "None" }, { "Body", "None" }, { "Arms", "None" }, { "Legs", "None" },
        { "Backpack", "None" }, { "Primary", "None" }, { "Sidearm", "None" }, { "Ammo", "Matches weapon" }, { "Grenades", "None" } };
    if (Menu_GearCount(0) < 0) { Hint("Loading gear (you need to be in the universe)..."); return; }
    SearchBox("##gearFilter", "Search gear", gearFilter, sizeof(gearFilter));
    const float half = Columns(2);
    for (int s = 0; s < Gear_SlotCount; ++s) {
        if (s % 2) ImGui::SameLine();
        GearCombo(s, kGear[s].label, kGear[s].none, gear[s], gearFilter, half);
    }
    if (PrimaryButton("Equip gear")) { Menu_RequestEquip(gear); keepOpen = false; }
}

// =============================================================================================
// Travel
// =============================================================================================

static bool HasText(const char* filter) { return filter[strspn(filter, " _")] != 0; }

static void DrawTravelTab() {
    static TravelPlace    places[3000];
    static int            order[3000];
    static TravelBookmark marks[400];
    static char  filter[64] = "";
    static char  markName[64] = "";
    static char  selected[96] = "";       // entity name of the selected place
    static float altitude = 2000.0f;
    static bool  showMinor = false;

    static int np = 0, placesVersion = -1;
    char here[32];
    Travel_CurrentSystem(here, sizeof(here));
    const int version = Travel_PlacesVersion();
    const bool placesChanged = version != placesVersion;
    if (placesChanged) { placesVersion = version; np = Travel_GetPlaces(places, 3000); }
    const int nm = Travel_GetBookmarks(marks, 400);
    const bool searching = HasText(filter);

    // Systems that have anything in them; the one you're in first, then A-Z.
    const char* systems[48];
    int ns = 0;
    auto addSystem = [&](const char* sys) {
        if (!sys[0]) return;
        for (int i = 0; i < ns; ++i) if (_stricmp(systems[i], sys) == 0) return;
        if (ns < 48) systems[ns++] = sys;
    };
    if (here[0]) addSystem(here);
    for (int i = 0; i < np; ++i) addSystem(places[i].system);
    for (int i = 0; i < nm; ++i) addSystem(marks[i].system);
    std::sort(systems + (here[0] ? 1 : 0), systems + ns, [](const char* a, const char* b) { return _stricmp(a, b) < 0; });

    if (placesChanged) {                           // OOC_Stanton_1, 1a, 1b, 2 ... reads in orbit order
        for (int i = 0; i < np; ++i) order[i] = i;
        std::sort(order, order + np, [&](int a, int b) { return _stricmp(places[a].entity, places[b].entity) < 0; });
    }

    SearchBox("##travelFilter", "Search places and saved spots", filter, sizeof(filter));

    Section("Places");
    ImGui::SetNextItemWidth(-1);
    UiSliderFloat("##altitude", &altitude, 100.0f, 20000.0f, "Arrive %.0f m above the ground", ImGuiSliderFlags_Logarithmic);
    UiToggle("Show interiors and small zones", &showMinor);
    ImGui::SetItemTooltip("Elevator lobbies, hangars, asteroid-belt segments and similar. Hidden by default.");
    const TravelPlace* pick = nullptr;
    bool go = false;
    for (int s = 0; s < ns; ++s) {
        int shown = 0;
        for (int k = 0; k < np; ++k) {
            const TravelPlace& p = places[order[k]];
            if (p.kind == Place_Minor && !showMinor) continue;
            if (_stricmp(p.system, systems[s]) == 0 && (!searching || MatchesFilter(p.name, filter) || MatchesFilter(p.entity, filter))) ++shown;
        }
        if (!shown) continue;
        const bool isHere = here[0] && _stricmp(systems[s], here) == 0;
        char header[80];
        const bool unnamed = _strnicmp(systems[s], "SolarSystem", 11) == 0;
        snprintf(header, sizeof(header), "%s%s (%d)###sys_%s", unnamed ? "Unnamed system" : systems[s],
                 isHere ? ", you are here" : "", shown, systems[s]);
        if (!UiGroup(header, isHere, searching)) continue;
        char table[48];
        snprintf(table, sizeof(table), "##places_%s", systems[s]);
        const float rows = static_cast<float>(shown < 10 ? shown : 10);
        const float height = (rows + 1) * ImGui::GetFrameHeight() + 4;
        if (!ImGui::BeginTable(table, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY, ImVec2(0, height))) continue;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Place", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableHeadersRow();
        for (int k = 0; k < np; ++k) {
            const TravelPlace& p = places[order[k]];
            if (_stricmp(p.system, systems[s]) != 0 || (p.kind == Place_Minor && !showMinor)) continue;
            if (searching && !MatchesFilter(p.name, filter) && !MatchesFilter(p.entity, filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[128];
            snprintf(label, sizeof(label), "%s##%s", p.name, p.entity);
            const bool isSel = _stricmp(selected, p.entity) == 0;
            if (ImGui::Selectable(label, isSel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                strcpy_s(selected, p.entity);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { pick = &p; go = true; }
            }
            ImGui::SetItemTooltip("%s", p.entity);
            ImGui::TableNextColumn();
            static const char* const kKind[] = { "Planet", "Moon", "Place", "Interior" };
            const int kindIdx = p.kind >= 0 && p.kind <= 3 ? p.kind : 2;
            ImGui::TextDisabled("%s%s", kKind[kindIdx], kindIdx <= Place_Moon && p.radius <= 0 ? ", orbit" : "");
        }
        ImGui::EndTable();
    }
    if (!pick)
        for (int i = 0; i < np; ++i)
            if (_stricmp(places[i].entity, selected) == 0) { pick = &places[i]; break; }
    char goLabel[96];
    snprintf(goLabel, sizeof(goLabel), pick ? "Go to %s" : "Pick a place to go", pick ? pick->name : "");
    ImGui::BeginDisabled(!pick);
    if (PrimaryButton(goLabel)) go = true;
    ImGui::EndDisabled();
    if (go && pick) Travel_RequestPlace(*pick, altitude);

    float progress = 0;
    if (Travel_Scanning(progress)) {
        ImGui::ProgressBar(progress, ImVec2(-1, 0), "Scanning...");
    } else {
        if (UiButton("Scan the game for places", ImVec2(-1, 0))) Travel_RequestScan();
        Tip("The scan finds the planets, moons, stations, Lagrange points, comm arrays and jump points of every loaded "
            "system and adds them here. It only reads; it takes a few seconds.");
    }

    Section("Saved spots");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 160 - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##markName", "Name this spot", markName, sizeof(markName));
    ImGui::SameLine();
    if (UiButton("Save this spot", ImVec2(160, 0))) { Travel_RequestSaveBookmark(markName); markName[0] = 0; }
    Tip("F7 saves where you're standing and F8 takes you back. Teleports only work within the system you're in.");
    if (!nm) Hint("Nothing saved yet.");
    for (int s = 0; s < ns; ++s) {
        int shown = 0;
        for (int i = 0; i < nm; ++i)
            if (_stricmp(marks[i].system, systems[s]) == 0 && (!searching || MatchesFilter(marks[i].name, filter))) ++shown;
        if (!shown) continue;
        char node[80];
        snprintf(node, sizeof(node), "%s (%d)###marks_%s", systems[s], shown, systems[s]);
        if (!UiGroup(node, true, searching)) continue;
        for (int i = 0; i < nm; ++i) {
            if (_stricmp(marks[i].system, systems[s]) != 0 || (searching && !MatchesFilter(marks[i].name, filter))) continue;
            ImGui::PushID(i);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float rowH = ImGui::GetFrameHeight() + 8, w = ImGui::GetContentRegionAvail().x;
            const ImVec2 r0 = ImGui::GetCursorScreenPos();
            const bool rowHot = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(r0, r0 + ImVec2(w, rowH));
            const float rh = Anim(ImGui::GetID("##row"), rowHot ? 1.0f : 0.0f, 12.0f);
            dl->AddRectFilled(r0, r0 + ImVec2(w, rowH), Col(Mix(Hex(0x1F1F23), Hex(0x26262B), rh)), 8);
            dl->AddText(ImVec2(r0.x + 14, r0.y + (rowH - ImGui::GetFontSize()) * 0.5f), Col(Mix(kBody, kText, rh)), marks[i].name);
            ImGui::SetCursorScreenPos(ImVec2(r0.x + w - 4 - 80 - 6 - 70, r0.y + 4));
            if (UiButton("Go", ImVec2(70, 0))) Travel_RequestBookmark(i);
            ImGui::SameLine(0, 6);
            if (UiButton("Delete", ImVec2(80, 0))) Travel_RequestDeleteBookmark(i);
            ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + rowH - 4));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
    }
}

// =============================================================================================
// Vehicles
// =============================================================================================

static void DrawVehiclesTab(bool& keepOpen) {
    static int  selected = 0;
    static char filter[64] = "";
    static MenuSpawnOptions opt;

    Section("Spawn a ship");
    const int count = Menu_ShipCount();
    if (count < 0) {
        Hint("Loading ships (you need to be in the universe)...");
    } else if (count == 0) {
        Hint("No ships found. Check data\\ships.txt.");
    } else {
        const MenuShip* ships = Menu_Ships();
        if (selected >= count) selected = 0;
        if (SearchBox("##shipFilter", "Search ships", filter, sizeof(filter)))
            for (int i = 0; i < count; ++i)
                if (MatchesFilter(ships[i].name, filter)) { selected = i; break; }
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter
                                    | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##ships", 3, flags, ImVec2(0, 230))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Ship", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 44);
            ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 64);
            ImGui::TableHeadersRow();
            for (int i = 0; i < count; ++i) {
                if (!MatchesFilter(ships[i].name, filter)) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char label[96];
                snprintf(label, sizeof(label), "%s##s%d", ships[i].name, i);
                if (ImGui::Selectable(label, i == selected, ImGuiSelectableFlags_SpanAllColumns)) selected = i;
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%d", ships[i].size);
                ImGui::TableNextColumn();
                if (ships[i].length > 0) ImGui::TextDisabled("%.0f m", ships[i].length);
            }
            ImGui::EndTable();
        }

        ImGui::SetNextItemWidth(-1);
        UiSliderFloat("##height", &opt.height, 0.0f, 500.0f, "Spawn %.0f m above you");
        static const char* const kBoard[] = { "Don't board", "Board in the pilot seat", "Board in a seat by name", "Choose a seat after it spawns" };
        ImGui::SetNextItemWidth(-1);
        if (UiBeginCombo("##board", kBoard[opt.seatMode >= 0 && opt.seatMode < 4 ? opt.seatMode : 0])) {
            for (int i = 0; i < 4; ++i) if (UiOption(kBoard[i], opt.seatMode == i)) opt.seatMode = i;
            UiEndCombo();
        }
        if (opt.seatMode == SeatMode_Named) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##seatName", "Seat name, e.g. copilot or turret left", opt.seatName, sizeof(opt.seatName));
            ImGui::SetItemTooltip("Every word must appear in the seat's name. The Crew tab shows a ship's seat names.");
        }
        const bool boarding = opt.seatMode == SeatMode_Pilot || opt.seatMode == SeatMode_Named;
        ImGui::BeginDisabled(!boarding);
        UiToggle("Remove the NPC in my seat", &opt.replaceNpc);
        ImGui::SameLine(0, 24);
        UiToggle("Power on", &opt.flightReady);
        ImGui::SetItemTooltip("Powers the ship on once you're in a pilot seat.");
        ImGui::EndDisabled();

        char spawn[96];
        snprintf(spawn, sizeof(spawn), "Spawn %s", ships[selected].name);
        if (PrimaryButton(spawn)) {
            MenuSpawnOptions send = opt;
            if (!boarding) send.flightReady = false;
            Menu_RequestSpawn(selected, send);
            if (opt.seatMode != SeatMode_PickLater) keepOpen = false;
        }
    }

    Section("Current ship");
    static bool shipAmmo = false;
    if (UiToggle("Infinite ship ammo", &shipAmmo)) Menu_SetInfiniteShipAmmo(shipAmmo);
    ImGui::SetItemTooltip("Refills the magazines of the ship you're aboard, and the ship in the Crew tab.");
    if (UiButton("Power on / off", ImVec2(-1, 0))) Menu_RequestFlightReady();
    Tip("Toggles Flight Ready on the ship in the Crew tab, as if you pressed R in its pilot seat.");
}

// =============================================================================================
// Crew
// =============================================================================================

// "VNCL_Mauler_Gunner_Seat_200006242347" on a VNCL_Mauler -> "Gunner Seat 3"
static void PrettySeatNames(const MenuSeat* seats, int n, const char* ship, char (*out)[64]) {
    static char base[128][64];
    for (int i = 0; i < n; ++i) {
        char name[64];
        strcpy_s(name, seats[i].name);
        if (char* cut = strrchr(name, '_'); cut && cut[1] && strspn(cut + 1, "0123456789") == strlen(cut + 1)) *cut = 0;
        const char* a = name;          // drop the leading words the seat shares with the ship's name
        const char* b = ship;
        for (;;) {
            const size_t la = strcspn(a, "_"), lb = strcspn(b, "_ ");
            if (!la || la != lb || _strnicmp(a, b, la) != 0 || !a[la]) break;
            a += la + 1;
            b += lb + (b[lb] ? 1 : 0);
        }
        strcpy_s(base[i], a);
        for (char* c = base[i]; *c; ++c) if (*c == '_') *c = ' ';
    }
    for (int i = 0; i < n; ++i) {      // number repeats: Gunner Seat 1, Gunner Seat 2, ...
        int total = 0, before = 0;
        for (int j = 0; j < n; ++j)
            if (_stricmp(base[i], base[j]) == 0) { ++total; if (j < i) ++before; }
        if (total > 1) snprintf(out[i], 64, "%s %d", base[i], before + 1);
        else strcpy_s(out[i], 64, base[i]);
    }
}

static void DrawCrewTab() {
    if (!Menu_SeatControlAvailable()) {
        Section("Crew");
        Hint("Seat control isn't available in this game version. mod.log has the details.");
        return;
    }
    static MenuSeat seats[128];
    static char pretty[128][64];
    static unsigned long long selectedSeat = 0;
    static bool replace = true;
    char ship[64] = "";
    const int count = Menu_GetSeats(seats, 128, ship, sizeof(ship));

    Section("Ship");
    ImGui::TextUnformatted(count < 0 ? "No ship selected" : ship);
    ImGui::SameLine();
    const float button = 190;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - button);
    if (UiButton("Use the ship I'm in", ImVec2(button, 0))) Menu_TargetShipImIn();
    if (count < 0) { Hint("Spawn or board a ship first."); return; }
    if (count == 0) { Hint("Waiting for the ship to load..."); return; }

    Section("Seats");
    PrettySeatNames(seats, count, ship, pretty);
    int sel = -1;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter
                                | ImGuiTableFlags_BordersInnerH;
    if (ImGui::BeginTable("##seats", 2, flags, ImVec2(0, 250))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Seat", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Occupant", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();
        for (int i = 0; i < count; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[96];
            snprintf(label, sizeof(label), "%s##seat%llu", pretty[i], seats[i].id);
            if (ImGui::Selectable(label, seats[i].id == selectedSeat, ImGuiSelectableFlags_SpanAllColumns)) selectedSeat = seats[i].id;
            ImGui::SetItemTooltip("%s", seats[i].name);
            if (seats[i].id == selectedSeat) sel = i;
            ImGui::TableNextColumn();
            switch (seats[i].state) {
            case SeatState_You:   ImGui::TextColored(kBlue, "You"); break;
            case SeatState_Npc:   ImGui::TextUnformatted("NPC"); break;
            case SeatState_Taken: ImGui::TextDisabled("Unknown"); break;
            default:              ImGui::TextDisabled("Empty"); break;
            }
        }
        ImGui::EndTable();
    }

    const int state = sel >= 0 ? seats[sel].state : -1;
    const float quarter = Columns(4);
    ImGui::BeginDisabled(sel < 0 || state == SeatState_You);
    if (UiButton("Sit here", ImVec2(quarter, 0))) Menu_RequestSit(seats[sel].id, replace);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state != SeatState_Npc && state != SeatState_You);
    if (UiButton("Stand up", ImVec2(quarter, 0))) Menu_RequestStandUp(seats[sel].id);
    ImGui::SetItemTooltip("Whoever is in the seat gets up and stays aboard.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state != SeatState_Npc);
    if (UiButton("Remove NPC", ImVec2(quarter, 0))) Menu_RequestKick(seats[sel].id);
    ImGui::SetItemTooltip("Takes the NPC out of the game.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state != SeatState_Empty);
    if (UiButton("Add NPC", ImVec2(quarter, 0))) Menu_RequestAddCrew(seats[sel].id, g_npcPick);
    ImGui::EndDisabled();
    UiToggle("If an NPC is in the seat I pick, remove it", &replace);

    Section("Crew");
    ImGui::TextUnformatted("NPC to add to seats");
    const bool haveNpcs = NpcPicker();
    const float third = Columns(3);
    ImGui::BeginDisabled(!haveNpcs);
    if (UiButton("Fill empty seats", ImVec2(third, 0))) Menu_RequestFillCrew(g_npcPick);
    Tip("NPCs you add sit in their seats. They don't fly the ship or operate turrets yet.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (UiButton("All NPCs stand up", ImVec2(third, 0))) Menu_RequestStandAll();
    ImGui::SameLine();
    if (UiButton("Remove all NPCs", ImVec2(third, 0))) Menu_RequestClearCrew();
}

// =============================================================================================
// NPCs
// =============================================================================================

static void DrawNpcsTab(bool& keepOpen) {
    Section("Spawn NPCs");
    if (!NpcPicker()) return;
    static int howMany = 1;
    ImGui::SetNextItemWidth(-1);
    UiSliderInt("##howMany", &howMany, 1, 10, howMany == 1 ? "1 NPC" : "%d NPCs");
    if (PrimaryButton("Spawn in front of me")) { Menu_RequestNpc(g_npcPick, howMany); keepOpen = false; }
    if (UiButton("Remove spawned NPCs", ImVec2(-1, 0))) Menu_RequestClearNpcs();
    Tip("Removes every NPC this menu has spawned, crew included.");
}

// =============================================================================================
// Build
// =============================================================================================

static void DrawBuildTab(bool& keepOpen) {
    static int  build = 0, buildTab = 0;
    static char buildFilter[64] = "";
    const int buildables = Menu_BuildCount();
    Section("Objects");
    if (buildables < 0) { Hint("Loading build objects (you need to be in the universe)..."); return; }
    if (buildables == 0) { Hint("No build objects found. Check data\\buildables.txt."); return; }
    if (build >= buildables) build = 0;
    if (ImGui::BeginTabBar("##buildTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int c = 0; c < Menu_BuildCategoryCount(); ++c) {
            char tab[40];
            snprintf(tab, sizeof(tab), "%s##cat%d", Menu_BuildCategoryName(c), c);
            if (tab[0] >= 'a' && tab[0] <= 'z') tab[0] -= 'a' - 'A';
            if (ImGui::BeginTabItem(tab)) { buildTab = c; ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }
    SearchBox("##buildFilter", "Search all objects", buildFilter, sizeof(buildFilter));
    const bool searching = buildFilter[strspn(buildFilter, " _")] != 0;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Hex(0x111113));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    const bool listOpen = ImGui::BeginChild("##buildList", ImVec2(0, 260), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (listOpen) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
        for (int i = 0; i < buildables; ++i) {
            const char* name = Menu_BuildName(i);
            if (searching ? !MatchesFilter(name, buildFilter) : Menu_BuildCategoryOf(i) != buildTab) continue;
            char label[160];
            PrettyBuildName(label, sizeof(label) - 16, name);
            if (searching) {
                char tagged[160];
                snprintf(tagged, sizeof(tagged), "%s   (%s)", label, Menu_BuildCategory(i));
                strcpy_s(label, sizeof(label) - 16, tagged);
            }
            snprintf(label + strlen(label), 16, "##%d", i);
            if (UiOption(label, i == build)) build = i;
            ImGui::SetItemTooltip("%s", name);
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    Section("Placing");
    char picked[128];
    PrettyBuildName(picked, sizeof(picked), Menu_BuildName(build));
    ImGui::Text("Selected: %s", picked);
    float reach = Menu_BuildReach();
    ImGui::SetNextItemWidth(-1);
    UiSliderFloat("##reach", &reach, 5.0f, 300.0f, "Reach %.0f m");
    ImGui::SetItemTooltip("How far ahead objects are placed. They land on the ground where you look, or under the point this far out.");
    Menu_SetBuild(build, reach);
    const bool building = Menu_BuildModeActive();
    if (PrimaryButton(building ? "Stop building (F6)" : "Start building (F6)")) {
        Menu_ToggleBuildMode();
        if (!building) keepOpen = false;
    }
    Tip("While building: left click places, R rotates, [ and ] change reach, Backspace undoes, F6 stops.");
    const float half = Columns(2);
    if (UiButton("Undo last", ImVec2(half, 0))) Menu_BuildUndo();
    ImGui::SameLine();
    char clearLabel[48];
    snprintf(clearLabel, sizeof(clearLabel), "Clear base (%d)###clearBase", Menu_BuildPlacedCount());
    if (UiButton(clearLabel, ImVec2(half, 0))) Menu_BuildClear();
}

static bool g_backToFirstTab = false;

static void DrawSq42Tab(bool& keepOpen) {
    static bool spoilerOk = false;
    if (!spoilerOk) {
        Section("Spoiler warning");
        ImGui::TextWrapped("This tab could have Squadron 42 spoilers.");
        ImGui::TextWrapped("Press OK to continue.");
        if (UiButton("OK", ImVec2(120, 0))) spoilerOk = true;
        ImGui::SameLine();
        if (UiButton("Back", ImVec2(120, 0))) g_backToFirstTab = true;
        return;
    }

    Section("Outfits");
    const int outfits = Menu_OutfitCount();
    static int outfit = 0;
    if (outfits < 0) {
        ImGui::TextWrapped("Loading outfits... (you need to be spawned in the universe)");
    } else if (outfits == 0) {
        ImGui::TextWrapped("No outfits found - check outfits.txt.");
    } else {
        static char filter[64] = "";
        if (outfit >= outfits) outfit = 0;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##outfitFilter", "search outfits...", filter, sizeof(filter));
        ImGui::SetNextItemWidth(-1);
        if (UiBeginCombo("##outfit", Menu_OutfitName(outfit), ImGuiComboFlags_HeightLargest)) {
            for (int i = 0; i < outfits; ++i) {
                const char* name = Menu_OutfitName(i);
                if (!MatchesFilter(name, filter)) continue;
                ImGui::PushID(i);
                if (UiOption(name, i == outfit)) outfit = i;
                if (i == outfit) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            UiEndCombo();
        }
        if (UiButton("Wear SQ42 outfit", ImVec2(-1, 0))) {
            Menu_RequestWearOutfit(outfit);
            keepOpen = false;
        }
    }
    static bool visor = false;
    if (UiToggle("SQ42 visor HUD (applies on the next Equip or outfit)", &visor))
        Menu_SetS42VisorHud(visor);

    Section("Settings");
    for (int i = 0; i < Menu_S42SettingCount(); ++i) {
        bool on = Menu_S42SettingOn(i);
        ImGui::PushID(i);
        const bool known = Menu_S42SettingKnown(i);   // greyed until the game thread has read the cvar
        ImGui::BeginDisabled(!known);
        if (UiToggle(Menu_S42SettingLabel(i), &on)) Menu_RequestS42Setting(i, on);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", known ? Menu_S42SettingTip(i) : "Reading this setting from the game...");
        ImGui::PopID();
    }

    Section("Spawn");
    {
        static int   thing = -1;
        static char  thingFilter[64] = "";
        static bool  inFront = true;
        static float ahead = 8.0f;
        const int buildables = Menu_BuildCount();
        if (buildables < 0) {
            ImGui::TextWrapped("Loading... (you need to be spawned in the universe)");
        } else if (buildables == 0) {
            ImGui::TextWrapped("No buildables found - check buildables.txt.");
        } else {
            if (thing < 0) {
                thing = 0;
                for (int i = 0; i < buildables; ++i)
                    if (_stricmp(Menu_BuildCategory(i), "sq42") == 0) { thing = i; break; }
            }
            if (thing >= buildables) thing = 0;
            UiToggle("Spawn in front of you", &inFront);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##thingFilter", "search the [sq42] group...", thingFilter, sizeof(thingFilter));
            ImGui::SetNextItemWidth(-1);
            if (UiBeginCombo("##sq42thing", Menu_BuildName(thing), ImGuiComboFlags_HeightLargest)) {
                for (int i = 0; i < buildables; ++i) {
                    // Only the [sq42] group, like the original; the text filter narrows it further.
                    if (_stricmp(Menu_BuildCategory(i), "sq42") != 0) continue;
                    const char* name = Menu_BuildName(i);
                    if (!MatchesFilter(name, thingFilter)) continue;
                    ImGui::PushID(i);
                    if (UiOption(name, i == thing)) thing = i;
                    if (i == thing) ImGui::SetItemDefaultFocus();
                    ImGui::PopID();
                }
                UiEndCombo();
            }
            if (inFront) {
                ImGui::SetNextItemWidth(-1);
                UiSliderFloat("##ahead", &ahead, 1.0f, 50.0f, "Ahead %.0f m");
            }
            if (UiButton("Spawn it", ImVec2(-1, 0))) {
                Menu_RequestPlace(thing, inFront, ahead);
                keepOpen = false;
            }
            ImGui::SetItemTooltip("Undo and Clear base in the build section remove these too.");
        }
    }

    Section("Ships");
    struct Entry { const char* label; const char* cls; bool enemyWing; float height; bool sit; };
    static const struct { const char* label; const char* cls; } kSq42Ships[] = {
        { "Idris-P (the Stanton's class)", "AEGS_Idris_P" },
        { "Gladius (SQ42 fighter)",        "AEGS_Gladius" },
        { "Retaliator (has an S42 HUD)",   "AEGS_Retaliator" },
        { "Starfarer (ch 5, 7, 9)",        "MISC_Starfarer" },
        { "Avenger Stalker (S42 wreck)",   "AEGS_Avenger_Stalker" },
        { "Hornet (Cal Mason's ship)",     "ANVL_Hornet_F7C" },
        { "Vanduul Blade (AI)",            "VNCL_Blade_PU_AI_VAN" },
        { "Vanduul Scythe (AI)",           "VNCL_Scythe_PU_AI_VAN" },
        { "Vanduul Glaive (AI)",           "VNCL_Glaive_PU_AI_VAN" },
        { "Vanduul Stinger (AI)",          "VNCL_Stinger_PU_AI_VAN" },
    };
    Entry list[16];
    static_assert(sizeof(kSq42Ships) / sizeof(kSq42Ships[0]) + 2 <= sizeof(list) / sizeof(list[0]),
                  "SQ42 ship table outgrew Entry list[]");
    int   n = 0;
    for (size_t i = 0; i < sizeof(kSq42Ships) / sizeof(kSq42Ships[0]); ++i)
        list[n++] = { kSq42Ships[i].label, kSq42Ships[i].cls, false, 0.0f, true };
    for (int i = 0; i < n; ++i)
        if (strncmp(list[i].cls, "VNCL_", 5) == 0) { list[i].height = 300.0f; list[i].sit = false; }

    // Not the original's dormant "[battle]" mode (two Bengals fighting each other): these
    // spawn one UEE Bengal, the second with a Vanduul wing when an enemy side was found.
    static char bengalWing[64];
    const bool enemySide = Menu_EnemySideAvailable();
    strcpy_s(bengalWing, enemySide ? "Bengal + Vanduul wing" : "Bengal + wing (UEE, no enemy side found)");
    list[n++] = { "Bengal (UEE)", "RSI_Bengal_PU_AI_UEE", false,      1500.0f, false };
    list[n++] = { bengalWing,     "RSI_Bengal_PU_AI_UEE", enemySide, 1500.0f, false };

    static int   pick = 0;
    static char  sqFilter[64] = "";
    static float height = 30.0f;   // the original's fixed spawn height for your own ships
    static bool  sit = true;
    if (pick >= n) pick = 0;
    // Greys out classes this game build doesn't have, like the original, instead of failing
    // after the click with "unknown entity class".
    auto known = [](const char* cls) {
        const int count = Menu_ShipCount();
        if (count < 0) return true;   // ship list still loading: don't block
        const MenuShip* ships = Menu_Ships();
        for (int i = 0; i < count; ++i)
            if (_stricmp(ships[i].name, cls) == 0) return true;
        return false;
    };

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##sq42shipFilter", "search ships...", sqFilter, sizeof(sqFilter));
    ImGui::SetNextItemWidth(-1);
    if (UiBeginCombo("##sq42ships", list[pick].label, ImGuiComboFlags_HeightLargest)) {
        for (int i = 0; i < n; ++i) {
            if (sqFilter[0] && !MatchesFilter(list[i].label, sqFilter)) continue;
            ImGui::PushID(i);
            const bool have = known(list[i].cls);
            if (UiOption(list[i].label, i == pick, have)) pick = i;
            if (!have) ImGui::SetItemTooltip("%s isn't in this game build's ship list.", list[i].cls);
            if (i == pick) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        UiEndCombo();
    }
    const bool pickKnown = known(list[pick].cls);
    if (list[pick].sit) {
        ImGui::SetNextItemWidth(-1);
        UiSliderFloat("##sq42height", &height, 0.0f, 500.0f, "Height above me %.0f m");
        UiToggle("Put me in the pilot seat", &sit);
    }
    ImGui::BeginDisabled(!pickKnown);
    const bool spawnClicked = PrimaryButton("Spawn", 42);
    Tip("Your ships put you in the pilot seat; the Vanduul ones spawn 300 m up and come for you.");
    ImGui::EndDisabled();
    if (spawnClicked) {
        Menu_RequestSpawnClass(list[pick].cls,
                               list[pick].sit ? height : list[pick].height,
                               list[pick].sit && sit, list[pick].sit && sit,
                               list[pick].enemyWing);
        keepOpen = false;
    }

    Section("Console");
    static char cmd[256] = "";
    const bool consoleReady = Menu_ConsoleReady();
    ImGui::BeginDisabled(!consoleReady);
    const float runWidth = ImGui::CalcTextSize("Run").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(-(runWidth + ImGui::GetStyle().ItemSpacing.x));
    bool run = ImGui::InputTextWithHint("##console",
                                        "a console command, e.g. i_target_selector.targeting2_enabled 1",
                                        cmd, sizeof(cmd), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    run |= UiButton("Run");
    Tip("Runs in the game's own console. What it did shows in the game's log, not here.");
    ImGui::EndDisabled();
    if (run && cmd[0]) {
        Menu_RunConsole(cmd);
        cmd[0] = 0;
    }
    if (!consoleReady) ImGui::TextDisabled("The game's console wasn't found yet.");
}

static void CardFrame(ImVec2 lo, ImVec2 hi, Icon icon, const char* title) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lo, hi, Col(kCard), 10);
    ImGui::PushFont(g_bold, 0.0f);
    const float cy = lo.y + 22;
    DrawIcon(icon, ImVec2(lo.x + 24, cy), 7, Col(kBlue));
    dl->AddText(ImVec2(lo.x + 42, cy - ImGui::GetFontSize() * 0.5f), Col(kText), title);
    ImGui::PopFont();
    dl->AddLine(ImVec2(lo.x + 14, lo.y + 44), ImVec2(hi.x - 14, lo.y + 44), Col(kLine), 1.0f);
}

static void NavRows(float x, float y, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rh = 36, step = 40;
    const float hy = Anim(ImGui::GetID("##navpick"), y + float(g_page) * step, 20.0f);
    dl->AddRectFilled(ImVec2(x, hy), ImVec2(x + w, hy + rh), Col(kRow), 8);
    for (int i = 0; i < kPageCount; ++i) {
        const ImVec2 p(x, y + float(i) * step);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##nav", ImVec2(w, rh)) && g_page != i) { g_page = i; g_pageSince = ImGui::GetTime(); }
        const float h = Anim(ImGui::GetID("##hot"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
        const float on = Anim(ImGui::GetID("##on"), g_page == i ? 1.0f : 0.0f);
        ImGui::PopID();
        DrawIcon(kPages[i].icon, ImVec2(p.x + 20, p.y + rh * 0.5f), 7, Col(Mix(Mix(kFaint, kMuted, h), kBlue, on)));
        dl->AddText(ImVec2(p.x + 40, p.y + (rh - ImGui::GetFontSize()) * 0.5f), Col(Mix(Mix(kMuted, kBody, h), kText, on)), kPages[i].name);
    }
}

static bool DrawMenu() {
    bool keepOpen = true;
    if (g_backToFirstTab) { g_page = 0; g_pageSince = ImGui::GetTime(); g_backToFirstTab = false; }
    DrawBackdrop();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                 | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double now = ImGui::GetTime();
    const float open = EaseOut(float((now - g_openSince) / 0.25));
    const float slide = (1.0f - open) * 8.0f;
    const float top = 46, pad = 16, navW = 196, gap = 14;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, open);

    ImGui::PushFont(g_bold, 0.0f);
    {
        const float y = (top - ImGui::GetFontSize()) * 0.5f;
        float x = pad + 4;
        dl->AddText(ImVec2(x, y), Col(kBlue), "fork offline");
        x += ImGui::CalcTextSize("fork offline").x;
        dl->AddText(ImVec2(x, y), Col(kFaint), "  -  ");
        x += ImGui::CalcTextSize("  -  ").x;
        const ImVec2 ts = ImGui::CalcTextSize(kDiscordText);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ImGui::InvisibleButton("##discord", ts)) g_openDiscord = true;
        const bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float h = Anim(ImGui::GetID("##discord"), hot ? 1.0f : 0.0f);
        dl->AddText(ImVec2(x, y), Col(Mix(kText, kBlue, h)), kDiscordText);
        dl->AddLine(ImVec2(x, y + ts.y), ImVec2(x + ts.x, y + ts.y), Col(kBlue, h), 1.0f);
    }
    ImGui::PopFont();
    {
        const float s = 28;
        const ImVec2 p(size.x - 10 - s, 9);
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("##close", ImVec2(s, s))) keepOpen = false;
        const float h = Anim(ImGui::GetID("##close"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
        dl->AddRectFilled(p, p + ImVec2(s, s), Col(kRed, h), 6);
        const ImVec2 c = p + ImVec2(s, s) * 0.5f;
        const ImU32 ic = Col(Mix(kBody, kText, h));
        dl->AddLine(c - ImVec2(5, 5), c + ImVec2(5, 5), ic, 1.4f);
        dl->AddLine(c + ImVec2(-5, 5), c + ImVec2(5, -5), ic, 1.4f);
    }

    const float y0 = top + slide, y1 = size.y - pad + slide;
    CardFrame(ImVec2(pad, y0), ImVec2(pad + navW, y1), kIcoMark, "Menu");
    NavRows(pad + 10, y0 + 54, navW - 20);

    const float px = pad + navW + gap;
    const float pf = EaseOut(float((now - g_pageSince) / 0.22));
    ImGui::SetCursorScreenPos(ImVec2(px, y0 + (1.0f - pf) * 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, open * pf);
    char id[16];
    snprintf(id, sizeof(id), "##page%d", g_page);
    if (ImGui::BeginChild(id, ImVec2(size.x - px - pad, y1 - y0))) {
        g_cardIndex = 0;
        switch (g_page) {
        case 0: DrawPlayerTab(keepOpen); break;
        case 1: DrawTravelTab(); break;
        case 2: DrawVehiclesTab(keepOpen); break;
        case 3: DrawCrewTab(); break;
        case 4: DrawNpcsTab(keepOpen); break;
        case 5: DrawBuildTab(keepOpen); break;
        case 6: DrawSq42Tab(keepOpen); break;
        }
        CloseCard();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    ImGui::GetForegroundDrawList()->AddRect(ImVec2(0, 0), size, ImGui::GetColorU32(kLine), 0, 0, 1.0f);
    ImGui::End();
    return keepOpen;
}

static DWORD WINAPI MenuThread(LPVOID) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MenuWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"starcitzenofflinemods_menu";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"fork offline", WS_POPUP,
                            100, 100, kMenuW, kMenuH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_wnd || !CreateDevice()) return 0;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().MouseDrawCursor = true;
    ImGui::StyleColorsDark();
    ApplyTheme();
    LoadFonts();
    ImGui_ImplWin32_Init(g_wnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadBackground();
    if (SUCCEEDED(com)) CoUninitialize();

    bool visible = false, wasDown = false;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const bool down = (GetAsyncKeyState('M') & 0x8000) != 0;
        const bool typing = visible && GetForegroundWindow() == g_wnd && ImGui::GetIO().WantTextInput;
        if (down && !wasDown && !typing && OurProcessHasFocus()) { visible = !visible; ShowMenu(visible); }
        wasDown = down;
        if (visible && !IsWindowVisible(g_wnd)) { visible = false; ClipCursor(nullptr); g_clipped = false; }
        if (!visible) { Sleep(50); continue; }
        KeepCursorInMenu();

        if (g_resizeW && g_resizeH) {
            ReleaseRenderTarget();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const bool keepOpen = DrawMenu();
        ImGui::Render();
        const float clear[4] = { kBg.x, kBg.y, kBg.z, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);

        if (g_openDiscord) {
            g_openDiscord = false;
            visible = false;
            ClipCursor(nullptr);
            g_clipped = false;
            ShowWindow(g_wnd, SW_HIDE);
            ShellExecuteW(nullptr, L"open", kDiscordUrl, nullptr, nullptr, SW_SHOWNORMAL);
        } else if (!keepOpen) { visible = false; ShowMenu(false); }
    }
}

void Menu_Start(HWND gameWindow) {
    g_game = gameWindow;
    if (HANDLE t = CreateThread(nullptr, 0, MenuThread, nullptr, 0, nullptr)) CloseHandle(t);
}
