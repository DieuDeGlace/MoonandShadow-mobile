#include "SteamShadows.cpp"
#include <cassert>
int main(){
 ID3D11Device* d=nullptr;ID3D11DeviceContext* c=nullptr;
 HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c);if(FAILED(hr))return 1;
 bool ok=shadows::initialize(c);printf("Shadow shaders, resources and context-state support: %s\n",ok?"PASS":"FAIL");
 ID3DBlob *code=nullptr,*listing=nullptr;
 assert(SUCCEEDED(D3DCompile(shadows::hlsl,strlen(shadows::hlsl),"DepthOrder",nullptr,nullptr,"receiver","vs_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,nullptr)));
 assert(SUCCEEDED(D3DDisassemble(code->GetBufferPointer(),code->GetBufferSize(),0,nullptr,&listing)));
 std::string assembly((char*)listing->GetBufferPointer(),listing->GetBufferSize());
 // Regression: FXC used to exchange the native y-first multiply and x MAD.
 // Checking compiled instructions catches this even when source looks correct.
 auto y=assembly.find("cb0[4].yyyy");auto x=assembly.find("cb0[4].xxxx");
 assert(y!=std::string::npos&&x!=std::string::npos&&y<x);
 auto begin=assembly.rfind("\n",y);assert(assembly.substr(begin+1,4)=="mul ");
 begin=assembly.rfind("\n",x);assert(assembly.substr(begin+1,4)=="mad ");
 puts("Native matrix depth arithmetic order preserved in compiled receiver: PASS");
 shadows::drop(listing);shadows::drop(code);return ok?0:2;
}
