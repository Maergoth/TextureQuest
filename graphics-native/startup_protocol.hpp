#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include "native/routing.h"
#include "startup_wire.hpp"
namespace startup {
struct GateBlock {uint32_t magic=0x31534754,stage=0,release=0,pid=0;uint64_t birth=0;char nonce[33]{};};
static_assert(offsetof(GateBlock,stage)==4&&offsetof(GateBlock,release)==8);
// Separate child-only environment bridge. No file-hook configuration exists.
struct Configure {uint32_t magic=0x33504854,bytes=sizeof(Configure),pid=0,reserved=0;uint64_t birth=0;char nonce[33]{};wchar_t base[32768]{},session[32768]{};
 uint64_t primary_birth=0,gate_block=0,gate_code=0,entry=0;uint32_t primary_tid=0,entry_protect=0;uint8_t patch[14]{},zero[2]{};
};
struct ProfileStatus {char magic[8]{};uint32_t bytes=sizeof(ProfileStatus),version=3,pid=0,error=0;uint64_t birth=0;char nonce[33]{};uint8_t pad[7]{};uint32_t ready=0,held=0,unknown=0,reserved=0;};
static_assert(sizeof(ProfileStatus)==88);
inline uint64_t Number(FILETIME f){return(uint64_t(f.dwHighDateTime)<<32)|f.dwLowDateTime;}
inline bool Birth(HANDLE p,uint64_t& n){FILETIME c,e,k,u;if(!GetProcessTimes(p,&c,&e,&k,&u))return false;n=Number(c);return n!=0;}
inline bool Hex(const std::string& s,size_t n){if(s.size()!=n)return false;for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
inline bool Zero(const uint8_t* p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return false;return true;}
inline bool StatusIdentity(const tqgr::StatusV3& s,DWORD pid,uint64_t birth,const std::string& nonce){
 return !memcmp(s.magic,"TQPHST03",8)&&s.bytes==248&&s.version==3&&s.pid==pid&&s.birth100ns==birth&&
 nonce.size()==32&&!memcmp(s.nonce,nonce.c_str(),33)&&Zero(s.pad,7)&&s.reserved==0&&
 s.attempted<=1&&s.registered<=1&&s.input_ready<=1&&s.main_ready<=1&&s.comparison_ready<=1&&
 s.default_safe==0&&s.user_original<=1&&s.stopping<=1&&s.protection_enabled<=1&&s.protection_previous==0xffffffffu&&s.group_guard_ready<=1&&
 s.private_experimental==1&&s.native_bypass_gap==1&&s.profile_ready==1&&s.requested<=INT64_MAX&&s.acknowledged<=s.requested;
}
inline bool StockStartupReady(const tqgr::StatusV3& s,uint32_t rows){
 // Original is a pending authenticated request. Only admitted main Present commits it.
 return s.attempted==1&&s.registered==1&&s.input_ready==1&&s.error==0&&s.stopping==0&&
 s.canonical_stock_rows==rows&&s.canonical_upscaled_rows==0&&s.requested>=1;
}
}
