#pragma once
// Included inside shadows, after the ordinary replay and bounds helpers.
// Only depth-only model submissions change. Receivers, terrain motion, clouds
// and all shadow filtering continue through their existing paths.
static ID3D11VertexShader *instancedCasterShader = nullptr;
static ID3D11Buffer *casterWorldBuffer = nullptr;
static ID3D11ShaderResourceView *casterWorldView = nullptr;

static void initializeCasterBatches() {
    ID3DBlob *code = nullptr, *error = nullptr;
    HRESULT hr =
        D3DCompile(hlsl, strlen(hlsl), "SteamShadowInstances", nullptr, nullptr, "instancedCaster",
                   "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (error) {
        log((char *)error->GetBufferPointer());
        drop(error);
    }
    bool ok = SUCCEEDED(hr) &&
              SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                   nullptr, &instancedCasterShader));
    drop(code);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = UINT(MAX_RECORDED_DRAWS * 16 * sizeof(float));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    view.Buffer.NumElements = UINT(MAX_RECORDED_DRAWS * 4);
    ok = ok && SUCCEEDED(device->CreateBuffer(&desc, nullptr, &casterWorldBuffer)) &&
         SUCCEEDED(device->CreateShaderResourceView(casterWorldBuffer, &view, &casterWorldView));
    if (!ok) {
        drop(instancedCasterShader);
        drop(casterWorldView);
        drop(casterWorldBuffer);
        batchCastersEnabled = false;
    }
    log(ok ? "Instanced model casters ready (bounded 256 KiB transform table)."
           : "Instanced casters unavailable; original caster draws retained.");
}

// Every input that affects the caster geometry is part of the key. Transforms
// may differ; material, lighting and colour are not read by the depth shader.
using CasterKey = std::array<uint64_t, 12>;
static CasterKey casterKey(const Draw &x) {
    return {uint64_t(uintptr_t(x.vb)),
            uint64_t(uintptr_t(x.ib)),
            uint64_t(uintptr_t(x.layout)),
            x.stride,
            x.offset,
            x.ibOffset,
            x.count,
            x.start,
            uint64_t(uint32_t(x.base)),
            uint64_t(x.format),
            uint64_t(x.topology),
            x.indexed};
}
struct CasterKeyHash {
    size_t operator()(const CasterKey &key) const {
        uint64_t hash = 1469598103934665603ull;
        for (auto value : key)
            hash = (hash ^ value) * 1099511628211ull;
        return size_t(hash);
    }
};
struct CasterGroup {
    const Draw *geometry;
    size_t head, tail;
    UINT count = 1, firstInstance = 0;
};
struct CasterLink {
    const Draw *draw;
    size_t next = SIZE_MAX;
};
// Scratch allocations are reused, never retain game resources, and are bounded
// by MAX_RECORDED_DRAWS. Groups are rebuilt from this frame, for each map's cull.
struct CasterPlan {
    std::unordered_map<CasterKey, size_t, CasterKeyHash> lookup;
    std::vector<CasterGroup> groups;
    std::vector<CasterLink> links;
    std::vector<std::array<float, 16>> worlds;

    void prepare(const Params &p) {
        lookup.clear();
        groups.clear();
        links.clear();
        worlds.clear();
        for (const auto &x : draws) {
            if (x.terrain || x.lilyPad || x.alpha < OPAQUE_ALPHA || !intersectsMap(x, p))
                continue;
            auto slot = lookup.emplace(casterKey(x), groups.size());
            size_t link = links.size();
            links.push_back({&x});
            if (slot.second)
                groups.push_back({&x, link, link});
            else {
                auto &group = groups[slot.first->second];
                links[group.tail].next = link;
                group.tail = link;
                ++group.count;
            }
        }
        for (auto &group : groups) {
            if (group.count < 2)
                continue; // Unique meshes still use the original draw.
            group.firstInstance = UINT(worlds.size());
            for (size_t i = group.head; i != SIZE_MAX; i = links[i].next) {
                worlds.emplace_back();
                memcpy(worlds.back().data(), links[i].draw->world, 64);
            }
        }
    }
};
static CasterPlan casterPlan;

// See-through models (stealth): drawn after the opaque ones with the dithered
// caster shader; nearly invisible ones cast nothing.
static void renderTranslucentCasters(Params &p) {
    bool bound = false;
    float savedAlpha = p.modelAmbient[3];
    for (const auto &x : draws) {
        if (x.terrain || x.lilyPad || x.alpha >= OPAQUE_ALPHA || x.alpha < INVISIBLE_ALPHA ||
            !intersectsMap(x, p))
            continue;
        if (!bound) {
            ctx->PSSetShader(translucentCaster, nullptr, 0);
            ctx->PSSetConstantBuffers(0, 1, &constants);
            bound = true;
        }
        p.modelAmbient[3] = x.alpha;
        bind(x, p);
        ++lastModelCasters;
        ++lastModelCasterDraws;
        ++lastTranslucentCasters;
    }
    if (bound)
        ctx->PSSetShader(nullptr, nullptr, 0);
    p.modelAmbient[3] = savedAlpha;
}

static void renderOpaqueModelCasters(Params &p);
static void renderModelCasters(Params &p) {
    renderOpaqueModelCasters(p);
    renderTranslucentCasters(p);
}
static void renderOpaqueModelCasters(Params &p) {
    auto ordinary = [&] {
        for (const auto &x : draws)
            if (!x.terrain && !x.lilyPad && x.alpha >= OPAQUE_ALPHA && intersectsMap(x, p)) {
                bind(x, p);
                ++lastModelCasters;
                ++lastModelCasterDraws;
            }
    };
    if (!batchCastersEnabled || !instancedCasterShader || !casterWorldBuffer || !casterWorldView ||
        draws.size() > MAX_RECORDED_DRAWS) {
        ordinary();
        return;
    }
    casterPlan.prepare(p);
    const auto &worlds = casterPlan.worlds;
    // A failed discard must not render with stale transforms.
    if (!worlds.empty() &&
        !uploadTo(casterWorldBuffer, worlds.data(), worlds.size() * sizeof(worlds[0]))) {
        ordinary();
        return;
    }
    replayBindings.haveParameters = false;
    bool instanced = false;
    float savedBase = p.depthAdjust[0];
    for (const auto &group : casterPlan.groups) {
        bool batch = group.count > 1;
        if (batch != instanced) {
            ctx->VSSetShader(batch ? instancedCasterShader : caster, nullptr, 0);
            instanced = batch;
        }
        const auto &x = *group.geometry;
        if (batch) {
            bindGeometry(x);
            // Explicit base keeps typed-buffer addressing independent of the
            // input assembler's StartInstanceLocation interpretation.
            p.depthAdjust[0] = float(group.firstInstance);
            if (!uploadTo(constants, &p, sizeof(p))) {
                ctx->VSSetShader(caster, nullptr, 0);
                instanced = false;
                for (size_t i = group.head; i != SIZE_MAX; i = casterPlan.links[i].next) {
                    bind(*casterPlan.links[i].draw, p);
                    ++lastModelCasters;
                    ++lastModelCasterDraws;
                }
                continue;
            }
            replayBindings.haveParameters = false;
            ctx->VSSetShaderResources(0, 1, &casterWorldView);
            if (x.indexed)
                ctx->DrawIndexedInstanced(x.count, group.count, x.start, x.base, 0);
            else
                ctx->DrawInstanced(x.count, group.count, x.start, 0);
            lastBatchedCasters += group.count;
        } else
            bind(x, p);
        lastModelCasters += group.count;
        ++lastModelCasterDraws;
    }
    // Clouds and receivers use the original shaders/resources. The isolated
    // context state restores any game's vertex SRV after the whole shadow pass.
    if (instanced)
        ctx->VSSetShader(caster, nullptr, 0);
    ID3D11ShaderResourceView *empty = nullptr;
    ctx->VSSetShaderResources(0, 1, &empty);
    p.depthAdjust[0] = savedBase;
}
