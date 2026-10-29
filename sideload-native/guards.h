#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "routing.h"
namespace bg {
inline bool Hex(const std::string& s,size_t n)noexcept{if(s.size()!=n)return false;for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
inline bool Covered(size_t returned,size_t offset,size_t width)noexcept{return offset<=returned&&width<=returned-offset;}
template<class Port,class Visit>inline bool ThreadRows(Port port,Visit visit){
    typename Port::Entry entry{};entry.dwSize=sizeof(entry);if(!port.First(entry))return false;
    do{if(!Covered(entry.dwSize,offsetof(typename Port::Entry,th32ThreadID),sizeof(entry.th32ThreadID))||!Covered(entry.dwSize,offsetof(typename Port::Entry,th32OwnerProcessID),sizeof(entry.th32OwnerProcessID)))return false;if(!visit(entry))return false;entry.dwSize=sizeof(entry);}while(port.Next(entry));
    return port.LastError()==Port::NoMore;
}
enum ParentRole{unknown_parent,main_candidate,game_child};
struct RowEvidence{bool observation_only;int64_t redirects;bool private_active;};
inline RowEvidence Evidence(uint64_t birth,int64_t redirects,bool active)noexcept{return birth?RowEvidence{false,redirects,active}:RowEvidence{true,0,false};}
inline bool CanAttachBirth(uint64_t birth)noexcept{return birth!=0;}
inline ParentRole Parent(bool known,bool exact_game,uint64_t parent_birth,uint64_t child_birth)noexcept{if(!known||!parent_birth||!child_birth||parent_birth>child_birth)return unknown_parent;return exact_game?game_child:main_candidate;}
inline std::vector<unsigned char> Owner(const std::wstring& game,const std::string& nonce,const std::string& manifest,const std::string& bindings){
    std::wstring canonical;if(!Hex(nonce,32)||!Hex(manifest,64)||!Hex(bindings,64)||!tq::Canonical(game,canonical)||canonical.size()>=tq::kPathChars)return{};
    std::vector<unsigned char> bytes={'T','Q','B','G','O','W','N','2'};auto n=[&](uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<unsigned char>(value>>(8*i)));};n(2);n(static_cast<uint32_t>(canonical.size()));
    for(const auto* text:{&nonce,&manifest,&bindings})bytes.insert(bytes.end(),text->begin(),text->end());for(wchar_t c:canonical){bytes.push_back(static_cast<unsigned char>(c));bytes.push_back(static_cast<unsigned char>(c>>8));}return bytes;
}
inline std::string StatusPrefix(const std::string& nonce,const std::string& owner){return"{\"schema\":\"tq-background-status-v2\",\"install_nonce\":\""+nonce+"\",\"owner_sha256\":\""+owner+"\",\"run_id\":\"";}
inline bool OwnedLatest(const std::string& bytes,const std::string& nonce,const std::string& owner) {
    if(bytes.size()>1024*1024||!Hex(nonce,32)||!Hex(owner,64))return false;const auto prefix=StatusPrefix(nonce,owner);
    return bytes.size()>=prefix.size()+35&&bytes.compare(0,prefix.size(),prefix)==0&&Hex(bytes.substr(prefix.size(),32),32)&&bytes.compare(prefix.size()+32,2,"\",")==0&&bytes.size()>=2&&bytes.compare(bytes.size()-2,2,"}\n")==0;
}
inline bool StartAllowed(bool owner_matches,bool fresh_event,bool legacy_pending,bool prior_present,bool prior_owned)noexcept{return owner_matches&&fresh_event&&!legacy_pending&&(!prior_present||prior_owned);}
enum LatestKind{latest_unknown,latest_absent,latest_file};
template<class Port>inline bool PublishStatus(Port& port){const auto kind=port.Lookup();if(kind==latest_unknown)return false;return kind==latest_file?port.Replace():port.MoveNew();}
}
