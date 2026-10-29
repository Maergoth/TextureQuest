#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <vector>
#include "routing.h"
#pragma comment(lib,"OneCore.lib")
namespace bg {
inline uint64_t Number(FILETIME t)noexcept{return(static_cast<uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
inline bool Birth(HANDLE p,uint64_t& value){FILETIME c,e,k,u;if(!GetProcessTimes(p,&c,&e,&k,&u))return false;value=Number(c);return true;}
inline bool Fresh(uint64_t birth){FILETIME n;GetSystemTimeAsFileTime(&n);return FreshAt(birth,Number(n));}
inline bool Selected(const tq::Manifest& m,const wchar_t* path){wchar_t key[tq::kPathChars];if(!tq::Normalize(path,nullptr,key))return false;for(const auto& row:m.rows)if(tq::Equal(row.original.c_str(),key))return true;return false;}
struct HandleRow{void* object;ULONG_PTR pid,value;ULONG access;USHORT trace,type;ULONG attributes,reserved;};
using QuerySystem=LONG(NTAPI*)(ULONG,void*,ULONG,ULONG*);
static_assert(sizeof(WIN32_MEMORY_REGION_INFORMATION)==32,"AMD64 official MemoryRegionInfo layout required");
enum MappedRegionKind { mapped_unknown, mapped_pagefile, mapped_datafile };
struct RegionQuery {
    HANDLE process;
    bool operator()(const void* address,WIN32_MEMORY_REGION_INFORMATION& info,SIZE_T& returned,DWORD& error)const {
        SetLastError(ERROR_SUCCESS);
        BOOL okay=QueryVirtualMemoryInformation(process,address,MemoryRegionInfo,&info,sizeof(info),&returned);
        error=okay?ERROR_SUCCESS:GetLastError();return okay!=FALSE;
    }
};
template<class Query> inline MappedRegionKind ClassifyMappedRegion(const MEMORY_BASIC_INFORMATION& basic,Query query,std::string& reason){
    WIN32_MEMORY_REGION_INFORMATION info{};SIZE_T returned=0;DWORD error=0;
    bool success=query(basic.BaseAddress,info,returned,error);
    // Documented individual fields, never Flags as an admission shortcut.
    bool page=info.MappedPageFile!=0,data=info.MappedDataFile!=0,image=info.MappedImage!=0;
    bool physical=info.MappedPhysical!=0,direct=info.DirectMapped!=0,private_region=info.Private!=0;
    uintptr_t allocation=reinterpret_cast<uintptr_t>(info.AllocationBase),base=reinterpret_cast<uintptr_t>(basic.BaseAddress);
    bool geometry=success&&returned==sizeof(info)&&basic.State==MEM_COMMIT&&basic.Type==MEM_MAPPED&&
        info.AllocationBase==basic.AllocationBase&&allocation!=0&&base>=allocation&&info.RegionSize!=0&&basic.RegionSize!=0&&
        info.RegionSize<=UINTPTR_MAX-allocation&&base-allocation<=info.RegionSize&&
        basic.RegionSize<=info.RegionSize-(base-allocation)&&info.CommitSize<=info.RegionSize&&
        info.AllocationProtect==basic.AllocationProtect;
    bool pure=geometry&&!private_region&&!image&&!physical&&!direct&&info.Reserved==0;
    if(pure&&page&&!data)return mapped_pagefile;
    if(pure&&data&&!page)return mapped_datafile;
    reason="mapped region type unknown q="+std::to_string(success)+" err="+std::to_string(error)+" n="+std::to_string(returned)+
        " alloc="+std::to_string(allocation)+" basic_alloc="+std::to_string(reinterpret_cast<uintptr_t>(basic.AllocationBase))+
        " base="+std::to_string(base)+" region="+std::to_string(info.RegionSize)+" basic_size="+std::to_string(basic.RegionSize)+
        " commit="+std::to_string(info.CommitSize)+" prot="+std::to_string(info.AllocationProtect)+","+std::to_string(basic.AllocationProtect)+" flags(page,data,image,physical,direct,private,reserved)="+
        std::to_string(page)+","+std::to_string(data)+","+std::to_string(image)+","+std::to_string(physical)+","+
        std::to_string(direct)+","+std::to_string(private_region)+","+std::to_string(info.Reserved);
    return mapped_unknown;
}
// Absence is only a current HANDLE/mapping observation. Earlier closed reads stay UNKNOWN.
inline bool Census(HANDLE process,DWORD pid,HANDLE own_file,const tq::Manifest& m,std::string& reason) {
    auto query=reinterpret_cast<QuerySystem>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation"));
    if(!query){reason="census API unavailable";return false;}
    std::vector<unsigned char> data(1024*1024);ULONG needed=0;LONG code=0;
    for(unsigned tries=0;tries<8;++tries){code=query(64,data.data(),static_cast<ULONG>(data.size()),&needed);if(code>=0)break;if(code!=static_cast<LONG>(0xc0000004)||data.size()>=64*1024*1024){reason="handle census failed/bounded";return false;}size_t next=needed>data.size()?needed+65536:data.size()*2;if(next>64*1024*1024){reason="handle census cap";return false;}data.resize(next);}
    if(code<0||data.size()<2*sizeof(ULONG_PTR)){reason="handle census incomplete";return false;}
    ULONG_PTR count=*reinterpret_cast<const ULONG_PTR*>(data.data());
    if(count>1000000||count>(data.size()-2*sizeof(ULONG_PTR))/sizeof(HandleRow)){reason="handle row bounds";return false;}
    const auto rows=reinterpret_cast<const HandleRow*>(data.data()+2*sizeof(ULONG_PTR));USHORT file_type=0;
    for(ULONG_PTR i=0;i<count;++i)if(rows[i].pid==GetCurrentProcessId()&&rows[i].value==reinterpret_cast<ULONG_PTR>(own_file))file_type=rows[i].type;
    if(!file_type){reason="own FILE type anchor absent";return false;}
    wchar_t path[tq::kPathChars];
    for(ULONG_PTR i=0;i<count;++i)if(rows[i].pid==pid&&rows[i].type==file_type){
        HANDLE copy=nullptr;if(!DuplicateHandle(process,reinterpret_cast<HANDLE>(rows[i].value),GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS)){reason="selected-process FILE duplication unknown";return false;}
        bool okay=true,hit=false;
        if(GetFileType(copy)==FILE_TYPE_DISK){DWORD n=GetFinalPathNameByHandleW(copy,path,static_cast<DWORD>(tq::kPathChars),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!n||n>=tq::kPathChars)okay=false;else hit=Selected(m,path);}
        if(!CloseHandle(copy)){reason="census duplicate close failed";return false;}
        if(!okay){reason="disk FILE path unknown";return false;}if(hit){reason="known existing selected stock FILE handle";return false;}
    }
    wchar_t device[32768];wchar_t drive[]={m.game_root[0],L':',0};
    if(!QueryDosDeviceW(drive,device,32768)){reason="mapped drive identity unknown";return false;}
    std::wstring device_root=device;SYSTEM_INFO info{};GetSystemInfo(&info);uintptr_t address=0,maximum=reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    for(unsigned regions=0;address<maximum;++regions){
        if(regions>=131072){reason="mapping census cap";return false;}MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQueryEx(process,reinterpret_cast<void*>(address),&region,sizeof(region))){reason="mapping census query unknown";return false;}
        uintptr_t start=reinterpret_cast<uintptr_t>(region.BaseAddress),next=start+region.RegionSize;if(next<=address){reason="mapping census geometry";return false;}
        if(region.State==MEM_COMMIT&&region.Type==MEM_MAPPED){
            auto kind=ClassifyMappedRegion(region,RegionQuery{process},reason);if(kind==mapped_unknown)return false;
            if(kind==mapped_pagefile){address=next;continue;}
            SetLastError(ERROR_SUCCESS);DWORD n=GetMappedFileNameW(process,region.BaseAddress,path,static_cast<DWORD>(tq::kPathChars));DWORD name_error=GetLastError();if(!n||n>=tq::kPathChars){reason="mapped data FILE path unknown n="+std::to_string(n)+" err="+std::to_string(name_error)+" base="+std::to_string(start);return false;}
            std::wstring name(path,n);if(name.size()>device_root.size()&&_wcsnicmp(name.c_str(),device_root.c_str(),device_root.size())==0){name=std::wstring(drive)+name.substr(device_root.size());if(Selected(m,name.c_str())){reason="known existing selected stock mapping";return false;}}}
        address=next;
    }
    reason="current HANDLE/mapping census clear; closed-read history UNKNOWN";return true;
}
}
