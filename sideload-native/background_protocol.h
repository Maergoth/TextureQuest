#pragma once
#include <cstdint>
#include <cstring>
#include "routing.h"
namespace bg {
enum Phase:int32_t { pending=0,active=2,permanent_stock=3 };
enum Action { original,private_open,first_index };
enum EventAction{ignore_event,uncertain_event,select_event};
inline EventAction ClassifyEvent(bool filename_matches,bool known_path_and_birth,bool exact_path)noexcept{return !filename_matches?ignore_event:!known_path_and_birth?uncertain_event:exact_path?select_event:ignore_event;}
inline bool FreshAt(uint64_t birth,uint64_t now)noexcept{return birth!=0&&now>=birth&&now-birth<=20000000ull;}
inline bool Admit(bool exact_main,bool baseline,bool known_identity,bool census_clear,uint64_t birth,uint64_t now)noexcept{return exact_main&&!baseline&&known_identity&&census_clear&&FreshAt(birth,now);}
inline Action Decide(int32_t& phase,bool index,bool eligible) noexcept {
    if(phase==permanent_stock)return original;
    if(phase==active)return private_open;
    if(!index||!eligible){phase=permanent_stock;return original;}
    return first_index;
}
inline void FinishIndex(int32_t& phase,bool succeeded)noexcept{phase=succeeded?active:permanent_stock;}
struct alignas(8) Status {
    char magic[8];uint32_t version,size,pid;uint64_t birth100ns;int32_t state,phase,error;
    uint32_t routes,prehistory_unknown;int64_t selected,succeeded,failed,stock;int32_t last_error,last_failed;
    char hash[65],message[192];
};
struct Config {
    char magic[8];uint32_t size,pid;uint64_t birth100ns;
    wchar_t manifest[tq::kPathChars],bindings[tq::kPathChars],package[tq::kPathChars],game[tq::kPathChars],mapping[96];char hash[65],bindings_hash[65];
};
inline void Prepare(Status& s,uint32_t pid,uint64_t birth,const std::string& hash)noexcept{
    std::memset(&s,0,sizeof(s));std::memcpy(s.magic,"TQBGST01",8);s.version=1;s.size=sizeof(s);s.pid=pid;s.birth100ns=birth;s.prehistory_unknown=1;if(hash.size()==64)std::memcpy(s.hash,hash.data(),64);
}
inline bool Matches(const Status& s,const Config& c)noexcept{return !std::memcmp(s.magic,"TQBGST01",8)&&s.version==1&&s.size==sizeof(s)&&s.pid==c.pid&&s.birth100ns==c.birth100ns&&s.prehistory_unknown==1&&!std::memcmp(s.hash,c.hash,65);}
template<class Atom>inline void Count(Status& s,bool selected,bool okay,uint32_t error,Atom atom)noexcept{
    if(selected){atom.Add(&s.selected,1);atom.Add(okay?&s.succeeded:&s.failed,1);atom.Set(&s.last_error,static_cast<int32_t>(error));atom.Set(&s.last_failed,okay?0:1);}else atom.Add(&s.stock,1);
}
}
