#pragma once
#include <array>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>
namespace tqdefault {
// Pure policy shared by the native host and the new CPU leaves. Values are not
// authority: native owners and private lease instances supply that separately.
struct Stamp {uint64_t device=0,thread=0,generation=0;std::array<uint64_t,12> descriptor{};std::array<uint64_t,48> metadata{};bool operator==(const Stamp&b)const{return device==b.device&&thread==b.thread&&generation==b.generation&&descriptor==b.descriptor&&metadata==b.metadata;}};
class CreationJoin {
 bool pending_=false,closed_=false;Stamp stamp_{};void* ticket_=nullptr;
public:
 bool Begin(const Stamp& s,void* private_ticket)noexcept{if(closed_)return false;if(pending_){closed_=true;return false;}pending_=true;stamp_=s;ticket_=private_ticket;return true;}
 void* Finish(const Stamp& s)noexcept{if(closed_||!pending_||!(s==stamp_)){closed_=true;return nullptr;}pending_=false;auto t=ticket_;ticket_=nullptr;return t;}
 bool Closed()const noexcept{return closed_;}bool Pending()const noexcept{return pending_;}
};
class SessionState {
 std::atomic<unsigned> phase_{0};std::atomic<bool> closed_{false};
public:
 bool Attempt()noexcept{unsigned n=0;return phase_.compare_exchange_strong(n,1);}
 bool Installed()noexcept{unsigned n=1;return !closed_&&phase_.compare_exchange_strong(n,2);}
 bool Bind()noexcept{unsigned n=2;return !closed_&&phase_.compare_exchange_strong(n,3);}
 void Close()noexcept{closed_=true;}bool Live()const noexcept{return !closed_&&phase_==3;}
 unsigned Phase()const noexcept{return phase_;}bool Closed()const noexcept{return closed_;}
};
template<class Owner,class Lock>class HeldFrame {
 Owner* owner_;Lock* lock_;std::thread::id thread_=std::this_thread::get_id();HeldFrame* previous_;
 inline static thread_local HeldFrame* current_=nullptr;
public:
 HeldFrame(Owner* owner,Lock& lock):owner_(owner),lock_(&lock),previous_(current_){current_=this;}
 ~HeldFrame(){current_=previous_;}
 HeldFrame(const HeldFrame&)=delete;
 static bool Owns(Owner* owner)noexcept{auto f=current_;return f&&f->owner_==owner&&f->thread_==std::this_thread::get_id()&&f->lock_&&f->lock_->owns_lock();}
};
enum class Mutation {pass,invalidate,close};
inline Mutation DecideMutation(bool same_device,bool selected_resource,bool selected_context,bool swap)noexcept{
 if(!same_device)return Mutation::pass;if(swap)return Mutation::close;
 if(!selected_resource)return Mutation::pass;return selected_context?Mutation::invalidate:Mutation::close;
}
template<class B,class O,class A,class F>void ForwardOnce(B before,O original,A after,F fault){try{before();}catch(...){fault();}original();try{after();}catch(...){fault();}}
}
