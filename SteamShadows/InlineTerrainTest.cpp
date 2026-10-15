// End to end: a terrain draw with inline shadows must give the same pixels as
// the previous path (native draw, then the receiver pass blending the shadow
// over it). Uses Steam's own world shaders read from the installed executable,
// SteamShadows' real receiver pass and inline draw function, a shadow map with
// shadowed and lit regions, local point lights, and a shifted floating origin.
// Exit 77 (skipped) when the executable is not available.
#include "SteamShadows.cpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

namespace {
std::vector<char> readShader(const char *path, std::streamoff offset) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    char header[32]{};
    input.seekg(offset);
    input.read(header, sizeof(header));
    if (!input || std::memcmp(header, "DXBC", 4))
        return {};
    uint32_t length = 0;
    std::memcpy(&length, header + 24, 4);
    std::vector<char> code(length);
    input.seekg(offset);
    input.read(code.data(), code.size());
    return input ? code : std::vector<char>{};
}
ID3D11Buffer *constant(ID3D11Device *d, const void *data, UINT bytes) {
    D3D11_BUFFER_DESC desc{(bytes + 15) / 16 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    std::vector<char> padded(desc.ByteWidth);
    memcpy(padded.data(), data, bytes);
    D3D11_SUBRESOURCE_DATA init{padded.data()};
    ID3D11Buffer *buffer = nullptr;
    assert(SUCCEEDED(d->CreateBuffer(&desc, &init, &buffer)));
    return buffer;
}
} // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *path = argc > 1 ? argv[1] : "../../../../cubeworld.exe";
    auto vsCode = readShader(path, 0x436fb0), plainCode = readShader(path, 0x43b500),
         shineCode = readShader(path, 0x438810);
    if (vsCode.empty() || plainCode.empty() || shineCode.empty()) {
        puts("cubeworld.exe not found; inline terrain test skipped.");
        return 77;
    }
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &d, nullptr, &c)))
        assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &d, nullptr, &c)));
    using namespace shadows;
    auto table = *(void ***)c;
    oldDI = replayDI = (DI)table[SLOT_DRAW_INDEXED];
    oldD = replayD = (D)table[SLOT_DRAW];
    oldClear = (Clear)table[SLOT_CLEAR_DEPTH_STENCIL_VIEW];
    auto deviceTable = *(void ***)d;
    oldCreateVS = (CreateVS)deviceTable[SLOT_CREATE_VERTEX_SHADER];
    oldCreatePS = (CreatePS)deviceTable[SLOT_CREATE_PIXEL_SHADER];
    resolution = 2048;
    nearResolution = 1024;
    assert(initialize(c));
    assert(inlineVS && inlinePS[0] && inlinePS[1]);

    // Steam's shaders, created through the tagging hook.
    ID3D11VertexShader *nativeVS = nullptr;
    ID3D11PixelShader *nativePS[2] = {};
    assert(SUCCEEDED(createVS(d, vsCode.data(), vsCode.size(), nullptr, &nativeVS)));
    assert(SUCCEEDED(createPS(d, plainCode.data(), plainCode.size(), nullptr, &nativePS[0])));
    assert(SUCCEEDED(createPS(d, shineCode.data(), shineCode.size(), nullptr, &nativePS[1])));
    assert(nativeKind(nativeVS) == 1 && nativeKind(nativePS[0]) == 2 &&
           nativeKind(nativePS[1]) == 3);
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 4, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ID3D11InputLayout *layout = nullptr;
    assert(SUCCEEDED(d->CreateInputLayout(elements, 2, vsCode.data(), vsCode.size(), &layout)));

    // Terrain-like quads: upward faces at mixed heights plus side faces.
    std::mt19937 random(7);
    std::vector<unsigned char> vertices;
    std::vector<unsigned short> indices;
    // One quad per 12-block grid cell: like voxel terrain, no two faces overlap
    // in the same plane (the receiver pass would blend such faces twice).
    for (int q = 0; q < 400; ++q) {
        int x = (q % 20) * 12, y = (q / 20) * 12, z = 96 + random() % 64, face = random() % 6;
        unsigned char color[4] = {(unsigned char)(80 + random() % 120),
                                  (unsigned char)(80 + random() % 120),
                                  (unsigned char)(60 + random() % 100), 255};
        int corners[4][3];
        for (int k = 0; k < 4; ++k) {
            int u = k & 1, v = k >> 1;
            if (face >= 4) // horizontal
                corners[k][0] = x + u * 12, corners[k][1] = y + v * 12, corners[k][2] = z;
            else if (face >= 2)
                corners[k][0] = x + u * 12, corners[k][1] = y, corners[k][2] = z + v * 12;
            else
                corners[k][0] = x, corners[k][1] = y + u * 12, corners[k][2] = z + v * 12;
        }
        unsigned short first = (unsigned short)(vertices.size() / 8);
        for (auto &corner : corners) {
            for (int a = 0; a < 3; ++a)
                vertices.push_back((unsigned char)std::min(255, corner[a]));
            vertices.push_back((unsigned char)face);
            vertices.insert(vertices.end(), color, color + 4);
        }
        unsigned short quad[6] = {0, 1, 2, 2, 1, 3};
        for (auto i : quad)
            indices.push_back(first + i);
    }
    D3D11_BUFFER_DESC bd{(UINT)vertices.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
    D3D11_SUBRESOURCE_DATA init{vertices.data()};
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    assert(SUCCEEDED(d->CreateBuffer(&bd, &init, &vb)));
    bd = {(UINT)(indices.size() * 2), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER};
    init = {indices.data()};
    assert(SUCCEEDED(d->CreateBuffer(&bd, &init, &ib)));

    const UINT size = 512;
    D3D11_TEXTURE2D_DESC td{size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                            D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ID3D11Texture2D *color = nullptr, *readback = nullptr, *depthTexture = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &color)));
    assert(SUCCEEDED(d->CreateRenderTargetView(color, nullptr, &rtv)));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &readback)));
    td.Usage = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &depthTexture)));
    assert(SUCCEEDED(d->CreateDepthStencilView(depthTexture, nullptr, &dsv)));
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ID3D11DepthStencilState *nativeDepth = nullptr;
    d->CreateDepthStencilState(&dsd, &nativeDepth);
    D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *nativeRaster = nullptr;
    d->CreateRasterizerState(&rd, &nativeRaster);
    D3D11_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};

    // Shadow maps: one plane at light depth .5, so higher surfaces are shadowed.
    oldClear(c, mapDepth, D3D11_CLEAR_DEPTH, .4f, 0);
    if (nearDepth)
        oldClear(c, nearDepth, D3D11_CLEAR_DEPTH, .4f, 0);

    // Native constants: a camera above the area, sun overhead, two local lights.
    float P[16]{}, V[16]{}, W[16]{}, material[4] = {.95f, .9f, .85f, 1}, points[80]{},
          light[12] = {.9f, .85f, .7f, 0, .25f, .25f, .35f, 0, .3f, .2f, .93f, 0},
          ambient[4] = {.35f, .35f, .4f, .55f}, white = 0, depthOffset = 0, glow = 0;
    P[0] = P[5] = 1.f / 130, P[10] = 1.f / 400, P[11] = .5f, P[15] = 1;
    V[0] = V[5] = V[10] = V[15] = 1;
    V[3] = -128, V[7] = -128, V[11] = -150;
    W[0] = W[5] = W[10] = W[15] = 1;
    points[0] = 40, points[1] = 50, points[2] = 130, points[3] = 30;
    points[40] = .9f, points[41] = .6f, points[42] = .3f;
    points[4] = 180, points[5] = 160, points[6] = 120, points[7] = 25;
    points[44] = .3f, points[45] = .5f, points[46] = .9f;
    float sky[16] = {.4f, .55f, .8f, 1, .7f, .8f, .95f, 1, .75f, .78f, .82f, 1, 1, .9f, .7f, 1};
    float fogScale = .004f, gradient = .1f, sunPos[4] = {.2f, .5f, 1, .4f}, alpha = 1,
          shininess = .5f;
    ID3D11Buffer *pcbs[6] = {constant(d, sky, 64),      constant(d, &fogScale, 4),
                             constant(d, &gradient, 4), constant(d, sunPos, 16),
                             constant(d, &alpha, 4),    constant(d, &shininess, 4)};

    Params p{};
    p.light[0] = 1.f / 128, p.light[3] = -1;
    p.light[5] = 1.f / 128, p.light[7] = -1;
    p.light[10] = 1.f / 200, p.light[11] = -.25f;
    p.light[15] = 1;
    memcpy(p.nearLight, p.light, 64);
    p.config[0] = 1.f / resolution, p.config[1] = .9f, p.config[2] = .002f, p.config[3] = 400;
    p.eye[0] = 128, p.eye[1] = 128, p.eye[2] = 150, p.eye[3] = .01f;
    p.sun[0] = .3f, p.sun[1] = .2f, p.sun[2] = .93f, p.sun[3] = 0;
    p.sunTone[0] = .03f, p.sunTone[1] = .4f, p.sunTone[2] = 1;
    p.cascade[0] = 1.f / nearResolution, p.cascade[1] = 300, p.cascade[2] = 240;

    // mode 0: native only, 1: native + receiver pass (previous path), 2: inline.
    auto run = [&](int mode, int shine, float originShift) -> std::vector<unsigned char> {
        bool inlineDraw = mode == 2;
        float world[16], view[16];
        memcpy(world, W, 64);
        memcpy(view, V, 64);
        // A floating origin moved by `originShift`: world positions shift, the
        // camera compensates, so the image is the same.
        world[3] -= originShift;
        // Local lights live in the same floating frame.
        float shifted[80];
        memcpy(shifted, points, sizeof(shifted));
        for (int j = 0; j < 10; ++j)
            shifted[j * 4] -= originShift;
        view[3] += originShift;
        ID3D11Buffer *vcbs[10] = {constant(d, P, 64),       constant(d, view, 64),
                                  constant(d, world, 64),   constant(d, material, 16),
                                  constant(d, shifted, 320), constant(d, light, 48),
                                  constant(d, ambient, 16), constant(d, &white, 4),
                                  constant(d, &depthOffset, 4), constant(d, &glow, 4)};
        float clear[4] = {.1f, .2f, .3f, 1};
        c->ClearRenderTargetView(rtv, clear);
        c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1, 0);
        UINT stride = 8, offset = 0;
        c->IASetInputLayout(layout);
        c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(nativeVS, nullptr, 0);
        c->PSSetShader(nativePS[shine], nullptr, 0);
        c->VSSetConstantBuffers(0, 10, vcbs);
        c->PSSetConstantBuffers(0, 6, pcbs);
        c->RSSetState(nativeRaster);
        c->RSSetViewports(1, &viewport);
        c->OMSetDepthStencilState(nativeDepth, 0);
        c->OMSetBlendState(nullptr, nullptr, ~0u);
        c->OMSetRenderTargets(1, &rtv, dsv);
        Draw x{};
        x.vb = vb, x.ib = ib, x.layout = layout;
        vb->AddRef(), ib->AddRef(), layout->AddRef();
        x.stride = 8, x.count = (UINT)indices.size(), x.format = DXGI_FORMAT_R16_UINT;
        x.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        x.terrain = true;
        memcpy(x.world, world, 64), memcpy(x.view, view, 64), memcpy(x.proj, P, 64);
        memcpy(x.light, light, 48), memcpy(x.pointLights, shifted, 320);
        memcpy(x.material, material, 16), memcpy(x.ambient, ambient, 16);
        x.lightProtection = std::clamp(std::max(white, glow), 0.f, 1.f);
        x.depthOffset = depthOffset;
        draws.push_back(x);
        if (inlineDraw) {
            // The previous frame (path A) left inlineFrame; this frame's origin moved.
            frameOriginKnown = true;
            frameOriginX = inlineFrame.originX + originShift;
            frameOriginY = inlineFrame.originY;
            inlineFrame.uploaded = false;
            assert(drawInlineTerrain(c, x.count, 0, 0));
            assert(draws.back().inlined);
        } else if (mode == 0) {
            oldDI(c, x.count, 0, 0);
        } else {
            oldDI(c, x.count, 0, 0);
            // The receiver pass over the stored native colour.
            target = rtv, depth = dsv;
            rtv->AddRef(), dsv->AddRef();
            frameOriginKnown = true;
            frameOriginX = frameOriginY = 0;
            internal = true;
            Params receiver = p;
            c->VSSetConstantBuffers(0, 1, &constants);
            c->PSSetConstantBuffers(0, 1, &constants);
            renderReceivers(receiver, viewport);
            internal = false;
            ID3D11ShaderResourceView *none[2]{};
            c->PSSetShaderResources(0, 2, none);
            replayBindings = {};
            assert(inlineFrame.ready && lastReceivers == 1);
        }
        clearDraws();
        c->OMSetRenderTargets(0, nullptr, nullptr);
        c->CopyResource(readback, color);
        D3D11_MAPPED_SUBRESOURCE map{};
        assert(SUCCEEDED(c->Map(readback, 0, D3D11_MAP_READ, 0, &map)));
        std::vector<unsigned char> image(size * size * 4);
        for (UINT y = 0; y < size; ++y)
            memcpy(&image[y * size * 4], (char *)map.pData + y * map.RowPitch, size * 4);
        c->Unmap(readback, 0);
        for (auto b : vcbs)
            b->Release();
        return image;
    };

    for (int shine = 0; shine < 2; ++shine)
        for (float shift : {0.f, 16.f}) {
            auto native = run(0, shine, 0);
            auto before = run(1, shine, 0); // also leaves inlineFrame for the next frame
            auto after = run(2, shine, shift);
            int worst = 0;
            size_t shadowed = 0, bad = 0, reported = 0;
            for (size_t i = 0; i < before.size(); i += 4) {
                int change = 0;
                for (int k = 0; k < 3; ++k) {
                    worst = std::max(worst, abs(int(before[i + k]) - int(after[i + k])));
                    change = std::max(change, abs(int(before[i + k]) - int(native[i + k])));
                }
                shadowed += change > 8;
                int diff = 0;
                for (int k = 0; k < 3; ++k)
                    diff = std::max(diff, abs(int(before[i + k]) - int(after[i + k])));
                if (diff > 2 && reported++ < 6)
                    printf("  pixel (%zu,%zu): native %d,%d,%d receiver %d,%d,%d inline %d,%d,%d\n",
                           (i / 4) % size, (i / 4) / size, native[i], native[i + 1], native[i + 2],
                           before[i], before[i + 1], before[i + 2], after[i], after[i + 1],
                           after[i + 2]);
                bad += diff > 2;
            }
            printf("  pixels differing by more than 2/255: %zu\n", bad);
            printf("%s, origin shift %.0f: %zu pixels changed by the shadow, inline vs receiver "
                   "max difference %d/255\n",
                   shine ? "shine" : "plain", shift, shadowed, worst);
            assert(shadowed > size * size / 20); // the shadow really shades part of the image
            assert(worst <= 2);
        }
    puts("Inline terrain shadows match the receiver pass PASS");
    return 0;
}
