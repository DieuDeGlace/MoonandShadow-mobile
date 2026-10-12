#include "SteamShadows.cpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

// The native shader is read from the user's installed executable, never
// copied into the repository. This is an optional local verification test.
namespace {
constexpr std::streamoff nativeOffset = 0x436fb0;
constexpr const char *defaultGame = "../../../../cubeworld.exe";

struct Vertex {
    float position[4];
    float color[4];
};
struct Captured {
    float clip[4];
    float position[3];
    float normal[3];
};

std::vector<char> readNativeShader(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    char header[32]{};
    input.seekg(nativeOffset);
    input.read(header, sizeof(header));
    if (!input || std::memcmp(header, "DXBC", 4))
        return {};
    uint32_t length = 0;
    std::memcpy(&length, header + 24, sizeof(length));
    if (length < sizeof(header) || length > 1024 * 1024)
        return {};
    std::vector<char> code(length);
    input.seekg(nativeOffset);
    input.read(code.data(), code.size());
    return input ? code : std::vector<char>{};
}

ID3DBlob *compile(const std::string &source, const char *entry, const char *profile) {
    ID3DBlob *code = nullptr, *error = nullptr;
    const D3D_SHADER_MACRO modelMacros[] = {
        {"MODEL_DIRECT_LIGHT", "1"}, {"MODEL_PLANE_FILTER", "1"}, {nullptr, nullptr}};
    // The capture GS must have the same interface as the actual model VS.
    bool model =
        !strcmp(entry, "modelReceiverVertex") || source.find("point V input") != std::string::npos;
    HRESULT result =
        D3DCompile(source.c_str(), source.size(), "NativeClipTest", model ? modelMacros : nullptr,
                   nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (error) {
        std::fprintf(stderr, "%s\n", static_cast<const char *>(error->GetBufferPointer()));
        error->Release();
    }
    assert(SUCCEEDED(result));
    return code;
}

constexpr const char *nativeCaptureSource = R"SHADER(
struct NativeOutput {
 float4 clip:SV_POSITION;float4 color:COLOR0;float4 originalClip:TEXCOORD0;
 float3 normal:TEXCOORD1;float3 direction:TEXCOORD2;float3 viewPosition:TEXCOORD3;
};
struct Captured {float4 clip:SV_POSITION;float3 position:TEXCOORD0;float3 normal:TEXCOORD1;};
[maxvertexcount(1)]
void capture(point NativeOutput input[1], inout PointStream<Captured> output) {
 Captured value;value.clip=input[0].clip;value.position=input[0].viewPosition;
 value.normal=input[0].normal;output.Append(value);
}
)SHADER";

constexpr const char *customCaptureSource = R"SHADER(
struct Captured {float4 clip:SV_POSITION;float3 position:TEXCOORD0;float3 normal:TEXCOORD1;};
[maxvertexcount(1)]
void capture(point V input[1], inout PointStream<Captured> output) {
 Captured value;value.clip=input[0].p;value.position=input[0].w;
 value.normal=input[0].n;output.Append(value);
}
)SHADER";

ID3D11GeometryShader *captureShader(ID3D11Device *device, const std::string &source) {
    auto code = compile(source, "capture", "gs_4_0");
    const D3D11_SO_DECLARATION_ENTRY declaration[] = {
        {0, "SV_POSITION", 0, 0, 4, 0}, {0, "TEXCOORD", 0, 0, 3, 0}, {0, "TEXCOORD", 1, 0, 3, 0}};
    UINT stride = sizeof(Captured);
    ID3D11GeometryShader *shader = nullptr;
    assert(SUCCEEDED(device->CreateGeometryShaderWithStreamOutput(
        code->GetBufferPointer(), code->GetBufferSize(), declaration, ARRAYSIZE(declaration),
        &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &shader)));
    code->Release();
    return shader;
}

uint32_t ulpDistance(float left, float right) {
    if (left == right)
        return 0;
    if (!std::isfinite(left) || !std::isfinite(right))
        return std::numeric_limits<uint32_t>::max();
    auto ordered = [](float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits & 0x80000000u ? ~bits : bits | 0x80000000u;
    };
    uint32_t a = ordered(left), b = ordered(right);
    return a > b ? a - b : b - a;
}

void worldMatrix(float out[16], float angle, float sx, float sy, float sz) {
    float c = std::cos(angle), s = std::sin(angle);
    float tilt = angle * .37f, ct = std::cos(tilt), st = std::sin(tilt);
    // Rotation around world Z and Y, followed by independent local-axis scale.
    float values[] = {c * ct * sx, -s * sy,     c * st * sz, 17.381947f, s * ct * sx, c * sy,
                      s * st * sz, -13.171391f, -st * sx,    0,          ct * sz,     93.317192f,
                      0,           0,           0,           1};
    std::memcpy(out, values, sizeof(values));
}

std::vector<Captured> readOutput(ID3D11DeviceContext *context, ID3D11Buffer *output,
                                 ID3D11Buffer *staging, UINT count) {
    ID3D11Buffer *none = nullptr;
    UINT offset = 0;
    context->SOSetTargets(1, &none, &offset);
    context->CopyResource(staging, output);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    assert(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
    std::vector<Captured> values(count);
    std::memcpy(values.data(), mapped.pData, count * sizeof(Captured));
    context->Unmap(staging, 0);
    return values;
}
} // namespace

int main(int argc, char **argv) {
    auto nativeCode = readNativeShader(argc > 1 ? argv[1] : defaultGame);
    if (nativeCode.empty()) {
        std::puts("SKIP: compatible native Steam shader not found in the local executable.");
        return 77;
    }
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, nullptr, &context)));
    ID3D11VertexShader *nativeShader = nullptr, *customShader = nullptr;
    assert(SUCCEEDED(
        device->CreateVertexShader(nativeCode.data(), nativeCode.size(), nullptr, &nativeShader)));
    auto customCode = compile(shadows::hlsl, "modelReceiverVertex", "vs_4_0");
    assert(SUCCEEDED(device->CreateVertexShader(
        customCode->GetBufferPointer(), customCode->GetBufferSize(), nullptr, &customShader)));
    auto nativeCapture = captureShader(device, nativeCaptureSource);
    auto customCapture = captureShader(device, std::string(shadows::hlsl) + customCaptureSource);
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ID3D11InputLayout *layout = nullptr;
    assert(SUCCEEDED(device->CreateInputLayout(elements, ARRAYSIZE(elements), nativeCode.data(),
                                               nativeCode.size(), &layout)));
    customCode->Release();

    std::vector<Vertex> vertices;
    for (UINT face = 0; face < 6; ++face)
        for (UINT x : {0u, 1u, 32u, 73u, 127u, 191u, 254u, 255u}) {
            Vertex vertex{{x / 255.f, ((x * 3 + 11) % 256) / 255.f, ((x * 5 + 19) % 256) / 255.f,
                           face / 255.f},
                          {.3f, .5f, .7f, 1}};
            vertices.push_back(vertex);
        }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = UINT(vertices.size() * sizeof(Vertex));
    description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{vertices.data()};
    ID3D11Buffer *vertexBuffer = nullptr;
    assert(SUCCEEDED(device->CreateBuffer(&description, &initial, &vertexBuffer)));
    UINT stride = sizeof(Vertex), offset = 0;
    context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    context->IASetInputLayout(layout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

    description.ByteWidth = UINT(vertices.size() * sizeof(Captured));
    description.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    ID3D11Buffer *output = nullptr, *staging = nullptr;
    assert(SUCCEEDED(device->CreateBuffer(&description, nullptr, &output)));
    description.BindFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateBuffer(&description, nullptr, &staging)));
    description.Usage = D3D11_USAGE_DEFAULT;
    description.CPUAccessFlags = 0;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    description.ByteWidth = sizeof(shadows::Params);
    ID3D11Buffer *parameters = nullptr;
    assert(SUCCEEDED(device->CreateBuffer(&description, nullptr, &parameters)));
    description.ByteWidth = 64;
    ID3D11Buffer *matrices[3]{};
    for (auto &matrix : matrices)
        assert(SUCCEEDED(device->CreateBuffer(&description, nullptr, &matrix)));
    description.ByteWidth = 16;
    ID3D11Buffer *depthOffsetBuffer = nullptr;
    assert(SUCCEEDED(device->CreateBuffer(&description, nullptr, &depthOffsetBuffer)));
    float zeros[80]{};
    description.ByteWidth = sizeof(zeros);
    D3D11_SUBRESOURCE_DATA zeroInitial{zeros};
    ID3D11Buffer *zeroBuffer = nullptr;
    assert(SUCCEEDED(device->CreateBuffer(&description, &zeroInitial, &zeroBuffer)));

    // The camera was read from the live Steam renderer. Fixtures also cover
    // mirror transforms, unequal scale, rotation, and native depth offsets.
    const float recordedView[] = {-.491885900f,
                                  .870659649f,
                                  0,
                                  -.000005245f,
                                  .294210434f,
                                  .166216463f,
                                  .941175997f,
                                  -102.268218994f,
                                  .819443941f,
                                  .462951213f,
                                  -.337916702f,
                                  44.718025208f,
                                  0,
                                  0,
                                  0,
                                  1};
    const float projection[] = {1.084611f, 0, 0,         0,         0, 1.928197f, 0, 0,
                                0,         0, 1.000074f, -.125009f, 0, 0,         1, 0};
    uint32_t maximumUlp = 0;
    unsigned differences = 0, valuesCompared = 0;
    float maximumPositionError = 0, maximumNormalError = 0;
    for (UINT testCase = 0; testCase < 24; ++testCase) {
        shadows::Params fixture{};
        std::memcpy(fixture.view, recordedView, sizeof(recordedView));
        std::memcpy(fixture.proj, projection, sizeof(projection));
        float scale = testCase % 3 == 0 ? .157286f : testCase % 3 == 1 ? .084f : 1.f;
        worldMatrix(fixture.world, testCase * .271f, (testCase & 1 ? -1 : 1) * scale,
                    (testCase & 2 ? .7f : 1.f) * scale, (testCase & 4 ? 1.3f : 1.f) * scale);
        fixture.depthAdjust[0] = testCase & 8 ? .0001f : 0.f;
        float nativeDepthOffset[4] = {fixture.depthAdjust[0], 0, 0, 0};
        const float *nativeMatrices[] = {fixture.proj, fixture.view, fixture.world};
        for (UINT i = 0; i < 3; ++i)
            context->UpdateSubresource(matrices[i], 0, nullptr, nativeMatrices[i], 0, 0);
        context->UpdateSubresource(depthOffsetBuffer, 0, nullptr, nativeDepthOffset, 0, 0);
        context->UpdateSubresource(parameters, 0, nullptr, &fixture, 0, 0);
        ID3D11Buffer *nativeBuffers[] = {matrices[0],       matrices[1], matrices[2], zeroBuffer,
                                         zeroBuffer,        zeroBuffer,  zeroBuffer,  zeroBuffer,
                                         depthOffsetBuffer, zeroBuffer};
        context->VSSetConstantBuffers(0, ARRAYSIZE(nativeBuffers), nativeBuffers);
        context->VSSetShader(nativeShader, nullptr, 0);
        context->GSSetShader(nativeCapture, nullptr, 0);
        context->SOSetTargets(1, &output, &offset);
        context->Draw(UINT(vertices.size()), 0);
        auto native = readOutput(context, output, staging, UINT(vertices.size()));

        context->VSSetConstantBuffers(0, 1, &parameters);
        context->VSSetShader(customShader, nullptr, 0);
        context->GSSetShader(customCapture, nullptr, 0);
        context->SOSetTargets(1, &output, &offset);
        context->Draw(UINT(vertices.size()), 0);
        auto custom = readOutput(context, output, staging, UINT(vertices.size()));
        for (size_t vertex = 0; vertex < vertices.size(); ++vertex) {
            for (UINT component = 0; component < 4; ++component) {
                uint32_t distance =
                    ulpDistance(native[vertex].clip[component], custom[vertex].clip[component]);
                maximumUlp = (std::max)(maximumUlp, distance);
                ++valuesCompared;
                if (distance > 1) {
                    if (differences < 8)
                        std::printf(
                            "Clip mismatch case=%u vertex=%zu component=%u native=%.9g custom=%.9g ULP=%u\n",
                            testCase, vertex, component, native[vertex].clip[component],
                            custom[vertex].clip[component], distance);
                    ++differences;
                }
            }
            // Independently transform the custom world point into camera space
            // for a diagnostic of native position output, separate from clip.
            for (UINT row = 0; row < 3; ++row) {
                double expected = fixture.view[row * 4 + 3];
                for (UINT axis = 0; axis < 3; ++axis)
                    expected +=
                        double(fixture.view[row * 4 + axis]) * custom[vertex].position[axis];
                maximumPositionError =
                    (std::max)(maximumPositionError,
                               float(std::abs(expected - native[vertex].position[row])));
                double worldNormal = 0;
                for (UINT axis = 0; axis < 3; ++axis)
                    worldNormal +=
                        double(fixture.view[axis * 4 + row]) * native[vertex].normal[axis];
                maximumNormalError =
                    (std::max)(maximumNormalError,
                               float(std::abs(worldNormal - custom[vertex].normal[row])));
            }
        }
    }
    std::printf(
        "Native Steam clip comparison: %u values, %u mismatches above one ULP, max ULP=%u; view-position discrepancy=%.9g\n",
        valuesCompared, differences, maximumUlp, maximumPositionError);
    std::printf("Native versus inverse-transpose world normal: max difference=%.9g\n",
                maximumNormalError);
    assert(maximumNormalError < .00001f);
    context->ClearState();
    for (auto *matrix : matrices)
        matrix->Release();
    zeroBuffer->Release();
    depthOffsetBuffer->Release();
    parameters->Release();
    staging->Release();
    output->Release();
    vertexBuffer->Release();
    layout->Release();
    customCapture->Release();
    nativeCapture->Release();
    customShader->Release();
    nativeShader->Release();
    context->Release();
    device->Release();
    return differences ? 1 : 0;
}
