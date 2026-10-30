#pragma once
#include "startup_protocol.hpp"
struct ProxyProfileWire {
 uint32_t bytes=sizeof(ProxyProfileWire),pid=0;uint64_t birth=0;char nonce[33]{};
 wchar_t base[32768]{},game[32768]{},session[32768]{};char base_ini[65]{},game_ini[65]{};
};
inline bool ProxyCopy(void*to,const void*from,size_t n)noexcept{__try{memcpy(to,from,n);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
