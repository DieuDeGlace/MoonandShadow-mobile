#include "SteamShadows.cpp"
#include <cassert>
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
    shadows::cascadesEnabled = false;
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &d, nullptr, &c)));
    shadows::resolution = 512;
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
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 128;
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
    ground.light[10] = 1;
    shadows::draws.push_back(ground);
    shadows::sceneViewport = {0, 0, 128, 128, 0, 1};
    shadows::CloudCaster cloud{};
    cloud.draw = ground;
    cloud.draw.vb->AddRef();
    cloud.draw.layout->AddRef();
    cloud.draw.world[0] = cloud.draw.world[5] = .5f;
    cloud.draw.world[11] = 40;
    cloud.seen = GetTickCount64();
    shadows::clouds.push_back(cloud);
    shadows::frameOriginKnown = true;
    shadows::render();
    assert(shadows::lastCloudCasters == 1);
    c->CopyResource(staging, color);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    assert(SUCCEEDED(c->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
    auto pixels = (unsigned char *)mapped.pData;
    assert(pixels[64 * mapped.RowPitch + 64 * 4] < 200);
    assert(pixels[64 * mapped.RowPitch + 42 * 4] > 240);
    c->Unmap(staging, 0);
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
        "Cloud caster darkens ground only beneath its footprint; parked state releases scene targets: PASS");
}
