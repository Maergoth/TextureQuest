#pragma once
#include <atomic>
#include <cstdint>
// A changed epoch immediately stops admission. Only the owning Present callback
// may admit it, after all admitted CPU jobs and submitted GPU fences retire.
class TextureQuestAioGate {
    std::atomic<std::uint64_t> control_{2}, admitted_{0};
    std::atomic<std::uint32_t> active_{0};
public:
    void Request(bool enabled) {
        auto old=control_.load(std::memory_order_acquire);
        while (bool(old&1)!=enabled && !control_.compare_exchange_weak(old,
            (((old>>1)+1)<<1)|std::uint64_t(enabled), std::memory_order_acq_rel)) {}
    }
    void Invalidate() { auto old=control_.load(std::memory_order_acquire); while(!control_.compare_exchange_weak(old,(((old>>1)+1)<<1)|(old&1),std::memory_order_acq_rel)){} }
    std::uint64_t Token() const { return control_.load(std::memory_order_acquire); }
    bool Effective() const { const auto t=Token(); return (t&1) && admitted_.load(std::memory_order_acquire)==t; }
    std::uint32_t Active() const { return active_.load(std::memory_order_acquire); }
    bool Admit(std::uint64_t expected) {
        if (Active()!=0 || Token()!=expected) return false;
        admitted_.store(expected,std::memory_order_release);
        return Token()==expected;
    }
    bool Enter() {
        const auto t=Token();
        if (!(t&1) || admitted_.load(std::memory_order_acquire)!=t) return false;
        active_.fetch_add(1,std::memory_order_acq_rel);
        if (Token()==t && admitted_.load(std::memory_order_acquire)==t) return true;
        active_.fetch_sub(1,std::memory_order_release); return false;
    }
    void Leave() { active_.fetch_sub(1,std::memory_order_release); }
};
class TextureQuestAioActivity {
    TextureQuestAioGate &gate_; bool entered_;
public:
    explicit TextureQuestAioActivity(TextureQuestAioGate &g):gate_(g),entered_(g.Enter()){}
    ~TextureQuestAioActivity(){if(entered_)gate_.Leave();}
    explicit operator bool() const{return entered_;}
    TextureQuestAioActivity(const TextureQuestAioActivity&)=delete;
};
