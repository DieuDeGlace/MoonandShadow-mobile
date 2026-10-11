#define MOON_TEST
#include "MoonLight.cpp"
#include <cassert>
int main() {
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &d, nullptr, &c)));
    moonlight::oldUpdate = (moonlight::Update)(*(void ***)c)[48];
    ID3D11Buffer *b[9]{};
    UINT sizes[] = {64, 64, 64, 16, 320, 48, 16, 16, 16};
    float contents[9][80]{};
    contents[0][0] = contents[0][5] = contents[0][10] = 1;
    for (int i = 0; i < 4; i++)
        contents[3][i] = 1;
    contents[5][8] = .6;
    contents[5][10] = -.8;
    contents[7][0] = 1;
    for (int i = 0; i < 9; i++) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizes[i];
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{contents[i]};
        assert(SUCCEEDED(d->CreateBuffer(&desc, &data, &b[i])));
        if (sizes[i] <= 64)
            b[i]->SetPrivateData(moonlight::cacheGuid, sizes[i], contents[i]);
    }
    c->VSSetConstantBuffers(0, 9, b);
    moonlight::testPhase = moonrules::state(0);
    moonlight::testCloud = true;
    auto saved = moonlight::prepare(c);
    assert(saved.changed[3] && saved.changed[5] && saved.changed[6] && saved.changed[7]);
    ID3D11Buffer *changed[9]{};
    c->VSGetConstantBuffers(0, 9, changed);
    assert(changed[5] == moonlight::light && changed[3] == moonlight::material);
    for (auto &v : changed)
        moonlight::drop(v);
    moonlight::restore(c, saved);
    c->VSGetConstantBuffers(0, 9, changed);
    for (int i = 0; i < 9; i++) {
        assert(changed[i] == b[i]);
        moonlight::drop(changed[i]);
    }
    float original[12];
    assert(moonlight::read(b[5], original, 48));
    assert(memcmp(original, contents[5], 48) == 0);
    moonlight::testPhase = moonrules::state(12 * 3600000.f);
    saved = moonlight::prepare(c);
    for (bool change : saved.changed)
        assert(!change);
    moonlight::restore(c, saved);
    // A HUD-style projection must not be changed at night.
    moonlight::testPhase = moonrules::state(0);
    contents[0][15] = 1;
    b[0]->SetPrivateData(moonlight::cacheGuid, 64, contents[0]);
    saved = moonlight::prepare(c);
    for (bool change : saved.changed)
        assert(!change);
    moonlight::restore(c, saved);
    assert(moonlight::initializeSky());
    moonlight::oldD = (moonlight::D)(*(void ***)c)[13];
    ID3D11Texture2D *target = nullptr, *depth = nullptr, *staging = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 640;
    td.Height = 360;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &target)));
    assert(SUCCEEDED(d->CreateRenderTargetView(target, nullptr, &rtv)));
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &depth)));
    assert(SUCCEEDED(d->CreateDepthStencilView(depth, nullptr, &dsv)));
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &staging)));
    for (int i : {0, 5, 10, 15})
        moonlight::sceneView[i] = 1;
    moonlight::sceneProjection[0] = moonlight::sceneProjection[5] = moonlight::sceneProjection[10] =
        moonlight::sceneProjection[14] = 1;
    moonlight::testPhase.direction[0] = moonlight::testPhase.direction[1] = 0;
    moonlight::testPhase.direction[2] = 1;
    moonlight::skyViewport = {0, 0, 640, 360, 0, 1};
    const float black[4]{};
    for (int occluded = 0; occluded < 2; occluded++) {
        c->ClearRenderTargetView(rtv, black);
        c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, occluded ? 0 : 1, 0);
        moonlight::skyTarget = rtv;
        rtv->AddRef();
        moonlight::sceneDepth = dsv;
        dsv->AddRef();
        moonlight::skyReady = true;
        moonlight::skyFinished = false;
        moonlight::renderSky();
        // The parked moon state must not retain resize-sensitive scene attachments.
        ID3DDeviceContextState *original = nullptr;
        moonlight::context1->SwapDeviceContextState(moonlight::skyState, &original);
        ID3D11RenderTargetView *parkedTarget = nullptr;
        ID3D11DepthStencilView *parkedDepth = nullptr;
        c->OMGetRenderTargets(1, &parkedTarget, &parkedDepth);
        assert(!parkedTarget && !parkedDepth);
        moonlight::context1->SwapDeviceContextState(original, nullptr);
        moonlight::drop(original);
        c->CopyResource(staging, target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        assert(SUCCEEDED(c->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
        auto pixel = (unsigned char *)mapped.pData + 180 * mapped.RowPitch + 320 * 4;
        if (occluded)
            assert(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0);
        else
            assert(pixel[0] > 100 && pixel[2] > pixel[0]);
        c->Unmap(staging, 0);
        c->VSGetConstantBuffers(0, 9, changed);
        for (int i = 0; i < 9; i++) {
            assert(changed[i] == b[i]);
            moonlight::drop(changed[i]);
        }
    }
    // Exercise an actual hidden-window swap chain after rendering the moon.
    HWND window =
        CreateWindowExW(0, L"STATIC", L"Moon resize regression", WS_OVERLAPPEDWINDOW, 0, 0, 640,
                        360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(window);
    IDXGIDevice *dxgiDevice = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory *factory = nullptr;
    assert(SUCCEEDED(d->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice)));
    assert(SUCCEEDED(dxgiDevice->GetAdapter(&adapter)));
    assert(SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), (void **)&factory)));
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferDesc.Width = 640;
    swapDesc.BufferDesc.Height = 360;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 1;
    swapDesc.OutputWindow = window;
    swapDesc.Windowed = TRUE;
    IDXGISwapChain *swap = nullptr;
    assert(SUCCEEDED(factory->CreateSwapChain(d, &swapDesc, &swap)));
    ID3D11Texture2D *backBuffer = nullptr;
    ID3D11RenderTargetView *backView = nullptr;
    assert(SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)));
    assert(SUCCEEDED(d->CreateRenderTargetView(backBuffer, nullptr, &backView)));
    moonlight::skyTarget = backView;
    backView->AddRef();
    moonlight::sceneDepth = dsv;
    dsv->AddRef();
    moonlight::skyReady = true;
    moonlight::skyFinished = false;
    moonlight::renderSky();
    c->OMSetRenderTargets(0, nullptr, nullptr);
    moonlight::drop(backView);
    moonlight::drop(backBuffer);
    assert(SUCCEEDED(swap->ResizeBuffers(0, 1280, 720, DXGI_FORMAT_UNKNOWN, 0)));
    assert(SUCCEEDED(swap->ResizeBuffers(0, 1920, 1080, DXGI_FORMAT_UNKNOWN, 0)));
    moonlight::drop(swap);
    moonlight::drop(factory);
    moonlight::drop(adapter);
    moonlight::drop(dxgiDevice);
    DestroyWindow(window);
    puts("Actual swap-chain resize after moon rendering: 720p and 1080p PASS");
    moonlight::drop(rtv);
    moonlight::drop(dsv);
    moonlight::drop(target);
    moonlight::drop(depth);
    moonlight::drop(staging);
    for (auto &v : b)
        moonlight::drop(v);
    moonlight::drop(c);
    moonlight::drop(d);
    puts(
        "Moon lighting/cloud buffers and bindings restored, daylight/HUD excluded, blue moon disk renders and terrain depth occludes it: PASS");
}
