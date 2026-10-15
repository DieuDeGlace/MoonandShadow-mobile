// SteamShadows: real-time directional sun/moon shadows for Cube World (Steam,
// 64-bit, D3D11). Built on its own or as part of MoonandShadow (MOON_AND_SHADOW).
//
// Loaded native terrain casts even outside the camera frustum; visible meshes receive.
//
// Per frame:
//   * Map/Unmap/UpdateSubresource hooks keep a CPU copy (private data) of every
//     small constant buffer, so draw matrices can be read without GPU readback.
//   * Draw/DrawIndexed hooks record() every opaque native world draw (and cache
//     cloud draws separately).
//   * When the world pass ends (HUD draw, depth clear or present), render():
//       1. replays all loaded terrain chunks, recorded non-terrain draws and
//          cached clouds into a light-space depth map ("caster" pass);
//       2. replays the recorded draws over the scene with the "visibility"
//          shader, darkening shadowed pixels and adding a little sun warmth.
//   * F7 toggles shadows. MoonLight's moon direction is used at night.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "../NativeScene.h"
#include "../MoonLight/MoonRules.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include "../GpuPassTimer.h"
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>

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

// ===========================================================================
// Constants and state
// ===========================================================================

// ID3D11DeviceContext vtable slots.
constexpr UINT SLOT_DRAW_INDEXED = 12, SLOT_DRAW = 13, SLOT_MAP = 14, SLOT_UNMAP = 15,
               SLOT_UPDATE_SUBRESOURCE = 48, SLOT_CLEAR_DEPTH_STENCIL_VIEW = 53;
// Vertex-shader constant buffer slots of a native world draw.
constexpr UINT CB_PROJECTION = 0, CB_VIEW = 1, CB_WORLD = 2, CB_MATERIAL = 3, CB_POINT_LIGHTS = 4,
               CB_LIGHT = 5;
// The native world shader also binds four 16-byte buffers at b6-b9: ambient
// colour, white flash, depth offset and glow.
constexpr UINT CB_EXTRA = 6;
// Offset of a chunk buffer's triangle count (not described by CWSDK).
constexpr size_t CHUNK_BUFFER_TRIANGLES = 0x44;
constexpr size_t MAX_RECORDED_DRAWS = 4096, MAX_CLOUDS = 96, MAX_TERRAIN_MESHES = 512;
constexpr size_t TERRAIN_BYTES_LIMIT = 96 * 1024 * 1024;

// Private-data key for the CPU copy of each constant buffer (shared with MoonLight).
static const GUID cacheGuid = nativeScene::constantCacheGuid;
static bool internal = false; // Our own draws are in progress.
static bool enabled = true, finished = false, failed = false;
// Requests from the exported control functions.
static std::atomic<int> toggleRequest{0}, captureRequest{0}, disableRequest{0};
// Settings (see SteamShadows.ini).
static bool cloudShadows = true, softShadows = false;
static UINT resolution = 4096;
static float radius = 120, strength = 1.f, bias = .008f, sunWarmth = .03f, fadeStart = .4f;
// Fade by distance from the camera (like Omega: zooming out shortens the
// shadow reach) instead of by ground distance from the camera's map position.
static bool cameraFade = true;
// GPU objects.
static ID3D11Device *device = nullptr;
static ID3D11DeviceContext *ctx = nullptr;
static ID3D11DeviceContext1 *ctx1 = nullptr;
static ID3DDeviceContextState *state = nullptr; // Isolated pipeline state for our passes.
static ID3D11VertexShader *caster = nullptr, *receiver = nullptr;
static ID3D11PixelShader *visibility = nullptr, *modelVisibility = nullptr;
// Direct-light receivers (used for all scene draws): the vertex shader also
// estimates the native direct-sunlight share; terrain and models differ only
// in their pixel shader (models add the receiver-plane filter).
static ID3D11VertexShader *modelReceiver = nullptr;
static ID3D11PixelShader *terrainVisibility = nullptr;
static ID3D11Buffer *constants = nullptr;
// Caster input layouts for native (UBYTE4) and animated (float4) terrain vertices.
static ID3D11InputLayout *nativeCasterLayout = nullptr, *floatCasterLayout = nullptr;
// Near shadow cascade: a sharper map covering `nearRadius` around the camera.
static bool cascadesEnabled = true;
static UINT nearResolution = 2048;
static float nearRadius = 32;
static ID3D11Texture2D *nearTexture = nullptr;
static ID3D11DepthStencilView *nearDepth = nullptr;
static ID3D11ShaderResourceView *nearView = nullptr;
static ID3D11Texture2D *texture = nullptr; // The (far) shadow map.
static ID3D11DepthStencilView *mapDepth = nullptr;
static ID3D11ShaderResourceView *mapView = nullptr;
static ID3D11DepthStencilState *writeDepth = nullptr, *testDepth = nullptr;
static ID3D11RasterizerState *raster = nullptr, *receiverRaster = nullptr;
static ID3D11BlendState *blend = nullptr, *modelBlend = nullptr, *opaque = nullptr;
static ID3D11SamplerState *sampler = nullptr, *rawSampler = nullptr;
// The scene target/depth captured with the first recorded draw of the frame.
static ID3D11RenderTargetView *target = nullptr;
static ID3D11DepthStencilView *depth = nullptr;
static D3D11_VIEWPORT sceneViewport{};
// Statistics and diagnostics.
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
// Replay and constant uploads bypass hooks installed below this one. Null
// falls back to the old* chain (tests, standalone builds).
static DI replayDI;
static D replayD;
static Map rawMap;
static Unmap rawUnmap;
static void **table = nullptr; // Hooked context vtable (set once installed).
// Sparse CPU cost of the per-draw capture hook, sampled every 60th frame.
static bool hookTimingFrame = false;
static unsigned hookPresents = 0, hookSamples = 0, hookDraws = 0;
static LONGLONG hookTicks = 0;
struct Mapped {
    void *data;
    UINT bytes;
};
static thread_local std::unordered_map<ID3D11Resource *, Mapped> mapped; // Currently mapped.

// ===========================================================================
// Recorded draws and cached clouds
// ===========================================================================

// One recorded draw with everything needed to replay it. Holds references on
// vb/ib/layout.
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
    float material[4] = {1, 1, 1, 1};
    float ambient[4] = {.3f, .3f, .3f, 0};
    float lightProtection = 0;
    float depthOffset = 0;
};
static std::vector<Draw> draws; // This frame's receivers.

// A cloud cached in absolute world coordinates (clouds are often not drawn every
// frame, but should keep casting shadows).
struct CloudCaster {
    Draw draw{};
    double x = 0, y = 0;
    ULONGLONG seen = 0;
};
static std::vector<Draw> pendingClouds; // Cloud draws seen this frame.
static std::vector<CloudCaster> clouds;
static cube::World *cloudWorld = nullptr;
// World position of the render origin (Cube World renders relative to the player).
static double frameOriginX = 0, frameOriginY = 0;
static bool frameOriginKnown = false;
static unsigned lastCloudCasters = 0;

static void releaseDraw(Draw &d) {
    drop(d.vb);
    drop(d.ib);
    drop(d.layout);
}
static void clearClouds() {
    for (auto &d : pendingClouds)
        releaseDraw(d);
    pendingClouds.clear();
    for (auto &c : clouds)
        releaseDraw(c.draw);
    clouds.clear();
    frameOriginKnown = false;
}
// Current cloud drift applied by WorldMotion.dll (zero without it).
static void cloudDrift(float out[2]) {
    out[0] = out[1] = 0;
    auto module = GetModuleHandleW(L"WorldMotion.dll");
    auto query =
        module ? (BOOL(WINAPI *)(float *))GetProcAddress(module, "QueryCloudDrift") : nullptr;
    if (query)
        query(out);
}
// End of frame: expires clouds unseen for 2 minutes and moves this frame's cloud
// draws into the cache (replacing the same cloud at the same position).
static void commitClouds() {
    ULONGLONG now = GetTickCount64();
    for (auto it = clouds.begin(); it != clouds.end();)
        if (now - it->seen > 120000) {
            releaseDraw(it->draw);
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
                releaseDraw(found->draw);
                found->draw = d;
                found->x = x;
                found->y = y;
                found->seen = now;
                d.vb = d.ib = nullptr;
                d.layout = nullptr;
            } else if (clouds.size() < MAX_CLOUDS) {
                clouds.push_back({d, x, y, now});
                d.vb = d.ib = nullptr;
                d.layout = nullptr;
            }
        }
    for (auto &d : pendingClouds)
        releaseDraw(d);
    pendingClouds.clear();
}

// ===========================================================================
// Loaded terrain (shadow casters beyond the camera view)
// ===========================================================================

struct TerrainMesh {
    ID3D11Buffer *vb;
    ID3D11Buffer *ib;
    nativeScene::Chunk info;
    UINT count; // Index count.
    DXGI_FORMAT format;
};
static std::vector<TerrainMesh> terrainMeshes;
static nativeScene::Chunks loadedChunks; // Native vertex buffer -> chunk.
static cube::World *terrainWorld = nullptr;
static ULONGLONG terrainRefresh = 0;
static UINT lastTerrainCasters = 0, lastModelCasters = 0, lastReceivers = 0;
static void clearTerrain() {
    for (auto &m : terrainMeshes) {
        drop(m.vb);
        drop(m.ib);
    }
    terrainMeshes.clear();
    loadedChunks.clear();
}

// Every 500 ms: collects the chunk meshes of all loaded zones within
// radius + 64 blocks of the player (bounded to 512 meshes / 96 MiB).
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
                UINT count = *(UINT *)((unsigned char *)cb + CHUNK_BUFFER_TRIANGLES);
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
                    vd.ByteWidth % 8 || bytes + vd.ByteWidth + id.ByteWidth > TERRAIN_BYTES_LIMIT ||
                    next.size() >= MAX_TERRAIN_MESHES)
                    continue;
                nativeScene::Chunk info{
                    zone->position, *(int *)((unsigned char *)cb + nativeScene::CHUNK_BUFFER_Z)};
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

// The shader constant buffer (layout matches cbuffer Params in the HLSL).
struct Params {
    float world[16], view[16], proj[16], light[16], config[4], eye[4], sun[4], points[80],
        lantern[4], sunTone[4], depthAdjust[4], nearLight[16], cascade[4], modelFront[4],
        modelBack[4], modelAmbient[4], modelDirection[4];
};
static void clearDraws() {
    for (auto &x : draws)
        releaseDraw(x);
    draws.clear();
    drop(target);
    drop(depth);
}
// Size of `r` if it is a constant buffer of at most 512 bytes, otherwise 0.
static UINT bufferBytes(ID3D11Resource *r) {
    return nativeScene::cachedConstantBytes(r);
}
// Rendering must never wait for a GPU readback. Missing constants skip one draw
// until the existing CPU Map/Update hooks observe the next native buffer write.
static bool readBuffer(ID3D11Buffer *b, void *out, UINT bytes) {
    if (!b)
        return false;
    UINT size = bytes;
    return SUCCEEDED(b->GetPrivateData(cacheGuid, &size, out)) && size == bytes;
}

// ===========================================================================
// Light-space setup
// ===========================================================================

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
// Cast daytime shadows from the game's own light direction (SunFromGame).
static bool sunFromGame = true;
static Vec lastClockSun{0, 0, 1}; // Clock-driven direction, for the diagnostic log.

// MoonLight's current state, if MoonandShadow/MoonLight is loaded and the moon is up.
static bool queryMoon(MoonState &moon) {
    auto moonModule = GetModuleHandleW(L"MoonandShadow.dll");
    if (!moonModule)
        moonModule = GetModuleHandleW(L"MoonLight.dll");
    auto query = moonModule
                     ? (BOOL(WINAPI *)(MoonState *))GetProcAddress(moonModule, "QueryMoonLight")
                     : nullptr;
    return query && query(&moon) && moon.version == 1;
}

// Fills the per-frame parts of `p` from the reference draw `d`: camera, the
// player's lantern, the sun (or moon) direction and the orthographic light
// matrix centred on the camera. Returns false if the camera cannot be recovered.
static bool lightMatrix(Params &p, const Draw &d) {
    auto g = nativeScene::game();
    auto player = g && g->world ? g->world->local_creature : nullptr;
    // The lantern protects the area around the player from shadow.
    if (frameOriginKnown && player && (player->entity_data.binary_toggles & 1)) {
        p.lantern[0] = (float)(player->entity_data.position.x / 65536. - frameOriginX);
        p.lantern[1] = (float)(player->entity_data.position.y / 65536. - frameOriginY);
        p.lantern[2] = (float)(player->entity_data.position.z / 65536. + 1.5);
        p.lantern[3] = 12.f;
    }
    memcpy(p.view, d.view, 64);
    memcpy(p.proj, d.proj, 64);
    // Camera position = -R^-1 * t, from the view matrix rows (via cofactors).
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
    MoonState moon{};
    bool moonEnabled = queryMoon(moon);
    Vec native = sun;
    if (g && g->world && g->world->local_creature) {
        auto cycle = moonrules::shadows(g->world->state.time, strength,
                                        moonEnabled ? moon.shadowStrength : 0.f);
        sun = norm({cycle.direction[0], cycle.direction[1], cycle.direction[2]});
        activeStrength = cycle.strength;
        lastClockSun = sun;
        // By day the game shades faces with its own light direction. Casting
        // from the clock-driven sun instead darkened faces the game lights
        // (and the grazing rule judged faces against the wrong angle), so
        // follow the native direction while it is above the horizon.
        bool day = moonrules::state(g->world->state.time).direction[2] < 0;
        if (sunFromGame && day && native.z > .02f) {
            sun = native;
            activeStrength = strength * moonrules::horizonFade(native.z);
        }
    } else if (moonEnabled) {
        sun = norm({moon.direction[0], moon.direction[1], moon.direction[2]});
        activeStrength = moon.shadowStrength * moon.weight * moonrules::horizonFade(sun.z);
    }
    if (captureRequest.load() == 2) // PreviewSteamShadows: a fixed high sun.
        sun = norm({.5f, .25f, .8f});
    // Light basis (right, up, forward), snapped to whole shadow-map texels.
    Vec f{-sun.x, -sun.y, -sun.z};
    Vec r = norm(cross(f, fabsf(f.z) > .95f ? Vec{0, 1, 0} : Vec{0, 0, 1})), u = cross(r, f);
    float snap = 2 * radius / resolution;
    float x = floorf(dot(r, eye) / snap + .5f) * snap, y = floorf(dot(u, eye) / snap + .5f) * snap;
    float span = cloudShadows ? 2048.f : 512.f; // Depth range; clouds need more.
    // clang-format off
    float m[16] = {
        r.x / radius, r.y / radius, r.z / radius, -x / radius,
        u.x / radius, u.y / radius, u.z / radius, -y / radius,
        f.x / span,   f.y / span,   f.z / span,   .5f - dot(f, eye) / span,
        0,            0,            0,            1};
    // clang-format on
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
    // Already faded near the horizon (moonrules::horizonFade).
    p.config[1] = activeStrength;
    p.config[2] = bias / span;
    p.config[3] = radius;
    p.sunTone[1] = fadeStart;
    p.sunTone[2] = cameraFade ? 1.f : 0.f;
    if (g && g->world) {
        // Warm sunlight only by day, strongest when the sun is high.
        auto moonPhase = moonrules::state(g->world->state.time);
        p.sunTone[0] = sunWarmth * (1 - moonPhase.weight) *
                       std::clamp(-moonPhase.direction[2] * 2.f, 0.f, 1.f) *
                       moonrules::horizonFade(-moonPhase.direction[2]);
    }
    if (cascadesEnabled) {
        float nearSnap = 2 * nearRadius / nearResolution;
        float nx = floorf(dot(r, eye) / nearSnap + .5f) * nearSnap;
        float ny = floorf(dot(u, eye) / nearSnap + .5f) * nearSnap;
        memcpy(p.nearLight, p.light, 64);
        for (int i = 0; i < 3; ++i) {
            p.nearLight[i] *= radius / nearRadius;
            p.nearLight[4 + i] *= radius / nearRadius;
        }
        p.nearLight[3] = -nx / nearRadius;
        p.nearLight[7] = -ny / nearRadius;
        p.cascade[0] = 1.f / nearResolution;
        p.cascade[1] = nearRadius;
        p.cascade[2] = nearRadius * .8f;
        p.cascade[3] = 1;
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

// ===========================================================================
// Shaders
// ===========================================================================

// caster:     light-space depth of terrain/models/clouds.
// receiver:   reproduces the native vertex transform exactly (see combine) and
//             accumulates point-light influence per vertex.
// visibility: compares against the shadow maps (near cascade close to the
//             camera, far map beyond, blended between) and outputs
//             premultiplied sun warmth (rgb) and shadow opacity (alpha).
// Direct-light variants (MODEL_DIRECT_LIGHT), used for every scene receiver:
//   modelReceiverVertex also estimates which share of the native light is
//   direct sunlight (same formula as the game's world vertex shader), and
//   visibility writes a second blend source that dims only that share. Models
//   additionally compile with MODEL_PLANE_FILTER (receiver-plane filtering).
// Params: config = (texel size, strength, bias, radius), eye.w = 2/depth span,
// sun.w = soft shadows, sunTone = (warmth, fade start, camera fade), depthAdjust = (native
// depth offset, small-entity flag, light count, light count valid),
// nearLight = near-cascade light matrix, cascade = (near texel size, near
// radius, blend start, near enabled), model* = native model lighting
// (front colour + material alpha, back colour, ambient, direction + light
// protection).
static const char *hlsl = R"HLSL(
cbuffer Params:register(b0){row_major float4x4 world,view,proj,light;float4 config,eye,sun;float4 pointPositions[10],pointColors[10],lantern,sunTone,depthAdjust;row_major float4x4 nearLight;float4 cascade,modelFront,modelBack,modelAmbient,modelDirection;};
struct V{
    float4 p:SV_POSITION;
    float3 w:TEXCOORD0;
    float3 n:TEXCOORD1;
    float local:TEXCOORD2;
#ifdef MODEL_DIRECT_LIGHT
    float3 directFraction:TEXCOORD3; // Share of native light that is direct sunlight.
#endif
};

// Alpha (w) of a native vertex holds the voxel face id (0..5).
float3 normal(float a){int n=(int)round(a*255);return n==0?float3(1,0,0):n==1?float3(-1,0,0):n==2?float3(0,1,0):n==3?float3(0,-1,0):n==4?float3(0,0,1):float3(0,0,-1);}

V caster(float4 p:POSITION){
    V o=(V)0;
    float4 w=mul(world,float4(p.xyz*255,1));
    o.p=mul(light,w);
    o.w=w.xyz;
    return o;
}

// Keep the native y-multiply then x/z/w MAD order. Without precise, FXC
// swaps the first two terms, producing angle-dependent coplanar triangles.
float4 combine(row_major float4x4 m,float4 r){precise float4 a=r.y*m[1];a=mad(m[0],r.x,a);a=mad(m[2],r.z,a);return mad(m[3],r.w,a);}

V receiver(float4 p:POSITION){
    V o;
    float4 v=float4(p.xyz*255,1);
    float4 w=mul(world,v);
    row_major float4x4 vw=float4x4(combine(world,view[0]),combine(world,view[1]),combine(world,view[2]),combine(world,view[3]));
    o.p=float4(dot(combine(vw,proj[0]),v),dot(combine(vw,proj[1]),v),dot(combine(vw,proj[2])-float4(0,0,0,depthAdjust.x),v),dot(combine(vw,proj[3]),v));
    o.w=w.xyz;
    o.n=normalize(mul((float3x3)world,normal(p.w)));
    o.local=0;
#ifdef MODEL_DIRECT_LIGHT
    // Omega carries world position directly from the same transform as its
    // caster and uses inverse-transpose model normals. Axis normals allow this
    // compact cofactor form, including mirrored and nonuniform transforms.
    float3 a=float3(world[0].x,world[1].x,world[2].x);
    float3 b=float3(world[0].y,world[1].y,world[2].y);
    float3 c=float3(world[0].z,world[1].z,world[2].z);
    float3 n=normal(p.w);
    float handedness=dot(a,cross(b,c))<0?-1:1;
    o.n=normalize((cross(b,c)*n.x+cross(c,a)*n.y+cross(a,b)*n.z)*handedness);
    float3 localColor=0;
#endif
    // Match native point-light attenuation at vertices; no extra light shadow maps.
    int lightCount=depthAdjust.w>0?(int)depthAdjust.z:10;
    [loop]for(int j=0;j<lightCount;j++)if(pointPositions[j].w>0){
        float3 delta=o.w-pointPositions[j].xyz;
        float d2=dot(delta,delta);
        float falloff=saturate(1-d2/(pointPositions[j].w*pointPositions[j].w));
        float facing=clamp(-4*dot(o.n,delta*rsqrt(max(d2,.0001))),.3,1);
        o.local+=falloff*falloff*facing*max(pointColors[j].r,max(pointColors[j].g,pointColors[j].b));
#ifdef MODEL_DIRECT_LIGHT
        localColor+=falloff*falloff*facing*pointColors[j].rgb;
#endif
    }
    o.local=saturate(o.local);
#ifdef MODEL_DIRECT_LIGHT
    // Separate native sunlight from ambient/back/local light. A shadow may
    // remove sunlight; it must not multiply all native illumination by 0.25.
    float orientation=.5+.5*abs(dot(o.n,float3(.597022,.398015,.696526)));
    float facing=max(0,.3+.7*dot(o.n,modelDirection.xyz));
    float3 back=modelBack.rgb*orientation;
    float3 ambient=modelAmbient.rgb*orientation+localColor;
    float3 full=saturate(ambient+saturate(back+modelFront.rgb*facing)*modelFront.w);
    float3 indirect=saturate(ambient+saturate(back)*modelFront.w);
    o.directFraction=saturate((full-indirect)/max(full,.00001))*(1-modelDirection.w);
#endif
    return o;
}

V modelReceiverVertex(float4 p:POSITION){return receiver(p);}

Texture2D<float> shadowMap:register(t0);
Texture2D<float> nearShadowMap:register(t1);
SamplerComparisonState shadowSampler:register(s0);

#ifdef MODEL_PLANE_FILTER
SamplerState rawShadowSampler:register(s1);

// Hardware PCF compares four depths to one reference. On a tilted model face,
// that shadows alternating portions of the face's own texels. A single gather
// keeps the same 2x2 footprint but compares each texel to its receiver plane.
float planeComparison(Texture2D<float> map, float2 uv, float z, float2 gradient,
                      float texel, float baseBias, float2 offset) {
    float2 grid = (uv + offset * texel) / texel - .5;
    float2 fraction = frac(grid);
    float2 lower = floor(grid);
    float2 delta = (lower + .5) * texel - uv;
    float reference = z + dot(gradient, delta) - baseBias;
    // Gather order is lower-left, lower-right, upper-right, upper-left.
    // Gather at the quad's center so hardware coordinate rounding cannot
    // select a neighboring quad at a subtexel boundary. Weights stay unsnapped.
    float4 depths = map.Gather(rawShadowSampler, (lower + 1) * texel);
    float4 references = reference + texel * float4(gradient.y, gradient.x + gradient.y,
                                                  gradient.x, 0);
    float4 taps = step(references, depths);
    return lerp(lerp(taps.w, taps.z, fraction.x), lerp(taps.x, taps.y, fraction.x), fraction.y);
}

// Whether a world position projects inside the given map (with a 2-texel border).
bool modelMapCovered(float3 worldPosition, row_major float4x4 transform, float texel) {
    float3 p = mul(transform, float4(worldPosition, 1)).xyz;
    return all(abs(p.xy) < 1 - 2 * texel) && p.z >= 0 && p.z <= 1;
}

float modelPlaneVisibility(V i, row_major float4x4 transform, float4 region, Texture2D<float> map) {
    // At noon the light grazes vertical voxel faces. Their shadow projection
    // is thinner than a texel, so nearby faces can alternate across the filter
    // footprint. Give those faces one consistent directional shade instead.
    // Native ambient/back/local light is retained by the model blend below.
    float facing = dot(normalize(i.n), sun.xyz);
    float exposure = smoothstep(.15, .30, facing);
    [branch] if (exposure <= 0)
        return 0;
    if (!modelMapCovered(i.w, transform, region.x))
        return exposure;
    float3 p = mul(transform, float4(i.w, 1)).xyz;
    float2 uv = float2(p.x, -p.y) * .5 + .5;
    // Signed plane gradient in shadow UV coordinates. The orthographic rows
    // carry their own scale; eye.w is twice the inverse light-depth span.
    float denominator = dot(i.n, transform[2].xyz);
    denominator = (denominator < 0 ? -1 : 1) * max(abs(denominator), eye.w * .0005);
    float factor = region.w * region.w * eye.w * eye.w * .5 / denominator;
    float2 gradient = float2(-dot(i.n, transform[0].xyz), dot(i.n, transform[1].xyz)) * factor;
    float baseBias = region.z * (1 + 2 * (1 - saturate(facing)));
    [branch] if (sun.w > .5) {
        float lit = 0;
        [unroll] for (int y = -1; y <= 1; y++)
            [unroll] for (int x = -1; x <= 1; x++)
                lit += planeComparison(map, uv, p.z, gradient, region.x, baseBias, float2(x, y));
        return exposure * lit / 9;
    }
    return exposure * planeComparison(map, uv, p.z, gradient, region.x, baseBias, 0);
}
#endif

// Sun visibility (1 = lit) from one shadow map. region = (texel size, strength,
// bias, radius) of that map, laid out like `config`.
float shadowVisibility(V i,row_major float4x4 lightTransform,float4 region,Texture2D<float> map){
#ifdef MODEL_PLANE_FILTER
    return modelPlaneVisibility(i,lightTransform,region,map);
#else
    float facing=dot(normalize(i.n),sun.xyz);
    // Terrain faces the light only grazes (vertical cliff sides near noon)
    // project thinner than a shadow texel and self-shadow in stripes. Like
    // models, give them one consistent directional shade; faces lit at more
    // than ~17 degrees (ground, tops) keep normal occluder shadows.
    float exposure=depthAdjust.y>0?1:smoothstep(.15,.30,facing);
    float3 p=mul(lightTransform,float4(i.w,1)).xyz;
    float2 uv=float2(p.x,-p.y)*.5+.5;
    // Receiver-plane depth change across the comparison sampler footprint.
    // Derivatives come before any early return: a 2x2 quad can span faces
    // with different exposure (cliff edges), where they would be undefined.
    float2 ux=ddx(uv),uy=ddy(uv);
    float zx=ddx(p.z),zy=ddy(p.z);
    [branch]if(exposure<=0)return 0;
    float det=ux.x*uy.y-ux.y*uy.x;
    float slope=sqrt(saturate(1-facing*facing))/max(abs(facing),.1);
    float footprint=region.x*region.w*eye.w*min(slope,8);
    if(abs(det)>1e-12){float2 gradient=float2(zx*uy.y-zy*ux.y,zy*ux.x-zx*uy.x)/det;footprint=region.x*(abs(gradient.x)+abs(gradient.y));}
    if(any(uv<region.x)||any(uv>1-region.x)||p.z<0||p.z>1)return 0;
    // Small sprite faces need a stable normal-derived footprint. Screen
    // derivatives can mix adjacent faces across a pixel quad on these models.
    // Slope-scaled base bias (as models use) for faces just past the grazing band.
    float baseBias=region.z*(1+2*(1-saturate(facing)));
    if(depthAdjust.y>0)
        footprint=0;
    float bias=baseBias+min(footprint,region.x*region.w*eye.w*16)*(sun.w>.5?2.1:1.1);
    float lit=0;
    [branch]if(sun.w>.5){
        // Soft shadows: 3x3 comparison taps.
        [unroll]for(int y=-1;y<=1;y++)
            [unroll]for(int x=-1;x<=1;x++)
                lit+=map.SampleCmpLevelZero(shadowSampler,uv+float2(x,y)*region.x,p.z-bias);
        lit/=9;
    }else lit=map.SampleCmpLevelZero(shadowSampler,uv,p.z-bias);
    // Faces turned away or grazing have little or no direct contribution; this
    // also keeps their own depth samples from producing bright edge teeth.
    return lit*exposure;
#endif
}

#ifdef MODEL_DIRECT_LIGHT
struct ModelOutput { float4 color:SV_TARGET0; float4 transmission:SV_TARGET1; };
ModelOutput visibility(V i) {
#else
float4 visibility(V i):SV_TARGET{
#endif
    // Shadows end at config.w (Radius): ground distance from the camera's map
    // position, or with camera fade (sunTone.z) the full distance to the camera.
    float3 offset=i.w-eye.xyz;
    float distance=length(offset.xy);
    float fade=1-smoothstep(config.w*sunTone.y,config.w,length(float3(offset.xy,offset.z*sunTone.z)));
    // Fully faded pixels are unchanged: skip the shadow-map samples.
    [branch]if(fade<=0){
#ifdef MODEL_DIRECT_LIGHT
        ModelOutput none;
        none.color=0;
        none.transmission=1;
        return none;
#else
        return 0;
#endif
    }
    float facing=dot(normalize(i.n),sun.xyz);
    float lit;
    // Near cascade inside cascade.z, far map beyond cascade.y, blended between.
    bool useNear=cascade.w>.5;
#ifdef MODEL_PLANE_FILTER
    // Tall models inside the XY radius can lie outside the projected near map.
    // Use the far map there instead of applying a false dark border.
    useNear=useNear&&modelMapCovered(i.w,nearLight,cascade.x);
#endif
    [branch]if(useNear&&distance<cascade.z)lit=shadowVisibility(i,nearLight,float4(cascade.x,config.y,config.z,cascade.y),nearShadowMap);
    else{
        lit=shadowVisibility(i,light,config,shadowMap);
        [branch]if(useNear&&distance<cascade.y){
            float nearLit=shadowVisibility(i,nearLight,float4(cascade.x,config.y,config.z,cascade.y),nearShadowMap);
            lit=lerp(nearLit,lit,smoothstep(cascade.z,cascade.y,distance));
        }
    }
    float opacity=(1-lit)*config.y*fade;
    float local=saturate(i.local);
    float lanternProtection=0;
    if(lantern.w>0){
        float3 delta=i.w-lantern.xyz;
        float d2=dot(delta,delta);
        float falloff=saturate(1-d2/(lantern.w*lantern.w));
        lanternProtection=saturate(4*falloff*falloff*clamp(-4*dot(normalize(i.n),delta*rsqrt(max(d2,.0001))),.3,1));
    }
    // Preserve the native lantern color. Synthetic fill tied to shadow visibility
    // stamped a second colored silhouette into the illuminated ground.
    float protection=max(local,lanternProtection);
    float alpha=opacity*(1-protection);
    float3 sunlight=float3(1,.65,.2)*sunTone.x*lit*fade*saturate(facing)*(1-local);
#ifdef MODEL_DIRECT_LIGHT
    // Two source colors blend into the same target: sunlight is added as before,
    // while the transmission factor attenuates only the native sunlight share.
    ModelOutput output;
    output.color=float4(sunlight,0);
    output.transmission=float4(saturate(1-alpha*i.directFraction),1);
    return output;
#else
    return float4(sunlight,alpha);
#endif
}
)HLSL";

// Creates every GPU resource on first use (for context `context`). Returns
// false if creation failed or a different context is used later.
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
    // Shaders, in order: shadow caster VS, plain receiver VS and visibility PS
    // (kept for the filtering tests), the direct-light receiver VS, the model
    // visibility PS and the terrain visibility PS (same source, different macros).
    const char *names[] = {"caster",     "receiver",   "visibility",
                           "modelReceiverVertex", "visibility", "visibility"};
    const D3D_SHADER_MACRO modelMacros[] = {
        {"MODEL_PLANE_FILTER", "1"}, {"MODEL_DIRECT_LIGHT", "1"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO terrainMacros[] = {{"MODEL_DIRECT_LIGHT", "1"}, {nullptr, nullptr}};
    for (int i = 0; i < 6 && ok; ++i) {
        ID3DBlob *code = nullptr, *error = nullptr;
        bool vertexShader = i < 2 || i == 3;
        const D3D_SHADER_MACRO *macros = i == 5 ? terrainMacros : i >= 3 ? modelMacros : nullptr;
        // The model pixel shader uses Texture2D.Gather, which needs shader model 4.1.
        const char *profile = vertexShader ? "vs_4_0" : i == 4 ? "ps_4_1" : "ps_4_0";
        HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "SteamShadows", macros, nullptr, names[i],
                                profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
        if (error) {
            log((char *)error->GetBufferPointer());
            drop(error);
        }
        ok = SUCCEEDED(hr);
        if (ok)
            ok = SUCCEEDED(
                vertexShader
                    ? device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                 nullptr,
                                                 i == 3   ? &modelReceiver
                                                 : i == 1 ? &receiver
                                                          : &caster)
                    : device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                nullptr,
                                                i == 5   ? &terrainVisibility
                                                : i == 4 ? &modelVisibility
                                                         : &visibility));
        if (ok && i == 0) {
            // Caster layouts: native terrain (UBYTE4 as UNORM) and animated terrain (float4).
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
    // Replay uploads once per draw. Discard-renaming avoids the driver copy and
    // ordering that UpdateSubresource inserts for a buffer the GPU still reads.
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (ok)
        ok = SUCCEEDED(device->CreateBuffer(&cb, nullptr, &constants));
    // Shadow map: 32-bit depth, also readable as a float texture.
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
    if (ok && cascadesEnabled) {
        td.Width = td.Height = nearResolution;
        ok = SUCCEEDED(device->CreateTexture2D(&td, nullptr, &nearTexture)) &&
             SUCCEEDED(device->CreateDepthStencilView(nearTexture, &dd, &nearDepth)) &&
             SUCCEEDED(device->CreateShaderResourceView(nearTexture, &sd, &nearView));
    }
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
    // Receiver blend: colour = warmth + scene * (1 - opacity); destination alpha kept.
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
    // Models keep native ambient/back/point lighting. The pixel shader emits
    // a per-channel sunlight attenuation multiplier for the existing image
    // (dual-source blend: colour = sunlight + scene * transmission).
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_SRC1_COLOR;
    if (ok)
        ok = SUCCEEDED(device->CreateBlendState(&bd, &modelBlend));
    // Hardware 2x2 PCF; outside the map counts as lit. `rawSampler` (point, no
    // comparison) feeds the model plane filter's Gather.
    D3D11_SAMPLER_DESC ss{};
    ss.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    ss.BorderColor[0] = ss.BorderColor[1] = ss.BorderColor[2] = ss.BorderColor[3] = 1;
    ss.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    ss.MaxLOD = D3D11_FLOAT32_MAX;
    if (ok)
        ok = SUCCEEDED(device->CreateSamplerState(&ss, &sampler));
    ss.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (ok)
        ok = SUCCEEDED(device->CreateSamplerState(&ss, &rawSampler));
    failed = !ok;
    char status[256];
    sprintf_s(
        status,
        "D3D11 sun shadow resources ready (%u, radius=%.0f, loader API 7.3); loaded terrain casters and cached cloud casters enabled.",
        resolution, radius);
    log(ok ? status : "Resource initialization failed; original rendering preserved.");
    if (ok)
        log("Model shadows: direct world transforms, inverse-transpose normals, sunlight-only attenuation.");
    if (ok && cascadesEnabled) {
        sprintf_s(
            status,
            "Experimental cascades active: near=%u radius=%.0f, far=%u radius=%.0f; one receiver pass.",
            nearResolution, nearRadius, resolution, radius);
        log(status);
    }
    return ok;
}

// Saves the scene target as a 32-bit BMP (diagnostics for CaptureSteamShadows).
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
                fh.bfType = 0x4d42; // "BM"
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
                    if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) // BMP wants BGRA.
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

// ===========================================================================
// Replay
// ===========================================================================

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

static void uploadParams(const Params &p) {
    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = rawMap ? rawMap(ctx, constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)
                        : ctx->Map(constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr))
        return;
    memcpy(m.pData, &p, sizeof(p));
    if (rawUnmap)
        rawUnmap(ctx, constants, 0);
    else
        ctx->Unmap(constants, 0);
}
// Uploads `p` (with x's world matrix), binds x's geometry and draws it. During
// our own passes (internal), unchanged state is not re-sent.
static void bind(const Draw &x, Params &p) {
    memcpy(p.world, x.world, 64);
    auto &bindings = replayBindings;
    if (internal && bindings.context != ctx) {
        bindings = {};
        bindings.context = ctx;
    }
    if (!internal || !bindings.haveParameters || memcmp(&bindings.parameters, &p, sizeof(p)) != 0) {
        uploadParams(p);
        if (internal) {
            bindings.parameters = p;
            bindings.haveParameters = true;
            ++bindings.uploads;
        }
    }
    // Animated terrain (WorldMotion) uses 20-byte float vertices.
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
        (replayDI ? replayDI : oldDI)(ctx, x.count, x.start, x.base);
    else
        (replayD ? replayD : oldD)(ctx, x.count, x.start);
    drop(animated);
}

// Conservative world box of a native mesh. Shaders expand normalized positions
// by 255. Terrain chunks span 64 blocks horizontally (plus a block for motion)
// and 0..255 vertically; models use +/-255 so signed formats stay covered.
static void drawBounds(const Draw &draw, float center[3], float extent[3]) {
    float horizontal = draw.terrain ? 32.f : 0.f, vertical = draw.terrain ? 127.5f : 0.f;
    float halfHorizontal = draw.terrain ? 33.f : 255.f, halfVertical = draw.terrain ? 127.5f : 255.f;
    for (int row = 0; row < 3; ++row) {
        const float *m = draw.world + row * 4;
        center[row] = (m[0] + m[1]) * horizontal + m[2] * vertical + m[3];
        extent[row] = (std::abs(m[0]) + std::abs(m[1])) * halfHorizontal +
                      std::abs(m[2]) * halfVertical;
    }
}
// Project a caster's conservative bounds into the light map. Tall chunks can
// cast into the near region from well outside its ground radius; casters
// wholly outside the orthographic map cannot write any of its texels.
static bool intersectsMap(const Draw &draw, const Params &parameters) {
    float center[3], half[3];
    drawBounds(draw, center, half);
    for (int row : {0, 4}) {
        const float *m = parameters.light + row;
        float projected = m[0] * center[0] + m[1] * center[1] + m[2] * center[2] + m[3];
        float extent = std::abs(m[0]) * half[0] + std::abs(m[1]) * half[1] +
                       std::abs(m[2]) * half[2];
        if (projected + extent < -1 || projected - extent > 1)
            return false;
    }
    return true;
}
// Receiver fade reaches zero at the shadow radius (ground distance, or full
// camera distance with CameraFade), where the receiver leaves the scene color
// unchanged. Skipping wholly faded receivers is exact.
static bool beyondFade(const Draw &draw, const Params &parameters) {
    float center[3], half[3];
    drawBounds(draw, center, half);
    float d[3];
    for (int i = 0; i < 3; ++i)
        d[i] = std::max(std::abs(center[i] - parameters.eye[i]) - half[i], 0.f);
    d[2] *= parameters.sunTone[2];
    float limit = parameters.config[3] + 1;
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > limit * limit;
}

// Caster pass, terrain: every loaded chunk (positioned relative to the render
// origin derived from `reference`), then the recorded non-terrain draws.
// Casters that cannot reach the current map are skipped.
static void renderTerrainCasters(const Draw &reference, Params &p, bool nearPass) {
    double ox = double(reference.terrainInfo.zone.x) * 64 - reference.world[3],
           oy = double(reference.terrainInfo.zone.y) * 64 - reference.world[7];
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
        if (intersectsMap(x, p)) {
            bind(x, p);
            lastTerrainCasters++;
        }
        drop(animated);
    }
    for (auto &x : draws)
        if (!x.terrain && intersectsMap(x, p)) {
            bind(x, p);
            ++lastModelCasters;
        }
}

// Caster pass, clouds: cached clouds at their current (drifted) position,
// skipped when entirely outside the current map (`passRadius` half-width).
// Clouds cast only: never replay them as darkened scene receivers.
static void renderCloudCasters(Params &p, float passRadius) {
    float drift[2]{};
    cloudDrift(drift);
    for (auto &cached : clouds) {
        Draw x = cached.draw;
        x.world[3] = (float)(cached.x - frameOriginX) + drift[0];
        x.world[7] = (float)(cached.y - frameOriginY) + drift[1];
        float extent = 24 * (std::max)(std::abs(x.world[0]),
                                       (std::max)(std::abs(x.world[5]), std::abs(x.world[10])));
        float lx = p.light[0] * x.world[3] + p.light[1] * x.world[7] + p.light[2] * x.world[11] +
                   p.light[3],
              ly = p.light[4] * x.world[3] + p.light[5] * x.world[7] + p.light[6] * x.world[11] +
                   p.light[7];
        if (std::abs(lx) > 1 + extent / passRadius || std::abs(ly) > 1 + extent / passRadius)
            continue;
        bind(x, p);
        ++lastCloudCasters;
    }
}

// Caster pass into one shadow map: the far map (cascade 0) or the near map
// (cascade 1). `p.light` must already hold that map's light matrix.
static void renderCasters(Params &p, unsigned cascade) {
    bool nearPass = cascade != 0;
    UINT passResolution = nearPass ? nearResolution : resolution;
    float passRadius = nearPass ? nearRadius : radius;
    ctx->OMSetRenderTargets(0, nullptr, nearPass ? nearDepth : mapDepth);
    oldClear(ctx, nearPass ? nearDepth : mapDepth, D3D11_CLEAR_DEPTH, 1, 0);
    D3D11_VIEWPORT vp{0, 0, (float)passResolution, (float)passResolution, 0, 1};
    ctx->RSSetViewports(1, &vp);
    // Derive the current floating world origin from a visible native terrain chunk.
    auto reference =
        std::find_if(draws.begin(), draws.end(), [](const Draw &d) { return d.terrain; });
    lastTerrainCasters = 0;
    if (reference != draws.end() && !terrainMeshes.empty())
        renderTerrainCasters(*reference, p, nearPass);
    else
        for (auto &x : draws)
            if (intersectsMap(x, p)) {
                bind(x, p);
                ++lastModelCasters;
            }
    lastCloudCasters = 0;
    if (cloudShadows && frameOriginKnown)
        renderCloudCasters(p, passRadius);
}

// Receiver pass: every recorded draw again. Every receiver removes only the
// native direct-sunlight share where it is shadowed, so at full strength a
// shadowed surface matches a face turned away from the sun in the unmodified
// game; ambient, back and point light are kept. Terrain and models differ only
// in their pixel shader (models add the receiver-plane filter).
static void renderReceivers(Params &p, const D3D11_VIEWPORT &viewport) {
    ctx->RSSetState(receiverRaster);
    ctx->OMSetRenderTargets(1, &target, depth);
    ctx->OMSetDepthStencilState(testDepth, 0);
    ctx->OMSetBlendState(modelBlend, nullptr, ~0u);
    ctx->RSSetViewports(1, &viewport);
    ctx->VSSetShader(modelReceiver, nullptr, 0);
    ID3D11ShaderResourceView *shadowViews[] = {mapView, nearView};
    ctx->PSSetShaderResources(0, 2, shadowViews);
    ID3D11SamplerState *shadowSamplers[] = {sampler, rawSampler};
    ctx->PSSetSamplers(0, 2, shadowSamplers);
    // Native model linkage is verified; close models can now use the
    // focused map, retaining hair/armor contact shadows at finer density.
    p.cascade[3] = cascadesEnabled ? 1.f : 0.f;
    ID3D11PixelShader *bound = nullptr;
    lastReceivers = 0;
    for (auto &x : draws) {
        if (beyondFade(x, p))
            continue;
        ++lastReceivers;
        ID3D11PixelShader *shader = x.terrain ? terrainVisibility : modelVisibility;
        if (shader != bound) {
            ctx->PSSetShader(shader, nullptr, 0);
            bound = shader;
        }
        // Native lighting: front/back light colours, ambient and direction.
        memcpy(p.modelFront, x.light, 16);
        memcpy(p.modelBack, x.light + 4, 16);
        memcpy(p.modelAmbient, x.ambient, 16);
        memcpy(p.modelDirection, x.light + 8, 16);
        p.modelFront[3] = x.material[3];
        p.modelDirection[3] = x.lightProtection;
        memcpy(p.view, x.view, 64);
        memcpy(p.proj, x.proj, 64);
        memcpy(p.points, x.pointLights, sizeof(p.points));
        compactPointLights(p);
        p.depthAdjust[0] = x.depthOffset;
        // Native models use varied scales. Selecting their tolerance by scale
        // can leave parts of the same actor on the terrain self-shadow path.
        p.depthAdjust[1] = x.terrain ? 0.f : 1.f;
        bind(x, p);
    }
}

// Renders shadows for this frame's recorded draws (once per frame) and clears them.
static void render() {
    if (draws.empty())
        return;
    // A camera/projection can also belong to the rotating menu preview. Only
    // native terrain establishes the world origin for the supported game.
    if (nativeScene::supported() && !frameOriginKnown) {
        clearDraws();
        return;
    }
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
    if (p.config[1] <= 0 && captureRequest.load() == 0) { // No visible shadow (night/horizon).
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

    // 1. Caster pass into the shadow map.
    ctx->RSSetState(raster);
    ctx->VSSetConstantBuffers(0, 1, &constants);
    ctx->PSSetConstantBuffers(0, 1, &constants);
    ctx->VSSetShader(caster, nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ctx->OMSetBlendState(opaque, nullptr, ~0u);
    ctx->OMSetDepthStencilState(writeDepth, 0);
    Params receiverParameters = p;
    lastModelCasters = 0;
    for (unsigned cascade = 0; cascade < (cascadesEnabled ? 2u : 1u); ++cascade) {
        if (cascade)
            memcpy(p.light, receiverParameters.nearLight, 64);
        renderCasters(p, cascade);
    }
    p = receiverParameters;
    gpuTimer.split(ctx);

    // 2. Receiver pass over the scene.
    renderReceivers(p, originalViewport);
    ID3D11ShaderResourceView *nullViews[2]{};
    ctx->PSSetShaderResources(0, 2, nullViews);
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
            "Shadow frame %u: captured=%u, receivers=%u, model casters (both maps)=%u, last-map terrain casters=%u, CPU average=%.3f ms, eye=(%.3f %.3f %.3f), sun=(%.3f %.3f %.3f)%s",
            frames, lastCount, lastReceivers, lastModelCasters, lastTerrainCasters,
            totalMs / frames, p.eye[0], p.eye[1], p.eye[2], p.sun[0], p.sun[1], p.sun[2],
            shot ? ", captured before/after" : "");
        log(s);
        // How far the game's light is from the clock-driven sun (daytime).
        Vec native = norm({draws.front().light[8], draws.front().light[9],
                           draws.front().light[10]});
        float cosine = std::clamp(dot(native, lastClockSun), -1.f, 1.f);
        sprintf_s(s,
                  "Sun: game=(%.3f %.3f %.3f) clock=(%.3f %.3f %.3f), %.1f degrees apart; casting from %s",
                  native.x, native.y, native.z, lastClockSun.x, lastClockSun.y, lastClockSun.z,
                  acosf(cosine) * 57.2958f, sunFromGame ? "game light by day" : "clock");
        log(s);
    }
    clearDraws();
}

// ===========================================================================
// Recording
// ===========================================================================

// If the current draw uses a cloud sprite's buffer, records it for the cloud
// cache and returns true (clouds are never receivers).
static bool recordCloud(ID3D11DeviceContext *c, cube::World *world, UINT count, UINT start,
                        INT base, bool indexed) {
    Draw cloud{};
    c->IAGetVertexBuffers(0, 1, &cloud.vb, &cloud.stride, &cloud.offset);
    if (!nativeScene::cloud(world, cloud.vb)) {
        drop(cloud.vb);
        return false;
    }
    ID3D11Buffer *matrices[3]{};
    c->VSGetConstantBuffers(0, 3, matrices);
    bool valid = readBuffer(matrices[CB_PROJECTION], cloud.proj, 64) &&
                 readBuffer(matrices[CB_WORLD], cloud.world, 64) && std::abs(cloud.proj[15]) < .5f;
    // WorldMotion tags drifted clouds; store the undrifted position.
    float capturedDrift[2]{};
    UINT driftBytes = sizeof(capturedDrift);
    if (matrices[CB_WORLD])
        matrices[CB_WORLD]->GetPrivateData(nativeScene::cloudDriftGuid, &driftBytes, capturedDrift);
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
        releaseDraw(cloud);
    return true;
}

// Native world draws bind six vertex constant buffers of these sizes.
static bool nativeWorldBuffers(ID3D11Buffer *const buffers[6]) {
    UINT expected[] = {64, 64, 64, 16, 320, 48};
    bool world = true;
    for (int i = 0; i < 6; i++) {
        D3D11_BUFFER_DESC d{};
        if (buffers[i])
            buffers[i]->GetDesc(&d);
        if (d.ByteWidth != expected[i])
            world = false;
    }
    return world;
}

// Called before every native draw. Records opaque world draws as shadow
// receivers, caches clouds, and triggers render() when the world pass ends.
// Returns true when the draw was recorded.
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
    if (cloudShadows && enabled && nativeCloudWorld && pendingClouds.size() < MAX_CLOUDS &&
        recordCloud(c, nativeCloudWorld, count, start, base, indexed))
        return false;
    ID3D11Buffer *buffers[6]{};
    c->VSGetConstantBuffers(0, 6, buffers);
    if (!nativeWorldBuffers(buffers)) {
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
    if (!initialize(c) || finished || !enabled || draws.size() >= MAX_RECORDED_DRAWS) {
        for (auto &b : buffers)
            drop(b);
        return false;
    }
    Draw x{};
    bool ok = readBuffer(buffers[CB_PROJECTION], x.proj, 64) &&
              readBuffer(buffers[CB_VIEW], x.view, 64) &&
              readBuffer(buffers[CB_WORLD], x.world, 64) &&
              readBuffer(buffers[CB_LIGHT], x.light, 48);
    readBuffer(buffers[CB_POINT_LIGHTS], x.pointLights, 320);
    readBuffer(buffers[CB_MATERIAL], x.material, 16);
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
    // Blended (transparent) draws are not shadow receivers.
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
        releaseDraw(x);
        return false;
    }
    auto game = nativeScene::game();
    auto nativeWorld = game ? game->world : nullptr;
    refreshTerrain(nativeWorld);
    if (nativeScene::cloud(nativeWorld, x.vb)) {
        releaseDraw(x);
        return false;
    }
    // Alternate cameras and clouds are excluded before comparing scene views.
    // Pitch-dependent draw ordering must not finish an incomplete shadow pass.
    if (!draws.empty() && memcmp(x.view, draws.front().view, 64) != 0) {
        releaseDraw(x);
        return false;
    }
    // Native terrain chunks (possibly replaced by WorldMotion's copy) also tell
    // us where the render origin is in the world.
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
    // Native lighting extras and depth offset (b6-b9; the same shader draws
    // terrain and models).
    ID3D11Buffer *extraBuffers[4]{};
    c->VSGetConstantBuffers(CB_EXTRA, 4, extraBuffers);
    float whiteValues[4]{}, offsetValues[4]{}, glowValues[4]{};
    readBuffer(extraBuffers[0], x.ambient, 16);
    readBuffer(extraBuffers[1], whiteValues, 16);
    readBuffer(extraBuffers[2], offsetValues, 16);
    readBuffer(extraBuffers[3], glowValues, 16);
    for (auto &b : extraBuffers)
        drop(b);
    x.lightProtection = std::clamp(std::max(whiteValues[0], glowValues[0]), 0.f, 1.f);
    x.depthOffset = offsetValues[0];
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
        releaseDraw(x);
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

// ===========================================================================
// Hooks
// ===========================================================================

static void timedRecord(ID3D11DeviceContext *c, UINT count, UINT start, INT base, bool indexed) {
    if (!hookTimingFrame) {
        record(c, count, start, base, indexed);
        return;
    }
    LARGE_INTEGER begin{}, end{};
    QueryPerformanceCounter(&begin);
    record(c, count, start, base, indexed);
    QueryPerformanceCounter(&end);
    hookTicks += end.QuadPart - begin.QuadPart;
    ++hookDraws;
}
// Called once per presented frame. Logs the capture hook's CPU share.
static void presentTiming() {
    if (hookTimingFrame && ++hookSamples == 60) {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        char s[256];
        sprintf_s(s, "Capture hook CPU: %.3f ms/frame over %.0f native draws/frame (60 sparse frames)",
                  1000. * hookTicks / freq.QuadPart / hookSamples, double(hookDraws) / hookSamples);
        log(s);
        hookSamples = hookDraws = 0;
        hookTicks = 0;
    }
    hookTimingFrame = hookPresents++ % 60 == 0;
}
static void STDMETHODCALLTYPE drawIndexed(ID3D11DeviceContext *c, UINT n, UINT s, INT b) {
    timedRecord(c, n, s, b, true);
    oldDI(c, n, s, b);
}
static void STDMETHODCALLTYPE draw(ID3D11DeviceContext *c, UINT n, UINT s) {
    timedRecord(c, n, s, 0, false);
    oldD(c, n, s);
}
// Map/Unmap/Update: remember writes to small constant buffers.
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
// A depth clear starts a new scene: finish the previous one and handle toggles.
static void STDMETHODCALLTYPE clear(ID3D11DeviceContext *c, ID3D11DepthStencilView *v, UINT flags,
                                    FLOAT z, UINT8 stencil) {
    if (!internal && (!ctx || ctx == c) && (flags & D3D11_CLEAR_DEPTH)) {
        render();
        clearDraws();
        finished = false;
        auto motionModule = GetModuleHandleW(L"WorldMotion.dll");
        auto beginMotion =
            motionModule ? (void(WINAPI *)())GetProcAddress(motionModule, "BeginWorldMotionFrame")
                         : nullptr;
        if (beginMotion)
            beginMotion();
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
// Hooks the shared ID3D11DeviceContext vtable; all hooks or none.
static bool install(ID3D11DeviceContext *c) {
    void **v = *(void ***)c;
    if (table)
        return table == v;
    struct Hook {
        UINT slot;
        void *fn;
        void **original;
    };
    Hook hooks[] = {{SLOT_DRAW_INDEXED, (void *)drawIndexed, (void **)&oldDI},
                    {SLOT_DRAW, (void *)draw, (void **)&oldD},
                    {SLOT_MAP, (void *)map, (void **)&oldMap},
                    {SLOT_UNMAP, (void *)unmap, (void **)&oldUnmap},
                    {SLOT_UPDATE_SUBRESOURCE, (void *)update, (void **)&oldUpdate},
                    {SLOT_CLEAR_DEPTH_STENCIL_VIEW, (void *)clear, (void **)&oldClear}};
    for (auto &h : hooks)
        *h.original = v[h.slot];
    replayDI = oldDI;
    replayD = oldD;
    rawMap = oldMap;
    rawUnmap = oldUnmap;
    // In the combined mod MoonLight already mirrors native constants into the
    // shared cache. A second Map/Unmap/Update layer would store every write
    // twice. Replay also skips MoonLight's per-draw hook: replayed draws use
    // this pass's own constants, which MoonLight never modifies.
    bool shareConstants = false;
#ifdef MOON_AND_SHADOW
    if (moonlight::table == v) {
        shareConstants = true;
        replayDI = moonlight::oldDI;
        replayD = moonlight::oldD;
        rawMap = moonlight::oldMap;
        rawUnmap = moonlight::oldUnmap;
    }
#endif
    int done = 0;
    for (auto &h : hooks) {
        if (shareConstants && (h.slot == SLOT_MAP || h.slot == SLOT_UNMAP ||
                               h.slot == SLOT_UPDATE_SUBRESOURCE)) {
            done++;
            continue;
        }
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
static void loadSettings() {
    const wchar_t *ini = L".\\Mods\\SteamShadows.ini";
    fadeStart =
        std::clamp((int)GetPrivateProfileIntW(L"Shadows", L"FadeStartPercent", 40, ini), 10, 90) *
        .01f;
    sunWarmth =
        std::clamp((int)GetPrivateProfileIntW(L"Shadows", L"SunWarmthPercent", 3, ini), 0, 10) *
        .01f;
    softShadows = GetPrivateProfileIntW(L"Shadows", L"SoftShadows", 0, ini) != 0;
    cameraFade = GetPrivateProfileIntW(L"Shadows", L"CameraFade", 1, ini) != 0;
    sunFromGame = GetPrivateProfileIntW(L"Shadows", L"SunFromGame", 1, ini) != 0;
    cloudShadows = GetPrivateProfileIntW(L"Shadows", L"CloudShadows", 1, ini) != 0;
    resolution = GetPrivateProfileIntW(L"Shadows", L"Resolution", 4096, ini);
    resolution = resolution < 512 ? 512 : resolution > 4096 ? 4096 : resolution;
    radius = (float)GetPrivateProfileIntW(L"Shadows", L"Radius", 120, ini);
    cascadesEnabled = GetPrivateProfileIntW(L"Shadows", L"Cascades", 1, ini) != 0;
    nearResolution =
        std::clamp((UINT)GetPrivateProfileIntW(L"Shadows", L"NearResolution", 2048, ini), 512u,
                   4096u);
    // Uses the configured radius before it is clamped below.
    nearRadius = std::clamp((float)GetPrivateProfileIntW(L"Shadows", L"NearRadius", 32, ini), 8.f,
                            std::max(8.f, radius * .75f));
    radius = radius < 8 ? 8 : radius > 128 ? 128 : radius;
    // 100 removes all direct sunlight in shadow: as dark as faces turned away
    // from the sun in the unmodified game.
    strength = GetPrivateProfileIntW(L"Shadows", L"StrengthPercent", 100, ini) * .01f;
    strength = strength < 0 ? 0 : strength > 1 ? 1 : strength;
}
static DWORD attach() {
    loadSettings();
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

// ===========================================================================
// Entry points (standalone SteamShadows build; MoonandShadow.cpp has its own)
// ===========================================================================

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
        shadows::presentTiming();
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
// Logs the scene matrices and saves before/after screenshots of the next frame.
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
