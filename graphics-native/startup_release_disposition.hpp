#pragma once
// Pure shared phase policy. No process, IO, allocation or cleanup operations.
namespace startup {
struct FailureDisposition {bool keep_parent=false,disable_commands=false,suspend_primary=false;};
class ReleaseDisposition {
 bool attempted_=false;
public:
 bool Attempted()const noexcept{return attempted_;}
 bool PossiblyReleased(bool confirmed)const noexcept{return attempted_||confirmed;}
 // Mark possible release BEFORE invoking any writer. Never invoke it twice.
 // A reported failure/short write/exception proves nothing about target bytes.
 template<class Writer>bool AttemptWord(Writer writer)noexcept{
  if(attempted_)return false;attempted_=true;
  try{writer();return true;}catch(...){return false;}
 }
 FailureDisposition OnFailure(bool owns_child,bool owns_primary,bool resumed,bool confirmed)const noexcept{
  if(!owns_child)return {};
  if(PossiblyReleased(confirmed))return {true,true,false};
  return {false,false,owns_primary&&resumed};
 }
};
}
