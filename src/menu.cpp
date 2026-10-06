#include "menu.h"
#include "common.h"
#include "travel.h"
#include "version.h"
#include <algorithm>
#include <d3d11.h>
#include <wincodec.h>
#include <vector>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "spawner.h"
#include "cvars.h"
#include "build.h"
#include "third_party/imgui/imgui.h"
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
static const ImVec4 kMuted     = Hex(0x9FB59A);
static const ImVec4 kLeaf      = Hex(0x86C64B);   // accent
static const ImVec4 kLeafDeep  = Hex(0x3E6B27);

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

static void Section(const char* title) {
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushFont(nullptr, kHeadingSize);
    ImGui::SeparatorText(title);
    ImGui::PopFont();
}

static void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static bool PrimaryButton(const char* label, float height = 36.0f) {
    ImGui::PushStyleColor(ImGuiCol_Button, kLeafDeep);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0x4F8732));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kLeaf);
    const bool pressed = ImGui::Button(label, ImVec2(-1, height));
    ImGui::PopStyleColor(3);
    return pressed;
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
    if (ImGui::BeginCombo("##npc", preview, ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < npcs; ++i) {
            const char* name = Menu_NpcName(i);
            if (!MatchesFilter(name, filter)) continue;
            char label[160];
            PrettyBuildName(label, sizeof(label) - 16, name);
            snprintf(label + strlen(label), 16, "##n%d", i);
            if (ImGui::Selectable(label, i == g_npcPick)) g_npcPick = i;
            ImGui::SetItemTooltip("%s", name);
            if (i == g_npcPick) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
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
    if (!ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge)) return;
    if (ImGui::Selectable(none, pick < 0)) pick = -1;
    for (int i = 0; i < n; ++i) {
        const char* name = Menu_GearName(slot, i);
        if (!MatchesFilter(name, filter)) continue;
        ImGui::PushID(i);
        if (ImGui::Selectable(name, i == pick)) pick = i;
        if (i == pick) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

static void DrawPlayerTab(bool& keepOpen) {
    Section("Movement");
    static bool  noclip = false;
    static float speed = 30.0f;
    if (ImGui::Checkbox("Noclip", &noclip)) Menu_SetNoclip(noclip, speed);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##noclipSpeed", &speed, 1.0f, 500.0f, "Speed %.0f", ImGuiSliderFlags_Logarithmic))
        Menu_SetNoclipSpeed(speed);
    Hint("F7 saves where you're standing and F8 takes you back. The Travel tab has named spots and places.");

    Section("Protection");
    static bool god = true, ammo = false;
    if (ImGui::Checkbox("God mode", &god)) Menu_SetGodMode(god);
    ImGui::SameLine(0, 24);
    if (ImGui::Checkbox("Infinite ammo", &ammo)) Menu_SetInfiniteAmmo(ammo);

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
    ImGui::SliderFloat("##altitude", &altitude, 100.0f, 20000.0f, "Arrive %.0f m above the ground", ImGuiSliderFlags_Logarithmic);
    ImGui::Checkbox("Show interiors and small zones", &showMinor);
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
        if (searching) ImGui::SetNextItemOpen(true);
        if (!ImGui::CollapsingHeader(header, isHere ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
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
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kLeafDeep);
        ImGui::ProgressBar(progress, ImVec2(-1, 0), "Scanning...");
        ImGui::PopStyleColor();
    } else if (ImGui::Button("Scan the game for places", ImVec2(-1, 0))) {
        Travel_RequestScan();
    }
    Hint("The scan finds the planets, moons, stations, Lagrange points, comm arrays and jump points of every loaded "
         "system and adds them here. It only reads; it takes a few seconds.");

    Section("Saved spots");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 160 - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##markName", "Name this spot", markName, sizeof(markName));
    ImGui::SameLine();
    if (ImGui::Button("Save this spot", ImVec2(160, 0))) { Travel_RequestSaveBookmark(markName); markName[0] = 0; }
    if (!nm) Hint("Nothing saved yet. Stand somewhere, name it, and press Save this spot.");
    for (int s = 0; s < ns; ++s) {
        int shown = 0;
        for (int i = 0; i < nm; ++i)
            if (_stricmp(marks[i].system, systems[s]) == 0 && (!searching || MatchesFilter(marks[i].name, filter))) ++shown;
        if (!shown) continue;
        char node[80];
        snprintf(node, sizeof(node), "%s (%d)###marks_%s", systems[s], shown, systems[s]);
        if (searching) ImGui::SetNextItemOpen(true);
        if (!ImGui::TreeNodeEx(node, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) continue;
        for (int i = 0; i < nm; ++i) {
            if (_stricmp(marks[i].system, systems[s]) != 0 || (searching && !MatchesFilter(marks[i].name, filter))) continue;
            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(marks[i].name);
            const float buttons = 70 + 80 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - buttons);
            if (ImGui::Button("Go", ImVec2(70, 0))) Travel_RequestBookmark(i);
            ImGui::SameLine();
            if (ImGui::Button("Delete", ImVec2(80, 0))) Travel_RequestDeleteBookmark(i);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    Hint("F7 and F8 still work as a quick save slot. Teleports only work within the system you're in.");
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
        ImGui::SliderFloat("##height", &opt.height, 0.0f, 500.0f, "Spawn %.0f m above you");
        static const char* const kBoard[] = { "Don't board", "Board in the pilot seat", "Board in a seat by name", "Choose a seat after it spawns" };
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##board", &opt.seatMode, kBoard, 4);
        if (opt.seatMode == SeatMode_Named) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##seatName", "Seat name, e.g. copilot or turret left", opt.seatName, sizeof(opt.seatName));
            ImGui::SetItemTooltip("Every word must appear in the seat's name. The Crew tab shows a ship's seat names.");
        }
        const bool boarding = opt.seatMode == SeatMode_Pilot || opt.seatMode == SeatMode_Named;
        ImGui::BeginDisabled(!boarding);
        ImGui::Checkbox("Remove the NPC in my seat", &opt.replaceNpc);
        ImGui::SameLine(0, 24);
        ImGui::Checkbox("Power on", &opt.flightReady);
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
    if (ImGui::Checkbox("Infinite ship ammo", &shipAmmo)) Menu_SetInfiniteShipAmmo(shipAmmo);
    ImGui::SetItemTooltip("Refills the magazines of the ship you're aboard, and the ship in the Crew tab.");
    if (ImGui::Button("Power on / off", ImVec2(-1, 0))) Menu_RequestFlightReady();
    Hint("Toggles Flight Ready on the ship in the Crew tab, as if you pressed R in its pilot seat.");
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
    if (ImGui::Button("Use the ship I'm in", ImVec2(button, 0))) Menu_TargetShipImIn();
    if (count < 0) { Hint("Spawn a ship, or board one and press 'Use the ship I'm in'."); return; }
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
            case SeatState_You:   ImGui::TextColored(kLeaf, "You"); break;
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
    if (ImGui::Button("Sit here", ImVec2(quarter, 0))) Menu_RequestSit(seats[sel].id, replace);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state != SeatState_Npc && state != SeatState_You);
    if (ImGui::Button("Stand up", ImVec2(quarter, 0))) Menu_RequestStandUp(seats[sel].id);
    ImGui::SetItemTooltip("Whoever is in the seat gets up and stays aboard.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state != SeatState_Npc);
    if (ImGui::Button("Remove NPC", ImVec2(quarter, 0))) Menu_RequestKick(seats[sel].id);
    ImGui::SetItemTooltip("Takes the NPC out of the game.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state != SeatState_Empty);
    if (ImGui::Button("Add NPC", ImVec2(quarter, 0))) Menu_RequestAddCrew(seats[sel].id, g_npcPick);
    ImGui::EndDisabled();
    ImGui::Checkbox("If an NPC is in the seat I pick, remove it", &replace);

    Section("Crew");
    Hint("NPC to add to seats:");
    const bool haveNpcs = NpcPicker();
    const float third = Columns(3);
    ImGui::BeginDisabled(!haveNpcs);
    if (ImGui::Button("Fill empty seats", ImVec2(third, 0))) Menu_RequestFillCrew(g_npcPick);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("All NPCs stand up", ImVec2(third, 0))) Menu_RequestStandAll();
    ImGui::SameLine();
    if (ImGui::Button("Remove all NPCs", ImVec2(third, 0))) Menu_RequestClearCrew();
    Hint("NPCs you add sit in their seats. They don't fly the ship or operate turrets yet.");
}

// =============================================================================================
// NPCs
// =============================================================================================

static void DrawNpcsTab(bool& keepOpen) {
    Section("Spawn NPCs");
    if (!NpcPicker()) return;
    static int howMany = 1;
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##howMany", &howMany, 1, 10, howMany == 1 ? "1 NPC" : "%d NPCs");
    if (PrimaryButton("Spawn in front of me")) { Menu_RequestNpc(g_npcPick, howMany); keepOpen = false; }
    if (ImGui::Button("Remove spawned NPCs", ImVec2(-1, 0))) Menu_RequestClearNpcs();
    Hint("Removes every NPC this menu has spawned, crew included.");
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
    if (ImGui::BeginChild("##buildList", ImVec2(0, 260), ImGuiChildFlags_Borders)) {
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
            if (ImGui::Selectable(label, i == build)) build = i;
            ImGui::SetItemTooltip("%s", name);
        }
    }
    ImGui::EndChild();

    Section("Placing");
    char picked[128];
    PrettyBuildName(picked, sizeof(picked), Menu_BuildName(build));
    ImGui::Text("Selected: %s", picked);
    float reach = Menu_BuildReach();
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##reach", &reach, 5.0f, 300.0f, "Reach %.0f m");
    ImGui::SetItemTooltip("How far ahead objects are placed. They land on the ground where you look, or under the point this far out.");
    Menu_SetBuild(build, reach);
    const bool building = Menu_BuildModeActive();
    if (PrimaryButton(building ? "Stop building (F6)" : "Start building (F6)")) {
        Menu_ToggleBuildMode();
        if (!building) keepOpen = false;
    }
    const float half = Columns(2);
    if (ImGui::Button("Undo last", ImVec2(half, 0))) Menu_BuildUndo();
    ImGui::SameLine();
    char clearLabel[48];
    snprintf(clearLabel, sizeof(clearLabel), "Clear base (%d)###clearBase", Menu_BuildPlacedCount());
    if (ImGui::Button(clearLabel, ImVec2(half, 0))) Menu_BuildClear();
    Hint("While building: left click places, R rotates, [ and ] change reach, Backspace undoes, F6 stops.");
}

// =============================================================================================
// Menu settings
// =============================================================================================

static void DrawMenuTab() {
    Section("Background");
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

    Section("About");
    Hint("ChrisWareOffline v" CWO_VERSION " is a work in progress.");
    Hint("Discord: discord.gg/bUAuKMJUJs");
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

// Set by the Squadron 42 spoiler gate's Back button; DrawMenu selects the first tab once.
static bool g_backToFirstTab = false;

static void DrawSq42Tab(bool& keepOpen) {
    static bool spoilerOk = false;
    if (!spoilerOk) {
        ImGui::SeparatorText("Spoiler warning");
        ImGui::TextWrapped("This tab could have Squadron 42 spoilers.");
        ImGui::TextWrapped("Press OK to continue.");
        if (ImGui::Button("OK", ImVec2(120, 0))) spoilerOk = true;
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(120, 0))) g_backToFirstTab = true;
        return;
    }

    ImGui::SeparatorText("Outfits");
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
        if (ImGui::BeginCombo("##outfit", Menu_OutfitName(outfit), ImGuiComboFlags_HeightLargest)) {
            for (int i = 0; i < outfits; ++i) {
                const char* name = Menu_OutfitName(i);
                if (!MatchesFilter(name, filter)) continue;
                ImGui::PushID(i);
                if (ImGui::Selectable(name, i == outfit)) outfit = i;
                if (i == outfit) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Wear SQ42 outfit", ImVec2(-1, 0))) {
            Menu_RequestWearOutfit(outfit);
            keepOpen = false;
        }
    }
    static bool visor = false;
    if (ImGui::Checkbox("SQ42 visor HUD (applies on the next Equip or outfit)", &visor))
        Menu_SetS42VisorHud(visor);

    ImGui::SeparatorText("Settings");
    for (int i = 0; i < Menu_S42SettingCount(); ++i) {
        bool on = Menu_S42SettingOn(i);
        ImGui::PushID(i);
        const bool known = Menu_S42SettingKnown(i);   // greyed until the game thread has read the cvar
        ImGui::BeginDisabled(!known);
        if (ImGui::Checkbox(Menu_S42SettingLabel(i), &on)) Menu_RequestS42Setting(i, on);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", known ? Menu_S42SettingTip(i) : "Reading this setting from the game...");
        ImGui::PopID();
    }

    ImGui::SeparatorText("Spawn");
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
            ImGui::Checkbox("Spawn in front of you", &inFront);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##thingFilter", "search the [sq42] group...", thingFilter, sizeof(thingFilter));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##sq42thing", Menu_BuildName(thing), ImGuiComboFlags_HeightLargest)) {
                for (int i = 0; i < buildables; ++i) {
                    // Only the [sq42] group, like the original; the text filter narrows it further.
                    if (_stricmp(Menu_BuildCategory(i), "sq42") != 0) continue;
                    const char* name = Menu_BuildName(i);
                    if (!MatchesFilter(name, thingFilter)) continue;
                    ImGui::PushID(i);
                    if (ImGui::Selectable(name, i == thing)) thing = i;
                    if (i == thing) ImGui::SetItemDefaultFocus();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (inFront) {
                ImGui::SetNextItemWidth(200);
                ImGui::SliderFloat("ahead (m)", &ahead, 1.0f, 50.0f, "%.0f");
            }
            if (ImGui::Button("Spawn it", ImVec2(-1, 0))) {
                Menu_RequestPlace(thing, inFront, ahead);
                keepOpen = false;
            }
            ImGui::SetItemTooltip("Undo and Clear base in the build section remove these too.");
        }
    }

    ImGui::SeparatorText("Ships");
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

    ImGui::TextWrapped("Your ships put you in the pilot seat; the Vanduul ones spawn 300 m up and come for you.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##sq42shipFilter", "search ships...", sqFilter, sizeof(sqFilter));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##sq42ships", list[pick].label, ImGuiComboFlags_HeightLargest)) {
        for (int i = 0; i < n; ++i) {
            if (sqFilter[0] && !MatchesFilter(list[i].label, sqFilter)) continue;
            ImGui::PushID(i);
            const bool have = known(list[i].cls);
            if (ImGui::Selectable(list[i].label, i == pick, have ? 0 : ImGuiSelectableFlags_Disabled)) pick = i;
            if (!have) ImGui::SetItemTooltip("%s isn't in this game build's ship list.", list[i].cls);
            if (i == pick) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const bool pickKnown = known(list[pick].cls);
    if (list[pick].sit) {
        ImGui::SliderFloat("height above me (m)", &height, 0.0f, 500.0f, "%.0f");
        ImGui::Checkbox("put me in the pilot seat", &sit);
    }
    ImGui::BeginDisabled(!pickKnown);
    const bool spawnClicked = ImGui::Button("Spawn", ImVec2(-1, 42));
    ImGui::EndDisabled();
    if (spawnClicked) {
        Menu_RequestSpawnClass(list[pick].cls,
                               list[pick].sit ? height : list[pick].height,
                               list[pick].sit && sit, list[pick].sit && sit,
                               list[pick].enemyWing);
        keepOpen = false;
    }

    ImGui::SeparatorText("Console");
    static char cmd[256] = "";
    const bool consoleReady = Menu_ConsoleReady();
    ImGui::BeginDisabled(!consoleReady);
    const float runWidth = ImGui::CalcTextSize("Run").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(-(runWidth + ImGui::GetStyle().ItemSpacing.x));
    bool run = ImGui::InputTextWithHint("##console",
                                        "a console command, e.g. i_target_selector.targeting2_enabled 1",
                                        cmd, sizeof(cmd), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    run |= ImGui::Button("Run");
    ImGui::EndDisabled();
    if (run && cmd[0]) {
        Menu_RunConsole(cmd);
        cmd[0] = 0;
    }
    if (!consoleReady) ImGui::TextDisabled("The game's console wasn't found yet.");
    ImGui::TextWrapped("Runs in the game's own console. What it did shows in the game's log, not here.");
}

static bool DrawMenu() {
    bool keepOpen = true;
    DrawBackdrop();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("ChrisWareOffline v" CWO_VERSION "###main", &keepOpen,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                 | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);

    const float statusHeight = ImGui::GetTextLineHeight() * 2 + 18;
    const float bodyHeight = -(statusHeight + ImGui::GetStyle().ItemSpacing.y);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
        auto body = [&](auto draw) {
            if (ImGui::BeginChild("##body", ImVec2(0, bodyHeight))) draw();
            ImGui::EndChild();
        };
        const ImGuiTabItemFlags firstTab = g_backToFirstTab ? ImGuiTabItemFlags_SetSelected : 0;
        g_backToFirstTab = false;
        if (ImGui::BeginTabItem("Player", nullptr, firstTab)) { body([&] { DrawPlayerTab(keepOpen); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Travel"))   { body([&] { DrawTravelTab(); });           ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Vehicles")) { body([&] { DrawVehiclesTab(keepOpen); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Crew"))     { body([&] { DrawCrewTab(); });             ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("NPCs"))     { body([&] { DrawNpcsTab(keepOpen); });     ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Build"))    { body([&] { DrawBuildTab(keepOpen); });    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Squadron 42")) { body([&] { DrawSq42Tab(keepOpen); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Menu"))     { body([&] { DrawMenuTab(); });             ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    DrawStatusStrip(statusHeight);
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
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"ChrisWareOffline", WS_POPUP,
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
