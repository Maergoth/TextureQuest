#pragma once
// ORIGINAL bounded graphics-only observer. No shader replacement, GPU copy,
// engine memory read, input hook or graphics command suppression.
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
namespace tq {
class RendererBindingProbe {
 friend struct RendererBindingProbeCpuCheck;
 using Clock=std::chrono::steady_clock;
 template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
 struct Shader {uint64_t hash=0,generation=0,device=0;uint32_t bytes=0,stage=0;};
 struct Surface {uint32_t id=0,width=0,height=0,format=0,samples=0;bool target=false;};
 struct Buffer {
  uint32_t id=0,bytes=0,begin=0,length=0,version=0,writeFrame=0;
  uint64_t mapOffset=0,mapSize=0;const uint8_t*map=nullptr;
  uint32_t savedBegin=0,savedLength=0;std::array<uint8_t,576>saved{};
 };
 struct Binding {uint32_t id=0,first=0,count=0,version=0;};
 struct Range {
  uint64_t vs=0,ps=0,vgen=0,pgen=0;uint32_t frame=0,target=0,depth=0,draws=0,indices=0;
  uint32_t reads[8]{};Binding v0,p0,p1,p13;bool lamp=false;
 };
 struct Sample {uint32_t buffer=0,begin=0,bytes=0,version=0,frame=0;std::array<uint8_t,576>data{};};
 static uint32_t CurrentVersion(const Buffer&b){return b.savedLength==b.length&&b.length&&b.savedBegin==b.begin?b.version:0;}
 std::mutex lock_;std::map<uint64_t,Shader>shaders_;std::map<uint64_t,Surface>surfaces_;
 std::map<uint64_t,Buffer>buffers_;std::vector<Range>ranges_;std::vector<Sample>samples_;std::vector<Surface>retired_;
 uint64_t generation_=0,device_=0;uint32_t nextId_=0,frame_=0,draw_=0;
 bool active_=false,finished_=false,incomplete_=false,submitted_=false;
 Clock::time_point start_{};std::thread writer_;std::filesystem::path folder_;
 std::atomic<unsigned>writeStatus_{0}; //0idle/1capture/2saving/3saved/4failed
 static uint64_t Key(const void*p){return uint64_t(reinterpret_cast<uintptr_t>(p));}
 static uint64_t Hash(const void*data,size_t n){uint64_t h=0xcbf29ce484222325ull;auto*p=static_cast<const uint8_t*>(data);while(n--)h=(h^*p++)*0x100000001b3ull;return h;}
 static bool Lamp(uint64_t h){return h==0x8b55e251966bcd36ull||h==0xd878054c72da7a41ull||h==0x42ebee0d2ac17140ull||h==0x2afff55627e709dbull||h==0xe1d488a4887b4728ull||h==0x56e204312ac2da95ull;}
 void Limit(){active_=false;finished_=true;incomplete_=true;}
 Surface* SurfaceFor(ID3D11Resource*r,bool target) {
  if(!r)return nullptr;auto it=surfaces_.find(Key(r));if(it!=surfaces_.end()){it->second.target|=target;return &it->second;}
  if(surfaces_.size()+retired_.size()>=128){Limit();return nullptr;}
  Ptr<ID3D11Texture2D>t;if(FAILED(r->QueryInterface(IID_PPV_ARGS(&t))))return nullptr;
  D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);Surface s{++nextId_,d.Width,d.Height,UINT(d.Format),d.SampleDesc.Count,target};
  return &surfaces_.emplace(Key(r),s).first->second;
 }
 Binding Bound(ID3D11DeviceContext1*ctx,bool vertex,UINT slot,UINT rows) {
  Ptr<ID3D11Buffer>b;UINT first=0,count=0;
  if(vertex)ctx->VSGetConstantBuffers1(slot,1,&b,&first,&count);else ctx->PSGetConstantBuffers1(slot,1,&b,&first,&count);
  if(!b)return{};auto it=buffers_.find(Key(b.Get()));
  if(it==buffers_.end()) {
   if(buffers_.size()>=32){incomplete_=true;return{};}
   D3D11_BUFFER_DESC d{};b->GetDesc(&d);Buffer value{};value.id=++nextId_;value.bytes=d.ByteWidth;
   it=buffers_.emplace(Key(b.Get()),value).first;
  }
  auto&v=it->second;const uint64_t begin=uint64_t(first)*16;
  const uint32_t length=UINT(std::min<uint64_t>(std::min(rows,count)*16,begin<v.bytes?v.bytes-begin:0));
  if(begin>UINT32_MAX||!length)return{v.id,first,count,0};
  if(!v.length){v.begin=UINT(begin);v.length=length;}
  const bool covered=begin>=v.begin&&begin-v.begin<=v.length&&length<=v.length-(begin-v.begin);
  // One retained window per buffer: never overwrite the earlier VS window
  // with another ranged binding. Uncovered joins are explicitly unknown.
  if(!covered)incomplete_=true;
  return{v.id,first,count,covered?CurrentVersion(v):0};
 }
 void SaveBytes(Buffer&b,const uint8_t*data,uint64_t begin,uint64_t n) {
  if(!b.length)return;
  const bool overlaps=n&&begin<uint64_t(b.begin)+b.length&&(begin>=b.begin||n>uint64_t(b.begin)-begin);
  if(!overlaps)return;
  const bool complete=begin<=b.begin&&uint64_t(b.begin)-begin<=n&&b.length<=n-(uint64_t(b.begin)-begin);
  if(!data||!complete||b.length>b.saved.size()){b.savedLength=0;return;}
  std::memcpy(b.saved.data(),data+(b.begin-begin),b.length);b.savedBegin=b.begin;b.savedLength=b.length;b.writeFrame=frame_;++b.version;
  if(samples_.size()<32) {Sample s{};s.buffer=b.id;s.begin=b.begin;s.bytes=b.length;s.version=b.version;s.frame=frame_;s.data=b.saved;samples_.push_back(s);}else incomplete_=true;
 }
 std::string Json() {
  std::ostringstream o;o<<"{\"schema\":1,\"domain\":\"graphics-bindings-only\",\"incomplete\":"<<(incomplete_?"true":"false")<<",\"frames\":"<<frame_<<",\"draws\":"<<draw_<<",\"surfaces\":[";
  bool comma=false;for(const auto&entry:surfaces_){const auto&s=entry.second;if(comma)o<<',';comma=true;o<<"{\"id\":"<<s.id<<",\"width\":"<<s.width<<",\"height\":"<<s.height<<",\"format\":"<<s.format<<",\"samples\":"<<s.samples<<",\"target\":"<<(s.target?"true":"false")<<'}';}
  for(const auto&s:retired_){if(comma)o<<',';comma=true;o<<"{\"id\":"<<s.id<<",\"width\":"<<s.width<<",\"height\":"<<s.height<<",\"format\":"<<s.format<<",\"samples\":"<<s.samples<<",\"retired\":true}";}
  o<<"],\"ranges\":[";comma=false;
  auto binding=[&](const Binding&b){o<<"["<<b.id<<','<<b.first<<','<<b.count<<','<<b.version<<']';};
  for(const auto&r:ranges_){if(comma)o<<',';comma=true;o<<"{\"frame\":"<<r.frame<<",\"vs\":\""<<std::hex<<r.vs<<"\",\"ps\":\""<<r.ps<<std::dec<<"\",\"vgen\":"<<r.vgen<<",\"pgen\":"<<r.pgen<<",\"target\":"<<r.target<<",\"depth\":"<<r.depth<<",\"draws\":"<<r.draws<<",\"indexElements\":"<<r.indices<<",\"knownLampPS\":"<<(r.lamp?"true":"false")<<",\"vsB0\":";binding(r.v0);o<<",\"psB0\":";binding(r.p0);o<<",\"psB1\":";binding(r.p1);o<<",\"psB13\":";binding(r.p13);o<<",\"priorTargetReads\":[";for(unsigned i=0;i<8;++i){if(i)o<<',';o<<r.reads[i];}o<<"]}";}
  o<<"],\"bufferWrites\":[";comma=false;constexpr char hex[]="0123456789abcdef";
  for(const auto&s:samples_){if(comma)o<<',';comma=true;o<<"{\"buffer\":"<<s.buffer<<",\"byteOffset\":"<<s.begin<<",\"bytes\":"<<s.bytes<<",\"version\":"<<s.version<<",\"writeFrame\":"<<s.frame<<",\"observedCPUWriteHex\":\"";for(UINT i=0;i<s.bytes;++i)o<<hex[s.data[i]>>4]<<hex[s.data[i]&15];o<<"\"}";}
  o<<"],\"limits\":\"No runtime role inferred from cache names; original draw never suppressed; missing writes and resource joins remain unknown. No bytecode dump, process memory or GPU readback.\"}";return o.str();
 }
public:
 ~RendererBindingProbe(){Stop();}
 void Stop(){ {std::lock_guard<std::mutex>g(lock_);active_=false;for(auto&b:buffers_)b.second.map=nullptr;}if(writer_.joinable())writer_.join();}
 unsigned Status()const{return writeStatus_.load();}
 bool Arm(uint64_t nativeDevice,const std::filesystem::path&mapParent) {
  if(!nativeDevice)return false;
  if(writer_.joinable()){if(writeStatus_.load()==2)return false;writer_.join();}
  std::lock_guard<std::mutex>g(lock_);if(active_)return false;
  device_=nativeDevice;folder_=mapParent/L"diagnostics";surfaces_.clear();buffers_.clear();ranges_.clear();samples_.clear();retired_.clear();
  frame_=draw_=nextId_=0;incomplete_=finished_=submitted_=false;active_=true;start_=Clock::now();writeStatus_=1;return true;
 }
 void Pipeline(reshade::api::device*d,uint32_t count,const reshade::api::pipeline_subobject*objects,reshade::api::pipeline handle) {
  if(!d||d->get_api()!=reshade::api::device_api::d3d11)return;
  std::lock_guard<std::mutex>g(lock_);
  if(count>64||(count&&!objects)){incomplete_=true;return;}
  for(UINT i=0;i<count;++i){auto type=objects[i].type;if(type!=reshade::api::pipeline_subobject_type::vertex_shader&&type!=reshade::api::pipeline_subobject_type::pixel_shader)continue;
   if(shaders_.size()>=1024){incomplete_=true;continue;}if(objects[i].count!=1||!objects[i].data){incomplete_=true;continue;}
   const auto&s=*static_cast<const reshade::api::shader_desc*>(objects[i].data);
   if(!s.code||!s.code_size||s.code_size>65536)continue;shaders_[handle.handle]={Hash(s.code,s.code_size),++generation_,d->get_native(),UINT(s.code_size),type==reshade::api::pipeline_subobject_type::vertex_shader?1u:2u};}
 }
 void DestroyPipeline(reshade::api::pipeline p){std::lock_guard<std::mutex>g(lock_);shaders_.erase(p.handle);}
 void DestroyResource(reshade::api::resource r){std::lock_guard<std::mutex>g(lock_);auto i=surfaces_.find(r.handle);if(i!=surfaces_.end()){retired_.push_back(i->second);surfaces_.erase(i);}buffers_.erase(r.handle);}
 void Map(reshade::api::resource r,uint64_t offset,uint64_t size,reshade::api::map_access access,void**data) {
  if(writeStatus_.load(std::memory_order_relaxed)!=1)return;
  std::lock_guard<std::mutex>g(lock_);if(!active_||!data||!*data||access==reshade::api::map_access::read_only)return;
  auto i=buffers_.find(r.handle);if(i==buffers_.end())return;auto&b=i->second;
  if(offset>b.bytes)return;b.map=static_cast<const uint8_t*>(*data);b.mapOffset=offset;b.mapSize=std::min<uint64_t>(size,b.bytes-offset);
 }
 void Unmap(reshade::api::resource r){if(writeStatus_.load(std::memory_order_relaxed)!=1)return;std::lock_guard<std::mutex>g(lock_);auto i=buffers_.find(r.handle);if(i==buffers_.end())return;auto&b=i->second;if(active_&&b.map)SaveBytes(b,b.map,b.mapOffset,b.mapSize);b.map=nullptr;}
 void Update(reshade::api::resource r,const void*data,uint64_t offset,uint64_t size){if(writeStatus_.load(std::memory_order_relaxed)!=1)return;std::lock_guard<std::mutex>g(lock_);if(!active_)return;auto i=buffers_.find(r.handle);if(i!=buffers_.end())SaveBytes(i->second,static_cast<const uint8_t*>(data),offset,size);}
 void Draw(reshade::api::command_list*cmd,UINT elements) {
  if(writeStatus_.load(std::memory_order_relaxed)!=1)return;
  std::lock_guard<std::mutex>g(lock_);if(!active_||!cmd||cmd->get_device()->get_api()!=reshade::api::device_api::d3d11||cmd->get_device()->get_native()!=device_)return;
  auto*ctx=reinterpret_cast<ID3D11DeviceContext*>(cmd->get_native());if(!ctx||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){incomplete_=true;return;}
  if(Clock::now()-start_>std::chrono::seconds(5)){active_=false;finished_=true;return;}
  ++draw_;if(draw_>32768){Limit();return;}
  Ptr<ID3D11DeviceContext1>one;if(FAILED(ctx->QueryInterface(IID_PPV_ARGS(&one)))){Limit();return;}
  Ptr<ID3D11VertexShader>vs;Ptr<ID3D11PixelShader>ps;ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
  const auto vi=shaders_.find(Key(vs.Get())),pi=shaders_.find(Key(ps.Get()));
  Ptr<ID3D11RenderTargetView>rt;Ptr<ID3D11DepthStencilView>ds;ctx->OMGetRenderTargets(1,&rt,&ds);
  Ptr<ID3D11Resource>r,d;if(rt)rt->GetResource(&r);if(ds)ds->GetResource(&d);
  auto*target=SurfaceFor(r.Get(),true);auto*depth=SurfaceFor(d.Get(),false);if(!target)return;
  Range range{};range.frame=frame_;range.target=target->id;range.depth=depth?depth->id:0;range.draws=1;range.indices=elements;
  if(vi!=shaders_.end()){range.vs=vi->second.hash;range.vgen=vi->second.generation;}
  if(pi!=shaders_.end()){range.ps=pi->second.hash;range.pgen=pi->second.generation;}range.lamp=Lamp(range.ps);
  ID3D11ShaderResourceView*raw[16]={};ctx->PSGetShaderResources(0,16,raw);unsigned used=0;
  for(auto*view:raw){if(!view)continue;Ptr<ID3D11Resource>read;view->GetResource(&read);view->Release();auto i=surfaces_.find(Key(read.Get()));if(i!=surfaces_.end()&&i->second.target&&used<8)range.reads[used++]=i->second.id;}
  if(range.lamp){range.v0=Bound(one.Get(),true,0,36);range.p0=Bound(one.Get(),false,0,4);range.p1=Bound(one.Get(),false,1,1);range.p13=Bound(one.Get(),false,13,1);}
  if(!ranges_.empty()) {auto&last=ranges_.back();if(!range.lamp&&last.frame==range.frame&&last.vs==range.vs&&last.ps==range.ps&&last.vgen==range.vgen&&last.pgen==range.pgen&&last.target==range.target&&last.depth==range.depth&&std::memcmp(last.reads,range.reads,sizeof(range.reads))==0){++last.draws;last.indices+=elements;return;}}
  if(ranges_.size()>=512){Limit();return;}ranges_.push_back(range);
 }
 void Frame(uint64_t nativeDevice) {
  if(writeStatus_.load(std::memory_order_relaxed)!=1)return;
  std::string json;std::filesystem::path folder;
  {std::lock_guard<std::mutex>g(lock_);if(nativeDevice!=device_)return;
   if(active_&&(++frame_>=3||Clock::now()-start_>std::chrono::seconds(5))){active_=false;finished_=true;}
   if(!finished_||submitted_)return;submitted_=true;for(auto&b:buffers_)b.second.map=nullptr;json=Json();folder=folder_;writeStatus_=2;}
  writer_=std::thread([this,folder,json=std::move(json)](){
   try{if(json.size()>1024*1024){writeStatus_=4;return;}std::filesystem::create_directories(folder);
    const auto name=L"renderer-bindings-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".json";
    HANDLE file=CreateFileW((folder/name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE){writeStatus_=4;return;}
    DWORD wrote=0;const bool ok=WriteFile(file,json.data(),DWORD(json.size()),&wrote,nullptr)&&wrote==json.size();CloseHandle(file);writeStatus_=ok?3u:4u;
   }catch(...){writeStatus_=4;}
  });
 }
};
}
