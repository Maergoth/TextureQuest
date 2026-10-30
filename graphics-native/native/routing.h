#pragma once
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace tq {
constexpr size_t kPathChars=32768, kMaxManifest=4*1024*1024, kMaxRoutes=1024;
inline wchar_t Fold(wchar_t c) noexcept { return c>=L'A'&&c<=L'Z'?c+(L'a'-L'A'):c; }
inline bool Equal(const wchar_t* a,const wchar_t* b) noexcept {
    while(*a&&*b) { if(Fold(*a++)!=Fold(*b++)) return false; } return *a==*b;
}
inline bool Prefix(const wchar_t* path,const std::wstring& root) noexcept {
    size_t i=0;for(;i<root.size();++i) if(!path[i]||Fold(path[i])!=Fold(root[i])) return false;
    return path[i]==L'\\';
}
// Pure lexical Windows drive-path normalization. Device/UNC/drive-relative names refuse.
inline bool Normalize(const wchar_t* source,const wchar_t* base,wchar_t* out) noexcept {
    if(!source||!*source) return false;
    if(source[0]==L'\\'&&source[1]==L'\\'&&source[2]==L'?'&&source[3]==L'\\') {
        source+=4;
        // Extended-length paths disable ordinary Win32 slash/dot normalization.
        for(const wchar_t* p=source;*p;++p)if(*p==L'/')return false;
        const wchar_t* component=source;
        for(const wchar_t* p=source;;++p)if(*p==L'\\'||!*p){size_t n=static_cast<size_t>(p-component);if((n==1&&component[0]==L'.')||(n==2&&component[0]==L'.'&&component[1]==L'.'))return false;if(!*p)break;component=p+1;}
    }
    size_t used=0;
    const bool drive=((source[0]>=L'A'&&source[0]<=L'Z')||(source[0]>=L'a'&&source[0]<=L'z'))&&source[1]==L':'&&(source[2]==L'\\'||source[2]==L'/');
    auto push=[&](wchar_t c){if(used+1>=kPathChars)return false;out[used++]=c;return true;};
    if(drive) { if(!push(Fold(source[0]))||!push(L':')||!push(L'\\'))return false;source+=3; }
    else {
        if(!base||source[0]==L'\\'||source[0]==L'/'||source[1]==L':') return false;
        for(const wchar_t* p=base;*p;++p)if(!push(*p))return false;
        if(!push(L'\\'))return false;
    }
    while(*source) {
        while(*source==L'\\'||*source==L'/')++source;
        if(!*source)break;
        const wchar_t* start=source;
        while(*source&&*source!=L'\\'&&*source!=L'/')++source;
        size_t n=static_cast<size_t>(source-start);
        if(n==1&&start[0]==L'.')continue;
        if(n==2&&start[0]==L'.'&&start[1]==L'.') {
            if(used<=3)return false;
            if(out[used-1]==L'\\')--used;
            while(used>3&&out[used-1]!=L'\\')--used;
            continue;
        }
        // ADS, wildcard, trailing space/dot and Win32 namespace aliases are not route keys.
        if(start[n-1]==L' '||start[n-1]==L'.')return false;
        for(size_t i=0;i<n;++i) {
            wchar_t c=start[i];if(c<L' '||c==L':'||c==L'*'||c==L'?'||c==L'"'||c==L'<'||c==L'>'||c==L'|')return false;
            if(!push(Fold(c)))return false;
        }
        if(*source&&!push(L'\\'))return false;
    }
    if(used>3&&out[used-1]==L'\\')--used;
    out[used]=0;return used>=3;
}
struct Route { std::wstring original,replacement; };
struct Manifest { std::wstring game_root,package_root; std::vector<Route> rows; };
inline bool Canonical(const std::wstring& input,std::wstring& output) {
    wchar_t normalized[kPathChars];if(!Normalize(input.c_str(),nullptr,normalized))return false;output=normalized;return true;
}
// A portable component is exactly <owned data>\sideload\runtimes\<payload SHA256>.
// Derive the single downloads sibling from this layout, never from a user name.
inline bool ComponentDataRoot(const std::wstring& package,std::wstring& data) {
    data.clear();std::wstring root;if(!Canonical(package,root))return false;
    const std::wstring marker=L"\\sideload\\runtimes\\";
    if(root.size()<=marker.size()+64+3)return false;
    const size_t at=root.size()-marker.size()-64;
    if(root.compare(at,marker.size(),marker)!=0)return false;
    for(size_t i=root.size()-64;i<root.size();++i)
        if(!((root[i]>=L'0'&&root[i]<=L'9')||(root[i]>=L'a'&&root[i]<=L'f')))return false;
    data=root.substr(0,at);return data.size()>3;
}
inline bool DisjointTrees(const std::wstring& a,const std::wstring& b) {
    return a.size()>3&&b.size()>3&&!Equal(a.c_str(),b.c_str())&&!Prefix(a.c_str(),b)&&!Prefix(b.c_str(),a);
}
inline bool AllowedOriginal(const std::wstring& path,const std::wstring& root) {
    if(Equal(path.c_str(),(root+L"\\assetslib.vpl").c_str()))return true;
    const std::wstring paks=root+L"\\paks";
    if(!Prefix(path.c_str(),paks))return false;
    const wchar_t* leaf=path.c_str()+paks.size()+1;
    if(!*leaf||std::wcschr(leaf,L'\\'))return false;
    size_t n=std::wcslen(leaf);return n>4&&Equal(leaf+n-4,L".vpk");
}
inline bool Parse(const unsigned char* data,size_t bytes,const std::wstring& game_root,const std::wstring& package_root,Manifest& result,std::string& error) {
    result=Manifest{};
    auto refuse=[&](const char* why){error=why;result=Manifest{};return false;};
    if(bytes<16||bytes>kMaxManifest||std::memcmp(data,"TQCFW001",8))return refuse("manifest header/size");
    size_t pos=8;
    auto u32=[&](uint32_t& v){if(bytes-pos<4)return false;v=static_cast<uint32_t>(data[pos])|(static_cast<uint32_t>(data[pos+1])<<8)|(static_cast<uint32_t>(data[pos+2])<<16)|(static_cast<uint32_t>(data[pos+3])<<24);pos+=4;return true;};
    uint32_t version=0,count=0;if(!u32(version)||!u32(count)||version!=1||count==0||count>kMaxRoutes)return refuse("manifest version/count");
    std::wstring data_root;
    if(!Canonical(game_root,result.game_root)||!Canonical(package_root,result.package_root)||!ComponentDataRoot(result.package_root,data_root)||!DisjointTrees(result.game_root,data_root))return refuse("portable roots/layout/disjointness");
    const std::wstring downloads=data_root+L"\\downloads";
    for(uint32_t i=0;i<count;++i) {
        uint32_t a=0,b=0;if(!u32(a)||!u32(b)||a==0||b==0||a>=kPathChars||b>=kPathChars)return refuse("manifest length");
        if(static_cast<uint64_t>(a+b)*2>bytes-pos)return refuse("manifest truncated strings");
        auto read=[&](uint32_t n,std::wstring& s){s.reserve(n);for(uint32_t j=0;j<n;++j){uint16_t ch=static_cast<uint16_t>(data[pos])|static_cast<uint16_t>(data[pos+1]<<8);pos+=2;if(!ch)return false;s.push_back(static_cast<wchar_t>(ch));}return true;};
        std::wstring raw_a,raw_b,original,replacement;
        if(!read(a,raw_a)||!read(b,raw_b)||!Canonical(raw_a,original)||!Canonical(raw_b,replacement))return refuse("manifest path/NUL/absolute");
        if(!AllowedOriginal(original,result.game_root))return refuse("manifest original scope");
        if(!Prefix(replacement.c_str(),result.package_root)&&!Prefix(replacement.c_str(),downloads))return refuse("manifest replacement scope");
        for(const auto& row:result.rows)if(Equal(row.original.c_str(),original.c_str()))return refuse("manifest duplicate original");
        result.rows.push_back({original,replacement});
    }
    if(pos!=bytes)return refuse("manifest trailing bytes");
    error.clear();return true;
}
inline bool ReadOnlyOpen(uint32_t access,uint32_t creation,uint32_t flags) noexcept {
    constexpr uint32_t read_bits=0x80000000u|0x00000001u|0x00000008u|0x00000080u|0x00020000u|0x00100000u;
    return creation==3&&(access&~read_bits)==0&&(flags&(0x04000000u|0x00200000u))==0;
}
inline const wchar_t* Select(const Manifest& m,const wchar_t* safe_name,uint32_t access,uint32_t creation,uint32_t flags,wchar_t* scratch) noexcept {
    if(!ReadOnlyOpen(access,creation,flags)||!Normalize(safe_name,m.game_root.c_str(),scratch))return nullptr;
    for(const auto& row:m.rows)if(Equal(row.original.c_str(),scratch))return row.replacement.c_str();
    return nullptr;
}
// Same helper is used by production callback and fake-leaf CPU cases. ErrorPort
// exposes Get/Set; Forward's only callable is the original seven-argument ABI.
template<class Fn,class ErrorPort,class Security,class Handle>
inline auto Forward(const wchar_t* original_name,const wchar_t* selected,uint32_t access,uint32_t share,Security security,uint32_t creation,uint32_t flags,Handle template_handle,Fn original,ErrorPort error) -> decltype(original(original_name,access,share,security,creation,flags,template_handle)) {
    const uint32_t entry=error.Get();
    error.Set(entry);
    auto result=original(selected?selected:original_name,access,share,security,creation,flags,template_handle);
    const uint32_t returned=error.Get();
    error.Set(returned);return result;
}
}
