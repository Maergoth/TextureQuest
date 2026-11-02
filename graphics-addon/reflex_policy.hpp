#pragma once
#include "nvapi_reflex_types.hpp"
#include <cstdint>
#include <mutex>
namespace tq::reflex {
enum class State : uint32_t { Off, On, External, Unsupported, Error, Releasing, Requested };
enum class Reason : uint32_t { None, Unavailable, PriorExternal, DriverNotActive, QueryFailed, SetFailed, DisableFailed, CloseFailed, ForeignWriter, DeviceGoneUnconfirmed, ReleaseUnverified, NeutralDriverStillOn };
struct Snapshot {
 State Status=State::Off;Reason Why=Reason::None;NvStatus Code=Ok,OpenCode=Ok;
 bool Requested=false,Allowed=false,Owned=false,ObservedKnown=false,ObservedOn=false;
 // CleanupPending is current. CleanupFailed/CleanupCode retain historical failure.
 bool CleanupPending=false,CleanupFailed=false;
 bool NeutralAccepted=false,ReleaseVerificationUnknown=false,ModuleCleanupPending=false;NvStatus CleanupCode=Ok;
 uint8_t ObservedFgMultiplier=0;bool ObservedDfgControl=false;
 uint64_t Generation=0;void* Owner=nullptr;uintptr_t Window=0;
};
struct Api {
 virtual ~Api()=default;
 virtual NvStatus Open()=0;
 virtual NvStatus Get(void* native,SleepStatus& out)=0;
 virtual NvStatus Set(void* native,const SleepMode& request)=0;
 virtual NvStatus Close()=0;
};
// Owner/native/window admission belongs to ROOT's primary identity wrapper.
// Destroy/Shutdown require still-valid callback custody, outside DllMain.
class Policy {
 Api& Driver;mutable std::mutex Lock;Snapshot Last;
 void* Native=nullptr;
 bool Opened=false,CloseAttempted=false,Attempted=false,ReleaseAttempted=false,Foreign=false,PreviousDesired=false;
 uint64_t LastQuery=0,NextGeneration=0;
 // FG multiplier/DFG are observations, not proof of another NVAPI writer.
 static bool OtherSleep(const SleepStatus&s){return s.bUseGameSleep!=0;}
 void Pending(){Last.CleanupPending=Last.ReleaseVerificationUnknown||Last.ModuleCleanupPending||(Last.Owned&&(ReleaseAttempted||Foreign));}
 void CleanupFailure(NvStatus code){Last.CleanupFailed=true;Last.CleanupCode=code;Pending();}
 void NeutralSummary(){
  if(Last.ReleaseVerificationUnknown){Last.Status=State::Releasing;Last.Why=Reason::ReleaseUnverified;return;}
  if(Last.NeutralAccepted){Last.Status=State::Off;Last.Why=Last.ObservedKnown&&Last.ObservedOn?Reason::NeutralDriverStillOn:Reason::None;Last.Code=Ok;}
 }
 void Read(const SleepStatus&s){Last.ObservedKnown=true;Last.ObservedOn=s.bLowLatencyMode!=0;Last.ObservedFgMultiplier=s.fgMultiplier;Last.ObservedDfgControl=s.bDfgControl!=0;}
 bool CloseApi(bool retry=false){
  if(!Opened)return true;if(CloseAttempted&&!retry)return false;
  CloseAttempted=true;const auto code=Driver.Close();
  if(code==Ok){Opened=false;CloseAttempted=false;Last.ModuleCleanupPending=false;Pending();
   if(!Last.Owned&&Last.Why==Reason::CloseFailed){
    if(Last.NeutralAccepted)NeutralSummary();else{Last.Status=State::Off;Last.Why=Reason::None;Last.Code=Ok;}
   }
   return true;
  }
  Last.ModuleCleanupPending=true;CleanupFailure(code);
  if(!Last.Owned){Last.Status=State::Error;Last.Why=Reason::CloseFailed;Last.Code=code;}return false;
 }
 void Release(bool retry=false){
  if(!Last.Owned||(ReleaseAttempted&&!retry))return;ReleaseAttempted=true;
  if(Foreign){Last.Status=State::External;Last.Why=Reason::ForeignWriter;CleanupFailure(Error);return;}
  Last.Status=State::Releasing;
  const auto code=Driver.Set(Native,Request(false));Last.Code=code;
  if(code!=Ok){Last.Why=Reason::DisableFailed;CleanupFailure(code);return;}
  Last.Owned=false;ReleaseAttempted=false;Last.NeutralAccepted=true;
  SleepStatus status=EmptyStatus();const auto query=Driver.Get(Native,status);
  if(query!=Ok){Last.ReleaseVerificationUnknown=true;Last.Status=State::Releasing;Last.Why=Reason::ReleaseUnverified;Last.Code=query;Last.ObservedKnown=false;CleanupFailure(query);return;}
  Read(status);Last.ReleaseVerificationUnknown=false;NeutralSummary();Pending();
 }
public:
 explicit Policy(Api& driver):Driver(driver){}
 Policy(const Policy&)=delete;Policy&operator=(const Policy&)=delete;
 void Service(void* owner,void* native,uintptr_t window,bool requested,bool allowed,uint64_t now){
  const std::lock_guard guard(Lock);if(!owner||!native||!window)return;
  if(!Last.Owner){Last.Owner=owner;Native=native;Last.Window=window;Last.Generation=++NextGeneration;Last.Status=State::Off;Last.Why=Reason::None;Last.Code=Ok;Last.ObservedKnown=false;Last.NeutralAccepted=false;Last.ReleaseVerificationUnknown=false;Pending();}
  if(Last.Owner!=owner||Native!=native||Last.Window!=window)return;
  Last.Requested=requested;Last.Allowed=allowed;
  const bool desired=requested&&allowed,edge=desired!=PreviousDesired;PreviousDesired=desired;
  if(!desired){
   Attempted=false;
   if(Last.Owned)Release(edge); // Only a new explicit edge may retry, never idle frames.
   if(!Last.Owned){
    if(CloseApi(edge)){Pending();if(Last.NeutralAccepted)NeutralSummary();else if(!Last.CleanupPending){Last.Status=Last.ObservedKnown&&Last.ObservedOn?State::External:State::Off;Last.Why=Last.Status==State::External?Reason::PriorExternal:Reason::None;Last.Code=Ok;}}
   }
   return;
  }
  if(Last.Owned){
   if(Last.CleanupPending&&edge)Release(true);
   if(Last.Owned){
    if(!Foreign&&!ReleaseAttempted&&!Last.ModuleCleanupPending&&now>=LastQuery&&now-LastQuery>=1000){
     LastQuery=now;SleepStatus status=EmptyStatus();const auto code=Driver.Get(Native,status);Last.Code=code;
     if(code!=Ok){Last.Status=State::Error;Last.Why=Reason::QueryFailed;Last.ObservedKnown=false;return;}
     Read(status);if(OtherSleep(status)){Foreign=true;Last.Status=State::External;Last.Why=Reason::ForeignWriter;}
     else{Last.Status=status.bLowLatencyMode?State::On:State::Requested;Last.Why=status.bLowLatencyMode?Reason::None:Reason::DriverNotActive;}
    }
    return;
   }
  }
  if(Attempted)return;Attempted=true;
  if(Opened&&CloseAttempted&&!CloseApi(edge)){Last.Status=State::Error;Last.Why=Reason::CloseFailed;return;}
  if(!Opened){const auto code=Driver.Open();Opened=true;CloseAttempted=false;Last.Code=code;Last.OpenCode=code;
   if(code!=Ok){Last.Status=State::Unsupported;Last.Why=Reason::Unavailable;CloseApi();return;}
  }
  SleepStatus before=EmptyStatus();auto code=Driver.Get(Native,before);Last.Code=code;
  if(code!=Ok){Last.Status=State::Unsupported;Last.Why=Reason::Unavailable;Last.ObservedKnown=false;CloseApi();return;}
  Read(before);
  if(before.bLowLatencyMode||OtherSleep(before)){Last.Status=State::External;Last.Why=Reason::PriorExternal;CloseApi();return;}
  code=Driver.Set(Native,Request(true));Last.Code=code;
  if(code!=Ok){Last.Status=State::Error;Last.Why=Reason::SetFailed;CloseApi();return;}
  Last.Owned=true;ReleaseAttempted=false;Foreign=false;LastQuery=now;Last.NeutralAccepted=false;Last.ReleaseVerificationUnknown=false;Pending();
  SleepStatus after=EmptyStatus();code=Driver.Get(Native,after);Last.Code=code;
  if(code!=Ok){Last.Status=State::Error;Last.Why=Reason::QueryFailed;Last.ObservedKnown=false;return;}
  Read(after);if(OtherSleep(after)){Foreign=true;Last.Status=State::External;Last.Why=Reason::ForeignWriter;}
  else{Last.Status=after.bLowLatencyMode?State::On:State::Requested;Last.Why=after.bLowLatencyMode?Reason::None:Reason::DriverNotActive;}
 }
 void Destroy(void* owner){
  const std::lock_guard guard(Lock);if(!owner||owner!=Last.Owner)return;
  Release(true);const bool unconfirmed=Last.Owned||Last.ReleaseVerificationUnknown;
  if(unconfirmed){CleanupFailure(Last.Code!=Ok?Last.Code:Error);Last.Status=State::Error;Last.Why=Reason::DeviceGoneUnconfirmed;}
  CloseApi(true);Last.Owned=false;Last.Owner=nullptr;Last.Window=0;Native=nullptr;
  Attempted=false;ReleaseAttempted=false;Foreign=false;PreviousDesired=false;Last.ObservedKnown=false;
  if(!unconfirmed&&!Last.CleanupPending){Last.Status=State::Off;Last.Why=Reason::None;Last.Code=Ok;}
 }
 void Shutdown(){const std::lock_guard guard(Lock);Release(true);if(!Last.Owned){CloseApi(true);Pending();if(Last.NeutralAccepted&&!Last.ModuleCleanupPending)NeutralSummary();}}
 reflex::Snapshot GetSnapshot()const{const std::lock_guard guard(Lock);return Last;}
};
}
