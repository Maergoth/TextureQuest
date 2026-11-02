#pragma once
#include <array>
#include <cstdint>
#include <limits>

// CPU metadata only. The bridge mutex protects every operation and remains
// held from a consumer selection until its sampling list/fence are submitted.
// Fence completion masks are per-slot, never one cross-queue global timeline.
namespace tq_overlay_ring {
class State {
public:
    static constexpr unsigned Count=3, None=Count, All=(1u<<Count)-1;
    struct Slot {std::uint64_t Produced=0,Consumed=0,Publication=0;};
private:
    std::array<Slot,Count> slots_{};
    std::uint64_t publication_=0;
    unsigned writer_=None;
public:
    const Slot& Get(unsigned slot)const noexcept{return slots_[slot];}
    unsigned Writer()const noexcept{return writer_;}
    void Reset()noexcept{slots_={};publication_=0;writer_=None;}
    unsigned Latest(unsigned ready)const noexcept{
        unsigned result=None;std::uint64_t newest=0;
        for(unsigned i=0;i<Count;++i)
            if((ready&(1u<<i))&&slots_[i].Publication>newest){newest=slots_[i].Publication;result=i;}
        return result;
    }
    unsigned ReserveProducer(unsigned producer_done,unsigned consumer_done)noexcept{
        if(writer_!=None||publication_==std::numeric_limits<std::uint64_t>::max())return None;
        const unsigned latest=Latest(All),readable=Latest(producer_done);
        for(unsigned i=0;i<Count;++i)
            if(i!=latest&&i!=readable&&(producer_done&consumer_done&(1u<<i))){writer_=i;return i;}
        return None;
    }
    std::uint64_t NextProducer()const noexcept{
        return writer_<Count&&slots_[writer_].Produced!=std::numeric_limits<std::uint64_t>::max()?slots_[writer_].Produced+1:0;
    }
    bool PublishProducer(std::uint64_t value)noexcept{
        if(!value||value!=NextProducer())return false;
        auto&slot=slots_[writer_];slot.Produced=value;slot.Publication=++publication_;writer_=None;return true;
    }
    void CancelProducer()noexcept{writer_=None;}
    std::uint64_t NextConsumer(unsigned slot)const noexcept{
        return slot<Count&&slots_[slot].Consumed!=std::numeric_limits<std::uint64_t>::max()?slots_[slot].Consumed+1:0;
    }
    bool SubmitConsumer(unsigned slot,std::uint64_t value)noexcept{
        if(!value||value!=NextConsumer(slot))return false;
        slots_[slot].Consumed=value;return true;
    }
};
}
