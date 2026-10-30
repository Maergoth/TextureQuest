#pragma once
#include <atomic>
#include <cstddef>
namespace tqdefault {
enum class MutationTarget:unsigned {device_wide=0,resource=1,view=2};
// Caller serializes all ticket/count operations with the registry mutex. Only
// Closed() may be read outside it. No native lock, timer or thread factory here.
struct PendingTicket {enum class Phase {empty,open,finishing,registered};Phase phase=Phase::empty;};
class PendingAuthority {
 std::size_t unresolved_=0;std::atomic<bool> closed_{false};
public:
 bool Begin(PendingTicket&t)noexcept{if(closed_)return false;if(t.phase!=PendingTicket::Phase::empty){closed_=true;return false;}t.phase=PendingTicket::Phase::open;++unresolved_;return true;}
 bool Consume(PendingTicket&t)noexcept{if(closed_)return false;if(t.phase!=PendingTicket::Phase::open){closed_=true;return false;}t.phase=PendingTicket::Phase::finishing;return true;}
 bool CanCommit(const PendingTicket&t)const noexcept{return !closed_&&unresolved_&&t.phase==PendingTicket::Phase::finishing;}
 bool Commit(PendingTicket&t)noexcept{if(!CanCommit(t)){closed_=true;return false;}t.phase=PendingTicket::Phase::registered;--unresolved_;return true;}
 bool Observe(bool selected_device)noexcept{if(!selected_device||!unresolved_)return false;closed_=true;return true;}
 bool Closed()const noexcept{return closed_.load();}
 void Close()noexcept{closed_=true;}
 std::size_t Unresolved()const noexcept{return unresolved_;}
};
// Called under the same registry mutex as insertion+Commit. Registered() only
// retires proved matching rows; unknown resource/view identities are not guessed.
template<class Registered>bool MutationAuthority(PendingAuthority&p,bool selected_device,MutationTarget kind,Registered registered){
 if(!selected_device)return false;if(p.Observe(true))return true;
 if(kind==MutationTarget::device_wide){p.Close();return true;}registered();return false;
}
// Used by real host wrappers and new fake mutation entries. Notify is noexcept
// at the host boundary; the inherited handler's exact return is preserved.
template<class Notify,class Entry>decltype(auto) NotifyBefore(Notify notify,Entry entry){notify();return entry();}
}
