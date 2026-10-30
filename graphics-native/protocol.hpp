#pragma once
#include "engine.hpp"
#include "full_catalog.hpp"
#include <cstddef>
#include "wire.hpp"
namespace tqgr {
using namespace tqab;
inline bool Hex(const char* s,size_t n){for(size_t i=0;i<n;++i)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;return s[n]==0;}
inline bool InitValid(const InitV3& c,uint32_t pid,uint64_t birth){return !std::memcmp(c.magic,"TQPHIN03",8)&&c.bytes==sizeof(c)&&c.version==3&&c.pid==pid&&c.birth100ns==birth&&c.flags==3&&c.reserved==0&&c.provider&&c.bootstrap&&c.profile_receipt&&c.catalog_address&&c.catalog_bytes>=16&&c.catalog_bytes<=MaxCpu+65536&&Hex(c.nonce,32)&&Hex(c.catalog_sha256,64)&&std::all_of(std::begin(c.pad),std::end(c.pad),[](uint8_t n){return !n;})&&std::all_of(std::begin(c.pad2),std::end(c.pad2),[](uint8_t n){return !n;});}
inline bool CommandValid(const CommandV3& c,const InitV3& init){return !std::memcmp(c.magic,"TQPHCM03",8)&&c.bytes==sizeof(c)&&c.version==3&&c.pid==init.pid&&c.birth100ns==init.birth100ns&&c.mode<=1&&Hex(c.nonce,32)&&!std::memcmp(c.nonce,init.nonce,33)&&std::all_of(std::begin(c.pad),std::end(c.pad),[](uint8_t n){return !n;});}
inline bool Foreground(uint32_t owned_pid,uint64_t owned_birth,uint32_t foreground_pid,uint64_t self_birth){return owned_pid&&owned_birth&&owned_pid==foreground_pid&&owned_birth==self_birth;}
class Once {std::atomic<bool> tried_{false};public:bool Enter(){return !tried_.exchange(true);}bool Tried()const{return tried_.load();}};
struct Parsed {std::shared_ptr<const Catalog> catalog;uint32_t canonical_stock=0,canonical_upscaled=0;bool dual=false;};
inline Parsed Parse(const Bytes& bytes,const std::string& expected){
 require(bytes.size()>=24&&bytes.size()<=64*MiB+65536&&tq::Sha256(bytes.data(),bytes.size())==expected,"full graphics configuration digest");
 require(!memcmp(bytes.data(),"TQGXCFG1",8),"full graphics configuration magic");uint32_t version,dual,path_bytes,index_bytes;
 memcpy(&version,bytes.data()+8,4);memcpy(&dual,bytes.data()+12,4);memcpy(&path_bytes,bytes.data()+16,4);memcpy(&index_bytes,bytes.data()+20,4);
 require(version==1&&dual<=1&&path_bytes&&path_bytes<=32768&&index_bytes<=64*MiB&&uint64_t(24)+path_bytes+index_bytes==bytes.size(),"full graphics configuration framing");
 auto input=reinterpret_cast<const char*>(bytes.data()+24);int needed=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input,int(path_bytes),nullptr,0);require(needed>0,"graphics payload UTF8 path");std::wstring payload(needed,L'\0');require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input,int(path_bytes),payload.data(),needed)==needed&&payload.find(L'\0')==std::wstring::npos,"graphics payload path text");
 Bytes index(bytes.begin()+24+path_bytes,bytes.end());auto full=tqgx::ReadCatalog(index,payload);return {full.catalog,0,full.rows,dual==1};
}
}
