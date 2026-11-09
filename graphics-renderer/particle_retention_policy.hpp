#pragma once
#include "particle_field_policy.hpp"

// Retain only actually published world observations. A held/selected light never
// refreshes its observation time, and stale energy is never added to fresh energy.
namespace particle_retention_policy
{
namespace field=particle_field_policy;
constexpr size_t Capacity=256;
constexpr double GraceSeconds=2.0,FadeSeconds=1.0;
inline double Visibility(double age)
{
    if(!std::isfinite(age)||age<0||age>=GraceSeconds+FadeSeconds)return 0;
    return std::min(1.0,(GraceSeconds+FadeSeconds-age)/FadeSeconds);
}
inline field::Cell Scaled(field::Cell value,double weight)
{
    value.power=field::Scale(value.power,weight);value.mx*=weight;value.my*=weight;value.mz*=weight;value.weight*=weight;
    return value;
}
class Cache
{
    struct Entry {field::Cell cell;double observed=0,priority=0;};
    std::vector<Entry> entries_;
    uint64_t epoch_=0;
    double lastObservation_=-1;
    static bool Better(const Entry&a,const Entry&b)
    {
        if(a.observed!=b.observed)return a.observed>b.observed;
        if(a.priority!=b.priority)return a.priority>b.priority;
        return a.cell.key<b.cell.key;
    }
    static double Priority(const field::Cell&c,const DirectX::XMFLOAT3&receiver)
    {
        const auto p=field::Position(c);const double x=double(p.x)-receiver.x,y=double(p.y)-receiver.y,z=double(p.z)-receiver.z;
        return field::Luma(c.power)/std::max(4.0,x*x+y*y+z*z);
    }
public:
    void Clear(){entries_.clear();epoch_=0;lastObservation_=-1;}
    size_t Size()const{return entries_.size();}
    std::vector<field::Key> Keys(uint64_t epoch,double now)const
    {
        std::vector<field::Key> result;
        if(epoch_==epoch)for(const auto&e:entries_)if(Visibility(now-e.observed)>0)result.push_back(e.cell.key);
        return result;
    }
    // Incoming cells are the sorted unique result of one proven native packet.
    bool Observe(const std::vector<field::Cell>&cells,uint64_t epoch,double observed,double now,const DirectX::XMFLOAT3&receiver,
        const std::vector<field::Key>&superseded={})
    {
        if(!epoch||!std::isfinite(observed)||!std::isfinite(now)||observed<0||now<observed
            ||now-observed>field::CaptureSeconds||!field::Finite(receiver))return false;
        if(epoch_==epoch&&observed<lastObservation_)return false;
        for(size_t i=1;i<superseded.size();++i)if(!(superseded[i-1]<superseded[i]))return false;
        for(size_t i=0;i<cells.size();++i)
            if(!field::Good(cells[i].power)||!field::Finite(field::Position(cells[i]))
                ||!field::Finite(field::LightPosition(cells[i]))||!std::isfinite(cells[i].weight)||cells[i].weight<0
                ||(i&&!(cells[i-1].key<cells[i].key)))return false;
        if(epoch_!=epoch){Clear();epoch_=epoch;}
        std::vector<Entry> next;next.reserve(Capacity);
        const auto offer=[&](Entry entry){
            entry.priority=Priority(entry.cell,receiver);
            if(next.size()<Capacity){next.push_back(entry);std::push_heap(next.begin(),next.end(),Better);}
            else if(Better(entry,next.front())){std::pop_heap(next.begin(),next.end(),Better);next.back()=entry;std::push_heap(next.begin(),next.end(),Better);}
        };
        for(const auto&entry:entries_)
        {
            // Only an explicit, proven identity retirement may bypass expiry.
            // A nearby different visible source is never sufficient evidence.
            if(std::binary_search(superseded.begin(),superseded.end(),entry.cell.key))continue;
            const auto fresh=std::lower_bound(cells.begin(),cells.end(),entry.cell.key,[](const field::Cell&c,const field::Key&k){return c.key<k;});
            if((fresh==cells.end()||fresh->key!=entry.cell.key)&&Visibility(now-entry.observed)>0)offer(entry);
        }
        for(const auto&cell:cells)if(field::Luma(cell.power)>0)offer({cell,observed,0});
        std::sort(next.begin(),next.end(),[](const Entry&a,const Entry&b){return a.cell.key<b.cell.key;});
        entries_=std::move(next);lastObservation_=observed;return true;
    }
    double Strongest(uint64_t epoch,double now)const
    {
        if(epoch_!=epoch)return 0;double result=0;
        for(const auto&e:entries_)result=std::max(result,Visibility(now-e.observed));return result;
    }
    std::vector<field::Cell> Merge(const std::vector<field::Cell>&native,double nativeVisibility,uint64_t epoch,double now)const
    {
        std::vector<field::Cell> held;
        if(epoch_==epoch)for(const auto&e:entries_){const double v=Visibility(now-e.observed);if(v>0)held.push_back(Scaled(e.cell,v));}
        std::vector<field::Cell> result;result.reserve(held.size()+(nativeVisibility>0?native.size():0));
        size_t a=0,b=0;
        while(a<held.size()||b<native.size())
        {
            if(a<held.size()&&(b==native.size()||held[a].key<native[b].key))result.push_back(held[a++]);
            else if(b<native.size()&&(a==held.size()||native[b].key<held[a].key))
            {if(nativeVisibility>0)result.push_back(Scaled(native[b],nativeVisibility));++b;}
            else {result.push_back(held[a++]);++b;}
        }
        return result;
    }
};
}
