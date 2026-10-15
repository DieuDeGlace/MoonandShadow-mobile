// Inline terrain shadows re-express Steam's world shaders. This compares them
// with the real shaders read from the user's installed executable (never
// copied into the repository):
//   * vertex shader 0x436fb0: every output, captured with stream output, for
//     random vertices (native UBYTE4 and WorldMotion float layouts) under
//     random native constants;
//   * pixel shaders 0x438810 (shine) and 0x43b500 (plain): every pixel of
//     random triangles rendered to a float target.
// Exit 77 (skipped) when the executable is not available.
#include "SteamShadows.cpp"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {
const char *defaultGame = "../../../../cubeworld.exe";

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

ID3DBlob *compileInline(const char *entry, const char *profile, bool vertex, int shine) {
    std::string source = std::string(shadows::hlsl) + inlineTerrain::hlsl;
    const D3D_SHADER_MACRO vsMacros[] = {{"MODEL_DIRECT_LIGHT", "1"}, {"INLINE_TERRAIN", "1"},
                                         {"INLINE_VS", "1"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO psMacros[] = {{"MODEL_DIRECT_LIGHT", "1"}, {"INLINE_TERRAIN", "1"},
                                         {"INLINE_PS", "1"},
                                         {"INLINE_SHINE", shine ? "1" : "0"},
                                         {nullptr, nullptr}};
    ID3DBlob *code = nullptr, *error = nullptr;
    HRESULT hr = D3DCompile(source.c_str(), source.size(), "Inline", vertex ? vsMacros : psMacros,
                            nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                            &error);
    if (error) {
        fprintf(stderr, "%s\n", (const char *)error->GetBufferPointer());
        error->Release();
    }
    assert(SUCCEEDED(hr));
    return code;
}

uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
// Distance in units in the last place (0 for equal values, incl. +0/-0).
uint32_t ulps(float a, float b) {
    if (a == b)
        return 0;
    if (std::isnan(a) || std::isnan(b))
        return 0xffffffff;
    int32_t ia = int32_t(bits(a)), ib = int32_t(bits(b));
    if (ia < 0)
        ia = int32_t(0x80000000) - ia;
    if (ib < 0)
        ib = int32_t(0x80000000) - ib;
    int64_t d = int64_t(ia) - int64_t(ib);
    return uint32_t(d < 0 ? -d : d);
}

struct Buffers {
    ID3D11Buffer *b[10]{};
};
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
    const char *path = argc > 1 ? argv[1] : defaultGame;
    auto nativeVS = readShader(path, 0x436fb0), shinePS = readShader(path, 0x438810),
         plainPS = readShader(path, 0x43b500);
    if (nativeVS.empty() || shinePS.empty() || plainPS.empty()) {
        puts("cubeworld.exe not found; native shader equivalence skipped.");
        return 77;
    }
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &d, nullptr, &c)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &d, nullptr, &c)))
        return 1;
    std::mt19937 random(1234);
    auto uniform = [&](float lo, float hi) {
        return std::uniform_real_distribution<float>(lo, hi)(random);
    };

    // ---------------------------------------------------------------- VS
    auto ourVS = compileInline("nativeWorldVertex", "vs_4_0", true, 0);
    D3D11_SO_DECLARATION_ENTRY so[] = {
        {0, "SV_POSITION", 0, 0, 4, 0}, {0, "COLOR", 0, 0, 4, 0},   {0, "TEXCOORD", 0, 0, 4, 0},
        {0, "TEXCOORD", 1, 0, 3, 0},    {0, "TEXCOORD", 2, 0, 3, 0}, {0, "TEXCOORD", 3, 0, 3, 0}};
    const UINT outFloats = 21, stride = outFloats * 4;
    ID3D11VertexShader *vs[2];
    ID3D11GeometryShader *gs[2];
    assert(SUCCEEDED(d->CreateVertexShader(nativeVS.data(), nativeVS.size(), nullptr, &vs[0])));
    assert(SUCCEEDED(d->CreateVertexShader(ourVS->GetBufferPointer(), ourVS->GetBufferSize(),
                                           nullptr, &vs[1])));
    assert(SUCCEEDED(d->CreateGeometryShaderWithStreamOutput(
        nativeVS.data(), nativeVS.size(), so, 6, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr,
        &gs[0])));
    assert(SUCCEEDED(d->CreateGeometryShaderWithStreamOutput(
        ourVS->GetBufferPointer(), ourVS->GetBufferSize(), so, 6, &stride, 1,
        D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs[1])));
    D3D11_INPUT_ELEMENT_DESC unorm[] = {
        {"POSITION", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 4, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    D3D11_INPUT_ELEMENT_DESC floats[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ID3D11InputLayout *layouts[2];
    assert(SUCCEEDED(d->CreateInputLayout(unorm, 2, nativeVS.data(), nativeVS.size(), &layouts[0])));
    assert(SUCCEEDED(d->CreateInputLayout(floats, 2, nativeVS.data(), nativeVS.size(), &layouts[1])));
    const UINT vertices = 4096;
    uint32_t worstVS = 0, worstPosition = 0;
    unsigned vsCases = 0;
    for (int trial = 0; trial < 24; ++trial) {
        // Random native constants, including inactive/active lights and glow signs.
        float P[16], V[16], W[16], material[4], points[80]{}, light[12], ambient[4];
        for (int i = 0; i < 16; ++i)
            P[i] = uniform(-1.5f, 1.5f), V[i] = uniform(-1, 1), W[i] = uniform(-.2f, .2f);
        W[3] = uniform(-40, 40), W[7] = uniform(-40, 40), W[11] = uniform(-10, 10);
        W[12] = W[13] = W[14] = 0, W[15] = 1;
        if (trial % 3 == 0) // terrain: identity scale
            for (int i : {0, 5, 10})
                W[i] = 1, W[(i + 1) % 16] = 0;
        for (auto &m : material)
            m = uniform(0, 1);
        for (int j = 0; j < 10; ++j)
            if (random() % 2) {
                points[j * 4] = uniform(-40, 40), points[j * 4 + 1] = uniform(-40, 40);
                points[j * 4 + 2] = uniform(-10, 10), points[j * 4 + 3] = uniform(5, 40);
                for (int k = 0; k < 3; ++k)
                    points[40 + j * 4 + k] = uniform(0, 1);
            }
        for (auto &l : light)
            l = uniform(-1, 1);
        for (auto &a : ambient)
            a = uniform(0, 1);
        float white = trial % 4 == 0 ? uniform(0, 1) : 0, depth = uniform(-.01f, .01f);
        float glow = trial % 3 == 0 ? 0 : trial % 3 == 1 ? uniform(-1, -.01f) : uniform(.01f, 1);
        ID3D11Buffer *cbs[10] = {constant(d, P, 64),       constant(d, V, 64),
                                 constant(d, W, 64),       constant(d, material, 16),
                                 constant(d, points, 320), constant(d, light, 48),
                                 constant(d, ambient, 16), constant(d, &white, 4),
                                 constant(d, &depth, 4),   constant(d, &glow, 4)};
        bool floatLayout = trial % 2 == 1;
        UINT vstride = floatLayout ? 20 : 8;
        std::vector<unsigned char> data(vertices * vstride);
        for (UINT v = 0; v < vertices; ++v) {
            unsigned char *p = data.data() + v * vstride;
            unsigned char x = random() % 256, y = random() % 256, z = random() % 256,
                          face = random() % 7; // 6 = none of the six face ids
            if (floatLayout) {
                float f[4] = {x / 255.f + uniform(-.001f, .001f), y / 255.f, z / 255.f, face / 255.f};
                memcpy(p, f, 16);
                p += 16;
            } else {
                p[0] = x, p[1] = y, p[2] = z, p[3] = face;
                p += 4;
            }
            for (int k = 0; k < 4; ++k)
                p[k] = random() % 256;
        }
        D3D11_BUFFER_DESC vd{(UINT)data.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
        D3D11_SUBRESOURCE_DATA vinit{data.data()};
        ID3D11Buffer *vb = nullptr;
        assert(SUCCEEDED(d->CreateBuffer(&vd, &vinit, &vb)));
        std::vector<float> results[2];
        for (int s = 0; s < 2; ++s) {
            D3D11_BUFFER_DESC od{vertices * stride, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT};
            ID3D11Buffer *out = nullptr, *staging = nullptr;
            assert(SUCCEEDED(d->CreateBuffer(&od, nullptr, &out)));
            od.Usage = D3D11_USAGE_STAGING;
            od.BindFlags = 0;
            od.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            assert(SUCCEEDED(d->CreateBuffer(&od, nullptr, &staging)));
            UINT offset = 0;
            c->IASetInputLayout(layouts[floatLayout]);
            c->IASetVertexBuffers(0, 1, &vb, &vstride, &offset);
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            c->VSSetShader(vs[s], nullptr, 0);
            c->VSSetConstantBuffers(0, 10, cbs);
            c->GSSetShader(gs[s], nullptr, 0);
            c->SOSetTargets(1, &out, &offset);
            c->Draw(vertices, 0);
            ID3D11Buffer *none = nullptr;
            c->SOSetTargets(1, &none, &offset);
            c->CopyResource(staging, out);
            D3D11_MAPPED_SUBRESOURCE map{};
            assert(SUCCEEDED(c->Map(staging, 0, D3D11_MAP_READ, 0, &map)));
            results[s].assign((float *)map.pData, (float *)map.pData + vertices * outFloats);
            c->Unmap(staging, 0);
            staging->Release();
            out->Release();
        }
        for (size_t i = 0; i < results[0].size(); ++i) {
            uint32_t u = ulps(results[0][i], results[1][i]);
            // Values near zero compare by absolute difference.
            if (std::fabs(results[0][i] - results[1][i]) < 1e-7f)
                u = 0;
            // SV_POSITION and the clip copy (TEXCOORD0) decide depth and coverage.
            size_t output = i % outFloats;
            if (output < 4 || (output >= 8 && output < 12))
                worstPosition = std::max(worstPosition, u);
            if (u > worstVS) {
                worstVS = u;
                if (u > 4)
                    printf("VS mismatch trial %d vertex %zu output %zu: native=%.9g ours=%.9g\n",
                           trial, i / outFloats, i % outFloats, results[0][i], results[1][i]);
            }
        }
        vsCases += vertices;
        vb->Release();
        for (auto b : cbs)
            b->Release();
    }
    printf("World vertex shader: %u vertices x %u outputs, max difference %u ULP (positions %u)\n",
           vsCases, outFloats, worstVS, worstPosition);

    // ---------------------------------------------------------------- PS
    const UINT size = 256;
    D3D11_TEXTURE2D_DESC td{size, size, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0},
                            D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ID3D11Texture2D *rtTexture = nullptr, *readback = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &rtTexture)));
    assert(SUCCEEDED(d->CreateRenderTargetView(rtTexture, nullptr, &rtv)));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &readback)));
    D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    rd.DepthClipEnable = FALSE;
    ID3D11RasterizerState *raster = nullptr;
    d->CreateRasterizerState(&rd, &raster);
    uint32_t worstPS[2]{};
    for (int shine = 0; shine < 2; ++shine) {
        auto &native = shine ? shinePS : plainPS;
        auto ours = compileInline("nativeWorldPixel", "ps_4_0", false, shine);
        ID3D11PixelShader *ps[2];
        assert(SUCCEEDED(d->CreatePixelShader(native.data(), native.size(), nullptr, &ps[0])));
        assert(SUCCEEDED(d->CreatePixelShader(ours->GetBufferPointer(), ours->GetBufferSize(),
                                              nullptr, &ps[1])));
        for (int trial = 0; trial < 12; ++trial) {
            // Vertex constants place random triangles across the target.
            float P[16]{}, V[16]{}, W[16]{}, material[4] = {1, 1, 1, 1}, points[80]{},
                  light[12] = {.8f, .8f, .7f, 0, .3f, .3f, .4f, 0, .3f, .2f, .9f, 0},
                  ambient[4] = {.3f, .3f, .35f, .5f}, zero = 0;
            P[0] = P[5] = 1.f / 128, P[10] = .001f, P[15] = 1;
            P[3] = -1, P[7] = -1;
            V[0] = V[5] = V[10] = V[15] = 1;
            W[0] = W[5] = W[10] = W[15] = 1;
            ID3D11Buffer *vcbs[10] = {constant(d, P, 64),       constant(d, V, 64),
                                      constant(d, W, 64),       constant(d, material, 16),
                                      constant(d, points, 320), constant(d, light, 48),
                                      constant(d, ambient, 16), constant(d, &zero, 4),
                                      constant(d, &zero, 4),    constant(d, &zero, 4)};
            float sky[16];
            for (auto &s : sky)
                s = uniform(0, 1);
            float fogScale = uniform(.002f, .03f), gradient = uniform(-.5f, .5f),
                  sunPos[4] = {uniform(-1, 1), uniform(-1, 1), uniform(.5f, 2), uniform(0, 1)},
                  alpha = uniform(.2f, 1), shininess = trial % 2 ? uniform(.1f, 1) : 0;
            ID3D11Buffer *pcbs[6] = {constant(d, sky, 64),    constant(d, &fogScale, 4),
                                     constant(d, &gradient, 4), constant(d, sunPos, 16),
                                     constant(d, &alpha, 4),  constant(d, &shininess, 4)};
            const UINT tris = 600;
            std::vector<unsigned char> data(tris * 3 * 8);
            for (auto &b : data)
                b = random() % 256;
            for (UINT v = 0; v < tris * 3; ++v)
                data[v * 8 + 3] = random() % 6;
            D3D11_BUFFER_DESC vd{(UINT)data.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
            D3D11_SUBRESOURCE_DATA vinit{data.data()};
            ID3D11Buffer *vb = nullptr;
            assert(SUCCEEDED(d->CreateBuffer(&vd, &vinit, &vb)));
            std::vector<float> image[2];
            for (int s = 0; s < 2; ++s) {
                float clear[4] = {-7, -7, -7, -7};
                c->ClearRenderTargetView(rtv, clear);
                UINT vstride = 8, offset = 0;
                c->IASetInputLayout(layouts[0]);
                c->IASetVertexBuffers(0, 1, &vb, &vstride, &offset);
                c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                c->VSSetShader(vs[0], nullptr, 0);
                c->VSSetConstantBuffers(0, 10, vcbs);
                c->GSSetShader(nullptr, nullptr, 0);
                c->PSSetShader(ps[s], nullptr, 0);
                c->PSSetConstantBuffers(0, 6, pcbs);
                c->RSSetState(raster);
                D3D11_VIEWPORT vp{0, 0, (float)size, (float)size, 0, 1};
                c->RSSetViewports(1, &vp);
                c->OMSetRenderTargets(1, &rtv, nullptr);
                c->Draw(tris * 3, 0);
                c->OMSetRenderTargets(0, nullptr, nullptr);
                c->CopyResource(readback, rtTexture);
                D3D11_MAPPED_SUBRESOURCE map{};
                assert(SUCCEEDED(c->Map(readback, 0, D3D11_MAP_READ, 0, &map)));
                image[s].resize(size * size * 4);
                for (UINT y = 0; y < size; ++y)
                    memcpy(&image[s][y * size * 4], (char *)map.pData + y * map.RowPitch, size * 16);
                c->Unmap(readback, 0);
            }
            for (size_t i = 0; i < image[0].size(); ++i) {
                uint32_t u = std::fabs(image[0][i] - image[1][i]) < 1e-6f
                                 ? 0
                                 : ulps(image[0][i], image[1][i]);
                if (u > worstPS[shine]) {
                    worstPS[shine] = u;
                    if (u > 64)
                        printf("PS %s mismatch trial %d pixel %zu channel %zu: native=%.9g ours=%.9g\n",
                               shine ? "shine" : "plain", trial, i / 4, i % 4, image[0][i],
                               image[1][i]);
                }
            }
            vb->Release();
            for (auto b : vcbs)
                b->Release();
            for (auto b : pcbs)
                b->Release();
        }
        printf("World pixel shader (%s): %u pixels x 12 trials, max difference %u ULP\n",
               shine ? "shine 0x438810" : "plain 0x43b500", size * size, worstPS[shine]);
        ps[0]->Release();
        ps[1]->Release();
        ours->Release();
    }

    // Positions/depth must match exactly; colour within a few ULP is ~20000x below
    // one step of the 8-bit scene target.
    assert(worstPosition == 0);
    assert(worstVS <= 16);
    assert(worstPS[0] <= 64 && worstPS[1] <= 64);
    puts("Native world shader equivalence PASS");
    return 0;
}
