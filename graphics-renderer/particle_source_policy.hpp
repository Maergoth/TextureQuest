#pragma once
#include "particle_field_policy.hpp"
#include <limits>
#include <unordered_map>

// These are inferred world sources, not native emitter identities. Current
// geometry owns current power exactly once. Missing sources retain the existing
// bounded cache policy: proximity to a different visible source cannot prove
// that a culled source was superseded.
namespace particle_source_policy
{
namespace field=particle_field_policy;
constexpr double AcquireRadius=4.0,RetainRadius=6.0,OwnershipHysteresis=.75,PoseSeconds=.20;
constexpr size_t MaximumSamples=field::Triangles,MaximumDormant=256;
struct Sample {DirectX::XMFLOAT3 position{};field::Power power{};};
struct Hash
{
    size_t operator()(const field::Key &k)const noexcept
    {
        uint64_t h=uint32_t(k.x);h=(h^uint32_t(k.y))*1099511628211ull;
        return size_t((h^uint32_t(k.z))*1099511628211ull);
    }
};
inline double DistanceSquared(const DirectX::XMFLOAT3&a,const DirectX::XMFLOAT3&b)
{const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;return x*x+y*y+z*z;}
inline bool SpatialKey(const DirectX::XMFLOAT3 &p,double width,field::Key &key)
{
    if(!field::Finite(p))return false;
    const double v[]{std::floor(double(p.x)/width),std::floor(double(p.y)/width),std::floor(double(p.z)/width)};
    for(double n:v)if(n<std::numeric_limits<int>::min()+2.0||n>std::numeric_limits<int>::max()-2.0)return false;
    key={int(v[0]),int(v[1]),int(v[2])};return true;
}
struct Group
{
    field::Key key{};DirectX::XMFLOAT3 anchor{},emitted{};
    double observed=0,poseTime=0;
    float coreTarget=0,coreEmitted=0;double centroidRms=0;size_t positiveSamples=0;
};
struct CoreSummary
{size_t groups=0,positiveSamples=0;double maximumRms=0;float maximumTarget=0,maximumEmitted=0;};
struct Owner {field::Key spatial{},source{};double weight=0;};
struct Prepared
{
    std::vector<field::Cell> cells;std::vector<Group> groups;std::vector<Owner> owners;
    std::vector<field::Key> superseded;
    uint64_t epoch=0,nextId=0;double observed=-1;size_t comparisons=0;
};
class Groups
{
    std::vector<Group> groups_;
    std::vector<Owner> owners_;
    uint64_t epoch_=0,nextId_=0;
    double lastObservation_=-1;
    static field::Key Identity(uint64_t id)
    {return {int((id>>31)&0x7fffffffull),int(id&0x7fffffffull),std::numeric_limits<int>::min()};}
public:
    void Clear(){groups_.clear();owners_.clear();epoch_=nextId_=0;lastObservation_=-1;}
    size_t Size()const{return groups_.size();}
    size_t OwnerCount()const{return owners_.size();}
    // Explicit capture diagnostics only; no observation/pose clock is advanced.
    CoreSummary SummarizeCore()const
    {
        CoreSummary result;result.groups=groups_.size();
        for(const auto&g:groups_)
        {
            result.positiveSamples+=g.positiveSamples;result.maximumRms=std::max(result.maximumRms,g.centroidRms);
            result.maximumTarget=std::max(result.maximumTarget,g.coreTarget);result.maximumEmitted=std::max(result.maximumEmitted,g.coreEmitted);
        }
        return result;
    }
    // Prepare is transactional. Failed native packets cannot change identity,
    // ownership, observation time or a previously committed emitted pose.
    bool Prepare(std::vector<Sample> samples,const std::vector<field::Key>&held,uint64_t epoch,
        double observed,double now,Prepared &out,const std::vector<field::Key>&superseded={})const
    {
        if(!epoch||samples.size()>MaximumSamples||held.size()>MaximumDormant
            ||!std::isfinite(observed)||!std::isfinite(now)||observed<0||now<observed
            ||now-observed>field::CaptureSeconds||(epoch_==epoch&&observed<lastObservation_))return false;
        for(size_t i=1;i<held.size();++i)if(!(held[i-1]<held[i]))return false;
        for(size_t i=1;i<superseded.size();++i)if(!(superseded[i-1]<superseded[i]))return false;
        for(const auto&s:samples)
        {
            field::Key unused;
            if(!field::Good(s.power)||!SpatialKey(s.position,field::CellWidth,unused)
                ||std::abs(double(s.position.x))>20000000||std::abs(double(s.position.y))>20000000||std::abs(double(s.position.z))>20000000
                ||std::max({s.power.x,s.power.y,s.power.z})>1e20)return false;
        }
        std::sort(samples.begin(),samples.end(),[](const Sample&a,const Sample&b){
            return std::tie(a.position.x,a.position.y,a.position.z,a.power.x,a.power.y,a.power.z)
                <std::tie(b.position.x,b.position.y,b.position.z,b.power.x,b.power.y,b.power.z);});
        Prepared next;next.epoch=epoch;next.nextId=epoch_==epoch?nextId_:0;next.observed=observed;next.superseded=superseded;
        const bool sameEpoch=epoch_==epoch;
        struct Work {Group group;field::Cell cell;double gx=0,gy=0,gz=0,spreadMoment=0;size_t count=0,positiveSamples=0;bool newborn=false;};
        std::vector<Work> work;work.reserve((sameEpoch?groups_.size():0)+samples.size());
        std::unordered_map<field::Key,std::vector<size_t>,Hash> index;
        index.reserve((sameEpoch?groups_.size():0)+samples.size());
        const auto insert=[&](Group group,bool newborn=false){
            const size_t n=work.size();Work entry;entry.group=group;entry.cell.key=group.key;entry.newborn=newborn;work.push_back(entry);
            field::Key key;SpatialKey(group.anchor,RetainRadius,key);index[key].push_back(n);return n;};
        if(sameEpoch)for(const auto&g:groups_)
            if(now-g.observed<3.0&&now>=g.observed&&!std::binary_search(superseded.begin(),superseded.end(),g.key))insert(g);
        // Existing anchors may converge. Index their bucket contents with
        // balanced bounding trees too, rather than scanning every old source
        // for every triangle in an arbitrarily dense bucket. New anchors are
        // at least AcquireRadius apart during this preparation.
        struct Node {size_t entry=0,left=SIZE_MAX,right=SIZE_MAX;DirectX::XMFLOAT3 low{},high{};field::Key first{};};
        std::vector<Node> tree;tree.reserve(work.size());
        std::unordered_map<field::Key,size_t,Hash> roots;roots.reserve(index.size());
        const auto coordinate=[](const DirectX::XMFLOAT3&p,unsigned axis){return axis==0?p.x:(axis==1?p.y:p.z);};
        const auto build=[&](auto&&self,std::vector<size_t>&items,size_t begin,size_t end)->size_t
        {
            if(begin==end)return SIZE_MAX;
            auto low=work[items[begin]].group.anchor,high=low;auto first=work[items[begin]].group.key;
            for(size_t i=begin+1;i<end;++i)
            {
                const auto&p=work[items[i]].group.anchor;
                low={std::min(low.x,p.x),std::min(low.y,p.y),std::min(low.z,p.z)};
                high={std::max(high.x,p.x),std::max(high.y,p.y),std::max(high.z,p.z)};
                first=std::min(first,work[items[i]].group.key);
            }
            unsigned axis=0;if(high.y-low.y>high.x-low.x)axis=1;
            if(high.z-low.z>coordinate(high,axis)-coordinate(low,axis))axis=2;
            const size_t middle=begin+(end-begin)/2;
            std::nth_element(items.begin()+begin,items.begin()+middle,items.begin()+end,[&](size_t a,size_t b){
                const float x=coordinate(work[a].group.anchor,axis),y=coordinate(work[b].group.anchor,axis);
                return x!=y?x<y:work[a].group.key<work[b].group.key;});
            const size_t n=tree.size();tree.push_back({items[middle],SIZE_MAX,SIZE_MAX,low,high,first});
            const size_t left=self(self,items,begin,middle),right=self(self,items,middle+1,end);
            tree[n].left=left;tree[n].right=right;return n;
        };
        for(auto&bucket:index)roots.emplace(bucket.first,build(build,bucket.second,0,bucket.second.size()));
        index.clear();
        const auto boxDistance=[](const DirectX::XMFLOAT3&p,const Node&n){
            const double x=std::max({double(n.low.x)-p.x,0.0,double(p.x)-n.high.x});
            const double y=std::max({double(n.low.y)-p.y,0.0,double(p.y)-n.high.y});
            const double z=std::max({double(n.low.z)-p.z,0.0,double(p.z)-n.high.z});return x*x+y*y+z*z;};
        const auto search=[&](auto&&self,size_t node,const DirectX::XMFLOAT3&p,size_t&best,double&distance)->void
        {
            if(node==SIZE_MAX)return;
            const auto&n=tree[node];const double bound=boxDistance(p,n);
            if(bound>distance||(bound==distance&&best!=SIZE_MAX&&!(n.first<work[best].group.key)))return;
            ++next.comparisons;const double d=DistanceSquared(p,work[n.entry].group.anchor);
            if(d<distance||(d==distance&&(best==SIZE_MAX||work[n.entry].group.key<work[best].group.key)))
            {best=n.entry;distance=d;}
            size_t a=n.left,b=n.right;
            if(a==SIZE_MAX||(b!=SIZE_MAX&&(boxDistance(p,tree[b])<boxDistance(p,tree[a])
                ||(boxDistance(p,tree[b])==boxDistance(p,tree[a])&&tree[b].first<tree[a].first))))std::swap(a,b);
            self(self,a,p,best,distance);self(self,b,p,best,distance);
        };
        // Old group IDs are sorted; current births are monotonically appended.
        const auto sourceIndex=[&](const field::Key&key){
            const auto it=std::lower_bound(work.begin(),work.end(),key,[](const Work&w,const field::Key&k){return w.group.key<k;});
            return it!=work.end()&&it->group.key==key?size_t(it-work.begin()):SIZE_MAX;};
        std::vector<Owner> ownership;ownership.reserve(samples.size());
        DirectX::XMFLOAT3 previous{};size_t previousBest=SIZE_MAX;double previousDistance=0;bool havePrevious=false;
        for(const auto&s:samples)
        {
            field::Key bin,spatial;SpatialKey(s.position,RetainRadius,bin);SpatialKey(s.position,field::CellWidth,spatial);
            size_t best=SIZE_MAX;double bestDistance=AcquireRadius*AcquireRadius;
            if(havePrevious&&s.position.x==previous.x&&s.position.y==previous.y&&s.position.z==previous.z)
            {best=previousBest;bestDistance=previousDistance;}
            else for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
            {
                const field::Key key{bin.x+x,bin.y+y,bin.z+z};
                const auto root=roots.find(key);if(root!=roots.end())search(search,root->second,s.position,best,bestDistance);
                const auto found=index.find(key);if(found==index.end())continue;
                for(const size_t n:found->second)
                {
                    ++next.comparisons;const double distance=DistanceSquared(s.position,work[n].group.anchor);
                    if(distance<bestDistance||(distance==bestDistance&&(best==SIZE_MAX||work[n].group.key<work[best].group.key)))
                    {best=n;bestDistance=distance;}
                }
            }
            previous=s.position;previousBest=best;previousDistance=bestDistance;havePrevious=true;
            if(sameEpoch)
            {
                const auto old=std::lower_bound(owners_.begin(),owners_.end(),spatial,[](const Owner&o,const field::Key&k){return o.spatial<k;});
                if(old!=owners_.end()&&old->spatial==spatial)
                {
                    const size_t owner=sourceIndex(old->source);
                    if(owner!=SIZE_MAX)
                    {
                        const double distance=DistanceSquared(s.position,work[owner].group.anchor);
                        if(distance<=RetainRadius*RetainRadius&&(best==SIZE_MAX
                            ||std::sqrt(distance)<=std::sqrt(bestDistance)+OwnershipHysteresis))best=owner;
                    }
                }
            }
            if(best==SIZE_MAX)
            {
                if(next.nextId>=(uint64_t(1)<<62)-1)return false;
                Group born;born.key=Identity(++next.nextId);born.anchor=born.emitted=s.position;
                born.observed=observed;born.poseTime=now;best=insert(born,true);
                previousBest=best;previousDistance=0;
            }
            auto&w=work[best];const double weight=field::Luma(s.power);
            if(weight>0)
            {
                // Weighted Welford/merge increment about the DOUBLE raw mean,
                // avoiding subtraction of large absolute-world second moments.
                // Zero-power geometry never expands a measured source core.
                const double oldWeight=w.cell.weight;
                if(oldWeight>0)
                {
                    const double x=double(s.position.x)-w.cell.mx/oldWeight;
                    const double y=double(s.position.y)-w.cell.my/oldWeight;
                    const double z=double(s.position.z)-w.cell.mz/oldWeight;
                    w.spreadMoment+=oldWeight*weight/(oldWeight+weight)*(x*x+y*y+z*z);
                }
                ++w.positiveSamples;
            }
            field::Add(w.cell.power,s.power);
            w.cell.weight+=weight;w.cell.mx+=weight*s.position.x;w.cell.my+=weight*s.position.y;w.cell.mz+=weight*s.position.z;
            w.gx+=s.position.x;w.gy+=s.position.y;w.gz+=s.position.z;++w.count;
            ownership.push_back({spatial,w.group.key,weight});
        }
        next.cells.reserve(samples.size());next.groups.reserve(samples.size()+MaximumDormant);
        for(auto&w:work)
        {
            if(w.count)
            {
                w.group.anchor=w.cell.weight>0?field::Position(w.cell):DirectX::XMFLOAT3{
                    float(w.gx/w.count),float(w.gy/w.count),float(w.gz/w.count)};
                if(!std::isfinite(w.spreadMoment)||w.spreadMoment<0)return false;
                w.group.centroidRms=w.cell.weight>0?std::sqrt(w.spreadMoment/w.cell.weight):0;
                w.group.positiveSamples=w.positiveSamples;
                w.group.coreTarget=float(std::min(double(field::MaximumSourceCoreRadius),.5*w.group.centroidRms));
                if(w.newborn){w.group.emitted=w.group.anchor;w.group.coreEmitted=w.group.coreTarget;}
                w.group.observed=observed;w.cell.emittedPosition=w.group.emitted;w.cell.hasEmittedPosition=true;
                w.cell.sourceCoreRadius=w.group.coreTarget;w.cell.emittedSourceCoreRadius=w.group.coreEmitted;
                if(!field::Good(w.cell.power)||!field::Finite(w.group.anchor)||!field::Finite(w.group.emitted)
                    ||std::max({w.cell.power.x,w.cell.power.y,w.cell.power.z})>1e20)return false;
                next.cells.push_back(w.cell);next.groups.push_back(w.group);
            }
            else if(std::binary_search(held.begin(),held.end(),w.group.key))next.groups.push_back(w.group);
        }
        // One stable preference per occupied fine world bin. This is ownership
        // hysteresis only, never a world-bin source identity or energy key.
        std::sort(ownership.begin(),ownership.end(),[](const Owner&a,const Owner&b){
            return std::tie(a.spatial,a.source,a.weight)<std::tie(b.spatial,b.source,b.weight);});
        for(size_t i=0;i<ownership.size();)
        {
            const auto spatial=ownership[i].spatial;Owner winner;winner.spatial=spatial;double strongest=-1;
            while(i<ownership.size()&&ownership[i].spatial==spatial)
            {
                const auto source=ownership[i].source;double weight=0;size_t count=0;
                while(i<ownership.size()&&ownership[i].spatial==spatial&&ownership[i].source==source)
                {weight+=ownership[i++].weight;++count;}
                // Geometry still supplies an owner when an observed native
                // material is entirely dark; it must not retain old brightness.
                const double score=weight>0?weight:double(count)*1e-30;
                if(score>strongest){winner.source=source;winner.weight=weight;strongest=score;}
            }
            next.owners.push_back(winner);
        }
        out=std::move(next);return true;
    }
    void Commit(Prepared&&next)
    {groups_=std::move(next.groups);owners_=std::move(next.owners);epoch_=next.epoch;nextId_=next.nextId;lastObservation_=next.observed;}
    // Evaluation is presentation-time based, independent of packet cadence or
    // native RGB animation. Missing sources cannot refresh their observation.
    void Filter(std::vector<field::Cell>&cells,uint64_t epoch,double now)
    {
        if(epoch_!=epoch||!std::isfinite(now))return;
        for(auto&cell:cells)
        {
            const auto found=std::lower_bound(groups_.begin(),groups_.end(),cell.key,[](const Group&g,const field::Key&k){return g.key<k;});
            if(found==groups_.end()||found->key!=cell.key)continue;
            auto&g=*found;
            if(now>g.poseTime)
            {
                const double alpha=-std::expm1(-(now-g.poseTime)/PoseSeconds);
                g.emitted={float(g.emitted.x+alpha*(double(g.anchor.x)-g.emitted.x)),
                    float(g.emitted.y+alpha*(double(g.anchor.y)-g.emitted.y)),float(g.emitted.z+alpha*(double(g.anchor.z)-g.emitted.z))};
                g.coreEmitted=float(g.coreEmitted+alpha*(double(g.coreTarget)-g.coreEmitted));
                g.poseTime=now;
            }
            cell.emittedPosition=g.emitted;cell.hasEmittedPosition=true;
            cell.sourceCoreRadius=g.coreTarget;cell.emittedSourceCoreRadius=g.coreEmitted;
        }
    }
};
}
