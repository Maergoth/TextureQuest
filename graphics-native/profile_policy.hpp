#pragma once
#include "startup_policy.hpp"
#include "native/routing.h"
#include <algorithm>
namespace textureprofile {
inline bool Purpose(const std::string& purpose,const std::wstring& target,const std::wstring& fixture_root){
 std::wstring path;if(!tq::Canonical(target,path)||path!=target)return false;
 if(purpose=="game")return path==L"c:\\everquest 2 origins\\everquest2.exe";
 return purpose=="fixture"&&tq::Prefix(path.c_str(),fixture_root)&&path.substr(path.find_last_of(L'\\')+1)==L"everquest2.exe";
}
inline std::string Lower(std::string v){for(char&c:v)if(c>='A'&&c<='Z')c+=32;return v;}
inline std::string Trim(std::string v){auto a=v.find_first_not_of(" \t\r"),b=v.find_last_not_of(" \t\r");return a==std::string::npos?"":v.substr(a,b-a+1);}
inline bool Adjacent(const std::string& text){
 if(text.size()>65535)return false;for(unsigned char c:text)if(c!='\t'&&c!='\r'&&c!='\n'&&(c<32||c>=127))return false;
 std::string section;std::set<std::string> sections,keys;size_t at=0;
 while(at<text.size()){auto end=text.find('\n',at);if(end==std::string::npos)end=text.size();auto line=Trim(text.substr(at,end-at));at=end+1;if(line.empty()||line[0]==';'||line[0]=='#')continue;
  if(line[0]=='['){if(line.size()<3||line.back()!=']')return false;section=Lower(Trim(line.substr(1,line.size()-2)));if(section.empty()||!sections.insert(section).second)return false;continue;}
  auto eq=line.find('=');if(section.empty()||eq==std::string::npos)return false;auto key=Lower(Trim(line.substr(0,eq))),value=Trim(line.substr(eq+1));
  if(key.empty()||!keys.insert(section+'/'+key).second)return false;if(section=="install"&&key=="basepath"&&!value.empty())return false;
 }return true;
}
inline std::string Ini(const std::string& addon){
 policy::Require(addon.size()>3&&addon[1]==':'&&addon[2]=='\\'&&((addon[0]>='A'&&addon[0]<='Z')||(addon[0]>='a'&&addon[0]<='z'))&&addon.find_first_of("\r\n,=")==std::string::npos,"exact owned D addon path refused");
 // ini_file.cpp89 splits single comma; no spaces are introduced at the split.
 return "[INSTALL]\nMaxEffectRuntimes=0\n[GENERAL]\nDisable=1\n[ADDON]\nAddonPath="+addon+"\nDisabledAddons=Generic Depth,Effect Runtime Sync\n[PROXY]\nEnableProxyLibrary=0\n";
}
inline std::map<std::wstring,std::wstring> Environment(const std::vector<std::wstring>& inherited,const std::wstring& base){
 std::map<std::wstring,std::wstring> result;
 for(const auto& line:inherited){auto eq=line.find(L'=');if(eq==0)continue;policy::Require(eq!=std::wstring::npos,"invalid inherited environment entry");auto key=line.substr(0,eq);for(auto& c:key)if(c>=L'a'&&c<=L'z')c-=32;
  if(key.rfind(L"RESHADE_",0)==0||key.rfind(L"TQPH_",0)==0)continue;policy::Require(result.emplace(key,line.substr(eq+1)).second,"duplicate environment name");}
 result[L"RESHADE_BASE_PATH_OVERRIDE"]=base;result[L"RESHADE_DISABLE_INPUT_HOOK"]=L"1";result[L"RESHADE_DISABLE_LOADING_CHECK"]=L"1";result[L"TQPH_SESSION_ROOT"]=base;return result;
}
} // namespace textureprofile
