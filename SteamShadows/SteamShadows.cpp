#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "../NativeScene.h"
#include "../MoonLight/MoonRules.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include "../GpuPassTimer.h"
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>

// Loaded native terrain casts even outside the camera frustum; visible meshes receive.
namespace shadows {
static GpuPassTimer gpuTimer;
template <class T> void drop(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
static void log(const char *s) {
    FILE *f = nullptr;
    fopen_s(&f, "Mods\\SteamShadows.log", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}
static const GUID cacheGuid = {
    0x89d60734, 0xeda9, 0x4bf0, {0xb0, 0x61, 0xc9, 0x11, 0x33, 0x28, 0x21, 0x18}};
static bool internal = false, enabled = true, finished = false, failed = false;
static std::atomic<int> toggleRequest{0}, captureRequest{0}, disableRequest{0};
static bool cloudShadows = true, softShadows = false;
static UINT resolution = 4096;
static float radius = 120, strength = .48f, bias = .008f, sunWarmth = .03f, fadeStart = .4f;
static ID3D11Device *device = nullptr;
static ID3D11DeviceContext *ctx = nullptr;
static ID3D11DeviceContext1 *ctx1 = nullptr;
static ID3DDeviceContextState *state = nullptr;
static ID3D11VertexShader *caster = nullptr, *receiver = nullptr;
static ID3D11PixelShader *visibility = nullptr;
static ID3D11Buffer *constants = nullptr;
static ID3D11InputLayout *nativeCasterLayout = nullptr, *floatCasterLayout = nullptr;
static ID3D11Texture2D *texture = nullptr;
static ID3D11DepthStencilView *mapDepth = nullptr;
static ID3D11ShaderResourceView *mapView = nullptr;
static ID3D11DepthStencilState *writeDepth = nullptr, *testDepth = nullptr;
static ID3D11RasterizerState *raster = nullptr, *receiverRaster = nullptr;
static ID3D11BlendState *blend = nullptr, *opaque = nullptr;
static ID3D11SamplerState *sampler = nullptr;
static ID3D11RenderTargetView *target = nullptr;
static ID3D11DepthStencilView *depth = nullptr;
static D3D11_VIEWPORT sceneViewport{};
static unsigned frames = 0, lastCount = 0;
static double totalMs = 0;
static bool captureLogged = false;
static unsigned diagnosticRejects = 0;
using DI = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
using D = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using Map = HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP,
                                         UINT, D3D11_MAPPED_SUBRESOURCE *);
using Unmap = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
using Update = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                         const D3D11_BOX *, const void *, UINT, UINT);
using Clear = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT,
                                        FLOAT, UINT8);
static DI oldDI;
static D oldD;
static Map oldMap;
static Unmap oldUnmap;
static Update oldUpdate;
static Clear oldClear;
static void **table = nullptr;
struct Mapped {
    void *data;
    UINT bytes;
};
static thread_local std::unordered_map<ID3D11Resource *, Mapped> mapped;
struct Draw {
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    ID3D11InputLayout *layout = nullptr;
    UINT stride = 0, offset = 0, ibOffset = 0, count = 0, start = 0;
    INT base = 0;
    DXGI_FORMAT format{};
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    bool indexed = true, terrain = false;
    nativeScene::Chunk terrainInfo{};
    float world[16], view[16], proj[16], light[12];
    float pointLights[80]{};
    float depthOffset = 0;
};
static std::vector<Draw> draws;
struct CloudCaster {
    Draw draw{};
    double x = 0, y = 0;
    ULONGLONG seen = 0;
};
static std::vector<Draw> pendingClouds;
static std::vector<CloudCaster> clouds;
static cube::World *cloudWorld = nullptr;
static double frameOriginX = 0, frameOriginY = 0;
static bool frameOriginKnown = false;
static unsigned lastCloudCasters = 0;
static void releaseCloudDraw(Draw &d) {
    drop(d.vb);
    drop(d.ib);
    drop(d.layout);
}
static void clearClouds() {
    for (auto &d : pendingClouds)
        releaseCloudDraw(d);
    pendingClouds.clear();
    for (auto &c : clouds)
        releaseCloudDraw(c.draw);
    clouds.clear();
    frameOriginKnown = false;
}
static void cloudDrift(float out[2]) {
    out[0] = out[1] = 0;
    auto module = GetModuleHandleW(L"WorldMotion.dll");
    auto query =
        module ? (BOOL(WINAPI *)(float *))GetProcAddress(module, "QueryCloudDrift") : nullptr;
    if (query)
        query(out);
}
static void commitClouds() {
    ULONGLONG now = GetTickCount64();
    for (auto it = clouds.begin(); it != clouds.end();)
        if (now - it->seen > 120000) {
            releaseCloudDraw(it->draw);
            it = clouds.erase(it);
        } else
            ++it;
    if (frameOriginKnown)
        for (auto &d : pendingClouds) {
            double x = frameOriginX + d.world[3], y = frameOriginY + d.world[7];
            auto found = std::find_if(clouds.begin(), clouds.end(), [&](const CloudCaster &c) {
                return c.draw.vb == d.vb && std::abs(c.x - x) < .1 && std::abs(c.y - y) < .1 &&
                       std::abs(c.draw.world[11] - d.world[11]) < .1;
            });
            if (found != clouds.end()) {
                releaseCloudDraw(found->draw);
                found->draw = d;
                found->x = x;
                found->y = y;
                found->seen = now;
                d.vb = d.ib = nullptr;
                d.layout = nullptr;
            } else if (clouds.size() < 96) {
                clouds.push_back({d, x, y, now});
                d.vb = d.ib = nullptr;
                d.layout = nullptr;
            }
        }
    for (auto &d : pendingClouds)
        releaseCloudDraw(d);
    pendingClouds.clear();
}

struct TerrainMesh {
    ID3D11Buffer *vb;
    ID3D11Buffer *ib;
    nativeScene::Chunk info;
    UINT count;
    DXGI_FORMAT format;
};
static std::vector<TerrainMesh> terrainMeshes;
static nativeScene::Chunks loadedChunks;
static cube::World *terrainWorld = nullptr;
static ULONGLONG terrainRefresh = 0;
static UINT lastTerrainCasters = 0;
static void clearTerrain() {
    for (auto &m : terrainMeshes) {
        drop(m.vb);
        drop(m.ib);
    }
    terrainMeshes.clear();
    loadedChunks.clear();
}
static void refreshTerrain(cube::World *w) {
    if (w != terrainWorld) {
        clearTerrain();
        terrainWorld = w;
        terrainRefresh = 0;
    }
    ULONGLONG now = GetTickCount64();
    if (!w || now - terrainRefresh < 500)
        return;
    nativeScene::Locks lock(w);
    if (!lock.held || !w->local_creature)
        return;
    terrainRefresh = now;
    double px = w->local_creature->entity_data.position.x / 65536.,
           py = w->local_creature->entity_data.position.y / 65536.;
    std::vector<TerrainMesh> next;
    nativeScene::Chunks nextChunks;
    size_t bytes = 0;
    double range = radius + 64;
    int zoneX = (int)std::floor(px / 64), zoneY = (int)std::floor(py / 64),
        steps = (int)std::ceil(range / 64) + 1;
    for (int ix = -steps; ix <= steps; ix++)
        for (int iy = -steps; iy <= steps; iy++) {
            auto found = w->zones.find(IntVector2(zoneX + ix, zoneY + iy));
            if (found == w->zones.end())
                continue;
            auto zone = found->second;
            if (!zone)
                continue;
            double x = double(zone->position.x) * 64, y = double(zone->position.y) * 64;
            double dx = std::max(x - px, std::max(px - (x + 64), 0.)),
                   dy = std::max(y - py, std::max(py - (y + 64), 0.));
            if (dx * dx + dy * dy > range * range)
                continue;
            for (auto cb : zone->chunk.chunkbuffers) {
                if (!cb || !cb->vertex_buffer || !cb->index_buffer)
                    continue;
                auto v = (gfx::D3D11VertexBuffer *)cb->vertex_buffer;
                auto ib = (gfx::D3D11IndexBuffer *)cb->index_buffer;
                if (!v->buffer || !ib->buffer)
                    continue;
                UINT count = *(UINT *)((unsigned char *)cb + 0x44);
                if (!count || count > 1000000)
                    continue;
                count *= 3;
                DXGI_FORMAT format = (DXGI_FORMAT)ib->format;
                if (format != DXGI_FORMAT_R16_UINT && format != DXGI_FORMAT_R32_UINT)
                    continue;
                D3D11_BUFFER_DESC vd{}, id{};
                v->buffer->GetDesc(&vd);
                ib->buffer->GetDesc(&id);
                if (count > id.ByteWidth / (format == DXGI_FORMAT_R16_UINT ? 2 : 4) ||
                    vd.ByteWidth % 8 || bytes + vd.ByteWidth + id.ByteWidth > 96 * 1024 * 1024 ||
                    next.size() >= 512)
                    continue;
                nativeScene::Chunk info{zone->position, *(int *)((unsigned char *)cb + 0x5c)};
                v->buffer->AddRef();
                ib->buffer->AddRef();
                next.push_back({v->buffer, ib->buffer, info, count, format});
                nextChunks.emplace(v->buffer, info);
                bytes += vd.ByteWidth + id.ByteWidth;
            }
        }
    clearTerrain();
    terrainMeshes.swap(next);
    loadedChunks.swap(nextChunks);
}
struct Params {
    float world[16], view[16], proj[16], light[16], config[4], eye[4], sun[4], points[80],
        lantern[4], sunTone[4], depthAdjust[4];
};
static void clearDraws() {
    for (auto &x : draws) {
        drop(x.vb);
        drop(x.ib);
        drop(x.layout);
    }
    draws.clear();
    drop(target);
    drop(depth);
}
static UINT bufferBytes(ID3D11Resource *r) {
    ID3D11Buffer *b = nullptr;
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Buffer), (void **)&b)))
        return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    b->Release();
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth <= 512 ? d.ByteWidth : 0;
}
// Rendering must never wait for a GPU readback. Missing constants skip one draw
// until the existing CPU Map/Update hooks observe the next native buffer write.
static bool readBuffer(ID3D11Buffer *b, void *out, UINT bytes) {
    if (!b)
        return false;
    UINT size = bytes;
    return SUCCEEDED(b->GetPrivateData(cacheGuid, &size, out)) && size == bytes;
}
struct Vec {
    float x, y, z;
};
static float dot(Vec a, Vec b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static Vec cross(Vec a, Vec b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static Vec norm(Vec a) {
    float n = sqrtf(dot(a, a));
    return n > 1e-6f ? Vec{a.x / n, a.y / n, a.z / n} : Vec{0, 0, 1};
}
static bool lightMatrix(Params &p, const Draw &d) {
    auto g = nativeScene::game();
    auto player = g && g->world ? g->world->local_creature : nullptr;
    if (frameOriginKnown && player && (player->entity_data.binary_toggles & 1)) {
        p.lantern[0] = (float)(player->entity_data.position.x / 65536. - frameOriginX);
        p.lantern[1] = (float)(player->entity_data.position.y / 65536. - frameOriginY);
        p.lantern[2] = (float)(player->entity_data.position.z / 65536. + 1.5);
        p.lantern[3] = 12.f;
    }
    memcpy(p.view, d.view, 64);
    memcpy(p.proj, d.proj, 64);
    Vec a{d.view[0], d.view[1], d.view[2]}, b{d.view[4], d.view[5], d.view[6]},
        c{d.view[8], d.view[9], d.view[10]};
    float det = dot(a, cross(b, c));
    if (fabsf(det) < 1e-8f)
        return false;
    Vec bc = cross(b, c), ca = cross(c, a), ab = cross(a, b);
    Vec eye{(-d.view[3] * bc.x - d.view[7] * ca.x - d.view[11] * ab.x) / det,
            (-d.view[3] * bc.y - d.view[7] * ca.y - d.view[11] * ab.y) / det,
            (-d.view[3] * bc.z - d.view[7] * ca.z - d.view[11] * ab.z) / det};
    Vec sun = norm({d.light[8], d.light[9], d.light[10]});
    float activeStrength = strength;
    // MoonLight shares this map rather than installing a second caster pass.
    auto moonModule = GetModuleHandleW(L"MoonandShadow.dll");
    if (!moonModule)
        moonModule = GetModuleHandleW(L"MoonLight.dll");
    MoonState moon{};
    auto query = moonModule
                     ? (BOOL(WINAPI *)(MoonState *))GetProcAddress(moonModule, "QueryMoonLight")
                     : nullptr;
    bool moonEnabled = query && query(&moon) && moon.version == 1;
    if (g && g->world && g->world->local_creature) {
        auto cycle = moonrules::shadows(g->world->state.time, strength,
                                        moonEnabled ? moon.shadowStrength : 0.f);
        sun = norm({cycle.direction[0], cycle.direction[1], cycle.direction[2]});
        activeStrength = cycle.strength;
    } else if (moonEnabled) {
        sun = norm({moon.direction[0], moon.direction[1], moon.direction[2]});
        activeStrength = moon.shadowStrength * moon.weight;
    }
    if (captureRequest.load() == 2)
        sun = norm({.5f, .25f, .8f});
    Vec f{-sun.x, -sun.y, -sun.z};
    Vec r = norm(cross(f, fabsf(f.z) > .95f ? Vec{0, 1, 0} : Vec{0, 0, 1})), u = cross(r, f);
    float snap = 2 * radius / resolution;
    float x = floorf(dot(r, eye) / snap + .5f) * snap, y = floorf(dot(u, eye) / snap + .5f) * snap;
    float span = cloudShadows ? 2048.f : 512.f;
    float m[16] = {r.x / radius,
                   r.y / radius,
                   r.z / radius,
                   -x / radius,
                   u.x / radius,
                   u.y / radius,
                   u.z / radius,
                   -y / radius,
                   f.x / span,
                   f.y / span,
                   f.z / span,
                   .5f - dot(f, eye) / span,
                   0,
                   0,
                   0,
                   1};
    memcpy(p.light, m, 64);
    p.eye[0] = eye.x;
    p.eye[1] = eye.y;
    p.eye[2] = eye.z;
    p.eye[3] = 2.f / span;
    p.sun[0] = sun.x;
    p.sun[1] = sun.y;
    p.sun[2] = sun.z;
    p.sun[3] = softShadows ? 1.f : 0.f;
    p.config[0] = 1.f / resolution;
    p.config[1] = activeStrength * fmaxf(0.f, fminf(1.f, sun.z / .15f));
    p.config[2] = bias / span;
    p.config[3] = radius;
    p.sunTone[1] = fadeStart;
    if (g && g->world) {
        auto moonPhase = moonrules::state(g->world->state.time);
        p.sunTone[0] = sunWarmth * (1 - moonPhase.weight) *
                       std::clamp(-moonPhase.direction[2] * 2.f, 0.f, 1.f);
    }
    return std::isfinite(eye.x) && std::isfinite(eye.y) && std::isfinite(eye.z);
}
// Keep active lights in native order. Empty light slots otherwise cost a
// branch on every receiver vertex, even when the scene has no local lights.
static void compactPointLights(Params &params) {
    unsigned count = 0;
    for (unsigned source = 0; source < 10; ++source) {
        if (!(params.points[source * 4 + 3] > 0))
            continue;
        if (source != count) {
            memcpy(params.points + count * 4, params.points + source * 4, 16);
            memcpy(params.points + 40 + count * 4, params.points + 40 + source * 4, 16);
        }
        ++count;
    }
    // Spare fields retain the constant-buffer ABI. The flag allows older
    // unprepared parameters in diagnostics to use the original ten-slot loop.
    params.depthAdjust[2] = (float)count;
    params.depthAdjust[3] = 1;
}
static const char *hlsl = R"HLSL(
cbuffer Params:register(b0){row_major float4x4 world,view,proj,light;float4 config,eye,sun;float4 pointPositions[10],pointColors[10],lantern,sunTone,depthAdjust;};
struct V{float4 p:SV_POSITION;float3 w:TEXCOORD0;float3 n:TEXCOORD1;float local:TEXCOORD2;};
float3 normal(float a){int n=(int)round(a*255);return n==0?float3(1,0,0):n==1?float3(-1,0,0):n==2?float3(0,1,0):n==3?float3(0,-1,0):n==4?float3(0,0,1):float3(0,0,-1);}
V caster(float4 p:POSITION){V o;float4 w=mul(world,float4(p.xyz*255,1));o.p=mul(light,w);o.w=w.xyz;o.n=0;o.local=0;return o;}
// Keep the native y-multiply then x/z/w MAD order. Without precise, FXC
// swaps the first two terms, producing angle-dependent coplanar triangles.
float4 combine(row_major float4x4 m,float4 r){precise float4 a=r.y*m[1];a=mad(m[0],r.x,a);a=mad(m[2],r.z,a);return mad(m[3],r.w,a);}
V receiver(float4 p:POSITION){V o;float4 v=float4(p.xyz*255,1);float4 w=mul(world,v);row_major float4x4 vw=float4x4(combine(world,view[0]),combine(world,view[1]),combine(world,view[2]),combine(world,view[3]));o.p=float4(dot(combine(vw,proj[0]),v),dot(combine(vw,proj[1]),v),dot(combine(vw,proj[2])-float4(0,0,0,depthAdjust.x),v),dot(combine(vw,proj[3]),v));o.w=w.xyz;o.n=normalize(mul((float3x3)world,normal(p.w)));o.local=0;
 // Match native point-light attenuation at vertices; no extra light shadow maps.
 int lightCount=depthAdjust.w>0?(int)depthAdjust.z:10;
 [loop]for(int j=0;j<lightCount;j++)if(pointPositions[j].w>0){
  float3 delta=o.w-pointPositions[j].xyz;float d2=dot(delta,delta);
  float falloff=saturate(1-d2/(pointPositions[j].w*pointPositions[j].w));
  float facing=clamp(-4*dot(o.n,delta*rsqrt(max(d2,.0001))),.3,1);
  o.local+=falloff*falloff*facing*max(pointColors[j].r,max(pointColors[j].g,pointColors[j].b));
 }o.local=saturate(o.local);return o;}
Texture2D<float> shadowMap:register(t0);SamplerComparisonState shadowSampler:register(s0);
float4 visibility(V i):SV_TARGET{float3 p=mul(light,float4(i.w,1)).xyz;float2 uv=float2(p.x,-p.y)*.5+.5;
 float facing=dot(normalize(i.n),sun.xyz);
 // Receiver-plane depth change across the comparison sampler footprint.
 float2 ux=ddx(uv),uy=ddy(uv);float zx=ddx(p.z),zy=ddy(p.z);
 float det=ux.x*uy.y-ux.y*uy.x;
 float slope=sqrt(saturate(1-facing*facing))/max(abs(facing),.1);
 float footprint=config.x*config.w*eye.w*min(slope,8);
 if(abs(det)>1e-12){float2 gradient=float2(zx*uy.y-zy*ux.y,zy*ux.x-zx*uy.x)/det;footprint=config.x*(abs(gradient.x)+abs(gradient.y));}
 if(any(uv<config.x)||any(uv>1-config.x)||p.z<0||p.z>1)return 0;
 // Small sprite faces need a stable normal-derived footprint. Screen
 // derivatives can mix adjacent faces across a pixel quad on these models.
 if(depthAdjust.y>0){
  float3 n=normalize(i.n);float denom=max(abs(dot(n,light[2].xyz)),1e-7);
  footprint=config.x*(abs(dot(n,light[0].xyz))+abs(dot(n,light[1].xyz)))/denom*config.w*config.w*eye.w*eye.w*.5;
 }
 float baseBias=depthAdjust.y>0?max(config.z,.1*eye.w*.5):config.z;
 float bias=baseBias+min(footprint,config.x*config.w*eye.w*16)*(sun.w>.5?2.1:1.1);float lit=0;
 [branch]if(sun.w>.5){
  [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)lit+=shadowMap.SampleCmpLevelZero(shadowSampler,uv+float2(x,y)*config.x,p.z-bias);
  lit/=9;
 }else lit=shadowMap.SampleCmpLevelZero(shadowSampler,uv,p.z-bias);
 // A face turned away from the directional light has no direct contribution.
 // Do not let its own depth samples produce alternating bright edge teeth.
 lit*=smoothstep(0,.08,facing);
 float fade=1-smoothstep(config.w*sunTone.y,config.w,length(i.w.xy-eye.xy));float opacity=(1-lit)*config.y*fade;
 float local=saturate(i.local);float lanternProtection=0;
 if(lantern.w>0){float3 delta=i.w-lantern.xyz;float d2=dot(delta,delta);
  float falloff=saturate(1-d2/(lantern.w*lantern.w));
  lanternProtection=saturate(4*falloff*falloff*clamp(-4*dot(normalize(i.n),delta*rsqrt(max(d2,.0001))),.3,1));
 }
 // Preserve the native lantern color. Synthetic fill tied to shadow visibility
 // stamped a second colored silhouette into the illuminated ground.
 float protection=max(local,lanternProtection);float alpha=opacity*(1-protection);
 float3 sunlight=float3(1,.65,.2)*sunTone.x*lit*fade*saturate(facing)*(1-local);
 return float4(sunlight,alpha);}
)HLSL";
static bool initialize(ID3D11DeviceContext *context) {
    if (device)
        return ctx == context && !failed;
    ctx = context;
    ctx->AddRef();
    ctx->GetDevice(&device);
    ID3D11Device1 *d1 = nullptr;
    bool ok = SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1)) &&
              SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1));
    D3D_FEATURE_LEVEL levels[] = {device->GetFeatureLevel()};
    if (ok)
        ok = SUCCEEDED(d1->CreateDeviceContextState(0, levels, 1, D3D11_SDK_VERSION,
                                                    __uuidof(ID3D11Device), nullptr, &state));
    drop(d1);
    const char *names[] = {"caster", "receiver", "visibility"};
    for (int i = 0; i < 3 && ok; i++) {
        ID3DBlob *code = nullptr, *error = nullptr;
        HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "SteamShadows", nullptr, nullptr, names[i],
                                i < 2 ? "vs_4_0" : "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &code, &error);
        if (error) {
            log((char *)error->GetBufferPointer());
            drop(error);
        }
        ok = SUCCEEDED(hr);
        if (ok)
            ok = SUCCEEDED(
                i < 2 ? device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                   nullptr, i ? &receiver : &caster)
                      : device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                  nullptr, &visibility));
        if (ok && i == 0) {
            D3D11_INPUT_ELEMENT_DESC element = {
                "POSITION", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
            ok = SUCCEEDED(device->CreateInputLayout(&element, 1, code->GetBufferPointer(),
                                                     code->GetBufferSize(), &nativeCasterLayout));
            element.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            if (ok)
                ok =
                    SUCCEEDED(device->CreateInputLayout(&element, 1, code->GetBufferPointer(),
                                                        code->GetBufferSize(), &floatCasterLayout));
        }
        drop(code);
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Params);
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.Usage = D3D11_USAGE_DEFAULT;
    if (ok)
        ok = SUCCEEDED(device->CreateBuffer(&cb, nullptr, &constants));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = resolution;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (ok)
        ok = SUCCEEDED(device->CreateTexture2D(&td, nullptr, &texture)) &&
             SUCCEEDED(device->CreateDepthStencilView(texture, &dd, &mapDepth)) &&
             SUCCEEDED(device->CreateShaderResourceView(texture, &sd, &mapView));
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (ok)
        ok = SUCCEEDED(device->CreateDepthStencilState(&ds, &writeDepth));
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (ok)
        ok = SUCCEEDED(device->CreateDepthStencilState(&ds, &testDepth));
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    rs.MultisampleEnable = TRUE;
    if (ok)
        ok = SUCCEEDED(device->CreateRasterizerState(&rs, &raster));
    // A two-ULP receiver-only depth offset prevents coplanar overlay fighting.
    rs.DepthBias = -2;
    rs.DepthBiasClamp = -0.000001f;
    if (ok)
        ok = SUCCEEDED(device->CreateRasterizerState(&rs, &receiverRaster));
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = 15;
    if (ok)
        ok = SUCCEEDED(device->CreateBlendState(&bd, &opaque));
    auto &rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = 7;
    if (ok)
        ok = SUCCEEDED(device->CreateBlendState(&bd, &blend));
    D3D11_SAMPLER_DESC ss{};
    ss.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    ss.BorderColor[0] = ss.BorderColor[1] = ss.BorderColor[2] = ss.BorderColor[3] = 1;
    ss.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    ss.MaxLOD = D3D11_FLOAT32_MAX;
    if (ok)
        ok = SUCCEEDED(device->CreateSamplerState(&ss, &sampler));
    failed = !ok;
    char status[256];
    sprintf_s(
        status,
        "D3D11 sun shadow resources ready (%u, radius=%.0f, loader API 7.3); loaded terrain casters and cached cloud casters enabled.",
        resolution, radius);
    log(ok ? status : "Resource initialization failed; original rendering preserved.");
    return ok;
}
static void capture(const char *path) {
    if (!target)
        return;
    ID3D11Resource *r = nullptr;
    target->GetResource(&r);
    ID3D11Texture2D *t = nullptr;
    r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t);
    drop(r);
    if (!t)
        return;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        drop(t);
        return;
    }
    ID3D11Texture2D *resolved = nullptr;
    if (d.SampleDesc.Count > 1) {
        auto rd = d;
        rd.SampleDesc.Count = 1;
        rd.SampleDesc.Quality = 0;
        rd.BindFlags = 0;
        if (SUCCEEDED(device->CreateTexture2D(&rd, nullptr, &resolved)))
            ctx->ResolveSubresource(resolved, 0, t, 0, d.Format);
    }
    ID3D11Texture2D *staging = nullptr;
    auto sd = d;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging))) {
        ctx->CopyResource(staging, resolved ? resolved : t);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(oldMap(ctx, staging, 0, D3D11_MAP_READ, 0, &m))) {
            FILE *f = nullptr;
            fopen_s(&f, path, "wb");
            if (f) {
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                fh.bfType = 0x4d42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                ih.biSize = sizeof(ih);
                ih.biWidth = d.Width;
                ih.biHeight = -(LONG)d.Height;
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                ih.biCompression = BI_RGB;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                std::vector<unsigned char> row(d.Width * 4);
                for (UINT y = 0; y < d.Height; y++) {
                    memcpy(row.data(), (char *)m.pData + y * m.RowPitch, row.size());
                    if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
                        for (UINT x = 0; x < d.Width; x++)
                            std::swap(row[x * 4], row[x * 4 + 2]);
                    fwrite(row.data(), 1, row.size(), f);
                }
                fclose(f);
            }
            oldUnmap(ctx, staging, 0);
        }
    }
    drop(staging);
    drop(resolved);
    drop(t);
}
// Motion can execute inside our draw hook, after capture. Resolve its mesh
// at replay time for receivers as well as casters, independent of load order.
static ID3D11Buffer *animatedTerrain(const Draw &x) {
    if (!x.terrain || !x.vb)
        return nullptr;
    ID3D11Buffer *source = x.vb;
    nativeScene::ChunkTag tag{};
    UINT tagBytes = sizeof(tag);
    if (SUCCEEDED(source->GetPrivateData(nativeScene::chunkTagGuid, &tagBytes, &tag)) &&
        tagBytes == sizeof(tag) && tag.original)
        source = tag.original;
    ID3D11Buffer *animated = nullptr;
    UINT bytes = sizeof(animated);
    if (FAILED(source->GetPrivateData(nativeScene::animatedGuid, &bytes, &animated)))
        return nullptr;
    return animated; // GetPrivateData retains interface; caller releases it.
}
// This cache is valid only while we own the context state during shadow replay.
// Native draws and diagnostic draws outside that pass always bind explicitly.
struct ReplayBindings {
    ID3D11DeviceContext *context = nullptr;
    ID3D11Buffer *vertex = nullptr, *index = nullptr;
    ID3D11InputLayout *layout = nullptr;
    UINT stride = 0, vertexOffset = 0, indexOffset = 0;
    DXGI_FORMAT format{};
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    Params parameters{};
    bool haveVertex = false, haveIndex = false, haveLayout = false;
    bool haveTopology = false, haveParameters = false;
    UINT uploads = 0, stateChanges = 0;
};
static ReplayBindings replayBindings;

static void bind(const Draw &x, Params &p) {
    memcpy(p.world, x.world, 64);
    auto &bindings = replayBindings;
    if (internal && bindings.context != ctx) {
        bindings = {};
        bindings.context = ctx;
    }
    if (!internal || !bindings.haveParameters || memcmp(&bindings.parameters, &p, sizeof(p)) != 0) {
        oldUpdate(ctx, constants, 0, nullptr, &p, 0, 0);
        if (internal) {
            bindings.parameters = p;
            bindings.haveParameters = true;
            ++bindings.uploads;
        }
    }
    auto animated = animatedTerrain(x);
    auto vb = animated ? animated : x.vb;
    UINT stride = animated ? 20 : x.stride, offset = animated ? 0 : x.offset;
    auto layout = animated ? floatCasterLayout : x.layout;
    if (!internal || !bindings.haveVertex || bindings.vertex != vb || bindings.stride != stride ||
        bindings.vertexOffset != offset) {
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        if (internal) {
            bindings.vertex = vb;
            bindings.stride = stride;
            bindings.vertexOffset = offset;
            bindings.haveVertex = true;
            ++bindings.stateChanges;
        }
    }
    if (!internal || !bindings.haveIndex || bindings.index != x.ib || bindings.format != x.format ||
        bindings.indexOffset != x.ibOffset) {
        ctx->IASetIndexBuffer(x.ib, x.format, x.ibOffset);
        if (internal) {
            bindings.index = x.ib;
            bindings.format = x.format;
            bindings.indexOffset = x.ibOffset;
            bindings.haveIndex = true;
            ++bindings.stateChanges;
        }
    }
    if (!internal || !bindings.haveLayout || bindings.layout != layout) {
        ctx->IASetInputLayout(layout);
        if (internal) {
            bindings.layout = layout;
            bindings.haveLayout = true;
            ++bindings.stateChanges;
        }
    }
    if (!internal || !bindings.haveTopology || bindings.topology != x.topology) {
        ctx->IASetPrimitiveTopology(x.topology);
        if (internal) {
            bindings.topology = x.topology;
            bindings.haveTopology = true;
            ++bindings.stateChanges;
        }
    }
    if (x.indexed)
        oldDI(ctx, x.count, x.start, x.base);
    else
        oldD(ctx, x.count, x.start);
    drop(animated);
}
static void render() {
    if (draws.empty())
        return;
    if (finished || !enabled || failed || !target || !depth) {
        if (enabled && diagnosticRejects++ < 8) {
            char s[256];
            sprintf_s(s,
                      "Shadow scene rejected: finished=%d failed=%d target=%p depth=%p meshes=%zu",
                      finished, failed, target, depth, draws.size());
            log(s);
        }
        clearDraws();
        return;
    }
    finished = true;
    Params p{};
    if (!lightMatrix(p, draws.front())) {
        clearDraws();
        return;
    }
    if (p.config[1] <= 0 && captureRequest.load() == 0) {
        clearDraws();
        return;
    }
    LARGE_INTEGER begin{}, end{}, freq{};
    QueryPerformanceCounter(&begin);
    QueryPerformanceFrequency(&freq);
    internal = true;
    bool shot = captureRequest.exchange(0) != 0;
    if (shot)
        capture("Mods\\SteamShadows-before.bmp");
    ID3DDeviceContextState *saved = nullptr;
    gpuTimer.begin(device, ctx, "Mods\\SteamShadows.log", "Shadows (map/application)");
    ctx1->SwapDeviceContextState(state, &saved);
    replayBindings = {};
    D3D11_VIEWPORT originalViewport = sceneViewport;
    ctx->RSSetState(raster);
    ctx->VSSetConstantBuffers(0, 1, &constants);
    ctx->PSSetConstantBuffers(0, 1, &constants);
    ctx->VSSetShader(caster, nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ctx->OMSetBlendState(opaque, nullptr, ~0u);
    ctx->OMSetDepthStencilState(writeDepth, 0);
    ctx->OMSetRenderTargets(0, nullptr, mapDepth);
    oldClear(ctx, mapDepth, D3D11_CLEAR_DEPTH, 1, 0);
    D3D11_VIEWPORT vp{0, 0, (float)resolution, (float)resolution, 0, 1};
    ctx->RSSetViewports(1, &vp);
    // Derive the current floating world origin from a visible native terrain chunk.
    auto reference =
        std::find_if(draws.begin(), draws.end(), [](const Draw &d) { return d.terrain; });
    lastTerrainCasters = 0;
    if (reference != draws.end() && !terrainMeshes.empty()) {
        double ox = double(reference->terrainInfo.zone.x) * 64 - reference->world[3],
               oy = double(reference->terrainInfo.zone.y) * 64 - reference->world[7];
        for (auto &m : terrainMeshes) {
            Draw x{};
            x.vb = m.vb;
            x.ib = m.ib;
            x.count = m.count;
            x.format = m.format;
            x.stride = 8;
            x.layout = nativeCasterLayout;
            x.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            x.world[0] = x.world[5] = x.world[10] = x.world[15] = 1;
            x.world[3] = (float)(double(m.info.zone.x) * 64 - ox);
            x.world[7] = (float)(double(m.info.zone.y) * 64 - oy);
            x.world[11] = (float)m.info.z;
            ID3D11Buffer *animated = nullptr;
            UINT bytes = sizeof(animated);
            if (SUCCEEDED(m.vb->GetPrivateData(nativeScene::animatedGuid, &bytes, &animated)) &&
                animated) {
                x.vb = animated;
                x.stride = 20;
                x.layout = floatCasterLayout;
            }
            bind(x, p);
            drop(animated);
            lastTerrainCasters++;
        }
        for (auto &x : draws)
            if (!x.terrain)
                bind(x, p);
    } else
        for (auto &x : draws)
            bind(x, p);
    // Clouds cast only: never replay them as darkened scene receivers.
    lastCloudCasters = 0;
    if (cloudShadows && frameOriginKnown) {
        float drift[2]{};
        cloudDrift(drift);
        for (auto &cached : clouds) {
            Draw x = cached.draw;
            x.world[3] = (float)(cached.x - frameOriginX) + drift[0];
            x.world[7] = (float)(cached.y - frameOriginY) + drift[1];
            float extent = 24 * (std::max)(std::abs(x.world[0]),
                                           (std::max)(std::abs(x.world[5]), std::abs(x.world[10])));
            float lx = p.light[0] * x.world[3] + p.light[1] * x.world[7] +
                       p.light[2] * x.world[11] + p.light[3],
                  ly = p.light[4] * x.world[3] + p.light[5] * x.world[7] +
                       p.light[6] * x.world[11] + p.light[7];
            if (std::abs(lx) > 1 + extent / radius || std::abs(ly) > 1 + extent / radius)
                continue;
            bind(x, p);
            ++lastCloudCasters;
        }
    }
    gpuTimer.split(ctx);
    ctx->RSSetState(receiverRaster);
    ctx->OMSetRenderTargets(1, &target, depth);
    ctx->OMSetDepthStencilState(testDepth, 0);
    ctx->OMSetBlendState(blend, nullptr, ~0u);
    ctx->RSSetViewports(1, &originalViewport);
    ctx->VSSetShader(receiver, nullptr, 0);
    ctx->PSSetShader(visibility, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &mapView);
    ctx->PSSetSamplers(0, 1, &sampler);
    for (auto &x : draws) {
        memcpy(p.view, x.view, 64);
        memcpy(p.proj, x.proj, 64);
        memcpy(p.points, x.pointLights, sizeof(p.points));
        compactPointLights(p);
        p.depthAdjust[0] = x.depthOffset;
        float modelScale =
            sqrtf(x.world[0] * x.world[0] + x.world[1] * x.world[1] + x.world[2] * x.world[2]);
        p.depthAdjust[1] = !x.terrain && modelScale < .5f ? 1.f : 0.f;
        bind(x, p);
    }
    ID3D11ShaderResourceView *nullView = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullView);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx1->SwapDeviceContextState(saved, nullptr);
    gpuTimer.end(ctx);
    replayBindings = {};
    drop(saved);
    if (shot)
        capture("Mods\\SteamShadows-after.bmp");
    internal = false;
    lastCount = (unsigned)draws.size();
    frames++;
    QueryPerformanceCounter(&end);
    totalMs += 1000. * (end.QuadPart - begin.QuadPart) / freq.QuadPart;
    if (frames == 1 || shot || frames % 3600 == 0) {
        char s[512];
        sprintf_s(
            s,
            "Shadow frame %u: receivers=%u, loaded terrain casters=%u, CPU average=%.3f ms, eye=(%.3f %.3f %.3f), sun=(%.3f %.3f %.3f)%s",
            frames, lastCount, lastTerrainCasters, totalMs / frames, p.eye[0], p.eye[1], p.eye[2],
            p.sun[0], p.sun[1], p.sun[2], shot ? ", captured before/after" : "");
        log(s);
    }
    clearDraws();
}
static bool record(ID3D11DeviceContext *c, UINT count, UINT start, INT base, bool indexed) {
    if (internal || failed || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;
    if (ctx && ctx != c)
        return false;
    auto cloudGame = nativeScene::game();
    auto nativeCloudWorld = cloudGame ? cloudGame->world : nullptr;
    if (nativeCloudWorld != cloudWorld) {
        clearClouds();
        cloudWorld = nativeCloudWorld;
    }
    if (cloudShadows && enabled && nativeCloudWorld && pendingClouds.size() < 96) {
        Draw cloud{};
        c->IAGetVertexBuffers(0, 1, &cloud.vb, &cloud.stride, &cloud.offset);
        if (nativeScene::cloud(nativeCloudWorld, cloud.vb)) {
            ID3D11Buffer *matrices[3]{};
            c->VSGetConstantBuffers(0, 3, matrices);
            bool valid = readBuffer(matrices[0], cloud.proj, 64) &&
                         readBuffer(matrices[2], cloud.world, 64) && std::abs(cloud.proj[15]) < .5f;
            float capturedDrift[2]{};
            UINT driftBytes = sizeof(capturedDrift);
            if (matrices[2])
                matrices[2]->GetPrivateData(nativeScene::cloudDriftGuid, &driftBytes,
                                            capturedDrift);
            for (auto &b : matrices)
                drop(b);
            c->IAGetIndexBuffer(&cloud.ib, &cloud.format, &cloud.ibOffset);
            c->IAGetInputLayout(&cloud.layout);
            c->IAGetPrimitiveTopology(&cloud.topology);
            cloud.count = count;
            cloud.start = start;
            cloud.base = base;
            cloud.indexed = indexed;
            cloud.world[3] -= capturedDrift[0];
            cloud.world[7] -= capturedDrift[1];
            if (valid && cloud.vb && cloud.layout && (cloud.stride == 8 || cloud.stride == 20) &&
                (!indexed || cloud.ib) && cloud.topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
                pendingClouds.push_back(cloud);
            else
                releaseCloudDraw(cloud);
            return false;
        }
        drop(cloud.vb);
    }
    ID3D11Buffer *buffers[6]{};
    c->VSGetConstantBuffers(0, 6, buffers);
    UINT expected[] = {64, 64, 64, 16, 320, 48};
    bool world = true;
    for (int i = 0; i < 6; i++) {
        D3D11_BUFFER_DESC d{};
        if (buffers[i])
            buffers[i]->GetDesc(&d);
        if (d.ByteWidth != expected[i])
            world = false;
    }
    if (!world) {
        for (auto &b : buffers)
            drop(b);
        // Sky and auxiliary draws can use another shader while retaining the main
        // scene attachments. Only flush when the game leaves the captured depth target.
        if (!draws.empty()) {
            ID3D11DepthStencilView *active = nullptr;
            c->OMGetRenderTargets(0, nullptr, &active);
            bool end = active != depth;
            drop(active);
            if (end)
                render();
        }
        return false;
    }
    if (!initialize(c) || finished || !enabled || draws.size() >= 4096) {
        for (auto &b : buffers)
            drop(b);
        return false;
    }
    Draw x{};
    bool ok = readBuffer(buffers[0], x.proj, 64) && readBuffer(buffers[1], x.view, 64) &&
              readBuffer(buffers[2], x.world, 64) && readBuffer(buffers[5], x.light, 48);
    readBuffer(buffers[4], x.pointLights, 320);
    for (auto &b : buffers)
        drop(b);
    if (!ok)
        return false;
    if (fabsf(x.proj[15]) > .5f) {
        render();
        return false;
    } // Orthographic HUD/minimap models are not world receivers.
    if (fabsf(x.view[3]) + fabsf(x.view[7]) + fabsf(x.view[11]) < .01f)
        return false; // Sky/preview cameras do not finish the world pass.
    // Terrain uses 64-block local meshes. Conservatively reject meshes outside
    // the useful receiver region before acquiring their GPU resources.
    bool unit = fabsf(x.world[0] - 1) < .001f && fabsf(x.world[5] - 1) < .001f &&
                fabsf(x.world[10] - 1) < .001f;
    float dx = unit ? std::max(x.world[3], std::max(-x.world[3] - 64.f, 0.f)) : x.world[3];
    float dy = unit ? std::max(x.world[7], std::max(-x.world[7] - 64.f, 0.f)) : x.world[7];
    float workRange = radius + (unit ? 32.f : 80.f);
    if (dx * dx + dy * dy > workRange * workRange)
        return false;
    if (captureRequest.load() && draws.empty() && !captureLogged) {
        captureLogged = true;
        char s[1024];
        sprintf_s(
            s,
            "Capture scene matrices: proj=[%.5f %.5f %.5f %.5f; %.5f %.5f %.5f %.5f; %.5f %.5f %.5f %.5f; %.5f %.5f %.5f %.5f] world scale=[%.5f %.5f %.5f], translation=[%.3f %.3f %.3f]",
            x.proj[0], x.proj[1], x.proj[2], x.proj[3], x.proj[4], x.proj[5], x.proj[6], x.proj[7],
            x.proj[8], x.proj[9], x.proj[10], x.proj[11], x.proj[12], x.proj[13], x.proj[14],
            x.proj[15], x.world[0], x.world[5], x.world[10], x.world[3], x.world[7], x.world[11]);
        log(s);
    }
    ID3D11BlendState *bs = nullptr;
    float factor[4];
    UINT mask;
    c->OMGetBlendState(&bs, factor, &mask);
    D3D11_BLEND_DESC bd{};
    if (bs) {
        bs->GetDesc(&bd);
        drop(bs);
    }
    if (bd.RenderTarget[0].BlendEnable) {
        if (captureRequest.load() && diagnosticRejects++ < 8)
            log("Skipped blended scene draw.");
        return false;
    }
    c->IAGetVertexBuffers(0, 1, &x.vb, &x.stride, &x.offset);
    c->IAGetIndexBuffer(&x.ib, &x.format, &x.ibOffset);
    c->IAGetInputLayout(&x.layout);
    c->IAGetPrimitiveTopology(&x.topology);
    if (!x.vb || !x.layout || (indexed && !x.ib) ||
        x.topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        drop(x.vb);
        drop(x.ib);
        drop(x.layout);
        return false;
    }
    auto game = nativeScene::game();
    auto nativeWorld = game ? game->world : nullptr;
    refreshTerrain(nativeWorld);
    if (nativeScene::cloud(nativeWorld, x.vb)) {
        drop(x.vb);
        drop(x.ib);
        drop(x.layout);
        return false;
    }
    // Alternate cameras and clouds are excluded before comparing scene views.
    // Pitch-dependent draw ordering must not finish an incomplete shadow pass.
    if (!draws.empty() && memcmp(x.view, draws.front().view, 64) != 0) {
        drop(x.vb);
        drop(x.ib);
        drop(x.layout);
        return false;
    }
    ID3D11Buffer *source = x.vb;
    nativeScene::ChunkTag tag{};
    UINT tagBytes = sizeof(tag);
    if (SUCCEEDED(x.vb->GetPrivateData(nativeScene::chunkTagGuid, &tagBytes, &tag)) &&
        tagBytes == sizeof(tag))
        source = tag.original;
    auto found = loadedChunks.find(source);
    if (found != loadedChunks.end() && fabsf(x.world[0] - 1) < .001f &&
        fabsf(x.world[5] - 1) < .001f && fabsf(x.world[10] - 1) < .001f) {
        x.terrain = true;
        x.terrainInfo = found->second;
        frameOriginX = double(x.terrainInfo.zone.x) * 64 - x.world[3];
        frameOriginY = double(x.terrainInfo.zone.y) * 64 - x.world[7];
        frameOriginKnown = true;
    }
    ID3D11Buffer *offsetBuffer = nullptr;
    c->VSGetConstantBuffers(8, 1, &offsetBuffer);
    float offsetValues[4]{};
    if (offsetBuffer && readBuffer(offsetBuffer, offsetValues, 16))
        x.depthOffset = offsetValues[0];
    drop(offsetBuffer);
    D3D11_VIEWPORT vp{};
    UINT vpCount = 1;
    c->RSGetViewports(&vpCount, &vp);
    if (!vpCount || vp.Width < 640 || vp.Height < 360 || vp.TopLeftX != 0 || vp.TopLeftY != 0) {
        if (captureRequest.load() && diagnosticRejects++ < 8) {
            char s[256];
            sprintf_s(s, "Skipped viewport %.0fx%.0f at %.0f,%.0f", vp.Width, vp.Height,
                      vp.TopLeftX, vp.TopLeftY);
            log(s);
        }
        drop(x.vb);
        drop(x.ib);
        drop(x.layout);
        return false;
    }
    if (draws.empty()) {
        c->OMGetRenderTargets(1, &target, &depth);
        sceneViewport = vp;
    }
    x.count = count;
    x.start = start;
    x.base = base;
    x.indexed = indexed;
    draws.push_back(x);
    return true;
}
static void STDMETHODCALLTYPE drawIndexed(ID3D11DeviceContext *c, UINT n, UINT s, INT b) {
    record(c, n, s, b, true);
    oldDI(c, n, s, b);
}
static void STDMETHODCALLTYPE draw(ID3D11DeviceContext *c, UINT n, UINT s) {
    record(c, n, s, 0, false);
    oldD(c, n, s);
}
static HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT s,
                                     D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *m) {
    HRESULT hr = oldMap(c, r, s, type, flags, m);
    if (!internal && SUCCEEDED(hr) && s == 0 && type != D3D11_MAP_READ) {
        UINT size = bufferBytes(r);
        if (size)
            mapped[r] = {m->pData, size};
    }
    return hr;
}
static void STDMETHODCALLTYPE unmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT s) {
    auto it = mapped.find(r);
    if (it != mapped.end()) {
        r->SetPrivateData(cacheGuid, it->second.bytes, it->second.data);
        mapped.erase(it);
    }
    oldUnmap(c, r, s);
}
static void STDMETHODCALLTYPE update(ID3D11DeviceContext *c, ID3D11Resource *r, UINT s,
                                     const D3D11_BOX *box, const void *data, UINT row, UINT slice) {
    if (!internal && !box && s == 0 && data) {
        UINT size = bufferBytes(r);
        if (size)
            r->SetPrivateData(cacheGuid, size, data);
    }
    oldUpdate(c, r, s, box, data, row, slice);
}
static void STDMETHODCALLTYPE clear(ID3D11DeviceContext *c, ID3D11DepthStencilView *v, UINT flags,
                                    FLOAT z, UINT8 stencil) {
    if (!internal && (!ctx || ctx == c) && (flags & D3D11_CLEAR_DEPTH)) {
        render();
        clearDraws();
        finished = false;
        if (toggleRequest.exchange(0) || GetAsyncKeyState(VK_F7) & 1) {
            enabled = !enabled;
            log(enabled ? "Shadows enabled." : "Shadows disabled.");
        }
        if (disableRequest.exchange(0))
            enabled = false;
    }
    oldClear(c, v, flags, z, stencil);
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
static bool install(ID3D11DeviceContext *c) {
    void **v = *(void ***)c;
    if (table)
        return table == v;
    struct Hook {
        UINT slot;
        void *fn;
        void **original;
    };
    Hook hooks[] = {
        {12, (void *)drawIndexed, (void **)&oldDI}, {13, (void *)draw, (void **)&oldD},
        {14, (void *)map, (void **)&oldMap},        {15, (void *)unmap, (void **)&oldUnmap},
        {48, (void *)update, (void **)&oldUpdate},  {53, (void *)clear, (void **)&oldClear}};
    for (auto &h : hooks)
        *h.original = v[h.slot];
    int done = 0;
    for (auto &h : hooks) {
        if (!patch(v + h.slot, h.fn)) {
            while (done--) {
                auto &undo = hooks[done];
                patch(v + undo.slot, *undo.original);
            }
            return false;
        }
        done++;
    }
    table = v;
    log("D3D11 mesh capture hooks attached; F7 toggles shadows.");
    return true;
}
static DWORD attach() {
    fadeStart = std::clamp((int)GetPrivateProfileIntW(L"Shadows", L"FadeStartPercent", 40,
                                                      L".\\Mods\\SteamShadows.ini"),
                           10, 90) *
                .01f;
    sunWarmth = std::clamp((int)GetPrivateProfileIntW(L"Shadows", L"SunWarmthPercent", 3,
                                                      L".\\Mods\\SteamShadows.ini"),
                           0, 10) *
                .01f;
    softShadows =
        GetPrivateProfileIntW(L"Shadows", L"SoftShadows", 0, L".\\Mods\\SteamShadows.ini") != 0;
    cloudShadows =
        GetPrivateProfileIntW(L"Shadows", L"CloudShadows", 1, L".\\Mods\\SteamShadows.ini") != 0;
    resolution =
        GetPrivateProfileIntW(L"Shadows", L"Resolution", 4096, L".\\Mods\\SteamShadows.ini");
    resolution = resolution < 512 ? 512 : resolution > 4096 ? 4096 : resolution;
    radius = (float)GetPrivateProfileIntW(L"Shadows", L"Radius", 120, L".\\Mods\\SteamShadows.ini");
    radius = radius < 8 ? 8 : radius > 128 ? 128 : radius;
    strength =
        GetPrivateProfileIntW(L"Shadows", L"StrengthPercent", 48, L".\\Mods\\SteamShadows.ini") *
        .01f;
    strength = strength < 0 ? 0 : strength > 1 ? 1 : strength;
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &d, nullptr, &c);
    bool ok = SUCCEEDED(hr) && install(c);
    drop(c);
    drop(d);
    return ok ? 1 : 0;
}
} // namespace shadows
#ifndef MOON_AND_SHADOW
class SteamShadows : public GenericMod {
  public:
    void Initialize() override {
        shadows::attach();
    }
    void OnPresent(IDXGISwapChain *, UINT, UINT) override {
        shadows::render();
        shadows::commitClouds();
        shadows::clearDraws();
        shadows::frameOriginKnown = false;
        shadows::finished = false;
    }
};
static_assert(sizeof(GenericMod) == 0x428, "Steam mod loader 7.3 callback layout required");
EXPORT GenericMod *MakeMod() {
    return new SteamShadows();
}
// This graphics-only mod uses its own CRT allocation; it calls no game allocators.
EXPORT void ModPreInitialize() {}
EXPORT int ModMajorVersion() {
    return 7;
}
EXPORT int ModMinorVersion() {
    return 3;
}
#endif
extern "C" __declspec(dllexport) DWORD WINAPI AttachSteamShadows(void *) {
    return shadows::attach();
}
extern "C" __declspec(dllexport) DWORD WINAPI ToggleSteamShadows(void *) {
    shadows::toggleRequest = 1;
    return 1;
}
extern "C" __declspec(dllexport) DWORD WINAPI DisableSteamShadows(void *) {
    shadows::disableRequest = 1;
    return 1;
}
extern "C" __declspec(dllexport) DWORD WINAPI CaptureSteamShadows(void *) {
    shadows::captureLogged = false;
    shadows::captureRequest = 1;
    return 1;
}
// One captured frame with a high sun, for validating the pass at dusk/night.
extern "C" __declspec(dllexport) DWORD WINAPI PreviewSteamShadows(void *) {
    shadows::captureLogged = false;
    shadows::captureRequest = 2;
    return 1;
}
