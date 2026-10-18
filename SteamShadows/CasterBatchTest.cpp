// Exercise the actual caster submission paths on D3D11. Every depth bit must
// match, including nonzero instance starts, mirrored poses and moving meshes.
#include "SteamShadows.cpp"
#include <cassert>
#include <random>

namespace {
using namespace shadows;
ID3D11Buffer *buffer(const void *data, UINT bytes, UINT flags) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.BindFlags = flags;
    D3D11_SUBRESOURCE_DATA init{data};
    ID3D11Buffer *out = nullptr;
    assert(SUCCEEDED(device->CreateBuffer(&desc, &init, &out)));
    return out;
}
HRESULT STDMETHODCALLTYPE failInstanceMap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                          D3D11_MAP mode, UINT flags,
                                          D3D11_MAPPED_SUBRESOURCE *out) {
    return r == casterWorldBuffer ? E_FAIL : c->Map(r, sub, mode, flags, out);
}
bool failNextParameter = false;
HRESULT STDMETHODCALLTYPE failParameterMap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                           D3D11_MAP mode, UINT flags,
                                           D3D11_MAPPED_SUBRESOURCE *out) {
    if (r == constants && failNextParameter) {
        failNextParameter = false;
        return E_FAIL;
    }
    return c->Map(r, sub, mode, flags, out);
}
struct Image {
    std::vector<uint32_t> depth;
    unsigned objects = 0, calls = 0, batched = 0;
};
Image renderDepth(Params p, bool batch, bool nearPass) {
    batchCastersEnabled = batch;
    replayBindings = {};
    internal = true;
    // Use the production isolation mechanism and a sentinel native VS resource.
    // Instanced caster resources must never leak into the restored scene state.
    ctx->VSSetShaderResources(0, 1, &casterWorldView);
    ID3DDeviceContextState *saved = nullptr;
    ctx1->SwapDeviceContextState(state, &saved);
    lastModelCasters = lastModelCasterDraws = lastBatchedCasters = 0;
    UINT size = nearPass ? nearResolution : resolution;
    auto surface = nearPass ? nearTexture : texture;
    auto dsv = nearPass ? nearDepth : mapDepth;
    ctx->OMSetRenderTargets(0, nullptr, dsv);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1, 0);
    D3D11_VIEWPORT vp{0, 0, float(size), float(size), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster);
    ctx->OMSetDepthStencilState(writeDepth, 0);
    ctx->OMSetBlendState(opaque, nullptr, ~0u);
    ctx->VSSetShader(caster, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &constants);
    ctx->PSSetShader(nullptr, nullptr, 0);
    renderModelCasters(p);
    Image result;
    result.objects = lastModelCasters;
    result.calls = lastModelCasterDraws;
    result.batched = lastBatchedCasters;
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView *leftover = nullptr;
    ctx->VSGetShaderResources(0, 1, &leftover);
    assert(!leftover);
    ID3D11VertexShader *vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    assert(vs == caster);
    drop(vs);
    D3D11_TEXTURE2D_DESC desc{};
    surface->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *readback = nullptr;
    assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback)));
    ctx->CopyResource(readback, surface);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    assert(SUCCEEDED(ctx->Map(readback, 0, D3D11_MAP_READ, 0, &mapped)));
    result.depth.resize(size_t(size) * size);
    for (UINT y = 0; y < size; ++y)
        memcpy(result.depth.data() + size_t(y) * size,
               (const char *)mapped.pData + size_t(y) * mapped.RowPitch, size * 4);
    ctx->Unmap(readback, 0);
    drop(readback);
    ctx1->SwapDeviceContextState(saved, nullptr);
    drop(saved);
    ctx->VSGetShaderResources(0, 1, &leftover);
    assert(leftover == casterWorldView);
    drop(leftover);
    ID3D11ShaderResourceView *empty = nullptr;
    ctx->VSSetShaderResources(0, 1, &empty);
    internal = false;
    return result;
}
void compare(const Image &a, const Image &b) {
    assert(a.objects == b.objects);
    size_t different = 0, shaded = 0;
    for (size_t i = 0; i < a.depth.size(); ++i) {
        different += a.depth[i] != b.depth[i];
        shaded += a.depth[i] != 0x3f800000;
    }
    printf("  %u objects: %u -> %u submissions, %zu covered pixels, %zu different depth bits\n",
           a.objects, a.calls, b.calls, shaded, different);
    assert(shaded > 100);
    assert(different == 0);
}
// Optional controlled benchmark, not a timing assertion or an in-game FPS test.
void benchmark() {
    constexpr unsigned repeats = 96;
    Params p{};
    p.light[0] = p.light[5] = 1.f / radius;
    p.light[10] = .004f;
    p.light[11] = .5f;
    p.light[15] = 1;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    for (bool batch : {false, true, false, true}) {
        D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ID3D11Query *disjoint = nullptr, *begin = nullptr, *end = nullptr;
        assert(SUCCEEDED(device->CreateQuery(&desc, &disjoint)));
        desc.Query = D3D11_QUERY_TIMESTAMP;
        assert(SUCCEEDED(device->CreateQuery(&desc, &begin)));
        assert(SUCCEEDED(device->CreateQuery(&desc, &end)));
        batchCastersEnabled = batch;
        internal = true;
        replayBindings = {};
        ctx->OMSetRenderTargets(0, nullptr, mapDepth);
        ctx->RSSetState(raster);
        ctx->OMSetDepthStencilState(writeDepth, 0);
        D3D11_VIEWPORT vp{0, 0, float(resolution), float(resolution), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->VSSetShader(caster, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &constants);
        ctx->PSSetShader(nullptr, nullptr, 0);
        ctx->Begin(disjoint);
        ctx->End(begin);
        LARGE_INTEGER first{}, last{};
        QueryPerformanceCounter(&first);
        for (unsigned i = 0; i < repeats; ++i) {
            ctx->ClearDepthStencilView(mapDepth, D3D11_CLEAR_DEPTH, 1, 0);
            renderModelCasters(p);
        }
        QueryPerformanceCounter(&last);
        ctx->End(end);
        ctx->End(disjoint);
        ctx->Flush(); // Benchmark only; production timing never flushes or waits.
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT info{};
        UINT64 startTime = 0, endTime = 0;
        auto deadline = GetTickCount64() + 10000;
        auto wait = [&](ID3D11Query *query, void *data, UINT bytes) {
            HRESULT hr;
            while ((hr = ctx->GetData(query, data, bytes, 0)) == S_FALSE &&
                   GetTickCount64() < deadline)
                Sleep(1);
            assert(hr == S_OK);
        };
        wait(disjoint, &info, sizeof(info));
        wait(begin, &startTime, sizeof(startTime));
        wait(end, &endTime, sizeof(endTime));
        assert(!info.Disjoint);
        printf("Controlled caster benchmark BatchCasters=%d: CPU submit %.4f ms/map, "
               "GPU interval %.4f ms/map (%u repeated maps)\n",
               batch, 1000. * (last.QuadPart - first.QuadPart) / frequency.QuadPart / repeats,
               1000. * (endTime - startTime) / info.Frequency / repeats, repeats);
        drop(disjoint);
        drop(begin);
        drop(end);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        internal = false;
    }
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
    auto table = *(void ***)c;
    oldDI = replayDI = (DI)table[SLOT_DRAW_INDEXED];
    oldD = replayD = (D)table[SLOT_DRAW];
    oldClear = (Clear)table[SLOT_CLEAR_DEPTH_STENCIL_VIEW];
    resolution = 512;
    nearResolution = 256;
    radius = 50;
    nearRadius = 24;
    inlineTerrainEnabled = false;
    assert(initialize(c));
    assert(batchCastersEnabled && instancedCasterShader && casterWorldView);
    printf("Caster instancing on %s\n", warp ? "WARP" : "hardware");

    // One padded float mesh and one native normalized-byte mesh. Exercise both
    // indexed and non-indexed draws with nonzero offsets/start/base indices.
    float vertices[9][4] = {{0, 0, 0, 0},          {-.04f, -.03f, 0, 0}, {.04f, -.03f, 0, 0},
                            {.04f, .03f, .01f, 0}, {-.04f, -.03f, 0, 0}, {.04f, .03f, .01f, 0},
                            {-.04f, .03f, 0, 0},   {0, 0, .02f, 0},      {.02f, .02f, .01f, 0}};
    unsigned char native[9][8]{};
    for (int i = 0; i < 9; ++i) {
        native[i][0] = static_cast<unsigned char>(12 + vertices[i][0] * 255);
        native[i][1] = static_cast<unsigned char>(12 + vertices[i][1] * 255);
        native[i][2] = static_cast<unsigned char>(5 + vertices[i][2] * 255);
    }
    uint16_t shortIndices[] = {0, 0, 1, 2, 3, 4, 5, 6};
    uint32_t longIndices[] = {0, 0, 1, 2, 3, 4, 5, 6};
    auto floats = buffer(vertices, sizeof(vertices), D3D11_BIND_VERTEX_BUFFER);
    auto bytes = buffer(native, sizeof(native), D3D11_BIND_VERTEX_BUFFER);
    auto indices16 = buffer(shortIndices, sizeof(shortIndices), D3D11_BIND_INDEX_BUFFER);
    auto indices32 = buffer(longIndices, sizeof(longIndices), D3D11_BIND_INDEX_BUFFER);
    std::mt19937 random(117);
    std::uniform_real_distribution<float> angle(-3.14f, 3.14f), scale(.15f, .8f);
    for (int i = 0; i < 256; ++i) {
        Draw x{};
        bool packed = i % 4 < 2;
        x.vb = packed ? bytes : floats;
        x.layout = packed ? nativeCasterLayout : floatCasterLayout;
        x.stride = packed ? 8 : 16;
        x.indexed = i % 2 == 0;
        x.ib = x.indexed ? (packed ? indices16 : indices32) : nullptr;
        x.format = packed ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        x.count = 6;
        x.start = x.indexed ? 1 : 0;
        x.offset = x.stride; // Skip the padded first vertex.
        x.ibOffset = packed ? 2 : 4;
        x.base = x.indexed ? -1 : 0;
        x.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        float a = angle(random), s = scale(random), mirror = i % 3 ? 1.f : -1.f;
        x.world[0] = cosf(a) * s * mirror;
        x.world[1] = -sinf(a) * s;
        x.world[2] = .03f * s;
        x.world[4] = sinf(a) * s * mirror;
        x.world[5] = cosf(a) * s;
        x.world[9] = .02f;
        x.world[10] = s * .8f;
        x.world[15] = 1;
        x.world[3] = float(i % 16) * 6 - 48;
        x.world[7] = float(i / 16) * 6 - 48;
        x.world[11] = float(i % 7) * .3f;
        x.lilyPad = i == 10; // Pad exclusion stays exact.
        x.terrain = i == 11; // Terrain keeps the independent motion path.
        draws.push_back(x);
    }
    // One unique draw interleaved among groups exercises ordinary/instanced
    // shader switching; its count/range must never be merged into another group.
    Draw unique = draws[0];
    unique.count = 3;
    draws.insert(draws.begin() + 1, unique);
    auto key = casterKey(unique);
    unique.world[3] += 20;
    assert(casterKey(unique) == key);
    unique.base += 1;
    assert(casterKey(unique) != key);

    for (int frame = 0; frame < 3; ++frame) {
        // Every frame uses new model transforms and fresh native vertex data.
        for (auto &x : draws) {
            x.world[3] += .17f;
            x.world[11] += .03f;
        }
        vertices[3][2] += .004f;
        ctx->UpdateSubresource(floats, 0, nullptr, vertices, 0, 0);
        for (bool nearPass : {false, true}) {
            Params p{};
            float r = nearPass ? nearRadius : radius;
            p.light[0] = p.light[5] = 1.f / r;
            p.light[2] = .4f / r;
            p.light[6] = .2f / r;
            p.light[8] = .001f;
            p.light[9] = -.002f;
            p.light[10] = .004f;
            p.light[11] = .5f;
            p.light[15] = 1;
            auto normal = renderDepth(p, false, nearPass);
            auto batched = renderDepth(p, true, nearPass);
            compare(normal, batched);
            assert(batched.calls < normal.calls / 4);
            assert(batched.batched > 20);
            rawMap = failInstanceMap;
            auto fallback = renderDepth(p, true, nearPass);
            rawMap = nullptr;
            compare(normal, fallback);
            assert(fallback.calls == normal.calls && fallback.batched == 0);
            failNextParameter = true;
            rawMap = failParameterMap;
            auto groupFallback = renderDepth(p, true, nearPass);
            rawMap = nullptr;
            assert(!failNextParameter);
            compare(normal, groupFallback);
            assert(groupFallback.calls > batched.calls && groupFallback.batched > 0);
        }
    }
    if (argc > 2 && !strcmp(argv[2], "benchmark"))
        benchmark();
    // The transform table never grows past its fixed allocation. An oversized
    // capture must take the original path, retaining every eligible caster.
    draws.resize(MAX_RECORDED_DRAWS + 1, draws.front());
    Params p{};
    p.light[0] = p.light[5] = 1.f / radius;
    p.light[10] = .004f;
    p.light[11] = .5f;
    p.light[15] = 1;
    auto overflow = renderDepth(p, true, false);
    assert(overflow.batched == 0 && overflow.calls == overflow.objects);
    // See-through (stealth) casters keep about `alpha` of their shadow-map
    // texels; nearly invisible ones cast nothing.
    draws.resize(64);
    auto covered = [&](float alpha) {
        for (auto &x : draws)
            x.alpha = alpha;
        auto image = renderDepth(p, true, false);
        size_t n = 0;
        for (auto d : image.depth)
            n += d != 0x3f800000;
        return n;
    };
    size_t full = covered(1), half = covered(.5f), quarter = covered(.25f), none = covered(.02f);
    printf("  see-through casters: alpha 1 -> %zu texels, .5 -> %zu, .25 -> %zu, .02 -> %zu\n",
           full, half, quarter, none);
    assert(full > 1000 && none == 0);
    assert(half > full * .4 && half < full * .6);
    assert(quarter > full * .15 && quarter < full * .35);
    draws.clear(); // Test draws borrowed references rather than retaining them.
    drop(floats);
    drop(bytes);
    drop(indices16);
    drop(indices32);
    puts(
        "Caster depth, current-frame motion, bounds, exclusions, upload fallbacks and state restoration: PASS");
}
