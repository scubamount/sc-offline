// titanlink's overlay: Titanfall's picture over the game, on a thread and a window of its own.
//
// A click-through, topmost, no-activate popup the size of the game's client area, drawn through
// DirectComposition with a premultiplied-alpha swap chain, so alpha 0 shows Star Citizen. Each
// frame it reads tl_frame through sco.ipc (any thread), opens the two textures the Titanfall side
// shares, copies the newer one under its keyed mutex and draws it with a small crosshair. It never
// touches the game: no hook, no device of the game's, only the game window's position. Values from
// the Titanfall side are checked: a handle must be a 32-bit legacy shared handle, and the texture
// it opens must be a plain 2D texture of at most 8192 x 8192 in a colour format, with a keyed
// mutex; anything else is closed again and logged once.
#include "titanlink.h"
#include "../bridge_common.h"
#include "../../common.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <atomic>
#include <cstring>

namespace {

std::atomic<bool> g_run{ false };
std::atomic<bool> g_show{ false };
HANDLE            g_thread = nullptr;
// Set before the thread starts and only read by it.
const sco_ipc_v1* g_ipc = nullptr;
sco_plugin*       g_self = nullptr;
uint64_t          g_ch = 0;

template <class T> void Rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

const char kShader[] = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { float2 size; float hasTex; float pad; };
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O VS(uint id : SV_VertexID) {
    O o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}
float4 PS(O i) : SV_Target {
    float4 c = hasTex > 0.5 ? tex.Sample(smp, i.uv) : float4(0, 0, 0, 0);
    float2 d = abs(i.pos.xy - size * 0.5);
    if ((d.x < 1.0 && d.y > 4.0 && d.y < 12.0) || (d.y < 1.0 && d.x > 4.0 && d.x < 12.0)) c = float4(0.85, 0.85, 0.85, 0.9);
    return c;
}
)";

bool AllowedFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

ID3DBlob* Compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "titanlink", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                          &code, &errors))) {
        Log("[titanlink] overlay shader %s failed: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        Rel(errors);
        return nullptr;
    }
    Rel(errors);
    return code;
}

struct Overlay {
    HWND                     wnd = nullptr;
    ID3D11Device*            dev = nullptr;
    ID3D11DeviceContext*     ctx = nullptr;
    IDXGIDevice*             dxgiDev = nullptr;
    IDXGIAdapter*            adapter = nullptr;
    IDXGIFactory2*           factory = nullptr;
    IDXGISwapChain1*         swap = nullptr;
    IDCompositionDevice*     dcomp = nullptr;
    IDCompositionTarget*     target = nullptr;
    IDCompositionVisual*     visual = nullptr;
    ID3D11VertexShader*      vs = nullptr;
    ID3D11PixelShader*       ps = nullptr;
    ID3D11Buffer*            cb = nullptr;
    ID3D11SamplerState*      sampler = nullptr;
    ID3D11BlendState*        blend = nullptr;
    ID3D11RasterizerState*   raster = nullptr;
    ID3D11RenderTargetView*  rtv = nullptr;
    ID3D11Texture2D*         shared[2] = {};
    IDXGIKeyedMutex*         keyed[2] = {};
    uint64_t                 opened[2] = {};
    ID3D11Texture2D*         local = nullptr;
    ID3D11ShaderResourceView* localSrv = nullptr;
    UINT                     sw = 64, sh = 64, lw = 0, lh = 0;
    DXGI_FORMAT              lf = DXGI_FORMAT_UNKNOWN;
    uint32_t                 lastSeq = 0;
    bool                     have = false, shown = false, badLogged = false, firstLogged = false;

    bool Init() {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"sc_offline_titanlink_overlay";
        RegisterClassExW(&wc);   // already registered by an earlier link: fine
        wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                  WS_EX_NOREDIRECTIONBITMAP,
                              wc.lpszClassName, L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        if (!wnd) { Log("[titanlink] overlay window failed (%lu)", GetLastError()); return false; }
        SetLayeredWindowAttributes(wnd, 0, 255, LWA_ALPHA);
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                     D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
            Log("[titanlink] no Direct3D 11 device for the overlay");
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = sw;
        sd.Height = sh;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev))) || FAILED(dxgiDev->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) || FAILED(factory->CreateSwapChainForComposition(dev, &sd, nullptr, &swap)) ||
            FAILED(DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&dcomp))) || FAILED(dcomp->CreateTargetForHwnd(wnd, TRUE, &target)) ||
            FAILED(dcomp->CreateVisual(&visual)) || FAILED(visual->SetContent(swap)) || FAILED(target->SetRoot(visual)) ||
            FAILED(dcomp->Commit())) {
            Log("[titanlink] overlay composition setup failed");
            return false;
        }
        ID3DBlob* vsCode = Compile("VS", "vs_5_0");
        ID3DBlob* psCode = Compile("PS", "ps_5_0");
        const bool shaders = vsCode && psCode &&
                             SUCCEEDED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) &&
                             SUCCEEDED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        Rel(vsCode);
        Rel(psCode);
        if (!shaders) return false;
        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SAMPLER_DESC smp = {};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateBuffer(&cbd, nullptr, &cb)) || FAILED(dev->CreateSamplerState(&smp, &sampler)) ||
            FAILED(dev->CreateBlendState(&bd, &blend)) || FAILED(dev->CreateRasterizerState(&rd, &raster))) {
            Log("[titanlink] overlay pipeline setup failed");
            return false;
        }
        Log("[titanlink] overlay ready");
        return true;
    }

    void CloseShared(int i) {
        Rel(keyed[i]);
        Rel(shared[i]);
    }

    // Opens the texture behind a handle the Titanfall side named, or leaves the slot empty.
    void OpenShared(int i, uint64_t handle) {
        CloseShared(i);
        opened[i] = handle;
        if (!handle) return;
        const char* why = nullptr;
        D3D11_TEXTURE2D_DESC td = {};
        if (handle > 0xFFFFFFFFull) {
            why = "not a legacy shared handle";
        } else if (FAILED(dev->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)), IID_PPV_ARGS(&shared[i])))) {
            why = "it doesn't open on this graphics card";
        } else {
            shared[i]->GetDesc(&td);
            if (td.Width < 1 || td.Height < 1 || td.Width > 8192 || td.Height > 8192 || td.ArraySize != 1 || td.SampleDesc.Count != 1 ||
                !AllowedFormat(td.Format) || !(td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))
                why = "not a plain colour texture with a keyed mutex, at most 8192 x 8192";
            else if (FAILED(shared[i]->QueryInterface(IID_PPV_ARGS(&keyed[i]))))
                why = "no keyed mutex";
        }
        if (why) {
            CloseShared(i);
            if (!badLogged) Log("[titanlink] couldn't use Titanfall's picture: %s", why);
            badLogged = true;
        }
    }

    // Copies the newer of the two shared pictures into `local`, if one is new and free.
    void TakeFrame() {
        tl_frame fr = {};
        if (g_ipc->block_read(g_self, g_ch, TL_OFF_FRAME, &fr, sizeof(fr)) != SCO_OK) return;
        for (int i = 0; i < 2; ++i)
            if (fr.handle[i] != opened[i]) OpenShared(i, fr.handle[i]);
        const int order[2] = { fr.tex_seq[0] >= fr.tex_seq[1] ? 0 : 1, fr.tex_seq[0] >= fr.tex_seq[1] ? 1 : 0 };
        for (int oi = 0; oi < 2; ++oi) {
            const int i = order[oi];
            if (!shared[i] || !keyed[i] || fr.tex_seq[i] == 0 || fr.tex_seq[i] == lastSeq) continue;
            if (keyed[i]->AcquireSync(1, 0) != S_OK) continue;
            D3D11_TEXTURE2D_DESC td = {};
            shared[i]->GetDesc(&td);
            if (!local || td.Width != lw || td.Height != lh || td.Format != lf) {
                Rel(localSrv);
                Rel(local);
                D3D11_TEXTURE2D_DESC ld = td;
                ld.MipLevels = 1;
                ld.MiscFlags = 0;
                ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                ld.Usage = D3D11_USAGE_DEFAULT;
                ld.CPUAccessFlags = 0;
                if (SUCCEEDED(dev->CreateTexture2D(&ld, nullptr, &local)) && FAILED(dev->CreateShaderResourceView(local, nullptr, &localSrv))) Rel(local);
                lw = td.Width;
                lh = td.Height;
                lf = td.Format;
            }
            if (local) ctx->CopySubresourceRegion(local, 0, 0, 0, 0, shared[i], 0, nullptr);
            keyed[i]->ReleaseSync(0);
            lastSeq = fr.tex_seq[i];
            have = local != nullptr;
            if (have && !firstLogged) {
                firstLogged = true;
                Log("[titanlink] first Titanfall picture in (%ux%u)", lw, lh);
            }
            break;
        }
    }

    void Frame(HWND game) {
        RECT rc = {};
        GetClientRect(game, &rc);
        POINT org = { 0, 0 };
        ClientToScreen(game, &org);
        const UINT w = static_cast<UINT>(rc.right > 1 ? rc.right : 1), h = static_cast<UINT>(rc.bottom > 1 ? rc.bottom : 1);
        if (w != sw || h != sh || !rtv) {
            Rel(rtv);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            if ((w != sw || h != sh) && SUCCEEDED(swap->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) { sw = w; sh = h; }
            ID3D11Texture2D* back = nullptr;
            if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
                dev->CreateRenderTargetView(back, nullptr, &rtv);
                back->Release();
            }
            if (!rtv) return;
        }
        SetWindowPos(wnd, HWND_TOPMOST, org.x, org.y, static_cast<int>(w), static_cast<int>(h), SWP_NOACTIVATE | (shown ? 0 : SWP_SHOWWINDOW));
        shown = true;
        TakeFrame();
        const float clear[4] = { 0, 0, 0, 0 };
        ctx->ClearRenderTargetView(rtv, clear);
        const D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(sw), static_cast<float>(sh), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(raster);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        const float cbv[4] = { static_cast<float>(sw), static_cast<float>(sh), have ? 1.0f : 0.0f, 0.0f };
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, cbv, sizeof(cbv));
            ctx->Unmap(cb, 0);
        }
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* srv = have ? localSrv : nullptr;
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
        ctx->Draw(3, 0);
        swap->Present(1, 0);
    }

    void Hide() {
        if (shown) ShowWindow(wnd, SW_HIDE);
        shown = false;
    }

    void Release() {
        for (int i = 0; i < 2; ++i) CloseShared(i);
        Rel(localSrv); Rel(local); Rel(rtv); Rel(raster); Rel(blend); Rel(sampler); Rel(cb); Rel(ps); Rel(vs);
        Rel(visual); Rel(target); Rel(dcomp); Rel(swap); Rel(factory); Rel(adapter); Rel(dxgiDev); Rel(ctx); Rel(dev);
        if (wnd) DestroyWindow(wnd);
        wnd = nullptr;
    }
};

DWORD WINAPI OverlayThread(LPVOID) {
    Overlay o;
    const bool ok = o.Init();
    HWND game = nullptr;
    while (g_run.load()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (!game || !IsWindow(game)) game = BridgeGameWindow();
        if (!ok || !g_show.load() || !game || IsIconic(game) || !GameHasFocus()) {
            o.Hide();
            Sleep(30);
            continue;
        }
        o.Frame(game);
    }
    o.Release();
    return 0;
}

}  // namespace

void TlOverlayStart(const sco_ipc_v1* ipc, sco_plugin* self, uint64_t channel) {
    if (g_thread) return;
    g_ipc = ipc;
    g_self = self;
    g_ch = channel;
    g_show = false;
    g_run = true;
    g_thread = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    if (!g_thread) {
        g_run = false;
        Log("[titanlink] couldn't start the overlay thread (%lu); Titanfall's picture won't show", GetLastError());
    }
}

void TlOverlayStop() {
    if (!g_thread) return;
    g_run = false;
    if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0) Log("[titanlink] the overlay thread didn't stop within 5 s");
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_show = false;
}

void TlOverlayShow(bool on) { g_show = on; }
