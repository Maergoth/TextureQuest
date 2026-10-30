#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace policy {
inline void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
inline uint64_t Decimal(const std::string& s,uint64_t limit=UINT64_MAX){Require(!s.empty()&&s.size()<=20,"decimal framing refused");uint64_t n=0;for(char c:s){Require(c>='0'&&c<='9',"decimal digit refused");uint64_t d=uint64_t(c-'0');Require(n<=limit/10&&(n<limit/10||d<=limit%10),"decimal overflow refused");n=n*10+d;}Require(s.size()==1||s[0]!='0',"noncanonical decimal refused");return n;}
inline std::map<std::string,std::string> Fields(const std::string& all,const std::string& magic,size_t maximum){
 Require(all.size()<=maximum&&all.size()>magic.size()&&all.rfind(magic,0)==0&&all.back()=='\n',"text framing refused");
 for(unsigned char c:all)Require(c=='\n'||(c>=32&&c<127),"strict ASCII text required");
 std::map<std::string,std::string> out;size_t at=magic.size();while(at<all.size()){auto end=all.find('\n',at),eq=all.find('=',at);Require(end!=std::string::npos&&eq>at&&eq<end&&eq+1<end,"field framing refused");Require(out.emplace(all.substr(at,eq-at),all.substr(eq+1,end-eq-1)).second,"duplicate field refused");at=end+1;}return out;
}
inline void Keys(const std::map<std::string,std::string>& fields,const std::set<std::string>& expected){std::set<std::string> actual;for(auto& p:fields)actual.insert(p.first);Require(actual==expected,"exact field set refused");}
inline bool Utf8(const unsigned char* p,size_t n){
 for(size_t i=0;i<n;){uint32_t c=p[i++];if(!c)return false;if(c<128)continue;unsigned more=0;uint32_t low=0;if(c>=0xc2&&c<=0xdf){more=1;c&=31;low=128;}else if(c>=0xe0&&c<=0xef){more=2;c&=15;low=2048;}else if(c>=0xf0&&c<=0xf4){more=3;c&=7;low=65536;}else return false;if(more>n-i)return false;while(more--){uint32_t d=p[i++];if((d&192)!=128)return false;c=(c<<6)|(d&63);}if(c<low||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;}return true;
}
struct Layout {uint32_t width,height,mips,family,representation;};
inline uint64_t BCBytes(const Layout& l){
 Require(l.width&&l.height&&l.width<=16384&&l.height<=16384&&l.mips&&l.mips<=15&&l.family>=1&&l.family<=3&&l.representation==1,"catalog layout refused");
 uint32_t full=1,w=l.width,h=l.height;while(w>1||h>1){w=w>1?w/2:1;h=h>1?h/2:1;full++;}Require(l.mips<=full,"excess mip count refused");
 uint64_t total=0;w=l.width;h=l.height;for(uint32_t i=0;i<l.mips;i++){total+=uint64_t((w+3)/4)*((h+3)/4)*(l.family==1?8:16);w=w>1?w/2:1;h=h>1?h/2:1;}return total;
}
struct Catalog {uint32_t rows=0;uint64_t payload_bytes=0;};
inline Catalog StockCatalog(const std::vector<unsigned char>& bytes){
 Require(bytes.size()>=16&&bytes.size()<=64ull*1024*1024+32768,"catalog length refused");size_t at=8;
 Require(!memcmp(bytes.data(),"TQABPA02",8),"catalog magic refused");
 auto take=[&](size_t n){Require(at<=bytes.size()&&n<=bytes.size()-at,"catalog truncated");size_t p=at;at+=n;return p;};
 auto word=[&](){uint32_t v;size_t p=take(4);memcpy(&v,bytes.data()+p,4);return v;};
 Require(word()==2,"catalog version refused");Catalog result;result.rows=word();Require(result.rows&&result.rows<=3,"catalog row cap refused");std::set<std::string> members,canonical;
 for(uint32_t row=0;row<result.rows;row++){
  Require(word()==1,"this coordinator requires explicit stock-canonical role1");uint32_t member_len=word(),provenance_len=word();Require(member_len&&member_len<=4096&&provenance_len&&provenance_len<=4096,"catalog label length refused");
  Layout a{},b{};auto layout=[&](Layout& l){l.width=word();l.height=word();l.mips=word();l.family=word();l.representation=word();};layout(a);layout(b);BCBytes(a);uint64_t expected=BCBytes(b);Require(a.family==b.family,"catalog family mismatch");
  size_t hashes=take(128);for(size_t i=hashes;i<hashes+128;i++)Require((bytes[i]>='0'&&bytes[i]<='9')||(bytes[i]>='a'&&bytes[i]<='f'),"fingerprint syntax refused");
  std::string key(reinterpret_cast<const char*>(bytes.data()+hashes),64);key+=":"+std::to_string(a.width)+":"+std::to_string(a.height)+":"+std::to_string(a.mips)+":"+std::to_string(a.family);Require(canonical.insert(key).second,"ambiguous canonical key refused");
  uint32_t payload=word();Require(payload==expected,"alternate BC byte count refused");result.payload_bytes+=payload;Require(result.payload_bytes<=64ull*1024*1024,"catalog aggregate payload cap refused");
  size_t member=take(member_len),provenance=take(provenance_len);Require(Utf8(bytes.data()+member,member_len)&&Utf8(bytes.data()+provenance,provenance_len),"catalog UTF8 label refused");Require(members.insert(std::string(reinterpret_cast<const char*>(bytes.data()+member),member_len)).second,"duplicate catalog member refused");take(payload);
 }
 Require(at==bytes.size(),"catalog trailing data refused");return result;
}
struct Control {uint64_t sequence=0,hwnd=0;uint32_t mode=0;bool command=false;};
inline Control ParseControl(const std::string& bytes,const std::string& nonce,uint32_t pid,uint64_t birth,uint64_t accepted,uint64_t observed){
 auto f=Fields(bytes,"TQPHC3\n",2048);Keys(f,{"nonce","pid","birth100ns","sequence","action","main_hwnd","mode"});
 Require(f.at("nonce")==nonce&&Decimal(f.at("pid"),UINT32_MAX)==pid&&Decimal(f.at("birth100ns"))==birth,"control identity refused");
 Control c;c.sequence=Decimal(f.at("sequence"),INT64_MAX);Require(c.sequence&&c.sequence>accepted,"control sequence stale");c.hwnd=Decimal(f.at("main_hwnd"));
 if(f.at("action")=="status"){Require(!c.hwnd&&f.at("mode")=="unchanged","status control operands refused");}
 else {Require(f.at("action")=="command"&&c.sequence>observed,"command sequence must exceed observed host requests");c.command=true;Require(f.at("mode")=="original"||f.at("mode")=="upscaled","control mode refused");c.mode=f.at("mode")=="original"?1:0;}
 return c;
}
}
