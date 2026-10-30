#pragma once
#include "engine.hpp"
#include <windows.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace tqgx {
using namespace tqab;
struct PayloadJob {uint64_t offset=0,bytes=0;Layout layout;std::string sha256,fingerprint;};
class PayloadFile {
 HANDLE file_=INVALID_HANDLE_VALUE;uint64_t size_=0;
public:
 PayloadFile(const std::wstring& path,uint64_t expected){
  file_=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  require(file_!=INVALID_HANDLE_VALUE,"graphics payload FILE unavailable");
  BY_HANDLE_FILE_INFORMATION i{};LARGE_INTEGER n{};
  if(!GetFileInformationByHandle(file_,&i)||(i.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||i.nNumberOfLinks!=1||!GetFileSizeEx(file_,&n)||n.QuadPart<0||uint64_t(n.QuadPart)!=expected){CloseHandle(file_);file_=INVALID_HANDLE_VALUE;throw std::runtime_error("graphics payload FILE identity/size");}size_=expected;
 }
 ~PayloadFile(){if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);}
 Bytes Read(const PayloadJob& job){
  require(job.bytes&&job.bytes<=MaxCpu&&job.offset<=size_&&job.bytes<=size_-job.offset,"graphics payload range");
  LARGE_INTEGER p{};p.QuadPart=job.offset;require(SetFilePointerEx(file_,p,nullptr,FILE_BEGIN)!=FALSE,"graphics payload seek");
  Bytes bytes(size_t(job.bytes));DWORD n=0;require(ReadFile(file_,bytes.data(),DWORD(bytes.size()),&n,nullptr)&&n==bytes.size(),"graphics payload read");return bytes;
 }
};

// Worker performs all disk reads and payload hashes. Rendering only requests
// a key or borrows an immutable completed entry; no render-thread disk I/O.
class PayloadPool final:public PayloadAccess {
 struct Entry {std::shared_ptr<const Bytes> bytes;uint64_t stamp=0;};
 std::function<Bytes(const PayloadJob&)> read_;const uint64_t budget_;
 std::mutex mutex_;std::condition_variable wake_;std::deque<PayloadJob> queue_;
 std::unordered_map<std::string,bool> queued_;std::unordered_map<std::string,Entry> entries_;
 uint64_t bytes_=0,clock_=0;bool stop_=false,enabled_=false,failed_=false;std::thread thread_;
 void Worker()noexcept{
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
  for(;;){PayloadJob job;{std::unique_lock lock(mutex_);wake_.wait(lock,[&]{return stop_||!queue_.empty();});if(stop_)return;job=std::move(queue_.front());queue_.pop_front();}
   try{auto data=read_(job);require(data.size()==job.bytes&&tq::Sha256(data.data(),data.size())==job.sha256&&Fingerprint(job.layout,data)==job.fingerprint,"graphics payload content changed");
    auto owned=std::make_shared<const Bytes>(std::move(data));std::lock_guard lock(mutex_);queued_.erase(job.fingerprint);if(!enabled_)continue;
    while(bytes_+owned->size()>budget_){auto victim=entries_.end();for(auto i=entries_.begin();i!=entries_.end();++i)if(i->second.bytes.use_count()==1&&(victim==entries_.end()||i->second.stamp<victim->second.stamp))victim=i;if(victim==entries_.end())break;bytes_-=victim->second.bytes->size();entries_.erase(victim);}
    if(bytes_+owned->size()<=budget_){auto [i,inserted]=entries_.emplace(job.fingerprint,Entry{owned,++clock_});if(inserted)bytes_+=owned->size();}
   }catch(...){std::lock_guard lock(mutex_);failed_=true;queued_.erase(job.fingerprint);queue_.clear();queued_.clear();}
  }
 }
public:
 PayloadPool(std::function<Bytes(const PayloadJob&)> read,uint64_t budget=128*MiB):read_(std::move(read)),budget_(budget){require(read_&&budget_>=MaxCpu&&budget_<=256*MiB,"graphics CPU cache budget");thread_=std::thread([this]{Worker();});}
 ~PayloadPool(){ {std::lock_guard lock(mutex_);stop_=true;}wake_.notify_one();if(thread_.joinable())thread_.join();}
 void Enabled(bool value)override{std::lock_guard lock(mutex_);enabled_=value;if(!value){queue_.clear();queued_.clear();entries_.clear();bytes_=0;}}
 bool Failed()override{std::lock_guard lock(mutex_);return failed_;}
 std::shared_ptr<const Bytes> Get(const Row& row)override{
  std::lock_guard lock(mutex_);if(failed_||!enabled_)return {};auto i=entries_.find(row.stock_sha256);if(i!=entries_.end()){i->second.stamp=++clock_;return i->second.bytes;}
  if(queue_.size()<256&&!queued_.count(row.stock_sha256)){queue_.push_back({row.payload_offset,row.payload_bytes,row.stock,row.payload_sha256,row.stock_sha256});queued_.emplace(row.stock_sha256,true);wake_.notify_one();}return {};
 }
 uint64_t ResidentBytes(){std::lock_guard lock(mutex_);return bytes_;}
};

struct FullCatalog {std::shared_ptr<const Catalog> catalog;std::shared_ptr<PayloadPool> payloads;uint32_t rows=0;};
inline FullCatalog ReadCatalog(const Bytes& index,const std::wstring& payload){
 size_t at=0;auto take=[&](size_t n){require(at<=index.size()&&n<=index.size()-at,"graphics index truncated");auto p=index.data()+at;at+=n;return p;};
 auto u32=[&](){uint32_t n;memcpy(&n,take(4),4);return n;};auto u64=[&](){uint64_t n;memcpy(&n,take(8),8);return n;};
 require(index.size()<=64*MiB&&!memcmp(take(8),"TQGX0001",8)&&u32()==1,"graphics index framing");uint32_t count=u32();uint64_t payload_bytes=u64();require(count&&count<=100000&&payload_bytes<=512ull*1024*MiB,"graphics index bounds");
 auto file=std::make_shared<PayloadFile>(payload,payload_bytes);auto pool=std::make_shared<PayloadPool>([file](const PayloadJob&job){return file->Read(job);});
 auto hex=[&](){auto p=take(32);std::string result;for(unsigned i=0;i<32;++i){result+="0123456789abcdef"[p[i]>>4];result+="0123456789abcdef"[p[i]&15];}return result;};
 std::vector<Row> rows;rows.reserve(count);
 for(uint32_t i=0;i<count;++i){uint32_t owner=u32(),member=u32();require(owner&&owner<=256&&member&&member<=4096,"graphics literal labels");Layout upload,original;
  auto layout=[&](Layout&l){l.width=u32();l.height=u32();l.mips=u32();l.family=u32();l.representation=u32();};layout(upload);layout(original);uint64_t offset=u64(),bytes=u64();
  auto source=hex(),target=hex(),sha=hex(),source_dds=hex(),target_dds=hex();auto p=take(owner);std::string name(reinterpret_cast<const char*>(p),owner);p=take(member);std::string literal(reinterpret_cast<const char*>(p),member);
  require(name.find('\0')==std::string::npos&&literal.find('\0')==std::string::npos&&bytes==ByteCount(original)&&offset<=payload_bytes&&bytes<=payload_bytes-offset,"graphics row range/layout");
  Row row{literal,name+":"+source_dds+":"+target_dds,source,target,upload,original,{},CanonicalRole::upscaled};row.payload_offset=offset;row.payload_bytes=bytes;row.payload_sha256=sha;rows.push_back(std::move(row));
 }
 require(at==index.size(),"graphics index trailing bytes");return {std::make_shared<Catalog>(std::move(rows),pool),pool,count};
}
} // namespace tqgx
