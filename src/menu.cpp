// The menu: its own window and D3D11 device, the ImGui frame, and a shell that lists the tabs,
// badges and overlays registered through sco-core's sco.ui service (sco/ui.h). The features'
// pages are drawn by their built-in plugins (src/builtins/*_ui.cpp); this file keeps the window,
// the look, the tab bar, the product's own Menu tab, the status strip and the shared widgets
// (menu_ui.h).
#include "menu.h"
#include "menu_imgui.h"
#include "menu_ui.h"
#include "common.h"
#include "version.h"
#include <atomic>
#include <d3d11.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "sco/ui.h"
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_internal.h"
#include "third_party/imgui/imgui_impl_win32.h"
#include "third_party/imgui/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND                    g_game;
static HWND                    g_wnd;
static ID3D11Device*           g_device;
static ID3D11DeviceContext*    g_context;
static IDXGISwapChain*         g_swap;
static ID3D11RenderTargetView* g_rtv;
static UINT                    g_resizeW, g_resizeH;

constexpr int kMenuW = 600, kMenuH = 900;

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
static RECT g_clipRect = {};   // last rect we clipped to, so we don't re-clip every frame

static void KeepCursorInMenu() {
    // ClipCursor is a cross-process, synchronous input call; doing it every frame while the menu
    // has foreground is enough to starve the game's own message thread. Only re-clip when the
    // menu window actually moves or resizes.
    if (GetForegroundWindow() != g_wnd) {
        if (g_clipped) { ClipCursor(nullptr); g_clipped = false; }
        return;
    }
    RECT r = {};
    GetWindowRect(g_wnd, &r);
    if (g_clipped && EqualRect(&r, &g_clipRect)) return;
    g_clipRect = r;
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

bool MatchesFilter(const char* name, const char* filter) {
    for (const char* p = filter; *p; ) {
        p += strspn(p, " _");
        const size_t n = strcspn(p, " _");
        if (n && !ContainsNoCase(name, p, n)) return false;
        p += n;
    }
    return true;
}

// =============================================================================================
// Look: dark Tegridy green. Soil-green behind everything, leaf green only on things you click,
// tractor yellow for warnings. An optional background image sits under a
// green wash so text stays readable.
// =============================================================================================

static ImVec4 Hex(uint32_t rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

static const ImVec4 kSoil      = Hex(0x0E1811);   // deepest green, under the image
static const ImVec4 kPanel     = Hex(0x172A1B);   // inputs, lists, status strip
static const ImVec4 kPanelUp   = Hex(0x1F3523);   // hover
static const ImVec4 kLine      = Hex(0x2F4B32);   // dividers, scrollbars
static const ImVec4 kText      = Hex(0xE6EFDD);
const ImVec4 kMuted     = Hex(0x9FB59A);
const ImVec4 kLeaf      = Hex(0x86C64B);   // accent
const ImVec4 kLeafDeep  = Hex(0x3E6B27);

constexpr float kBodySize    = 17.0f;
constexpr float kHeadingSize = 20.0f;

static void ApplyTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(16, 12);
    st.FramePadding = ImVec2(9, 5);
    st.ItemSpacing = ImVec2(8, 7);
    st.ItemInnerSpacing = ImVec2(6, 4);
    st.CellPadding = ImVec2(8, 4);
    st.ScrollbarSize = 11;
    st.WindowRounding = 0;
    st.ChildRounding = 4;
    st.FrameRounding = 4;
    st.PopupRounding = 4;
    st.GrabRounding = 4;
    st.TabRounding = 4;
    st.ScrollbarRounding = 4;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.FrameBorderSize = 0;
    st.TabBorderSize = 0;
    st.SeparatorTextBorderSize = 1;
    st.SeparatorTextPadding = ImVec2(0, 6);
    st.SeparatorTextAlign = ImVec2(0, 0.5f);

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text]                 = kText;
    c[ImGuiCol_TextDisabled]         = kMuted;
    c[ImGuiCol_WindowBg]             = ImVec4(0, 0, 0, 0);          // the background is drawn underneath
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = Hex(0x132216, 0.98f);
    c[ImGuiCol_Border]               = kLine;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = Hex(0x172A1B, 0.88f);
    c[ImGuiCol_FrameBgHovered]       = Hex(0x1F3523, 0.95f);
    c[ImGuiCol_FrameBgActive]        = Hex(0x26412A, 1.0f);
    c[ImGuiCol_TitleBg]              = Hex(0x0B140D, 0.95f);
    c[ImGuiCol_TitleBgActive]        = Hex(0x0B140D, 0.95f);
    c[ImGuiCol_TitleBgCollapsed]     = Hex(0x0B140D, 0.95f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = kLine;
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(0x3C5E3F);
    c[ImGuiCol_ScrollbarGrabActive]  = kLeafDeep;
    c[ImGuiCol_CheckMark]            = kLeaf;
    c[ImGuiCol_SliderGrab]           = kLeafDeep;
    c[ImGuiCol_SliderGrabActive]     = kLeaf;
    c[ImGuiCol_Button]               = Hex(0x1F3523, 0.92f);
    c[ImGuiCol_ButtonHovered]        = Hex(0x2A4730);
    c[ImGuiCol_ButtonActive]         = kLeafDeep;
    c[ImGuiCol_Header]               = Hex(0x3E6B27, 0.55f);
    c[ImGuiCol_HeaderHovered]        = Hex(0x1F3523, 0.95f);
    c[ImGuiCol_HeaderActive]         = kLeafDeep;
    c[ImGuiCol_Separator]            = kLine;
    c[ImGuiCol_SeparatorHovered]     = kLeafDeep;
    c[ImGuiCol_SeparatorActive]      = kLeaf;
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab]                  = Hex(0x0B140D, 0.70f);
    c[ImGuiCol_TabHovered]           = kPanelUp;
    c[ImGuiCol_TabSelected]          = Hex(0x172A1B, 0.95f);
    c[ImGuiCol_TabSelectedOverline]  = kLeaf;
    c[ImGuiCol_TabDimmed]            = Hex(0x0B140D, 0.70f);
    c[ImGuiCol_TabDimmedSelected]    = Hex(0x172A1B, 0.95f);
    c[ImGuiCol_TableHeaderBg]        = Hex(0x172A1B, 0.95f);
    c[ImGuiCol_TableBorderStrong]    = kLine;
    c[ImGuiCol_TableBorderLight]     = Hex(0x2F4B32, 0.6f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = Hex(0x172A1B, 0.35f);
    c[ImGuiCol_TextSelectedBg]       = Hex(0x3E6B27, 0.7f);
    c[ImGuiCol_NavCursor]            = kLeaf;
}

// Bahnschrift (DIN-style, ships with Windows 10+), then Segoe UI, then ImGui's own font.
static void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    char dir[MAX_PATH] = "C:\\Windows";
    GetWindowsDirectoryA(dir, sizeof(dir));
    static const char* const kFonts[] = { "bahnschrift.ttf", "segoeui.ttf" };
    ImFont* font = nullptr;
    for (const char* name : kFonts) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\Fonts\\%s", dir, name);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        if ((font = io.Fonts->AddFontFromFileTTF(path, kBodySize)) != nullptr) break;
    }
    ImGuiStyle& st = ImGui::GetStyle();
    st.FontSizeBase = font ? kBodySize : 13.0f;
    st.FontScaleMain = font ? 1.0f : 1.3f;
}

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
    dl->AddRectFilled(ImVec2(0, 0), size, ImGui::GetColorU32(kSoil));
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
    dl->AddRectFilled(ImVec2(0, 0), size, ImGui::GetColorU32(ImVec4(kSoil.x, kSoil.y, kSoil.z, g_bgDarkness / 100.0f)));
}

// --- small building blocks ------------------------------------------------------------------

void SectionHeading(const char* title) {
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushFont(nullptr, kHeadingSize);
    ImGui::SeparatorText(title);
    ImGui::PopFont();
}

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

bool PrimaryButton(const char* label, float height) {
    ImGui::PushStyleColor(ImGuiCol_Button, kLeafDeep);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0x4F8732));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kLeaf);
    const bool pressed = ImGui::Button(label, ImVec2(-1, height));
    ImGui::PopStyleColor(3);
    return pressed;
}

float Columns(int n) {
    return (ImGui::GetContentRegionAvail().x - (n - 1) * ImGui::GetStyle().ItemSpacing.x) / n;
}

bool SearchBox(const char* id, const char* hint, char* buf, size_t n) {
    ImGui::SetNextItemWidth(-1);
    return ImGui::InputTextWithHint(id, hint, buf, n);
}

void PrettyBuildName(char* out, size_t n, const char* name) {
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

// =============================================================================================
// Menu settings
// =============================================================================================

static void DrawMenuTab() {
    SectionHeading("Background");
    if (g_bgSrv) {
        ImGui::Checkbox("Show background image", &g_bgShow);
        ImGui::BeginDisabled(!g_bgShow);
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##dark", &g_bgDarkness, 20, 95, "Darkness %d%%");
        if (static_cast<float>(g_bgW) / g_bgH > 0.7f) {
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##pos", &g_bgPosition, 0.0f, 1.0f, "Image position");
            ImGui::SetItemTooltip("The menu is taller than the picture is wide. Slide to choose which part shows.");
        }
        ImGui::EndDisabled();
        Hint(g_bgPath);
    } else {
        Hint("To use a background image, save it as menu_background.png (or .jpg) here, then restart the game:");
        ImGui::TextWrapped("%s", g_bgPath[0] ? g_bgPath : "data\\menu_background.png");
    }

    SectionHeading("About");
    Hint(SCO_TITLE " is a work in progress, " SCO_BASED_ON ".");
    Hint("Bug reports: github.com/scubamount/sc-offline/issues");
}

// =============================================================================================
// Window
// =============================================================================================

static void DrawStatusStrip(float height) {
    char status[256];
    Menu_GetStatus(status, sizeof(status));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Hex(0x0B140D, 0.92f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 8));
    ImGui::BeginChild("##status", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    const ImVec2 p = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 4, p.y + height), ImGui::GetColorU32(kLeaf));
    ImGui::TextWrapped("%s", status);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// =============================================================================================
// The shell: sco.ui's tabs in order, then the Menu tab; overlays after the window
// =============================================================================================

static bool g_closeRequested = false;   // MenuClose(), during a frame
static bool g_backToFirstTab = false;   // MenuSelectFirstTab(), applied in the next frame

void MenuClose() { g_closeRequested = true; }
void MenuSelectFirstTab() { g_backToFirstTab = true; }

// One tab's page, drawn by its plugin as a callout under sco-core's crash guard. ImGui's error
// recovery puts its stacks back where they were before the call, so a draw that faulted half-way
// (sco-core then disables its plugin) or forgot an End() can't unbalance the menu's own.
static void DrawPluginTab(const sco::ui::TabInfo& tab, void* frame) {
    ImGuiErrorRecoveryState state;
    ImGui::ErrorRecoveryStoreState(&state);
    const sco::Result r = sco::ui::DrawTab(tab.id.c_str(), frame);
    ImGui::ErrorRecoveryTryToRecoverState(&state);
    if (r == sco::Result::Crashed) Log("[menu] tab %s (%s) crashed; plugin '%s' is disabled", tab.id.c_str(), tab.title.c_str(), tab.owner.c_str());
}

static void DrawPluginOverlays(void* frame) {
    ImGuiErrorRecoveryState state;
    ImGui::ErrorRecoveryStoreState(&state);
    sco::ui::DrawOverlays(frame);
    ImGui::ErrorRecoveryTryToRecoverState(&state);
}

static bool DrawMenu() {
    bool keepOpen = true;
    g_closeRequested = false;
    void* const frame = ImGui::GetCurrentContext();
    DrawBackdrop();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin(SCO_TITLE "###main", &keepOpen,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                 | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);

    const float statusHeight = ImGui::GetTextLineHeight() * 2 + 18;
    const float bodyHeight = -(statusHeight + ImGui::GetStyle().ItemSpacing.y);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
        auto body = [&](auto draw) {
            if (ImGui::BeginChild("##body", ImVec2(0, bodyHeight))) draw();
            ImGui::EndChild();
        };
        bool first = g_backToFirstTab;
        g_backToFirstTab = false;
        // By sco.ui order, ties in registration order. "###id" keeps a tab selected while its
        // badge changes.
        for (const sco::ui::TabInfo& t : sco::ui::Tabs()) {
            std::string label = t.badge.empty() ? t.title : t.title + " (" + t.badge + ")";
            label += "###" + t.id;
            const ImGuiTabItemFlags flags = first ? ImGuiTabItemFlags_SetSelected : 0;
            first = false;
            if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) { body([&] { DrawPluginTab(t, frame); }); ImGui::EndTabItem(); }
        }
        if (ImGui::BeginTabItem("Menu", nullptr, first ? ImGuiTabItemFlags_SetSelected : 0)) { body([&] { DrawMenuTab(); }); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    DrawStatusStrip(statusHeight);
    ImGui::End();
    // sc-offline has no surface over the game itself: overlays draw over the menu while it's open.
    DrawPluginOverlays(frame);
    return keepOpen && !g_closeRequested;
}

// =============================================================================================
// The frame, built on the game thread
// =============================================================================================
//
// sco.ui calls draw functions on the game thread only (a fault in one releases its plugin there),
// while this window and its D3D11 device belong to the menu thread. So the menu thread feeds ImGui
// (window messages, the Win32 and DX11 backends' NewFrame) and renders the draw data, and the game
// thread builds the frame in between: NewFrame, the shell with every tab, Render. They never use
// ImGui at the same time: the menu thread waits while the game thread builds, and a request the
// game thread hasn't started within kFrameWaitMs (a loading screen) is withdrawn, so the menu keeps
// its last picture until the game thread is back.

enum : LONG { Frame_Idle, Frame_Wanted, Frame_Building, Frame_Built };
static volatile LONG g_frameState = Frame_Idle;
static HANDLE        g_frameBuilt = nullptr;   // auto-reset: the game thread finished a frame
static bool          g_frameKeepOpen = true;   // DrawMenu's answer for the last frame
static std::atomic<bool> g_typing{ false };   // the menu has focus and a text box is active
constexpr DWORD kFrameWaitMs = 250;
// Perf probe (0.7.2 diagnostic); see BuildFrameOnGameThread.
static ULONGLONG g_perfMark = 0;
static int       g_perfFrames = 0;
static int       g_perfWithdrawn = 0;
static double    g_perfSumMs = 0.0, g_perfMaxMs = 0.0;

void Menu_GameThreadFrame() {
    if (InterlockedCompareExchange(&g_frameState, Frame_Building, Frame_Wanted) != Frame_Wanted) return;
    ImGui::NewFrame();
    g_frameKeepOpen = DrawMenu();
    ImGui::Render();
    InterlockedExchange(&g_frameState, Frame_Built);
    SetEvent(g_frameBuilt);
}

bool Menu_Typing() { return g_typing.load(); }

// Menu thread. True: the game thread built a frame. False: it didn't start one in time, and the
// request was withdrawn.
static bool BuildFrameOnGameThread() {
    const ULONGLONG t0 = GetTickCount64();
    InterlockedExchange(&g_frameState, Frame_Wanted);
    PostMessageW(g_game, WM_NULL, 0, 0);   // the message hook runs on the game's next message
    if (WaitForSingleObject(g_frameBuilt, kFrameWaitMs) != WAIT_OBJECT_0) {
        if (InterlockedCompareExchange(&g_frameState, Frame_Idle, Frame_Wanted) == Frame_Wanted) return false;
        WaitForSingleObject(g_frameBuilt, INFINITE);   // it has started: let it finish
    }
    InterlockedExchange(&g_frameState, Frame_Idle);
    // Perf probe (0.7.2 diagnostic): how long each game-thread frame build costs, summarized once
    // a second so this can't itself become the log spam it's meant to catch. If fps.ms climbs over
    // a few seconds the cost is in a tab's draw; if it's flat but fps.hz is low the cost is the
    // build interrupt itself.
    const double ms = static_cast<double>(GetTickCount64() - t0);
    g_perfFrames += 1;
    if (ms > g_perfMaxMs) g_perfMaxMs = ms;
    g_perfSumMs += ms;
    g_perfWithdrawn += (ms >= kFrameWaitMs) ? 1 : 0;
    const ULONGLONG now = GetTickCount64();
    if (now - g_perfMark >= 1000) {
        Log("[menu] perf: %.1f fps | build avg %.2f ms, max %.2f ms | withdrawn %d/s",
            g_perfFrames * 1000.0 / (now - g_perfMark), g_perfSumMs / g_perfFrames, g_perfMaxMs, g_perfWithdrawn);
        g_perfFrames = 0; g_perfSumMs = 0; g_perfMaxMs = 0; g_perfWithdrawn = 0; g_perfMark = now;
    }
    return true;
}

static DWORD WINAPI MenuThread(LPVOID) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MenuWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"starcitzenofflinemods_menu";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"sc-offline", WS_POPUP,
                            100, 100, kMenuW, kMenuH, nullptr, nullptr, wc.hInstance, nullptr);
    g_frameBuilt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wnd || !g_frameBuilt || !CreateDevice()) return 0;

    IMGUI_CHECKVERSION();
    // The allocator plugins that draw with ImGui use too (menu_imgui.h).
    ImGui::SetAllocatorFunctions(sc_offline_imgui::Alloc, sc_offline_imgui::Free);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().MouseDrawCursor = true;
    // A plugin's draw that faults or leaves a Begin open is recovered from (DrawPluginTab), quietly.
    ImGui::GetIO().ConfigErrorRecoveryEnableAssert = false;
    ImGui::GetIO().ConfigErrorRecoveryEnableTooltip = false;
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
        // M is the product's own key (reserved in sco.ui): plugins can't bind it.
        const bool down = (GetAsyncKeyState('M') & 0x8000) != 0;
        const bool typing = visible && GetForegroundWindow() == g_wnd && ImGui::GetIO().WantTextInput;
        g_typing = typing;
        if (down && !wasDown && !typing && OurProcessHasFocus()) { visible = !visible; ShowMenu(visible); }
        wasDown = down;
        if (visible && !IsWindowVisible(g_wnd)) { visible = false; ClipCursor(nullptr); g_clipped = false; }
        if (!visible) { g_typing = false; Sleep(50); continue; }
        KeepCursorInMenu();
        // Visible but not focused: freeze the picture and stop interrupting the game thread to
        // rebuild it, so the game runs at full speed. The menu resumes the moment it regains focus.
        if (GetForegroundWindow() != g_wnd) { Sleep(50); continue; }

        if (g_resizeW && g_resizeH) {
            ReleaseRenderTarget();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (!BuildFrameOnGameThread()) continue;
        const bool keepOpen = g_frameKeepOpen;
        const float clear[4] = { kSoil.x, kSoil.y, kSoil.z, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);

        if (!keepOpen) { visible = false; ShowMenu(false); }
    }
}

void Menu_Start(HWND gameWindow) {
    g_game = gameWindow;
    if (HANDLE t = CreateThread(nullptr, 0, MenuThread, nullptr, 0, nullptr)) CloseHandle(t);
}
