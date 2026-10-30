#pragma once
#include <d3d11_4.h>
#include "texturequest_provider_creation_v1.hpp"
#include "texturequest_creation_policy.hpp"
#include <exception>
#include <type_traits>
namespace tqpc {
struct Frame {
 Info info{};Frame* parent=nullptr;void* ticket=nullptr;Phase phase=Phase::capture;
 bool observed=false,reserved=false,prepared=false,matched=false,native_called=false,native_returned=false,settled=false;
 Outcome outcome=Outcome::failed;uint32_t init_seen=0;int exceptions=0;
};
void Begin(Frame&,const void*,Factory)noexcept;
void Prepare(Frame&,const Descriptor&,const Subresource*)noexcept;
void Result(Frame&,int32_t,void*)noexcept;
void Finish(Frame&)noexcept;
class CreationCall {
 Frame frame_;
public:
 CreationCall(const void* device,Factory factory)noexcept{Begin(frame_,device,factory);}
 ~CreationCall()noexcept{Finish(frame_);}
 CreationCall(const CreationCall&)=delete;
 bool Observed()const noexcept{return frame_.observed;}
 void Prepare2D(const D3D11_TEXTURE2D_DESC* d,const D3D11_SUBRESOURCE_DATA* data)noexcept{
  if(d)Prepare(frame_,{d->Width,d->Height,d->MipLevels,d->ArraySize,uint32_t(d->Format),d->SampleDesc.Count,d->SampleDesc.Quality,uint32_t(d->Usage),d->BindFlags,d->CPUAccessFlags,d->MiscFlags,0},reinterpret_cast<const Subresource*>(data));
 }
 void Prepare2D(const D3D11_TEXTURE2D_DESC1* d,const D3D11_SUBRESOURCE_DATA* data)noexcept{
  if(d)Prepare(frame_,{d->Width,d->Height,d->MipLevels,d->ArraySize,uint32_t(d->Format),d->SampleDesc.Count,d->SampleDesc.Quality,uint32_t(d->Usage),d->BindFlags,d->CPUAccessFlags,d->MiscFlags,uint32_t(d->TextureLayout)},reinterpret_cast<const Subresource*>(data));
 }
 template<class Native,class Object>auto Call(Native native,Object object){frame_.native_called=true;auto hr=native();Result(frame_,int32_t(hr),SUCCEEDED(hr)?static_cast<void*>(object()):nullptr);return hr;}
 void Init()noexcept{++frame_.init_seen;}
};
} // namespace tqpc
