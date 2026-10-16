// Cached terrain must cull like terrain, without losing tall or animated casters.
#include "SteamShadows.cpp"
#include <cassert>

namespace {
using namespace shadows;
struct FloatVertex {
    float p[4];
    unsigned color;
};
struct Result {
    std::vector<unsigned> depth;
    unsigned calls;
};
Result renderMap(const Params &input, bool tight, bool nearPass) {
    Params p = input;
    tightTerrainBounds = tight;
    terrainCandidates = terrainSubmissions = 0;
    internal = true;
    replayBindings = {};
    UINT size = nearPass ? nearResolution : resolution;
    auto map = nearPass ? nearTexture : texture;
    auto dsv = nearPass ? nearDepth : mapDepth;
    ctx->OMSetRenderTargets(0, nullptr, dsv);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1, 0);
    D3D11_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    ctx->RSSetState(raster);
    ctx->OMSetDepthStencilState(writeDepth, 0);
    ctx->VSSetShader(caster, nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &constants);
    Draw reference{};
    reference.terrain = true;
    reference.terrainInfo = {IntVector2(0, 0), 0};
    renderTerrainCasters(reference, p, nearPass);
    assert(terrainCandidates == terrainMeshes.size());
    Result result{{}, terrainSubmissions};
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    D3D11_TEXTURE2D_DESC desc{};
    map->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *readback = nullptr;
    assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback)));
    ctx->CopyResource(readback, map);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    assert(SUCCEEDED(ctx->Map(readback, 0, D3D11_MAP_READ, 0, &mapped)));
    result.depth.resize(size_t(size) * size);
    for (UINT y = 0; y < size; ++y)
        memcpy(result.depth.data() + size_t(y) * size,
               (char *)mapped.pData + size_t(y) * mapped.RowPitch, size * 4);
    ctx->Unmap(readback, 0);
    drop(readback);
    internal = false;
    return result;
}
} // namespace
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace shadows;
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    bool warp = argc > 1 && !strcmp(argv[1], "warp");
    assert(
        SUCCEEDED(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c)));
    auto vtable = *(void ***)c;
    oldDI = replayDI = (DI)vtable[SLOT_DRAW_INDEXED];
    oldD = replayD = (D)vtable[SLOT_DRAW];
    oldClear = (Clear)vtable[SLOT_CLEAR_DEPTH_STENCIL_VIEW];
    resolution = 512;
    nearResolution = 256;
    radius = 120;
    nearRadius = 50;
    inlineTerrainEnabled = false;
    assert(initialize(c));
    // Native chunk corners and intermediate surfaces, including both horizontal
    // extrema and the full byte-encoded vertical interval. Wind copies extend
    // the XY edges by the installed WorldMotion maximum (0.35 blocks).
    std::vector<unsigned char> packed;
    std::vector<FloatVertex> moving;
    std::vector<unsigned short> indices;
    for (unsigned z : {0u, 32u, 128u, 255u})
        for (unsigned y = 0; y < 64; y += 16)
            for (unsigned x = 0; x < 64; x += 16) {
                unsigned first = unsigned(packed.size() / 8);
                const unsigned xy[4][2] = {{x, y}, {x + 16, y}, {x, y + 16}, {x + 16, y + 16}};
                for (const auto &corner : xy) {
                    packed.insert(packed.end(), {(unsigned char)corner[0], (unsigned char)corner[1],
                                                 (unsigned char)z, 4, 120, 170, 90, 255});
                    float xx = float(corner[0]), yy = float(corner[1]);
                    xx += xx == 0 ? -.35f : xx == 64 ? .35f : 0;
                    yy += yy == 0 ? -.35f : yy == 64 ? .35f : 0;
                    moving.push_back({{xx / 255, yy / 255, float(z) / 255, 4.f / 255}, 0xff60a040});
                }
                for (unsigned i : {0u, 1u, 2u, 2u, 1u, 3u})
                    indices.push_back((unsigned short)(first + i));
            }
    auto makeBuffer = [&](const void *data, UINT bytes, UINT flags) {
        D3D11_BUFFER_DESC desc{bytes, D3D11_USAGE_DEFAULT, flags};
        D3D11_SUBRESOURCE_DATA init{data};
        ID3D11Buffer *out = nullptr;
        assert(SUCCEEDED(d->CreateBuffer(&desc, &init, &out)));
        return out;
    };
    auto vb = makeBuffer(packed.data(), UINT(packed.size()), D3D11_BIND_VERTEX_BUFFER);
    auto animated = makeBuffer(moving.data(), UINT(moving.size() * sizeof(FloatVertex)),
                               D3D11_BIND_VERTEX_BUFFER);
    auto ib = makeBuffer(indices.data(), UINT(indices.size() * 2), D3D11_BIND_INDEX_BUFFER);
    for (int x = -4; x <= 4; ++x)
        for (int y = -4; y <= 4; ++y) {
            vb->AddRef();
            ib->AddRef();
            terrainMeshes.push_back(
                {vb, ib, {IntVector2(x, y), 0}, UINT(indices.size()), DXGI_FORMAT_R16_UINT});
        }
    unsigned cases = 0;
    for (bool wind : {false, true}) {
        assert(SUCCEEDED(
            vb->SetPrivateDataInterface(nativeScene::animatedGuid, wind ? animated : nullptr)));
        for (Vec sun :
             {Vec{0, 0, 1}, Vec{.5f, .25f, .8f}, Vec{-.7f, .25f, .6f}, Vec{.9f, .1f, .3f}})
            for (bool nearPass : {false, true}) {
                sun = norm(sun);
                Vec f{-sun.x, -sun.y, -sun.z};
                Vec r = norm(cross(f, std::abs(f.z) > .95f ? Vec{0, 1, 0} : Vec{0, 0, 1})),
                    u = cross(r, f);
                float reach = nearPass ? nearRadius : radius;
                Params p{};
                p.light[0] = r.x / reach;
                p.light[1] = r.y / reach;
                p.light[2] = r.z / reach;
                p.light[4] = u.x / reach;
                p.light[5] = u.y / reach;
                p.light[6] = u.z / reach;
                p.light[8] = f.x / 1024;
                p.light[9] = f.y / 1024;
                p.light[10] = f.z / 1024;
                p.light[11] = .5f;
                p.light[15] = 1;
                auto before = renderMap(p, false, nearPass), after = renderMap(p, true, nearPass);
                assert(before.depth == after.depth);
                assert(after.calls <= before.calls);
                auto covered = std::count_if(after.depth.begin(), after.depth.end(),
                                             [](unsigned v) { return v != 0x3f800000; });
                assert(covered > 100);
                printf(
                    "%s %s sunZ=%.3f: %u -> %u terrain draws, %zu covered pixels, identical depth\n",
                    wind ? "wind" : "native", nearPass ? "near" : "far", sun.z, before.calls,
                    after.calls, size_t(covered));
                if (sun.z > .99f)
                    assert(after.calls < before.calls / 2);
                ++cases;
            }
    }
    clearTerrain();
    vb->SetPrivateDataInterface(nativeScene::animatedGuid, nullptr);
    drop(vb);
    drop(ib);
    drop(animated);
    printf("Terrain culling: %u native/animated, near/far, angled-light cases: PASS\n", cases);
}
