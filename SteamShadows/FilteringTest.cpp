#include "SteamShadows.cpp"
#include <cassert>
#include <array>
static void STDMETHODCALLTYPE directDraw(ID3D11DeviceContext *c, UINT n, UINT s) {
    c->Draw(n, s);
}
static void STDMETHODCALLTYPE directUpdate(ID3D11DeviceContext *c, ID3D11Resource *r, UINT s,
                                           const D3D11_BOX *b, const void *d, UINT row,
                                           UINT slice) {
    c->UpdateSubresource(r, s, b, d, row, slice);
}
static void STDMETHODCALLTYPE directClear(ID3D11DeviceContext *c, ID3D11DepthStencilView *d, UINT f,
                                          FLOAT z, UINT8 s) {
    c->ClearDepthStencilView(d, f, z, s);
}
int main() {
#if defined(TEST_CASCADES) || defined(TEST_MODEL_RECEIVER)
    shadows::cascadesEnabled = true;
    shadows::nearResolution = 128;
    shadows::nearRadius = 25;
#else
    shadows::cascadesEnabled = false;
#endif
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &d, nullptr, &c)));
    shadows::resolution = 128;
    shadows::softShadows = true;
    shadows::radius = 50;
    shadows::oldD = directDraw;
    shadows::oldUpdate = directUpdate;
    shadows::oldClear = directClear;
    assert(shadows::initialize(c));
    float vertices[6][4] = {
        {-20.f / 255, -20.f / 255, 0, 4.f / 255}, {20.f / 255, -20.f / 255, 0, 4.f / 255},
        {20.f / 255, 20.f / 255, 0, 4.f / 255},   {-20.f / 255, -20.f / 255, 0, 4.f / 255},
        {20.f / 255, 20.f / 255, 0, 4.f / 255},   {-20.f / 255, 20.f / 255, 0, 4.f / 255}};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(vertices);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{vertices};
    ID3D11Buffer *vb = nullptr;
    assert(SUCCEEDED(d->CreateBuffer(&bd, &data, &vb)));
    // Both capture-before-motion and motion-before-capture resolve the same mesh.
    ID3D11Buffer *moving = nullptr;
    assert(SUCCEEDED(d->CreateBuffer(&bd, &data, &moving)));
    shadows::Draw motionDraw{};
    motionDraw.vb = vb;
    motionDraw.terrain = true;
    assert(!shadows::animatedTerrain(motionDraw));
    assert(SUCCEEDED(vb->SetPrivateDataInterface(nativeScene::animatedGuid, moving)));
    auto resolved = shadows::animatedTerrain(motionDraw);
    assert(resolved == moving);
    resolved->Release();
    nativeScene::ChunkTag tag{vb, {}};
    assert(SUCCEEDED(moving->SetPrivateData(nativeScene::chunkTagGuid, sizeof(tag), &tag)));
    motionDraw.vb = moving;
    resolved = shadows::animatedTerrain(motionDraw);
    assert(resolved == moving);
    resolved->Release();
    motionDraw.terrain = false;
    assert(!shadows::animatedTerrain(motionDraw));
    motionDraw.terrain = true;
    vb->SetPrivateDataInterface(nativeScene::animatedGuid, nullptr);
    assert(!shadows::animatedTerrain(motionDraw));
    moving->Release();
    puts("Animated terrain receiver resolves both hook orders and motion toggle-off: PASS");
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 512;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *color = nullptr, *depth = nullptr, *staging = nullptr;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &color)));
    assert(SUCCEEDED(d->CreateRenderTargetView(color, nullptr, &shadows::target)));
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &depth)));
    assert(SUCCEEDED(d->CreateDepthStencilView(depth, nullptr, &shadows::depth)));
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &staging)));
    float white[4] = {1, 1, 1, 1};
    c->ClearRenderTargetView(shadows::target, white);
    c->ClearDepthStencilView(shadows::depth, D3D11_CLEAR_DEPTH, .6f, 0);
    shadows::Draw ground{};
    ground.terrain = true;
    ground.vb = vb;
    vb->AddRef();
    ground.stride = 16;
    ground.layout = shadows::floatCasterLayout;
    ground.layout->AddRef();
    ground.count = 6;
    ground.indexed = false;
    ground.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    for (int i : {0, 5, 10, 15})
        ground.world[i] = ground.view[i] = ground.proj[i] = 1;
    ground.view[10] = -1;
    ground.view[11] = 10;
    ground.proj[0] = ground.proj[5] = 1.f / 50;
    ground.proj[10] = .01f;
    ground.proj[11] = .5f;
    // Native sunlight (front colour) for the direct-light receivers to remove.
    ground.light[0] = ground.light[1] = ground.light[2] = .8f;
    ground.light[10] = 1;

    // Repeated replay binds should issue no redundant uploads or IA changes.
    auto emptyDraw = ground;
    emptyDraw.count = 0;
    shadows::Params cacheParams{};
    shadows::replayBindings = {};
    shadows::internal = true;
    for (int repeat = 0; repeat < 100; ++repeat)
        shadows::bind(emptyDraw, cacheParams);
    assert(shadows::replayBindings.uploads == 1);
    assert(shadows::replayBindings.stateChanges == 4);
    cacheParams.depthAdjust[0] = .001f;
    shadows::bind(emptyDraw, cacheParams);
    assert(shadows::replayBindings.uploads == 2);
    emptyDraw.offset = 4;
    shadows::bind(emptyDraw, cacheParams);
    assert(shadows::replayBindings.stateChanges == 5);
    shadows::internal = false;
    shadows::replayBindings = {};
    puts(
        "100 identical replay binds: 1 constant upload and 4 IA changes; changed inputs retained: PASS");
    {
        shadows::Params cull{};
        assert(shadows::lightMatrix(cull, ground));
        auto inside = ground, faded = ground, model = ground;
        assert(!shadows::beyondFade(inside, cull) && shadows::intersectsMap(inside, cull));
        // Terrain whose nearest block is past the radius has zero fade everywhere.
        faded.world[3] = shadows::radius + 3;
        assert(shadows::beyondFade(faded, cull));
        faded.world[3] = shadows::radius - 1;
        assert(!shadows::beyondFade(faded, cull));
        // A scaled model straddling the radius is kept; one far outside is culled.
        model.terrain = false;
        model.world[0] = model.world[5] = model.world[10] = 1.f / 16;
        model.world[3] = shadows::radius + 10;
        assert(!shadows::beyondFade(model, cull));
        model.world[3] = shadows::radius * 4;
        assert(shadows::beyondFade(model, cull) && !shadows::intersectsMap(model, cull));
        // Camera fade measures height too: a model far below the camera is faded
        // only when camera fade is on.
        model.world[3] = 0;
        model.world[11] = -shadows::radius * 2;
        cull.sunTone[2] = 1;
        assert(shadows::beyondFade(model, cull));
        cull.sunTone[2] = 0;
        assert(!shadows::beyondFade(model, cull));
    }
    puts("Receivers past the fade radius and casters outside the map are skipped: PASS");
    shadows::draws.push_back(ground);
    shadows::sceneViewport = {0, 0, 512, 512, 0, 1};
    shadows::CloudCaster cloud{};
    cloud.draw = ground;
    cloud.draw.vb->AddRef();
    cloud.draw.layout->AddRef();
    cloud.draw.world[0] = cloud.draw.world[5] = .5f;
    cloud.draw.world[11] = 40;
    cloud.seen = GetTickCount64();
    shadows::clouds.push_back(cloud);
    shadows::frameOriginKnown = true;
    auto keptTarget = shadows::target;
    keptTarget->AddRef();
    auto keptDepth = shadows::depth;
    keptDepth->AddRef();
    shadows::render();
    assert(shadows::lastCloudCasters == 1);
    c->CopyResource(staging, color);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    assert(SUCCEEDED(c->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
    auto pixels = (unsigned char *)mapped.pData;
    auto edgePixels = [&](unsigned char *p, UINT pitch) {
        int n = 0;
        for (int x = 0; x < 512; x++) {
            int value = p[256 * pitch + x * 4];
            if (value > 140 && value < 248)
                n++;
        }
        return n;
    };
    int soft = edgePixels(pixels, mapped.RowPitch);
    assert(pixels[256 * mapped.RowPitch + 256 * 4] < 200);
    c->Unmap(staging, 0);
    shadows::target = keptTarget;
    keptTarget->AddRef();
    shadows::depth = keptDepth;
    keptDepth->AddRef();
    c->ClearRenderTargetView(keptTarget, white);
    c->ClearDepthStencilView(keptDepth, D3D11_CLEAR_DEPTH, .6f, 0);
    ground.vb->AddRef();
    ground.layout->AddRef();
    shadows::draws.push_back(ground);
    shadows::softShadows = false;
    shadows::finished = false;
    shadows::render();
    c->CopyResource(staging, color);
    assert(SUCCEEDED(c->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
    pixels = (unsigned char *)mapped.pData;
    int crisp = edgePixels(pixels, mapped.RowPitch);
    assert(pixels[256 * mapped.RowPitch + 256 * 4] < 200);
    assert(pixels[256 * mapped.RowPitch + 168 * 4] > 240);
    c->Unmap(staging, 0);
    printf("Transition pixels: soft=%d crisp=%d\n", soft, crisp);
    assert(crisp < soft);

    // Exercise actual receiver pixels against the existing caster depth map.
    auto sampleLocal = [&](bool nativeLight, bool lantern, bool warm = false, int face = 4,
                           bool outside = false, bool entity = false, bool cached = false,
                           bool compact = false, bool modelPath = false, bool focusedModel = false,
                           float contactGap = -1, bool ambientOnly = false) {
        for (auto &v : vertices)
            v[3] = face / 255.f;
        c->UpdateSubresource(vb, 0, nullptr, vertices, 0, 0);
        float gray[] = {.5f, .5f, .5f, 1};
        c->ClearRenderTargetView(keptTarget, warm ? gray : white);
        shadows::Params params{};
        assert(shadows::lightMatrix(params, ground));
        assert(params.config[2] * 2048.f <= .00801f);
        if (warm)
            params.sunTone[0] = .08f;
        params.modelFront[0] = params.modelFront[1] = params.modelFront[2] = .8f;
        params.modelFront[3] = 1;
        params.modelBack[0] = params.modelBack[1] = params.modelBack[2] = .05f;
        params.modelAmbient[0] = params.modelAmbient[1] = params.modelAmbient[2] = .1f;
        params.modelDirection[2] = 1;
        if (ambientOnly)
            params.modelFront[0] = params.modelFront[1] = params.modelFront[2] = 0;
        params.depthAdjust[1] = entity ? 1.f : 0.f;
        if (entity && !focusedModel)
            params.cascade[3] = 0;
        if (nativeLight) {
            for (int slot : {1, 4, 8}) {
                params.points[slot * 4 + 2] = 5;
                params.points[slot * 4 + 3] = 50;
                params.points[40 + slot * 4] = .3f;
                params.points[41 + slot * 4] = .2f;
                params.points[42 + slot * 4] = .4f;
            }
        }
        if (lantern) {
            params.lantern[2] = 2;
            params.lantern[3] = 12;
        }
        if (compact) {
            shadows::compactPointLights(params);
            assert(params.depthAdjust[2] == (nativeLight ? 3.f : 0.f));
        }
        if (contactGap >= 0) {
            float closestDepth = params.light[11] - contactGap * params.eye[3] * .5f;
            c->ClearDepthStencilView(shadows::mapDepth, D3D11_CLEAR_DEPTH, closestDepth, 0);
            if (shadows::nearDepth)
                c->ClearDepthStencilView(shadows::nearDepth, D3D11_CLEAR_DEPTH, closestDepth, 0);
        }
        ID3DDeviceContextState *old = nullptr;
        shadows::ctx1->SwapDeviceContextState(shadows::state, &old);
        c->OMSetRenderTargets(1, &keptTarget, keptDepth);
        c->RSSetState(shadows::raster);
        c->RSSetViewports(1, &shadows::sceneViewport);
        c->OMSetDepthStencilState(shadows::testDepth, 0);
        c->OMSetBlendState((modelPath || focusedModel) ? shadows::modelBlend : shadows::blend,
                           nullptr, ~0u);
        c->VSSetShader((modelPath || focusedModel) ? shadows::modelReceiver : shadows::receiver,
                       nullptr, 0);
        c->PSSetShader((modelPath || focusedModel) ? shadows::modelVisibility : shadows::visibility,
                       nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &shadows::constants);
        c->PSSetConstantBuffers(0, 1, &shadows::constants);
        ID3D11ShaderResourceView *maps[] = {shadows::mapView, shadows::nearView};
        c->PSSetShaderResources(0, 2, maps);
        ID3D11SamplerState *shadowSamplers[] = {shadows::sampler, shadows::rawSampler};
        c->PSSetSamplers(0, 2, shadowSamplers);
        shadows::internal = cached;
        shadows::replayBindings = {};
        shadows::bind(ground, params);
        shadows::internal = false;
        shadows::replayBindings = {};
        ID3D11ShaderResourceView *nullViews[2]{};
        c->PSSetShaderResources(0, 2, nullViews);
        c->OMSetRenderTargets(0, nullptr, nullptr);
        shadows::ctx1->SwapDeviceContextState(old, nullptr);
        shadows::drop(old);
        c->CopyResource(staging, color);
        assert(SUCCEEDED(c->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
        auto pixel = (unsigned char *)mapped.pData + 256 * mapped.RowPitch +
                     ((warm || outside) ? 168 : 256) * 4;
        std::array<int, 3> result{pixel[0], pixel[1], pixel[2]};
        assert(pixel[3] == 255);
        if (focusedModel && contactGap >= 0) {
            for (int y = 248; y < 264; ++y)
                for (int x = 248; x < 264; ++x) {
                    auto sample = (unsigned char *)mapped.pData + y * mapped.RowPitch + x * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        assert(abs(sample[channel] - result[channel]) <= 1);
                }
        }
        c->Unmap(staging, 0);
        return result;
    };
    for (int face = 0; face < 6; ++face) {
        auto baseline = sampleLocal(false, false, false, face);
        auto optimized = sampleLocal(false, false, false, face, false, false, true);
        assert(baseline == optimized);
    }
    assert(sampleLocal(false, true, true, 4, false, true) ==
           sampleLocal(false, true, true, 4, false, true, true));
    puts(
        "Cached replay matches baseline GPU pixels for all voxel normals, entity bias and lantern: PASS");
    for (int face = 0; face < 6; ++face)
        for (bool localLight : {false, true})
            for (bool lanternLight : {false, true})
                assert(sampleLocal(localLight, lanternLight, true, face, false, true) ==
                       sampleLocal(localLight, lanternLight, true, face, false, true, true, true));
    puts("Compacted local lights match original GPU pixels for all voxel normals: PASS");
    for (int face : {4, 5}) {
        // The game already lights model faces. An unoccluded model must not
        // receive another directional darkening overlay on its away faces.
        // These are the two valid normals of this XY plane. Other orientations
        // are exercised with matching slanted geometry in ModelPlaneTest.
        auto model = sampleLocal(false, false, false, face, true, true, true, false, true);
        assert(model[0] > 240 && model[1] > 240 && model[2] > 240);
    }
    auto modelOccluded = sampleLocal(false, false, false, 4, false, true, true, false, true);
    assert(modelOccluded[0] < 200);
    puts(
        "Model receivers avoid duplicate directional shading while retaining cast occlusion: PASS");
    auto unlit = sampleLocal(false, false), local = sampleLocal(true, false),
         lantern = sampleLocal(false, true);
    assert(local[0] > unlit[0] + 20);
    assert(lantern[0] > unlit[0] + 20);
    for (int channel = 0; channel < 3; channel++)
        assert(lantern[channel] >= 253);
    for (int face = 0; face < 6; face++) {
        auto side = sampleLocal(false, false, false, face);
        assert(side[0] < 200);
    }
    puts("All six block-face normals receive occluder shadows: PASS");
    // Outside the occluder, sun-facing tops stay lit while away-facing sides
    // retain the same directional shade as inside the occluder.
    auto litTop = sampleLocal(false, false, false, 4, true);
    auto awayOutside = sampleLocal(false, false, false, 5, true);
    auto awayInside = sampleLocal(false, false, false, 5, false);
    assert(litTop[0] > 240);
    assert(awayOutside[0] < 200);
    assert(abs(awayOutside[0] - awayInside[0]) <= 2);
    puts("Away-facing shade stays consistent across caster boundaries: PASS");
    // Entity tolerance must preserve actual occluder shadows and lighting.
    auto entityShadow = sampleLocal(false, false, false, 4, false, true);
    auto entityLit = sampleLocal(false, false, false, 4, true, true);
    auto entityLantern = sampleLocal(false, true, false, 4, false, true);
    assert(entityShadow[0] < 200 && entityLit[0] > 240 && entityLantern[0] >= 253);
    puts("Entity bias retains occluder shadows and lantern illumination: PASS");
    auto warmPixel = sampleLocal(false, false, true);
    assert(warmPixel[0] > 135);
    assert(warmPixel[0] > warmPixel[2] + 8);
    puts("Native point-light and lantern preservation without synthetic shadow-colored fill: PASS");
    auto modelWarm = sampleLocal(false, false, true, 4, true, true, true, false, true);
    assert(modelWarm[0] > 135 && modelWarm[0] > modelWarm[2] + 8);
    puts("Model blend retains additive warm sunlight and native target alpha: PASS");
#ifdef TEST_MODEL_RECEIVER
    auto cleanFace = sampleLocal(false, false, false, 4, false, true, true, false, false, true, 0);
    auto closeHair =
        sampleLocal(false, false, false, 4, false, true, true, false, false, true, .08f);
    assert(cleanFace[0] > 240 && closeHair[0] < 200);
    puts(
        "Direct-world model: uniform clean face and 0.08-block sunlight contact shadow retained PASS");
    auto ambientFace =
        sampleLocal(false, false, false, 4, false, true, true, false, false, true, .08f, true);
    assert(ambientFace[0] >= 253 && ambientFace[1] >= 253 && ambientFace[2] >= 253);
    puts("Occluded model retains native ambient light when sunlight is absent: PASS");
#endif
    // A slanted light on a plane with no occluder must not stripe its own surface.
    shadows::clearClouds();
    shadows::target = keptTarget;
    keptTarget->AddRef();
    shadows::depth = keptDepth;
    keptDepth->AddRef();
    c->ClearRenderTargetView(keptTarget, white);
    c->ClearDepthStencilView(keptDepth, D3D11_CLEAR_DEPTH, .6f, 0);
    ground.light[8] = .6f;
    ground.light[9] = .2f;
    ground.light[10] = .77f;
    ground.vb->AddRef();
    ground.layout->AddRef();
    shadows::draws.push_back(ground);
    shadows::finished = false;
    shadows::render();
    c->CopyResource(staging, color);
    assert(SUCCEEDED(c->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
    pixels = (unsigned char *)mapped.pData;
    int acne = 0;
    for (int y = 180; y < 330; y++)
        for (int x = 180; x < 330; x++)
            if (pixels[y * mapped.RowPitch + x * 4] < 230)
                acne++;
    c->Unmap(staging, 0);
    printf("Unoccluded angled plane dark pixels: %d\n", acne);
    fflush(stdout);
    assert(acne == 0);
    shadows::drop(keptTarget);
    shadows::drop(keptDepth);
    ID3DDeviceContextState *saved = nullptr;
    shadows::ctx1->SwapDeviceContextState(shadows::state, &saved);
    ID3D11RenderTargetView *parked = nullptr;
    ID3D11DepthStencilView *parkedDepth = nullptr;
    c->OMGetRenderTargets(1, &parked, &parkedDepth);
    assert(!parked && !parkedDepth);
    shadows::ctx1->SwapDeviceContextState(saved, nullptr);
    shadows::drop(saved);
    shadows::clearClouds();
    shadows::drop(vb);
    shadows::drop(color);
    shadows::drop(depth);
    shadows::drop(staging);
    shadows::drop(c);
    shadows::drop(d);
    puts(
        "Crisp filtering reduces edge blur, retains shadow coverage and releases scene targets: PASS");
}
