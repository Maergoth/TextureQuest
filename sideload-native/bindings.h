#pragma once
#include "routing.h"
namespace bg {
struct Binding{std::wstring path;uint64_t bytes;int64_t mtime_ns;};
inline bool ParseBindings(const unsigned char* data,size_t size,const tq::Manifest& m,std::vector<Binding>& rows){
    rows.clear();if(size<16||size>tq::kMaxManifest||std::memcmp(data,"TQCFWB01",8))return false;size_t at=8;
    auto number=[&](unsigned n,uint64_t& out){if(n>size-at)return false;out=0;for(unsigned i=0;i<n;++i)out|=static_cast<uint64_t>(data[at++])<<(8*i);return true;};
    uint64_t version=0,count=0;if(!number(4,version)||!number(4,count)||version!=1||!count||count>2048)return false;
    for(size_t i=0;i<count;++i){uint64_t length=0,reserved=0,bytes=0,stamp=0;if(!number(4,length)||!number(4,reserved)||!number(8,bytes)||!number(8,stamp)||reserved||!length||length>=tq::kPathChars||length*2>size-at||bytes>INT64_MAX||stamp>INT64_MAX)return false;
        if(stamp%100)return false;
        std::wstring path;path.reserve(static_cast<size_t>(length));for(size_t j=0;j<length;++j){uint64_t c=0;if(!number(2,c)||!c)return false;path.push_back(static_cast<wchar_t>(c));}std::wstring canonical;if(!tq::Canonical(path,canonical))return false;
        for(const auto& r:rows)if(tq::Equal(r.path.c_str(),canonical.c_str()))return false;rows.push_back({canonical,bytes,static_cast<int64_t>(stamp)});
    }
    if(at!=size)return false;std::vector<std::wstring> expected;
    for(const auto& r:m.rows)for(const auto* path:{&r.original,&r.replacement}){bool present=false;for(const auto& e:expected)if(tq::Equal(e.c_str(),path->c_str()))present=true;if(!present)expected.push_back(*path);}
    if(rows.size()!=expected.size())return false;for(const auto& e:expected){bool present=false;for(const auto& r:rows)if(tq::Equal(e.c_str(),r.path.c_str()))present=true;if(!present)return false;}return true;
}
inline bool FreshBinding(uint64_t bytes,uint64_t filetime,const Binding& b)noexcept{
    constexpr uint64_t epoch=116444736000000000ull;
    return b.mtime_ns>=0&&b.mtime_ns%100==0&&filetime>=epoch&&filetime-epoch<=static_cast<uint64_t>(INT64_MAX/100)&&bytes==b.bytes&&static_cast<int64_t>((filetime-epoch)*100)==b.mtime_ns;
}
}
