#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstring>
#include "sun_state_policy.hpp"
namespace tq_sun
{
using Microsoft::WRL::ComPtr;
static ComPtr<ID3D11Buffer> OwnedSunBuffers[2];

// New sun writes target a private DISCARD version of the current bounded slice.
// Restoring the native binding never writes back into in-flight game constants.
class ScopedSunConstants
{
    ComPtr<ID3D11DeviceContext1> Context;
    ComPtr<ID3D11Buffer> Original;
    UINT First=0,Count=0;
    bool Pixel=false,Bound=false;
public:
    ScopedSunConstants()=default;
    ScopedSunConstants(const ScopedSunConstants &)=delete;
    ScopedSunConstants &operator=(const ScopedSunConstants &)=delete;
    bool Begin(ID3D11DeviceContext *native,bool pixel,UINT rows)
    {
        if(FAILED(native->QueryInterface(IID_PPV_ARGS(&Context))))return false;
        Pixel=pixel;
        if(pixel)Context->PSGetConstantBuffers1(0,1,&Original,&First,&Count);
        else Context->VSGetConstantBuffers1(0,1,&Original,&First,&Count);
        if(!Original)return false;
        D3D11_BUFFER_DESC desc{};Original->GetDesc(&desc);
        const UINT bytes=sun_state_policy::SliceBytes(First,Count,desc.ByteWidth,rows);
        if(desc.Usage!=D3D11_USAGE_DYNAMIC||!bytes)return false;
        std::array<uint8_t,4096> data{};D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(Context->Map(Original.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped)))return false;
        std::memcpy(data.data(),static_cast<const uint8_t *>(mapped.pData)+uint64_t(First)*16,bytes);
        Context->Unmap(Original.Get(),0);
        auto &owned=OwnedSunBuffers[pixel?1:0];
        if(!owned)
        {
            ComPtr<ID3D11Device> dev;Context->GetDevice(&dev);
            const D3D11_BUFFER_DESC wanted{4096,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE};
            if(FAILED(dev->CreateBuffer(&wanted,nullptr,&owned)))return false;
        }
        if(FAILED(Context->Map(owned.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)))return false;
        std::memcpy(mapped.pData,data.data(),data.size());Context->Unmap(owned.Get(),0);
        ID3D11Buffer *buffer=owned.Get();const UINT first=0,count=(bytes+255)/256*16;
        if(pixel)Context->PSSetConstantBuffers1(0,1,&buffer,&first,&count);
        else Context->VSSetConstantBuffers1(0,1,&buffer,&first,&count);
        Bound=true;return true;
    }
    ~ScopedSunConstants()
    {
        if(!Bound)return;ID3D11Buffer *buffer=Original.Get();
        if(Pixel){if(Count)Context->PSSetConstantBuffers1(0,1,&buffer,&First,&Count);else Context->PSSetConstantBuffers(0,1,&buffer);}
        else {if(Count)Context->VSSetConstantBuffers1(0,1,&buffer,&First,&Count);else Context->VSSetConstantBuffers(0,1,&buffer);}
    }
};

static void ResetConstants(){for(auto &buffer:OwnedSunBuffers)buffer.Reset();}
}
