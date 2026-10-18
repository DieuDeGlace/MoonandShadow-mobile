#include "SteamShadows.cpp"
#include <array>
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>

namespace {
constexpr UINT outputSize = 128;
constexpr float depthSpan = 2048.f;
constexpr float closeGap = .08f;

// Construct the depth texture from a world-space plane, independently of the
// production filter's receiver-plane gradient. The light looks down world Z.
std::vector<float> planeDepth(UINT size, float radius, float slopeX, float slopeY, float gap) {
    std::vector<float> result(size_t(size) * size);
    for (UINT y = 0; y < size; ++y)
        for (UINT x = 0; x < size; ++x) {
            float worldX = ((x + .5f) / size - .5f) * 2 * radius;
            float worldY = (.5f - (y + .5f) / size) * 2 * radius;
            float worldZ = slopeX * worldX + slopeY * worldY + gap;
            result[size_t(y) * size + x] = .5f - worldZ / depthSpan;
        }
    return result;
}

constexpr char fixture[] = R"FIXTURE(
cbuffer Fixture:register(b2){float4 geometry;float4 sampling;};
float4 fixtureVertex(uint vertex:SV_VertexID):SV_POSITION{
 float2 p=float2((vertex<<1)&2,vertex&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float4 fixturePixel(float4 pixel:SV_POSITION):SV_TARGET{
 float2 uv=.35+.3*pixel.xy/sampling.x+sampling.yz;
 V input;input.p=pixel;input.w.xy=(uv-.5)*float2(2*geometry.x,-2*geometry.x);
 input.w.z=dot(input.w.xy,geometry.yz);
 input.n=normalize(float3(-geometry.yz,1));input.local=0;
 float lit=shadowVisibility(input,light,config,shadowMap);
 return float4(lit,lit,lit,1);
})FIXTURE";

struct FixtureParameters {
    float geometry[4];
    float sampling[4];
};

ID3DBlob *compile(const std::string &source, const char *entry, const char *profile) {
    // Only the native-model path enables the new filter in production. This
    // fixture calls exactly that helper, without duplicating filtering math.
    D3D_SHADER_MACRO macros[] = {{"MODEL_PLANE_FILTER", "1"}, {nullptr, nullptr}};
    ID3DBlob *code = nullptr, *error = nullptr;
    HRESULT result = D3DCompile(source.c_str(), source.size(), "ModelPlaneTest", macros, nullptr,
                                entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (error) {
        fprintf(stderr, "%s\n", (const char *)error->GetBufferPointer());
        error->Release();
    }
    assert(SUCCEEDED(result));
    return code;
}
} // namespace

int main(int argc, char **argv) {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, nullptr, &context)));
    std::string shader = shadows::hlsl;
    if (argc == 2) {
        // Optional pre-change shader snapshot for proving the regression. The
        // normal CTest invocation always exercises current production source.
        std::ifstream input(argv[1], std::ios::binary);
        assert(input);
        shader.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    std::string source = shader + fixture;
    auto vertexCode = compile(source, "fixtureVertex", "vs_4_1");
    auto pixelCode = compile(source, "fixturePixel", "ps_4_1");
    ID3D11VertexShader *vertex = nullptr;
    ID3D11PixelShader *pixel = nullptr;
    assert(SUCCEEDED(device->CreateVertexShader(vertexCode->GetBufferPointer(),
                                                vertexCode->GetBufferSize(), nullptr, &vertex)));
    assert(SUCCEEDED(device->CreatePixelShader(pixelCode->GetBufferPointer(),
                                               pixelCode->GetBufferSize(), nullptr, &pixel)));
    vertexCode->Release();
    pixelCode->Release();

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(shadows::Params);
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer *parameters = nullptr, *fixtureParameters = nullptr;
    assert(SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &parameters)));
    buffer.ByteWidth = sizeof(FixtureParameters);
    assert(SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &fixtureParameters)));

    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = texture.Height = outputSize;
    texture.MipLevels = texture.ArraySize = 1;
    texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R32_FLOAT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *color = nullptr, *staging = nullptr;
    ID3D11RenderTargetView *target = nullptr;
    assert(SUCCEEDED(device->CreateTexture2D(&texture, nullptr, &color)));
    assert(SUCCEEDED(device->CreateRenderTargetView(color, nullptr, &target)));
    texture.BindFlags = 0;
    texture.Usage = D3D11_USAGE_STAGING;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&texture, nullptr, &staging)));

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sampler.BorderColor[0] = sampler.BorderColor[1] = sampler.BorderColor[2] =
        sampler.BorderColor[3] = 1;
    sampler.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState *comparison = nullptr, *raw = nullptr;
    assert(SUCCEEDED(device->CreateSamplerState(&sampler, &comparison)));
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    assert(SUCCEEDED(device->CreateSamplerState(&sampler, &raw)));
    ID3D11SamplerState *samplers[] = {comparison, raw};
    context->PSSetSamplers(0, 2, samplers);
    context->PSSetConstantBuffers(0, 1, &parameters);
    context->PSSetConstantBuffers(2, 1, &fixtureParameters);
    context->VSSetShader(vertex, nullptr, 0);
    context->PSSetShader(pixel, nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{0, 0, (float)outputSize, (float)outputSize, 0, 1};
    context->RSSetViewports(1, &viewport);
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rasterState = nullptr;
    assert(SUCCEEDED(device->CreateRasterizerState(&raster, &rasterState)));
    context->RSSetState(rasterState);

    unsigned failures = 0, cases = 0;
    for (bool useNearMap : {false, true}) {
        UINT size = useNearMap ? 2048 : 4096;
        float radius = useNearMap ? 32.f : 120.f;
        shadows::Params values{};
        values.light[0] = values.light[5] = 1 / radius;
        values.light[10] = -1 / depthSpan;
        values.light[11] = .5f;
        values.light[15] = 1;
        values.config[0] = 1.f / size;
        values.config[2] = .008f / depthSpan;
        values.config[3] = radius;
        values.eye[3] = 2 / depthSpan;
        values.sun[2] = 1;
        values.depthAdjust[1] = 1;
        context->UpdateSubresource(parameters, 0, nullptr, &values, 0, 0);

        // Above, within, and below the grazing transition, plus the accepted
        // contact-shadow slope. Sweep subtexel phases at every orientation.
        for (float steepness : {1.f, 3.5f, 5.f, 8.f, 10.f})
            for (float sign : {-1.f, 1.f})
                for (float gap : {0.f, closeGap}) {
                    auto depths = planeDepth(size, radius, steepness * sign * .75f,
                                             steepness * sign * .35f, gap);
                    texture.Width = texture.Height = size;
                    texture.Usage = D3D11_USAGE_DEFAULT;
                    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    texture.CPUAccessFlags = 0;
                    ID3D11Texture2D *map = nullptr;
                    ID3D11ShaderResourceView *view = nullptr;
                    D3D11_SUBRESOURCE_DATA data{depths.data(), UINT(size * sizeof(float)), 0};
                    assert(SUCCEEDED(device->CreateTexture2D(&texture, &data, &map)));
                    assert(SUCCEEDED(device->CreateShaderResourceView(map, nullptr, &view)));
                    context->PSSetShaderResources(0, 1, &view);

                    for (float phase : {0.f, .17f, .43f, .79f}) {
                        FixtureParameters test{
                            {radius, steepness * sign * .75f, steepness * sign * .35f, 0},
                            {(float)outputSize, phase / size, (.91f - phase) / size, 0}};
                        context->UpdateSubresource(fixtureParameters, 0, nullptr, &test, 0, 0);
                        context->Draw(3, 0);
                        context->CopyResource(staging, color);
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        assert(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
                        unsigned wrong = 0;
                        float minimum = 1, maximum = 0;
                        for (UINT y = 8; y < outputSize - 8; ++y) {
                            const auto *row = (const float *)((const char *)mapped.pData +
                                                              size_t(y) * mapped.RowPitch);
                            for (UINT x = 8; x < outputSize - 8; ++x) {
                                minimum = std::min(minimum, row[x]);
                                maximum = std::max(maximum, row[x]);
                                float facing =
                                    1.f / std::sqrt(1.f + steepness * steepness *
                                                              (.75f * .75f + .35f * .35f));
                                // Unoccluded: lit (grazing faces are compared off the
                                // surface, so they no longer take a directional shade).
                                // A close occluder shadows faces past the grazing band;
                                // in the band only uniformity is required (no stripes).
                                float sunSide = std::clamp(facing / .10f, 0.f, 1.f);
                                sunSide = sunSide * sunSide * (3 - 2 * sunSide);
                                bool grazing = facing < .30f;
                                if (!std::isfinite(row[x]))
                                    ++wrong;
                                else if (gap > 0 && grazing)
                                    continue;
                                else if (std::abs(row[x] - (gap > 0 ? 0.f : sunSide)) > .01f)
                                    ++wrong;
                            }
                        }
                        context->Unmap(staging, 0);
                        if (maximum - minimum > .01f)
                            ++wrong; // stripes or seams across subtexel positions
                        printf(
                            "%s slope=%.0f sign=%+.0f gap=%.2f phase=%.2f: wrong=%u range=[%.5f %.5f]\n",
                            useNearMap ? "near" : "far", steepness, sign, gap, phase, wrong,
                            minimum, maximum);
                        ++cases;
                        failures += wrong != 0;
                    }
                    ID3D11ShaderResourceView *nullView = nullptr;
                    context->PSSetShaderResources(0, 1, &nullView);
                    view->Release();
                    map->Release();
                }
    }
    context->ClearState();
    rasterState->Release();
    comparison->Release();
    raw->Release();
    parameters->Release();
    fixtureParameters->Release();
    target->Release();
    color->Release();
    staging->Release();
    vertex->Release();
    pixel->Release();
    context->Release();
    device->Release();
    printf("Model receiver-plane regression: %u/%u cases passed\n", cases - failures, cases);
    return failures ? 1 : 0;
}
