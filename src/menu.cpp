#include "menu.h"
#include <d3d11.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_impl_win32.h"
#include "third_party/imgui/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")

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

static void ShipLabel(char* out, size_t n, const MenuShip& s) {
    if (s.length > 0) snprintf(out, n, "%s   (size %d, ~%.0f m)", s.name, s.size, s.length);
    else snprintf(out, n, "%s   (size %d)", s.name, s.size);
}

static void DrawMainTab(bool& keepOpen) {
    static int   selected = 0;
    static float height = 20.0f;
    static bool  sit = true;
    static bool  flightReady = true;
    static char  filter[64] = "";

    const int count = Menu_ShipCount();
    if (count < 0) {
        ImGui::TextWrapped("Loading ship list... (you need to be spawned in the universe)");
    } else if (count == 0) {
        ImGui::TextWrapped("No ships found - check ships.txt.");
    } else {
        const MenuShip* ships = Menu_Ships();
        if (selected >= count) selected = 0;

        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##filter", "search ships...", filter, sizeof(filter))) {
            for (int i = 0; i < count; ++i)
                if (MatchesFilter(ships[i].name, filter)) { selected = i; break; }
        }

        char preview[112];
        ShipLabel(preview, sizeof(preview), ships[selected]);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##ship", preview, ImGuiComboFlags_HeightLargest)) {
            for (int i = 0; i < count; ++i) {
                if (!MatchesFilter(ships[i].name, filter)) continue;
                char label[128];
                ShipLabel(label, sizeof(label) - 8, ships[i]);
                snprintf(label + strlen(label), 8, "##%d", i);
                const bool isSelected = i == selected;
                if (ImGui::Selectable(label, isSelected)) selected = i;
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SliderFloat("height above me (m)", &height, 0.0f, 500.0f, "%.0f");
        ImGui::Checkbox("put me in the pilot seat", &sit);
        ImGui::SameLine();
        ImGui::Checkbox("power it on (Flight Ready)", &flightReady);
        if (ImGui::Button("Spawn", ImVec2(-1, 42))) {
            Menu_RequestSpawn(selected, height, sit, sit && flightReady);
            keepOpen = false;
        }
    }

    static bool  noclip = false;
    static float noclipSpeed = 30.0f;
    if (ImGui::Button(noclip ? "Noclip: ON" : "Noclip: OFF", ImVec2(160, 0))) {
        noclip = !noclip;
        Menu_SetNoclip(noclip, noclipSpeed);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputFloat("##noclipSpeed", &noclipSpeed, 0.0f, 0.0f, "%.0f")) {
        if (noclipSpeed < 1.0f) noclipSpeed = 1.0f;
        Menu_SetNoclipSpeed(noclipSpeed);
    }

    static bool godMode = true;
    if (ImGui::Checkbox("god mode", &godMode)) Menu_SetGodMode(godMode);
    ImGui::SameLine();
    static bool infiniteAmmo = false;
    if (ImGui::Checkbox("infinite ammo", &infiniteAmmo)) Menu_SetInfiniteAmmo(infiniteAmmo);

    ImGui::Separator();
    static int  gear[Gear_SlotCount] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    static char gearFilter[64] = "";
    static const struct { const char* label; const char* none; } kGear[Gear_SlotCount] = {
        { "undersuit", "(default)" }, { "helmet", "(none)" }, { "body", "(none)" }, { "arms", "(none)" },
        { "legs", "(none)" }, { "backpack", "(none)" }, { "gun", "(none)" }, { "sidearm", "(none)" },
        { "ammo", "(matches gun)" }, { "grenades", "(none)" } };
    if (Menu_GearCount(0) < 0) {
        ImGui::TextWrapped("Loading gear... (you need to be spawned in the universe)");
    } else {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##gearFilter", "search gear...", gearFilter, sizeof(gearFilter));
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        for (int s = 0; s < Gear_SlotCount; ++s) {
            if (s % 2) ImGui::SameLine();
            GearCombo(s, kGear[s].label, kGear[s].none, gear[s], gearFilter, half);
        }
        if (ImGui::Button("Equip", ImVec2(-1, 30))) {
            Menu_RequestEquip(gear);
            keepOpen = false;
        }
    }

    ImGui::Separator();
    static int  npc = 0, npcCount = 1;
    static char npcFilter[64] = "";
    const int npcs = Menu_NpcCount();
    if (npcs < 0) {
        ImGui::TextWrapped("Loading NPCs... (you need to be spawned in the universe)");
    } else if (npcs == 0) {
        ImGui::TextWrapped("No NPCs found - check npcs.txt.");
    } else {
        if (npc >= npcs) npc = 0;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##npcFilter", "search npcs...", npcFilter, sizeof(npcFilter))) {
            for (int i = 0; i < npcs; ++i)
                if (MatchesFilter(Menu_NpcName(i), npcFilter)) { npc = i; break; }
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##npc", Menu_NpcName(npc), ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < npcs; ++i) {
                const char* name = Menu_NpcName(i);
                if (!MatchesFilter(name, npcFilter)) continue;
                ImGui::PushID(i);
                if (ImGui::Selectable(name, i == npc)) npc = i;
                if (i == npc) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(200);
        ImGui::SliderInt("how many", &npcCount, 1, 10);
        ImGui::SameLine();
        const float button = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Spawn NPC", ImVec2(button, 0))) {
            Menu_RequestNpc(npc, npcCount);
            keepOpen = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear NPCs", ImVec2(-1, 0))) Menu_RequestClearNpcs();
    }

    ImGui::Separator();
    static int   build = 0;
    static char  buildFilter[64] = "";
    const int buildables = Menu_BuildCount();
    if (buildables < 0) {
        ImGui::TextWrapped("Loading build objects... (you need to be spawned in the universe)");
    } else if (buildables == 0) {
        ImGui::TextWrapped("No build objects found - check buildables.txt.");
    } else {
        if (build >= buildables) build = 0;
        static int buildTab = 0;
        if (ImGui::BeginTabBar("##buildTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (int c = 0; c < Menu_BuildCategoryCount(); ++c) {
                char tab[40];
                snprintf(tab, sizeof(tab), "%s##cat%d", Menu_BuildCategoryName(c), c);
                if (tab[0] >= 'a' && tab[0] <= 'z') tab[0] -= 'a' - 'A';
                if (ImGui::BeginTabItem(tab)) { buildTab = c; ImGui::EndTabItem(); }
            }
            ImGui::EndTabBar();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##buildFilter", "search all build objects...", buildFilter, sizeof(buildFilter));
        const bool searching = buildFilter[strspn(buildFilter, " _")] != 0;
        if (ImGui::BeginChild("##buildList", ImVec2(0, 150), ImGuiChildFlags_Borders)) {
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
        char picked[128];
        PrettyBuildName(picked, sizeof(picked), Menu_BuildName(build));
        ImGui::Text("Placing: %s", picked);
        float reach = Menu_BuildReach();
        ImGui::SetNextItemWidth(200);
        ImGui::SliderFloat("reach (m)", &reach, 5.0f, 300.0f, "%.0f");
        ImGui::SetItemTooltip("How far the camera reaches. Objects land on the ground where you look, or under the point this far out.");
        Menu_SetBuild(build, reach);
        const float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        const bool building = Menu_BuildModeActive();
        if (ImGui::Button(building ? "Exit build mode (F6)" : "Build mode (F6)", ImVec2(third, 0))) {
            Menu_ToggleBuildMode();
            if (!building) keepOpen = false;
        }
        ImGui::SetItemTooltip("Left click = place, R = rotate, [ and ] = shorter / longer reach, Backspace = undo, F6 = exit");
        ImGui::SameLine();
        if (ImGui::Button("Undo", ImVec2(third, 0))) Menu_BuildUndo();
        ImGui::SameLine();
        char clearLabel[48];
        snprintf(clearLabel, sizeof(clearLabel), "Clear base (%d)###clearBase", Menu_BuildPlacedCount());
        if (ImGui::Button(clearLabel, ImVec2(-1, 0))) Menu_BuildClear();
    }
}

static bool DrawMenu() {
    bool keepOpen = true;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("ChrisWareOffline", &keepOpen,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("This mod is a work in progress and not done at all");
    ImGui::TextUnformatted("Join our Discord server https://discord.gg/bUAuKMJUJs");
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Main")) { DrawMainTab(keepOpen); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
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
    ImGui::GetStyle().FontScaleMain = 1.2f;
    ImGui_ImplWin32_Init(g_wnd);
    ImGui_ImplDX11_Init(g_device, g_context);

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
        const float clear[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
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
