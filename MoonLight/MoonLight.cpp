#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "../NativeScene.h"
#include "MoonRules.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_map>
namespace moonlight {
template <class T> void drop(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
static void log(const char *s) {
    FILE *f = nullptr;
    fopen_s(&f, "Mods\\MoonLight.log", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}
static bool enabled = true, internal = false, visibleMoon = true;
static float amount = .04f, cloudScale = .55f, shadowStrength = .20f;
static std::atomic<float> sunX{0}, sunY{0}, sunZ{0};
static std::atomic<bool> sunKnown{false};
static const GUID cacheGuid = {
    0x3530b23e, 0xcdae, 0x4a69, {0xa1, 0x7e, 0x23, 0x18, 0xcb, 0x7e, 0x10, 0x06}};
using DI = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
using D = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using Map = HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP,
                                         UINT, D3D11_MAPPED_SUBRESOURCE *);
using Unmap = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
using Update = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                         const D3D11_BOX *, const void *, UINT, UINT);
static DI oldDI;
static D oldD;
static Map oldMap;
static Unmap oldUnmap;
static Update oldUpdate;
static void **table = nullptr;
static ID3D11Device *device = nullptr;
static ID3D11DeviceContext *context = nullptr;
static ID3D11Buffer *light = nullptr, *ambient = nullptr, *material = nullptr, *white = nullptr;
static ID3D11DeviceContext1 *context1 = nullptr;
static ID3DDeviceContextState *skyState = nullptr;
static ID3D11VertexShader *skyVS = nullptr;
static ID3D11PixelShader *skyPS = nullptr;
static ID3D11Buffer *skyConstants = nullptr;
static ID3D11BlendState *skyBlend = nullptr;
static ID3D11DepthStencilState *skyDepth = nullptr;
static ID3D11RasterizerState *skyRaster = nullptr;
static ID3D11RenderTargetView *skyTarget = nullptr;
static ID3D11DepthStencilView *sceneDepth = nullptr;
static D3D11_VIEWPORT skyViewport{};
static float sceneView[16]{}, sceneProjection[16]{};
static bool skyReady = false, skyFinished = false;
struct Mapped {
    void *data;
    UINT size;
};
static thread_local std::unordered_map<ID3D11Resource *, Mapped> mapped;
#ifdef MOON_TEST
static MoonState testPhase;
static bool testCloud = false;
#endif
static MoonState current() {
#ifdef MOON_TEST
    return testPhase;
#else
    auto g = nativeScene::game();
    auto s = enabled && g && g->world && g->world->local_creature
                 ? moonrules::state(g->world->state.time, shadowStrength)
                 : MoonState{};
    return s;
#endif
}
static UINT bytes(ID3D11Resource *r) {
    ID3D11Buffer *b = nullptr;
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Buffer), (void **)&b)))
        return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    drop(b);
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth <= 64 ? d.ByteWidth : 0;
}
static bool read(ID3D11Buffer *b, void *out, UINT n) {
    if (!b)
        return false;
    UINT got = n;
    return SUCCEEDED(b->GetPrivateData(cacheGuid, &got, out)) && got == n;
}
static void upload(ID3D11DeviceContext *c, ID3D11Buffer *b, const void *data, UINT n) {
    unsigned char previous[64];
    if (read(b, previous, n) && memcmp(previous, data, n) == 0)
        return;
    oldUpdate(c, b, 0, nullptr, data, 0, 0);
    b->SetPrivateData(cacheGuid, n, data);
}
static void clearSky() {
    drop(skyTarget);
    drop(sceneDepth);
    skyReady = false;
}
static const char *skyCode = R"HLSL(
cbuffer Moon:register(b0){float4 disk;};
float4 vs(uint id:SV_VertexID):SV_POSITION{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),.9999999,1);}
float4 ps(float4 p:SV_POSITION):SV_TARGET{
 float2 q=floor((p.xy-disk.xy)/3)*3/disk.z;float d=length(q);if(d>1)discard;
 float crater=0;
 crater+=length(q-float2(-.3,.2))<.20?.10:0;
 crater+=length(q-float2(.3,-.25))<.15?.07:0;
 crater+=length(q-float2(.15,.4))<.12?.06:0;
 float edge=1-smoothstep(.86,1,d);
 return float4(float3(.66,.76,.92)-crater, disk.w*(.8+.2*edge));
}
)HLSL";
static bool initializeSky() {
    if (skyVS)
        return skyState && skyPS && skyConstants && skyBlend && skyDepth && skyRaster;
    ID3D11Device1 *d1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1)) ||
        FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&context1))) {
        drop(d1);
        return false;
    }
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    bool ok = SUCCEEDED(d1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                                                     __uuidof(ID3D11Device), nullptr, &skyState));
    drop(d1);
    for (int i = 0; i < 2 && ok; i++) {
        ID3DBlob *code = nullptr, *error = nullptr;
        ok = SUCCEEDED(D3DCompile(skyCode, strlen(skyCode), "MoonDisk", nullptr, nullptr,
                                  i ? "ps" : "vs", i ? "ps_4_0" : "vs_4_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error));
        if (error)
            log((char *)error->GetBufferPointer());
        if (ok)
            ok = SUCCEEDED(i ? device->CreatePixelShader(code->GetBufferPointer(),
                                                         code->GetBufferSize(), nullptr, &skyPS)
                             : device->CreateVertexShader(code->GetBufferPointer(),
                                                          code->GetBufferSize(), nullptr, &skyVS));
        drop(code);
        drop(error);
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (ok)
        ok = SUCCEEDED(device->CreateBuffer(&cb, nullptr, &skyConstants));
    D3D11_BLEND_DESC blend{};
    auto &r = blend.RenderTarget[0];
    r.BlendEnable = TRUE;
    r.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    r.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    r.BlendOp = D3D11_BLEND_OP_ADD;
    r.SrcBlendAlpha = D3D11_BLEND_ZERO;
    r.DestBlendAlpha = D3D11_BLEND_ONE;
    r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    r.RenderTargetWriteMask = 7;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (ok)
        ok = SUCCEEDED(device->CreateBlendState(&blend, &skyBlend)) &&
             SUCCEEDED(device->CreateDepthStencilState(&depth, &skyDepth)) &&
             SUCCEEDED(device->CreateRasterizerState(&raster, &skyRaster));
    log(ok ? "Moon disk ready: clock-driven sun/moon cycle, sky-depth occlusion, one triangle."
           : "Moon disk resources unavailable; lighting remains enabled.");
    return ok;
}
static bool moonDiskPosition(float disk[4], MoonState phase) {
    float v[4]{}, clip[4]{};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            v[r] += sceneView[r * 4 + c] * phase.direction[c];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            clip[r] += sceneProjection[r * 4 + c] * v[c];
    if (!std::isfinite(clip[3]) || clip[3] <= .001f)
        return false;
    float x = clip[0] / clip[3], y = clip[1] / clip[3];
    if (fabsf(x) > 1.1f || fabsf(y) > 1.1f)
        return false;
    disk[0] = skyViewport.TopLeftX + (x * .5f + .5f) * skyViewport.Width;
    disk[1] = skyViewport.TopLeftY + (-y * .5f + .5f) * skyViewport.Height;
    disk[2] = skyViewport.Height * .018f;
    disk[3] = phase.weight * .75f;
    return true;
}
static void renderSky() {
    if (internal || !skyReady || skyFinished)
        return;
    skyFinished = true;
    auto phase = current();
    float disk[4]{};
    if (!visibleMoon || phase.weight <= 0 || !skyTarget || !sceneDepth ||
        !moonDiskPosition(disk, phase) || !initializeSky()) {
        clearSky();
        return;
    }
    internal = true;
    ID3DDeviceContextState *saved = nullptr;
    context1->SwapDeviceContextState(skyState, &saved);
    context->OMSetRenderTargets(1, &skyTarget, sceneDepth);
    context->OMSetBlendState(skyBlend, nullptr, ~0u);
    context->OMSetDepthStencilState(skyDepth, 0);
    context->RSSetState(skyRaster);
    context->RSSetViewports(1, &skyViewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(skyVS, nullptr, 0);
    context->PSSetShader(skyPS, nullptr, 0);
    oldUpdate(context, skyConstants, 0, nullptr, disk, 0, 0);
    context->PSSetConstantBuffers(0, 1, &skyConstants);
    oldD(context, 3, 0);
    // An inactive context state retains its bindings. Release the game's back
    // buffer/depth bindings before shelving our state so ResizeBuffers can work.
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context1->SwapDeviceContextState(saved, nullptr);
    drop(saved);
    internal = false;
    clearSky();
}
static bool initialize(ID3D11DeviceContext *c) {
    if (device)
        return context == c && light && ambient && material && white;
    context = c;
    context->AddRef();
    c->GetDevice(&device);
    D3D11_BUFFER_DESC d{};
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.ByteWidth = 48;
    bool ok = SUCCEEDED(device->CreateBuffer(&d, nullptr, &light));
    d.ByteWidth = 16;
    ok = ok && SUCCEEDED(device->CreateBuffer(&d, nullptr, &ambient)) &&
         SUCCEEDED(device->CreateBuffer(&d, nullptr, &material)) &&
         SUCCEEDED(device->CreateBuffer(&d, nullptr, &white));
    log(ok ? "Native world lighting buffers ready; no GPU readback or additional terrain pass."
           : "Moon lighting buffer creation failed.");
    return ok;
}
struct Saved {
    ID3D11Buffer *b[9]{};
    bool changed[9]{};
};
static Saved prepare(ID3D11DeviceContext *c) {
    Saved saved;
    if (internal || !enabled || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return saved;
    auto phase = current();
    if (phase.weight <= 0)
        return saved;
    c->VSGetConstantBuffers(0, 9, saved.b);
    const UINT sizes[] = {64, 64, 64, 16, 320, 48, 16, 16, 16};
    bool native = true;
    for (int i = 0; i < 9; i++) {
        D3D11_BUFFER_DESC d{};
        if (saved.b[i])
            saved.b[i]->GetDesc(&d);
        if (d.ByteWidth != sizes[i])
            native = false;
    }
    float projection[16]{};
    bool gotProjection = read(saved.b[0], projection, 64);
    if (gotProjection && fabsf(projection[15]) > .5f) {
        renderSky();
        return saved;
    }
    if (!native || !gotProjection)
        return saved;
    ID3D11Buffer *vb = nullptr;
    UINT stride = 0, offset = 0;
    c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    auto g = nativeScene::game();
    bool isCloud = nativeScene::cloud(g ? g->world : nullptr, vb);
    drop(vb);
#ifdef MOON_TEST
    isCloud = testCloud;
#endif
    float nativeLight[12]{}, a[4]{}, m[4]{}, w[4]{};
    if (!read(saved.b[5], nativeLight, 48) || !read(saved.b[6], a, 16) || !initialize(c))
        return saved;
    sunX = nativeLight[8];
    sunY = nativeLight[9];
    sunZ = nativeLight[10];
    sunKnown = true;
    phase = current();
    if (!skyFinished && !skyReady && !isCloud && read(saved.b[1], sceneView, 64) &&
        fabsf(sceneView[3]) + fabsf(sceneView[7]) + fabsf(sceneView[11]) > .01f) {
        UINT count = 1;
        c->RSGetViewports(&count, &skyViewport);
        if (count && skyViewport.Width >= 640 && skyViewport.Height >= 360 &&
            skyViewport.TopLeftX == 0 && skyViewport.TopLeftY == 0) {
            memcpy(sceneProjection, projection, 64);
            c->OMGetRenderTargets(1, &skyTarget, &sceneDepth);
            skyReady = skyTarget && sceneDepth;
        }
    }
    internal = true;
    float changed[12];
    moonrules::lighting(changed, nativeLight, phase, amount);
    upload(c, light, changed, 48);
    c->VSSetConstantBuffers(5, 1, &light);
    saved.changed[5] = true;
    const float tint[] = {.075f, .09f, .12f};
    for (int i = 0; i < 3; i++)
        a[i] = std::clamp(a[i] + tint[i] * phase.weight * amount, 0.f, 1.f);
    upload(c, ambient, a, 16);
    c->VSSetConstantBuffers(6, 1, &ambient);
    saved.changed[6] = true;
    if (isCloud && read(saved.b[3], m, 16) && read(saved.b[7], w, 16)) {
        float result[4];
        moonrules::cloud(result, m, phase.weight, cloudScale);
        upload(c, material, result, 16);
        w[0] *= 1 - phase.weight;
        upload(c, white, w, 16);
        c->VSSetConstantBuffers(3, 1, &material);
        c->VSSetConstantBuffers(7, 1, &white);
        saved.changed[3] = saved.changed[7] = true;
    }
    internal = false;
    return saved;
}
static void restore(ID3D11DeviceContext *c, Saved &s) {
    for (int i = 0; i < 9; i++) {
        if (s.changed[i])
            c->VSSetConstantBuffers(i, 1, &s.b[i]);
        drop(s.b[i]);
    }
}
static void STDMETHODCALLTYPE di(ID3D11DeviceContext *c, UINT n, UINT s, INT b) {
    auto saved = prepare(c);
    oldDI(c, n, s, b);
    restore(c, saved);
}
static void STDMETHODCALLTYPE draw(ID3D11DeviceContext *c, UINT n, UINT s) {
    auto saved = prepare(c);
    oldD(c, n, s);
    restore(c, saved);
}
static HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                     D3D11_MAP t, UINT flags, D3D11_MAPPED_SUBRESOURCE *m) {
    auto hr = oldMap(c, r, sub, t, flags, m);
    if (!internal && SUCCEEDED(hr) && sub == 0 && t != D3D11_MAP_READ) {
        UINT n = bytes(r);
        if (n)
            mapped[r] = {m->pData, n};
    }
    return hr;
}
static void STDMETHODCALLTYPE unmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub) {
    auto it = mapped.find(r);
    if (it != mapped.end()) {
        r->SetPrivateData(cacheGuid, it->second.size, it->second.data);
        mapped.erase(it);
    }
    oldUnmap(c, r, sub);
}
static void STDMETHODCALLTYPE update(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                     const D3D11_BOX *box, const void *data, UINT row, UINT slice) {
    if (!internal && sub == 0 && !box && data) {
        UINT n = bytes(r);
        if (n)
            r->SetPrivateData(cacheGuid, n, data);
    }
    oldUpdate(c, r, sub, box, data, row, slice);
}
static bool patch(void **slot, void *fn) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return false;
    InterlockedExchangePointer(slot, fn);
    DWORD unused;
    VirtualProtect(slot, sizeof(void *), old, &unused);
    return true;
}
static DWORD attach() {
    if (table)
        return 1;
    if (!nativeScene::supported()) {
        log("Unsupported game build; disabled.");
        enabled = false;
        return 0;
    }
    auto setting = [](const wchar_t *key, int value) {
        return GetPrivateProfileIntW(L"Moon", key, value, L".\\Mods\\MoonLight.ini");
    };
    enabled = setting(L"Enabled", 1) != 0;
    amount = std::clamp((int)setting(L"BrightnessPercent", 4), 0, 20) * .01f;
    cloudScale = std::clamp((int)setting(L"NightCloudBrightnessPercent", 55), 20, 100) * .01f;
    shadowStrength = std::clamp((int)setting(L"ShadowStrengthPercent", 20), 0, 100) * .01f;
    visibleMoon = setting(L"VisibleMoon", 1) != 0;
    if (!enabled)
        return 1;
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &d, nullptr, &c)))
        return 0;
    auto v = *(void ***)c;
    struct Hook {
        UINT slot;
        void *fn;
        void **old;
    };
    Hook hooks[] = {{12, (void *)di, (void **)&oldDI},
                    {13, (void *)draw, (void **)&oldD},
                    {14, (void *)map, (void **)&oldMap},
                    {15, (void *)unmap, (void **)&oldUnmap},
                    {48, (void *)update, (void **)&oldUpdate}};
    for (auto &h : hooks)
        *h.old = v[h.slot];
    bool ok = true;
    int done = 0;
    for (auto &h : hooks) {
        if (!patch(v + h.slot, h.fn)) {
            ok = false;
            while (done--) {
                auto &u = hooks[done];
                patch(v + u.slot, *u.old);
            }
            break;
        }
        done++;
    }
    if (ok)
        table = v;
    drop(c);
    drop(d);
    log(ok ? "MoonLight enabled, loader API 7.3: subtle moon lighting, night cloud dimming, shared SteamShadows pass."
           : "Hook installation failed; original rendering retained.");
    return ok ? 1 : 0;
}
} // namespace moonlight
#ifndef MOON_AND_SHADOW
class MoonLight : public GenericMod {
  public:
    void Initialize() override {
        moonlight::attach();
    }
    void OnPresent(IDXGISwapChain *, UINT, UINT) override {
        moonlight::renderSky();
        moonlight::clearSky();
        moonlight::skyFinished = false;
    }
};
static_assert(sizeof(GenericMod) == 0x428);
EXPORT GenericMod *MakeMod() {
    return new MoonLight();
}
EXPORT void ModPreInitialize() {}
EXPORT int ModMajorVersion() {
    return 7;
}
EXPORT int ModMinorVersion() {
    return 3;
}
#endif
extern "C" __declspec(dllexport) BOOL WINAPI QueryMoonLight(MoonState *out) {
    if (!out)
        return FALSE;
    *out = moonlight::current();
    return out->weight > 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI AttachMoonLight(void *) {
    return moonlight::attach();
}
