#pragma once
// Shared production request/custody/intake policy, with injectable FILE/child leaves.
// Leaf injection is used only in the separate new CPU intake case; no serialized
// caller Boolean supplies a ready token to the coordinator.
#include "profile_policy.hpp"
#include <utility>
namespace intake {
inline bool Hex(const std::string& s,size_t n){return s.size()==n&&std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
inline std::wstring Canon(const std::string& s){std::wstring p(s.begin(),s.end()),r;policy::Require(tq::Canonical(p,r),"intent canonical drive path");return r;}
inline bool D(const std::wstring& p){return p.size()>3&&p[0]==L'd'&&p[1]==L':'&&p[2]==L'\\';}
inline std::wstring Parent(const std::wstring& p){return p.substr(0,p.find_last_of(L'\\'));}
inline std::wstring Name(const std::wstring& p){return p.substr(p.find_last_of(L'\\')+1);}
class Reader;
class Token {
 std::map<std::string,std::string> fields_;std::wstring target_,session_;bool held_=false;
 Token(std::map<std::string,std::string> f,std::wstring t,std::wstring s):fields_(std::move(f)),target_(std::move(t)),session_(std::move(s)),held_(true){}
 friend class Reader;
public:
 Token(const Token&)=default;Token(Token&&)=default;
 const std::map<std::string,std::string>& Fields()const noexcept{return fields_;}
 const std::wstring& Target()const noexcept{return target_;}const std::wstring& Session()const noexcept{return session_;}
 bool FreshChild(const std::wstring& target,const std::string& image)const{return held_&&target==target_&&image==fields_.at("target_sha256");}
 bool ModuleCopy(const std::wstring& path,const std::string& hash)const{
  if(!held_||Parent(path)!=session_)return false;
  for(const auto role:{"provider","host","bootstrap"})if(Name(path)==Name(Canon(fields_.at(role)))&&hash==fields_.at(std::string(role)+"_sha256"))return true;return false;
 }
};
class Reader {
public:
 template<class Files>static Token Read(const std::string& text,const std::string& argv_purpose,const std::wstring& fixture_root,const std::string& source_map,Files& files){
  auto f=policy::Fields(text,"TQPH3\n",65535);
  policy::Keys(f,{"purpose","nonce","target","target_sha256","adjacent_config_sha256","provider","provider_sha256","provider_source_sha256","provider_binding","provider_binding_sha256","host","host_sha256","bootstrap","bootstrap_sha256","catalog","catalog_sha256","session","external_watchers_quiet","canonical_role","private_experimental"});
  policy::Require(Hex(source_map,64)&&f.at("provider_source_sha256")==source_map,"corrected full provider source-map authority");
  policy::Require(f.at("purpose")==argv_purpose&&f.at("external_watchers_quiet")=="1"&&f.at("canonical_role")=="stock"&&f.at("private_experimental")=="wrapper-observed-default-nativegap1"&&Hex(f.at("nonce"),32),"explicit purpose/private graph intent");
  for(const auto key:{"target_sha256","adjacent_config_sha256","provider_sha256","provider_binding_sha256","host_sha256","bootstrap_sha256","catalog_sha256"})policy::Require(Hex(f.at(key),64),"whole FILE pin framing");
  auto target=Canon(f.at("target")),session=Canon(f.at("session"));policy::Require(textureprofile::Purpose(argv_purpose,target,fixture_root)&&D(session)&&session.size()<220,"bounded exact-purpose target/new D session");
  for(const auto role:{"provider","provider_binding","host","bootstrap","catalog"})policy::Require(D(Canon(f.at(role))),"D module/binding/catalog custody");
  policy::Require(Name(Canon(f.at("provider")))==L"reshade64.dll"&&Name(Canon(f.at("host")))==L"texturequestproviderhostv3.addon64"&&Name(Canon(f.at("bootstrap")))==L"tqprovidertextureprofilev3.dll","coherent version3 image roles");
  // A separately request-pinned, ROOT-closed build binding joins exact source
  // and exact image. Neither an arbitrary DLL nor a header hash proves source.
  auto binding=policy::Fields(files.ReadBinding(Canon(f.at("provider_binding")),f.at("provider_binding_sha256")),"TQPHB3\n",1024);
  policy::Keys(binding,{"source_map_sha256","image_sha256"});policy::Require(binding.at("source_map_sha256")==source_map&&binding.at("image_sha256")==f.at("provider_sha256"),"source-map/image build binding mismatch");
  files.Hold("target",target,f.at("target_sha256"));files.Hold("adjacent",Parent(target)+L"\\reshade.ini",f.at("adjacent_config_sha256"));policy::Require(textureprofile::Adjacent(files.AdjacentText()),"held EXE-neighbor metadata precedence");
  for(const auto role:{"provider","host","bootstrap","catalog"})files.Hold(role,Canon(f.at(role)),f.at(std::string(role)+"_sha256"));
  return Token(std::move(f),std::move(target),std::move(session));
 }
};
template<class Create>decltype(auto) FreshChild(const Token& token,const std::wstring& target,const std::string& hash,Create create){policy::Require(token.FreshChild(target,hash),"fresh child lacks complete intake authority");return create();}
template<class Load>decltype(auto) ModuleCopy(const Token& token,const std::wstring& path,const std::string& hash,Load load){policy::Require(token.ModuleCopy(path,hash),"module copy lacks exact source/image/intake authority");return load();}
} // namespace intake
