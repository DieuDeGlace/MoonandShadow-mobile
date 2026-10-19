// Measured model caster bounds must cull more without changing one depth bit:
// unsigned, signed and float layouts, index ranges that skip far vertices,
// and every write path that retires a measured box (Map, UpdateSubresource,
// CopyResource, CopySubresourceRegion), checked against the +/-255 boxes.
#include "SteamShadows.cpp"
#include <cassert>
#include <random>

namespace {
using namespace shadows;
struct Image {
    std::vector<uint32_t> depth;
    unsigned objects = 0, candidates = 0;
};
ID3D11Buffer *buffer(const void *data, UINT bytes, UINT flags, bool dynamic = false) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.BindFlags = flags;
    if (dynamic) {
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    }
    D3D11_SUBRESOURCE_DATA init{data};
    ID3D11Buffer *out = nullptr;
    assert(SUCCEEDED(device->CreateBuffer(&desc, &init, &out)));
    return out;
}
Image renderDepth(const Params &input, bool nearPass, bool tight, bool batch) {
    Params p = input;
    tightModelBounds = tight;
    batchCastersEnabled = batch;
    casterCascade = nearPass;
    lastModelCasters = lastModelCasterDraws = lastBatchedCasters = 0;
    modelCandidates[nearPass] = modelAccepted[nearPass] = 0;
    replayBindings = {};
    internal = true;
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
    result.candidates = modelCandidates[nearPass];
    assert(modelAccepted[nearPass] == lastModelCasters);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
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
    internal = false;
    tightModelBounds = true;
    return result;
}
size_t differences(const Image &a, const Image &b) {
    size_t n = 0;
    for (size_t i = 0; i < a.depth.size(); ++i)
        n += a.depth[i] != b.depth[i];
    return n;
}
size_t covered(const Image &a) {
    return std::count_if(a.depth.begin(), a.depth.end(), [](uint32_t v) { return v != 0x3f800000; });
}
// One shadow frame's bounds work, then (test only) wait for its readback.
void settle() {
    for (int round = 0; round < 4; ++round) {
        resolveModelBounds();
        bool requested = !boundsRequests.empty();
        internal = true;
        measureModelBounds();
        internal = false;
        ctx->Flush();
        auto deadline = GetTickCount64() + 10000;
        for (;;) {
            collectModelBounds();
            bool busy = false;
            for (auto &slot : boundsSlots)
                busy = busy || slot.busy;
            if (!busy)
                break;
            assert(GetTickCount64() < deadline);
            Sleep(1);
        }
        if (!requested)
            break;
    }
    resolveModelBounds(); // Apply what is now known.
    assert(boundsRequests.empty());
}
Params lightFor(Vec sun, bool nearPass) {
    sun = norm(sun);
    Vec f{-sun.x, -sun.y, -sun.z};
    Vec r = norm(cross(f, std::abs(f.z) > .95f ? Vec{0, 1, 0} : Vec{0, 0, 1})), u = cross(r, f);
    float reach = nearPass ? nearRadius : radius;
    Params p{};
    p.light[0] = r.x / reach;
    p.light[1] = r.y / reach;
    p.light[2] = r.z / reach;
    p.light[3] = .07f; // Off-centre map: no symmetry hides a sign error.
    p.light[4] = u.x / reach;
    p.light[5] = u.y / reach;
    p.light[6] = u.z / reach;
    p.light[7] = -.05f;
    p.light[8] = f.x / 1024;
    p.light[9] = f.y / 1024;
    p.light[10] = f.z / 1024;
    p.light[11] = .5f;
    p.light[15] = 1;
    return p;
}
const Vec suns[] = {{0, 0, 1}, {.5f, .25f, .8f}, {-.7f, .25f, .6f}, {.9f, .1f, .3f}};
// Every sun, both maps, batched and not: identical depth to the +/-255 boxes.
// Returns submitted objects (tight, loose) summed over all cases.
std::pair<unsigned, unsigned> compareAll(const char *label) {
    unsigned tight = 0, loose = 0;
    for (Vec sun : suns)
        for (bool nearPass : {false, true})
            for (bool batch : {false, true}) {
                Params p = lightFor(sun, nearPass);
                auto a = renderDepth(p, nearPass, false, batch);
                auto b = renderDepth(p, nearPass, true, batch);
                assert(a.candidates == b.candidates);
                assert(b.objects <= a.objects);
                assert(covered(a) > 100);
                size_t diff = differences(a, b);
                if (diff) {
                    printf("%s: sun z %.2f %s batch=%d: %zu different depth bits\n", label, sun.z,
                           nearPass ? "near" : "far", batch, diff);
                    assert(false);
                }
                tight += b.objects;
                loose += a.objects;
            }
    printf("  %s: %u -> %u model caster objects over 16 maps, identical depth\n", label, loose,
           tight);
    return {tight, loose};
}
struct Corner {
    float x, y, z;
};
// Twelve triangles of the box spanned by lo..hi, as corner indices 0..7.
std::vector<uint32_t> boxIndices() {
    const uint32_t faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1},
                                  {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    std::vector<uint32_t> out;
    for (auto &f : faces)
        for (uint32_t i : {f[0], f[1], f[2], f[0], f[2], f[3]})
            out.push_back(i);
    return out;
}
Corner corner(Corner lo, Corner hi, uint32_t i) {
    return {i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z};
}
// The measured box (local units) must contain lo..hi and exceed it only by the pad.
void checkBox(const Draw &x, Corner lo, Corner hi) {
    auto found = frameBounds.find(casterKey(x));
    assert(found != frameBounds.end() && found->second.state == BOUNDS_KNOWN);
    const auto &b = found->second;
    const float l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    for (int i = 0; i < 3; ++i) {
        float exactCenter = (l[i] + h[i]) / 2, exactHalf = (h[i] - l[i]) / 2;
        float error = std::abs(b.center[i] - exactCenter);
        assert(b.half[i] >= exactHalf + error);
        assert(b.half[i] <= exactHalf + BOUNDS_PAD + .01f);
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
    oldMap = (Map)table[SLOT_MAP];
    oldUnmap = (Unmap)table[SLOT_UNMAP];
    oldUpdate = (Update)table[SLOT_UPDATE_SUBRESOURCE];
    oldCopy = (Copy)table[SLOT_COPY_RESOURCE];
    oldCopyRegion = (CopyRegion)table[SLOT_COPY_SUBRESOURCE_REGION];
    oldClear = (Clear)table[SLOT_CLEAR_DEPTH_STENCIL_VIEW];
    resolution = 512;
    nearResolution = 256;
    radius = 120;
    nearRadius = 50;
    inlineTerrainEnabled = false;
    tightModelBounds = true;
    assert(initialize(c));
    assert(tightModelBounds && boundsShader && boundsDepth);
    printf("Measured model bounds on %s\n", warp ? "WARP" : "hardware");

    // Native UNORM bytes: an off-centre box (local 100..140 etc.). Vertex 0 and
    // the trailing vertex are far away and referenced only outside the drawn
    // index range (before `start`), so the measured box must exclude them.
    const Corner packedLo{100, 105, 0}, packedHi{140, 135, 60};
    unsigned char packed[10][8]{};
    auto setPacked = [&](int v, Corner p) {
        packed[v][0] = (unsigned char)p.x;
        packed[v][1] = (unsigned char)p.y;
        packed[v][2] = (unsigned char)p.z;
        packed[v][3] = 4;
    };
    setPacked(0, {250, 250, 250});
    for (uint32_t i = 0; i < 8; ++i)
        setPacked(1 + i, corner(packedLo, packedHi, i));
    setPacked(9, {255, 0, 255});
    std::vector<uint16_t> packedIndices = {0, 8, 8, 8}; // ibOffset skips [0]; [1..3] precede start.
    for (uint32_t i : boxIndices())
        packedIndices.push_back(uint16_t(i)); // + base 1 -> vertices 1..8
    // Float positions, signed, drawn non-indexed from vertex 1; vertex 0 and
    // the last vertex lie outside the drawn range.
    const Corner floatLo{-.12f, -.1f, 0}, floatHi{.1f, .08f, .3f};
    auto expand = [&](Corner lo, Corner hi, Corner outside) {
        std::vector<std::array<float, 4>> out{{outside.x, outside.y, outside.z, 0}};
        for (uint32_t i : boxIndices()) {
            auto p = corner(lo, hi, i);
            out.push_back({p.x, p.y, p.z, 4.f / 255});
        }
        out.push_back({outside.x, -outside.y, outside.z, 0});
        return out;
    };
    auto floatVertices = expand(floatLo, floatHi, {.9f, .9f, .9f});
    // Signed 16-bit normalized positions, 32-bit indices.
    const Corner snormLo{-.2f, -.1f, 0}, snormHi{.05f, .1f, .25f};
    int16_t snorm[8][4]{};
    for (uint32_t i = 0; i < 8; ++i) {
        auto p = corner(snormLo, snormHi, i);
        snorm[i][0] = int16_t(std::lround(p.x * 32767));
        snorm[i][1] = int16_t(std::lround(p.y * 32767));
        snorm[i][2] = int16_t(std::lround(p.z * 32767));
    }
    std::vector<uint32_t> snormIndices = {7};
    for (uint32_t i : boxIndices())
        snormIndices.push_back(i);
    // Positions beyond the measurable range (|p| >= 4) stay on +/-255 bounds.
    auto unboundedVertices = expand({-.05f, -.05f, 0}, {6, .05f, .1f}, {0, 0, 0});
    // A dynamic (Map-written) mesh.
    const Corner dynamicLo{-.05f, -.15f, .02f}, dynamicHi{.06f, .02f, .2f};
    auto dynamicVertices = expand(dynamicLo, dynamicHi, {.9f, -.9f, .9f});

    auto bytesVB = buffer(packed, sizeof(packed), D3D11_BIND_VERTEX_BUFFER);
    auto taggedVB = buffer(packed, sizeof(packed), D3D11_BIND_VERTEX_BUFFER);
    auto packedIB = buffer(packedIndices.data(), UINT(packedIndices.size() * 2),
                           D3D11_BIND_INDEX_BUFFER);
    auto floatVB = buffer(floatVertices.data(), UINT(floatVertices.size() * 16),
                          D3D11_BIND_VERTEX_BUFFER);
    auto snormVB = buffer(snorm, sizeof(snorm), D3D11_BIND_VERTEX_BUFFER);
    auto snormIB = buffer(snormIndices.data(), UINT(snormIndices.size() * 4),
                          D3D11_BIND_INDEX_BUFFER);
    auto unboundedVB = buffer(unboundedVertices.data(), UINT(unboundedVertices.size() * 16),
                              D3D11_BIND_VERTEX_BUFFER);
    auto dynamicVB = buffer(dynamicVertices.data(), UINT(dynamicVertices.size() * 16),
                            D3D11_BIND_VERTEX_BUFFER, true);
    // WorldMotion-style published animation on a mesh buffer: never measured.
    assert(SUCCEEDED(taggedVB->SetPrivateDataInterface(nativeScene::animatedGuid, floatVB)));
    ID3D11InputLayout *snormLayout = nullptr;
    {
        ID3DBlob *code = nullptr;
        assert(SUCCEEDED(D3DCompile(hlsl, strlen(hlsl), "test", nullptr, nullptr, "caster",
                                    "vs_4_0", 0, 0, &code, nullptr)));
        D3D11_INPUT_ELEMENT_DESC element = {
            "POSITION", 0, DXGI_FORMAT_R16G16B16A16_SNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        assert(SUCCEEDED(d->CreateInputLayout(&element, 1, code->GetBufferPointer(),
                                              code->GetBufferSize(), &snormLayout)));
        drop(code);
    }

    enum Mesh { PACKED, FLOAT, SNORM, UNBOUNDED, DYNAMIC, TAGGED, MESHES };
    auto describe = [&](Draw &x, int mesh) {
        x.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        x.count = 36;
        switch (mesh) {
        case PACKED:
        case TAGGED:
            x.vb = mesh == PACKED ? bytesVB : taggedVB;
            x.ib = packedIB;
            x.layout = nativeCasterLayout;
            x.stride = 8;
            x.format = DXGI_FORMAT_R16_UINT;
            x.ibOffset = 2;
            x.start = 3;
            x.base = 1;
            break;
        case SNORM:
            x.vb = snormVB;
            x.ib = snormIB;
            x.layout = snormLayout;
            x.stride = 8;
            x.format = DXGI_FORMAT_R32_UINT;
            x.ibOffset = 4;
            break;
        default:
            x.vb = mesh == FLOAT ? floatVB : mesh == UNBOUNDED ? unboundedVB : dynamicVB;
            x.layout = floatCasterLayout;
            x.stride = 16;
            x.indexed = false;
            x.start = 1;
        }
    };
    // Native-scale models (0.084-0.157) over a wide area: many lie outside
    // the maps, but within reach of their +/-255 boxes (about 21-40 blocks).
    std::mt19937 random(2026);
    std::uniform_real_distribution<float> angle(-3.14f, 3.14f), scale(.084f, .157f),
        jitter(-6, 6);
    for (int i = 0; i < 900; ++i) {
        Draw x{};
        describe(x, i % MESHES);
        float a = angle(random), s = scale(random), mirror = i % 4 ? 1.f : -1.f;
        x.world[0] = cosf(a) * s * mirror;
        x.world[1] = -sinf(a) * s;
        x.world[2] = .02f * s;
        x.world[4] = sinf(a) * s * mirror;
        x.world[5] = cosf(a) * s;
        x.world[6] = -.03f * s;
        x.world[9] = .04f * s;
        x.world[10] = s;
        x.world[15] = 1;
        x.world[3] = float(i % 30) * 16 - 232 + jitter(random);
        x.world[7] = float(i / 30) * 16 - 232 + jitter(random);
        x.world[11] = jitter(random) + 6;
        draws.push_back(x);
    }
    const Draw *sample[MESHES]{};
    for (int m = 0; m < MESHES; ++m)
        sample[m] = &draws[m];

    // 1. Nothing measured yet: every model keeps the old box.
    resolveModelBounds();
    assert(boundsStats.tightDraws == 0 && boundsRequests.size() == 5);
    boundsRequests.clear();
    boundsCache.clear();
    auto unmeasured = compareAll("before measurement");
    assert(unmeasured.first == unmeasured.second);

    // 2. Measured boxes: exact (plus pad), fewer casters, identical depth.
    settle();
    checkBox(*sample[PACKED], packedLo, packedHi);
    auto scaled = [](Corner p) { return Corner{p.x * 255, p.y * 255, p.z * 255}; };
    checkBox(*sample[FLOAT], scaled(floatLo), scaled(floatHi));
    // The signed box as stored (rounded to 1/32767).
    auto decoded = [&](uint32_t i) {
        return Corner{snorm[i][0] / 32767.f, snorm[i][1] / 32767.f, snorm[i][2] / 32767.f};
    };
    checkBox(*sample[SNORM], scaled(decoded(0)), scaled(decoded(7)));
    checkBox(*sample[DYNAMIC], scaled(dynamicLo), scaled(dynamicHi));
    assert(frameBounds.find(casterKey(*sample[UNBOUNDED]))->second.state == BOUNDS_UNBOUNDED);
    assert(!sample[UNBOUNDED]->localBounds && !sample[TAGGED]->localBounds);
    unsigned expectedTight = 0;
    for (auto &x : draws)
        expectedTight += x.vb != unboundedVB && x.vb != taggedVB;
    assert(boundsStats.tightDraws == expectedTight);
    auto measured = compareAll("measured");
    assert(measured.first < measured.second * 85 / 100);

    // 3. Writes retire a box until it is measured again. Each new shape
    // reaches farther than the old box; the stale box would lose casters.
    auto staleCheck = [&](const char *label, ID3D11Buffer *vb) {
        resolveModelBounds();
        for (auto &x : draws)
            if (x.vb == vb)
                assert(!x.localBounds);
        auto retired = compareAll(label);
        settle();
        for (auto &x : draws)
            if (x.vb == vb)
                assert(x.localBounds);
        auto again = compareAll("re-measured");
        assert(again.first < retired.first);
    };
    // Stale-box sensitivity: reuse the float mesh's old box after an update
    // that bypasses the hooks. Depth must then differ from the +/-255 result.
    floatVertices[5][0] = .9f;
    oldUpdate(ctx, floatVB, 0, nullptr, floatVertices.data(), 0, 0);
    {
        size_t lost = 0;
        for (Vec sun : suns) {
            Params p = lightFor(sun, false);
            auto a = renderDepth(p, false, false, true);
            auto b = renderDepth(p, false, true, true);
            lost += differences(a, b);
        }
        printf("  stale box after an unobserved write: %zu different depth bits\n", lost);
        assert(lost > 0);
    }
    // The same write through the UpdateSubresource hook.
    update(ctx, floatVB, 0, nullptr, floatVertices.data(), 0, 0);
    staleCheck("after UpdateSubresource", floatVB);
    checkBox(*sample[FLOAT], scaled(floatLo), scaled({.9f, floatHi.y, floatHi.z}));

    // CopyResource into the packed mesh.
    setPacked(1 + 7, {250, 135, 60}); // Corner 7 (hi.x) moves out.
    auto source = buffer(packed, sizeof(packed), D3D11_BIND_VERTEX_BUFFER);
    copy(ctx, bytesVB, source);
    staleCheck("after CopyResource", bytesVB);
    checkBox(*sample[PACKED], packedLo, {250, packedHi.y, packedHi.z});
    drop(source);

    // CopySubresourceRegion into the signed mesh: corner 0 (lo.x) moves out.
    int16_t moved[4] = {-30000, snorm[0][1], snorm[0][2], 0};
    auto region = buffer(moved, sizeof(moved), D3D11_BIND_VERTEX_BUFFER);
    copyRegion(ctx, snormVB, 0, 0, 0, 0, region, 0, nullptr);
    staleCheck("after CopySubresourceRegion", snormVB);
    drop(region);

    // Map: each write retires the box; a buffer written repeatedly is volatile
    // and keeps the old box from then on, without new measurements.
    for (uint32_t write = 0; write < BOUNDS_VOLATILE_WRITES; ++write) {
        D3D11_MAPPED_SUBRESOURCE m{};
        assert(SUCCEEDED(map(ctx, dynamicVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)));
        dynamicVertices[3][1] = -.15f - .2f * (write + 1);
        memcpy(m.pData, dynamicVertices.data(), dynamicVertices.size() * 16);
        unmap(ctx, dynamicVB, 0);
        if (write + 1 < BOUNDS_VOLATILE_WRITES) {
            staleCheck("after Map", dynamicVB);
            checkBox(*sample[DYNAMIC], scaled({dynamicLo.x, dynamicVertices[3][1], dynamicLo.z}),
                     scaled(dynamicHi));
        }
    }
    unsigned before = boundsStats.measured;
    settle();
    assert(boundsStats.measured == before);
    for (auto &x : draws)
        if (x.vb == dynamicVB)
            assert(!x.localBounds);
    compareAll("volatile mesh");

    // 4. Every pending readback slot busy: requests wait, nothing is lost.
    boundsCache.clear();
    resolveModelBounds();
    assert(!boundsRequests.empty());
    for (auto &slot : boundsSlots)
        slot.busy = true;
    internal = true;
    measureModelBounds();
    internal = false;
    assert(boundsCache.empty());
    for (auto &slot : boundsSlots) {
        slot.busy = false;
        slot.keys.clear();
    }
    settle();
    assert(boundsStats.tightDraws > 0);

    draws.clear(); // Test draws borrowed references rather than retaining them.
    for (auto *b : {bytesVB, taggedVB, packedIB, floatVB, snormVB, snormIB, unboundedVB, dynamicVB})
        drop(b);
    drop(snormLayout);
    puts("Measured model bounds: exact boxes, culling, identical depth, write invalidation: PASS");
}
