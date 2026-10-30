#pragma once
// Ordinary local file/module validation only. No remote-process routines.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>
#include <cstdio>
#include "saved_sha256.h"
#include "startup_protocol.hpp"
#include "profile_policy.hpp"
#include "startup_policy.hpp"
#include "startup_release_disposition.hpp"
#include "texturequest_provider_mutation_v2.hpp"
#include "intent_reader.hpp"
#include "provider_source_contract.hpp"
#pragma comment(lib,"psapi.lib")
#pragma comment(lib,"user32.lib")
namespace fs=std::filesystem;
static bool close_failed=false;
struct Handle { HANDLE h=nullptr;Handle()=default;explicit Handle(HANDLE p):h(p){};~Handle(){if(h&&h!=INVALID_HANDLE_VALUE&&!CloseHandle(h))close_failed=true;}Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete; };
static void Need(bool b,const char* s){if(!b)throw std::runtime_error(s);}
static std::wstring Wide(const std::string& s){for(unsigned char c:s)Need(c>=32&&c<127,"request is strict ASCII");return std::wstring(s.begin(),s.end());}
static std::string Narrow(const std::wstring& s){int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);Need(n>0,"path UTF16");std::string result(n,'\0');Need(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),result.data(),n,nullptr,nullptr)==n,"path UTF8 conversion");return result;}
static std::wstring Canon(const std::wstring& s){std::wstring r;Need(tq::Canonical(s,r),"canonical drive path refused");return r;}
static bool DPath(const std::wstring& s){return s.size()>3&&s[0]==L'd'&&s[1]==L':'&&s[2]==L'\\';}
static std::string HexBytes(const unsigned char* p,size_t n){std::string r;for(size_t i=0;i<n;i++){r+="0123456789abcdef"[p[i]>>4];r+="0123456789abcdef"[p[i]&15];}return r;}
struct File {std::wstring path;Handle handle;std::vector<unsigned char> bytes;std::string hash;
 File(const std::wstring& p,const std::string& expected,size_t maximum=128*1024*1024,bool pinned=true):path(Canon(p)),handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)){
  Need(handle.h!=INVALID_HANDLE_VALUE,"pinned FILE open refused");BY_HANDLE_FILE_INFORMATION info{};Need(GetFileInformationByHandle(handle.h,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"not an ordinary pinned FILE");
  wchar_t final[32768];DWORD n=GetFinalPathNameByHandleW(handle.h,final,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);Need(n&&n<32768,"final FILE path unknown");std::wstring resolved(final,n);if(resolved.rfind(L"\\\\?\\",0)==0)resolved.erase(0,4);Need(Canon(resolved)==path,"FILE alias refused");
  LARGE_INTEGER size{};Need(GetFileSizeEx(handle.h,&size)&&size.QuadPart>0&&uint64_t(size.QuadPart)<=maximum,"FILE length refused");bytes.resize(size_t(size.QuadPart));DWORD got=0;Need(ReadFile(handle.h,bytes.data(),DWORD(bytes.size()),&got,nullptr)&&got==bytes.size(),"FILE read refused");char extra;Need(ReadFile(handle.h,&extra,1,&got,nullptr)&&got==0,"FILE EOF refused");hash=tq::Sha256(bytes.data(),bytes.size());Need(!pinned||(startup::Hex(expected,64)&&hash==expected),"FILE SHA256 mismatch");
 }
};
struct PE {IMAGE_NT_HEADERS64 nt{};std::vector<IMAGE_SECTION_HEADER> sections;
 explicit PE(const std::vector<unsigned char>& b){Need(b.size()>=sizeof(IMAGE_DOS_HEADER),"DOS header short");IMAGE_DOS_HEADER d;memcpy(&d,b.data(),sizeof d);Need(d.e_magic==IMAGE_DOS_SIGNATURE&&d.e_lfanew>=0&&size_t(d.e_lfanew)+sizeof(nt)<=b.size(),"PE header refused");memcpy(&nt,b.data()+d.e_lfanew,sizeof nt);Need(nt.Signature==IMAGE_NT_SIGNATURE&&nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64&&nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC&&nt.FileHeader.NumberOfSections>0&&nt.FileHeader.NumberOfSections<=96,"AMD64 PE required");size_t at=size_t(d.e_lfanew)+4+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;Need(at+nt.FileHeader.NumberOfSections*sizeof(IMAGE_SECTION_HEADER)<=b.size(),"section table short");sections.resize(nt.FileHeader.NumberOfSections);memcpy(sections.data(),b.data()+at,sections.size()*sizeof(IMAGE_SECTION_HEADER));}
 size_t Offset(DWORD rva,size_t n,const std::vector<unsigned char>& b)const{for(auto&s:sections)if(rva>=s.VirtualAddress&&uint64_t(rva)+n<=uint64_t(s.VirtualAddress)+s.SizeOfRawData){size_t at=s.PointerToRawData+rva-s.VirtualAddress;if(at<=b.size()&&n<=b.size()-at)return at;}throw std::runtime_error("PE RVA not FILE backed");}
 DWORD Export(const std::vector<unsigned char>& b,const char* wanted)const{auto d=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];auto at=Offset(d.VirtualAddress,sizeof(IMAGE_EXPORT_DIRECTORY),b);IMAGE_EXPORT_DIRECTORY e;memcpy(&e,b.data()+at,sizeof e);Need(e.NumberOfNames<=4096&&e.NumberOfFunctions<=4096,"export count refused");for(DWORD i=0;i<e.NumberOfNames;i++){DWORD name;WORD ordinal;memcpy(&name,b.data()+Offset(e.AddressOfNames+i*4,4,b),4);memcpy(&ordinal,b.data()+Offset(e.AddressOfNameOrdinals+i*2,2,b),2);auto n=Offset(name,strlen(wanted)+1,b);if(memcmp(b.data()+n,wanted,strlen(wanted)+1))continue;Need(ordinal<e.NumberOfFunctions,"export ordinal refused");DWORD rva;memcpy(&rva,b.data()+Offset(e.AddressOfFunctions+ordinal*4,4,b),4);Need(!(rva>=d.VirtualAddress&&uint64_t(rva)<uint64_t(d.VirtualAddress)+d.Size),"forwarded custom export refused");Offset(rva,1,b);bool executable=false;for(auto&s:sections)if(rva>=s.VirtualAddress&&uint64_t(rva)<uint64_t(s.VirtualAddress)+s.Misc.VirtualSize&&(s.Characteristics&IMAGE_SCN_MEM_EXECUTE))executable=true;Need(executable,"export is not executable FILE section");return rva;}throw std::runtime_error("required export missing");}
};
