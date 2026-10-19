#pragma once
// Included inside shadows, after CasterBatches.h. TightModelBounds=1: the local
// position range of each model mesh (one draw's vertex range) is measured once
// on the GPU, so light-space caster culling (intersectsMap) can test the mesh's
// real box instead of +/-255 local units on every axis. Until a mesh is
// measured, and for meshes that cannot be measured safely, the old box is kept.
//
// Measurement replays the draw as a point list into a 6x1 depth row: instance
// i writes component i of (p.xyz, -p.xyz) as depth, so the depth test keeps
// each axis minimum and the negated maximum. Depth testing needs no float
// blending, which GameNative (DXVK on Vulkan) does not guarantee. Rows are
// copied to a staging ring and read back without waiting, frames later.
//
// A cached box is keyed by identity stamps (private data) on the vertex buffer,
// index buffer and input layout, plus the draw range. A new resource at an old
// address has no stamp; Map, UpdateSubresource and Copy* on a stamped buffer
// replace its stamp, so the old box is never found again. Buffers written more
// than a few times, GPU-writable buffers and WorldMotion's animated terrain are
// never measured. Only casters use these boxes; receivers keep beyondFade's.
constexpr UINT BOUNDS_ROWS = 64, BOUNDS_SLOTS = 3;
constexpr uint32_t BOUNDS_VOLATILE_WRITES = 3;
constexpr size_t BOUNDS_CACHE_LIMIT = 16384;
// Local units (1/255 of a normalized position). Far above the depth encoding
// error (~6e-5) and CPU/GPU rounding; at native model scales about 0.05 blocks.
constexpr float BOUNDS_PAD = .5f;
static ID3D11VertexShader *boundsShader = nullptr;
static ID3D11Texture2D *boundsTexture = nullptr;
static ID3D11DepthStencilView *boundsDepth = nullptr;
static const GUID boundsStampGuid = {
    0x3c1f7d52, 0x96e4, 0x4b0a, {0x8d, 0x27, 0x51, 0xa9, 0x0e, 0x6b, 0xc3, 0x44}};
struct BoundsStamp {
    uint64_t id;
    uint32_t writes;
    uint32_t excluded; // GPU-writable or animated: never measured.
};
static std::atomic<uint64_t> nextBoundsStamp{1};
enum : uint8_t { BOUNDS_PENDING = 1, BOUNDS_KNOWN, BOUNDS_UNBOUNDED, BOUNDS_SKIPPED };
struct MeshBounds {
    uint8_t state = 0;
    float center[3]{}, half[3]{}; // Local units (positions * 255).
};
static std::unordered_map<CasterKey, MeshBounds, CasterKeyHash> boundsCache;
struct BoundsSlot {
    ID3D11Texture2D *staging = nullptr;
    std::vector<CasterKey> keys; // Row -> stamped key.
    bool busy = false;
};
static BoundsSlot boundsSlots[BOUNDS_SLOTS];
struct BoundsRequest {
    CasterKey key;
    const Draw *draw;
};
static std::vector<BoundsRequest> boundsRequests;
// This frame's draws by pointer key (pointers stay valid while draws hold them).
static std::unordered_map<CasterKey, MeshBounds, CasterKeyHash> frameBounds;
struct BoundsStats {
    unsigned measured = 0, unbounded = 0, skipped = 0, invalidated = 0, resets = 0;
    unsigned tightDraws = 0, modelDraws = 0; // Last frame.
};
static BoundsStats boundsStats;

static void initializeModelBounds() {
    static const char *source = R"HLSL(
float4 boundsPoint(float4 p:POSITION,uint i:SV_InstanceID):SV_POSITION{
    float3 q=i<3?p.xyz:-p.xyz;
    uint axis=i%3;
    float v=axis==0?q.x:axis==1?q.y:q.z;
    // |v| < 4 maps into (0, 1); saturation marks the mesh as unbounded.
    return float4((i+.5)/3-1,0,saturate(.5+.125*v),1);
}
)HLSL";
    ID3DBlob *code = nullptr, *error = nullptr;
    HRESULT hr = D3DCompile(source, strlen(source), "SteamShadowBounds", nullptr, nullptr,
                            "boundsPoint", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (error) {
        log((char *)error->GetBufferPointer());
        drop(error);
    }
    bool ok = SUCCEEDED(hr) &&
              SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                   nullptr, &boundsShader));
    drop(code);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 6;
    td.Height = BOUNDS_ROWS;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ok = ok && SUCCEEDED(device->CreateTexture2D(&td, nullptr, &boundsTexture)) &&
         SUCCEEDED(device->CreateDepthStencilView(boundsTexture, &dd, &boundsDepth));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto &slot : boundsSlots)
        ok = ok && SUCCEEDED(device->CreateTexture2D(&td, nullptr, &slot.staging));
    if (!ok) {
        drop(boundsShader);
        drop(boundsDepth);
        drop(boundsTexture);
        for (auto &slot : boundsSlots)
            drop(slot.staging);
        tightModelBounds = false;
    }
    log(ok ? "Measured model caster bounds ready (TightModelBounds=1)."
           : "Model bounds measurement unavailable; casters keep +/-255 local bounds.");
}

// The stamp id of `r`, created on first use. Zero for buffers never measured.
static uint64_t boundsStamp(ID3D11DeviceChild *r, bool buffer) {
    if (!r)
        return 0;
    BoundsStamp stamp{};
    UINT bytes = sizeof(stamp);
    if (SUCCEEDED(r->GetPrivateData(boundsStampGuid, &bytes, &stamp)) && bytes == sizeof(stamp))
        return stamp.excluded || stamp.writes >= BOUNDS_VOLATILE_WRITES ? 0 : stamp.id;
    stamp = {nextBoundsStamp++, 0, 0};
    if (buffer) {
        D3D11_BUFFER_DESC desc{};
        static_cast<ID3D11Buffer *>(r)->GetDesc(&desc);
        // A shader can rewrite these without any call the hooks observe.
        stamp.excluded =
            (desc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT)) != 0;
        UINT tagBytes = 0;
        if (SUCCEEDED(r->GetPrivateData(nativeScene::chunkTagGuid, &tagBytes, nullptr)))
            stamp.excluded = 1; // WorldMotion's animated copy of a chunk.
    }
    if (FAILED(r->SetPrivateData(boundsStampGuid, sizeof(stamp), &stamp)))
        return 0;
    return stamp.excluded ? 0 : stamp.id;
}
// Map/UpdateSubresource/Copy* hooks: a write to a stamped buffer retires its
// stamp. Unstamped resources (constant buffers, textures, unseen meshes) cost
// one private-data lookup.
static void noteBufferWrite(ID3D11Resource *r) {
    if (!r)
        return;
    BoundsStamp stamp{};
    UINT bytes = sizeof(stamp);
    if (FAILED(r->GetPrivateData(boundsStampGuid, &bytes, &stamp)) || bytes != sizeof(stamp))
        return;
    stamp.id = nextBoundsStamp++;
    if (stamp.writes < UINT32_MAX)
        ++stamp.writes;
    r->SetPrivateData(boundsStampGuid, sizeof(stamp), &stamp);
    ++boundsStats.invalidated;
}

// Reads every finished measurement. Never waits for the GPU.
static void collectModelBounds() {
    for (auto &slot : boundsSlots) {
        if (!slot.busy)
            continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = rawMap ? rawMap(ctx, slot.staging, 0, D3D11_MAP_READ,
                                     D3D11_MAP_FLAG_DO_NOT_WAIT, &m)
                            : ctx->Map(slot.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT,
                                       &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            continue;
        for (size_t row = 0; row < slot.keys.size(); ++row) {
            auto found = boundsCache.find(slot.keys[row]);
            if (found == boundsCache.end() || found->second.state != BOUNDS_PENDING)
                continue;
            auto &entry = found->second;
            if (FAILED(hr)) { // Retry later.
                boundsCache.erase(found);
                continue;
            }
            float d[6];
            memcpy(d, (const char *)m.pData + row * m.RowPitch, sizeof(d));
            bool bounded = true;
            for (float v : d)
                bounded = bounded && v > 0 && v < 1;
            float lo[3], hi[3];
            for (int i = 0; i < 3 && bounded; ++i) {
                lo[i] = (d[i] - .5f) * 8;
                hi[i] = (.5f - d[i + 3]) * 8;
                bounded = lo[i] <= hi[i];
            }
            if (!bounded) {
                entry.state = BOUNDS_UNBOUNDED;
                ++boundsStats.unbounded;
                continue;
            }
            for (int i = 0; i < 3; ++i) {
                entry.center[i] = (lo[i] + hi[i]) * 127.5f;
                entry.half[i] = (hi[i] - lo[i]) * 127.5f + BOUNDS_PAD;
            }
            entry.state = BOUNDS_KNOWN;
            ++boundsStats.measured;
        }
        if (SUCCEEDED(hr)) {
            if (rawUnmap)
                rawUnmap(ctx, slot.staging, 0);
            else
                ctx->Unmap(slot.staging, 0);
        }
        slot.keys.clear();
        slot.busy = false;
    }
}

static MeshBounds lookupModelBounds(const Draw &x) {
    MeshBounds none{};
    // WorldMotion publishes animated chunk meshes on the original buffer.
    ID3D11Buffer *animated = nullptr;
    UINT animatedBytes = sizeof(animated);
    if (SUCCEEDED(x.vb->GetPrivateData(nativeScene::animatedGuid, &animatedBytes, &animated)) &&
        animated) {
        drop(animated);
        ++boundsStats.skipped;
        return none;
    }
    uint64_t vb = boundsStamp(x.vb, true), ib = x.indexed ? boundsStamp(x.ib, true) : 0,
             layout = boundsStamp(x.layout, false);
    if (!vb || (x.indexed && !ib) || !layout) {
        ++boundsStats.skipped;
        return none;
    }
    CasterKey key = casterKey(x);
    key[0] = vb;
    key[1] = ib;
    key[2] = layout;
    auto found = boundsCache.find(key);
    if (found != boundsCache.end())
        return found->second;
    if (boundsRequests.size() >= BOUNDS_ROWS)
        return none;
    if (boundsCache.size() >= BOUNDS_CACHE_LIMIT) {
        // Retired stamps leave unreachable entries; start over (bounded memory).
        // Measurements still in flight are dropped by collectModelBounds.
        boundsCache.clear();
        ++boundsStats.resets;
    }
    boundsCache[key].state = BOUNDS_PENDING;
    boundsRequests.push_back({key, &x});
    return none;
}

// Start of a shadow frame: finished measurements become available, and each
// model draw takes its mesh's box if known (requesting it otherwise).
static void resolveModelBounds() {
    collectModelBounds();
    frameBounds.clear();
    for (auto &request : boundsRequests) // Never issued: request again.
        boundsCache.erase(request.key);
    boundsRequests.clear();
    boundsStats.tightDraws = boundsStats.modelDraws = 0;
    for (auto &x : draws) {
        x.localBounds = false;
        if (x.terrain || x.lilyPad)
            continue;
        ++boundsStats.modelDraws;
        auto key = casterKey(x);
        auto memo = frameBounds.find(key);
        if (memo == frameBounds.end())
            memo = frameBounds.emplace(key, lookupModelBounds(x)).first;
        if (memo->second.state != BOUNDS_KNOWN)
            continue;
        x.localBounds = true;
        memcpy(x.boundsCenter, memo->second.center, sizeof(x.boundsCenter));
        memcpy(x.boundsHalf, memo->second.half, sizeof(x.boundsHalf));
        ++boundsStats.tightDraws;
    }
}

// Issues this frame's requested measurements (inside the isolated shadow state)
// and copies them to a free staging slot.
static void measureModelBounds() {
    if (boundsRequests.empty())
        return;
    BoundsSlot *slot = nullptr;
    for (auto &s : boundsSlots)
        if (!s.busy) {
            slot = &s;
            break;
        }
    if (!slot) { // All readbacks in flight: ask again next frame.
        for (auto &request : boundsRequests)
            boundsCache.erase(request.key);
        boundsRequests.clear();
        return;
    }
    ctx->OMSetRenderTargets(0, nullptr, boundsDepth);
    oldClear(ctx, boundsDepth, D3D11_CLEAR_DEPTH, 1, 0);
    ctx->RSSetState(raster);
    ctx->OMSetDepthStencilState(writeDepth, 0);
    ctx->VSSetShader(boundsShader, nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    for (size_t row = 0; row < boundsRequests.size(); ++row) {
        const Draw &x = *boundsRequests[row].draw;
        D3D11_VIEWPORT vp{0, float(row), 6, 1, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetVertexBuffers(0, 1, &x.vb, &x.stride, &x.offset);
        ctx->IASetIndexBuffer(x.ib, x.format, x.ibOffset);
        ctx->IASetInputLayout(x.layout);
        if (x.indexed)
            ctx->DrawIndexedInstanced(x.count, 6, x.start, x.base, 0);
        else
            ctx->DrawInstanced(x.count, 6, x.start, 0);
        slot->keys.push_back(boundsRequests[row].key);
    }
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->CopyResource(slot->staging, boundsTexture);
    slot->busy = true;
    boundsRequests.clear();
    replayBindings = {}; // Input-assembler state changed behind the replay cache.
}
