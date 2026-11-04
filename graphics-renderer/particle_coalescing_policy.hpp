#pragma once
#include "particle_field_policy.hpp"
#include <limits>

// One bounded world region becomes one light before history/selection. Input
// fine cells are trilinear integration weights, not independent engine emitters.
namespace particle_coalescing_policy
{
namespace field=particle_field_policy;
constexpr double Width=4.0;
constexpr size_t MaximumCells=size_t(field::Triangles)*8;
inline bool Key(const DirectX::XMFLOAT3 &position,field::Key &key)
{
    if(!field::Finite(position))return false;
    const double value[]{std::floor(double(position.x)/Width),std::floor(double(position.y)/Width),std::floor(double(position.z)/Width)};
    for(double v:value)if(v<std::numeric_limits<int>::min()||v>std::numeric_limits<int>::max())return false;
    key={int(value[0]),int(value[1]),int(value[2])};return true;
}
inline bool Coalesce(std::vector<field::Cell> &cells)
{
    if(cells.size()>MaximumCells)return false;
    // Validate first: an invalid input never leaves partly re-keyed custody.
    for(const auto &cell:cells)
    {
        field::Key key;
        if(!field::Good(cell.power)||!std::isfinite(cell.weight)||cell.weight<0
            ||!std::isfinite(cell.mx)||!std::isfinite(cell.my)||!std::isfinite(cell.mz)
            ||!Key(field::Position(cell),key))return false;
    }
    for(auto &cell:cells){field::Key key;Key(field::Position(cell),key);cell.key=key;}
    // Collapse adds every RGB power and luminance first moment exactly once.
    // Position remains its energy-weighted centroid; native colour is not averaged.
    field::Collapse(cells);return true;
}
}
