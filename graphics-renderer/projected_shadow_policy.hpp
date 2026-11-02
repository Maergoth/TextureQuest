#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <DirectXMath.h>

namespace projected_shadow_policy
{
constexpr unsigned Count = 4, Size = 1024;
inline bool AdmittedVertex(uint64_t hash) { return hash == 0x851068275842e7b8ull; }
struct Direction { float x = 0, y = 0, z = 0; };
inline bool Normalize(float x, float y, float z, float w, Direction &out)
{
    const float length = x*x + y*y + z*z;
    if (!std::isfinite(length) || !std::isfinite(w) || std::abs(w) > 1e-6f || length < .0625f || length > 16)
        return false;
    const float scale = 1 / std::sqrt(length);
    out = {x * scale, y * scale, z * scale};
    return true;
}
inline bool Same(Direction a, Direction b)
{
    return a.x*b.x + a.y*b.y + a.z*b.z > .999999f;
}
struct Slot
{
    Direction direction;
    uint64_t seen = 0, built = 0;
    bool valid = false;
};
struct Slots
{
    std::array<Slot, Count> values{};
    int Observe(uint64_t frame, Direction direction)
    {
        if (!frame) return -1;
        for (unsigned i = 0; i < Count; ++i)
            if (values[i].seen && Same(values[i].direction, direction))
            { values[i].seen = frame; return static_cast<int>(i); }
        for (unsigned i = 0; i < Count; ++i)
            if (!values[i].seen || (frame > values[i].seen && frame - values[i].seen > 2))
            { values[i] = {direction, frame, 0, false}; return static_cast<int>(i); }
        return -1;
    }
    bool Ready(unsigned slot, uint64_t frame) const
    {
        return slot < Count && frame && values[slot].valid && values[slot].seen == frame
            && values[slot].built == frame - 1;
    }
    void Invalidate() { for (auto &value : values) value.valid = false; }
};
// the half-width of the square around the camera each map covers
constexpr float Extent = 48;
inline bool MakeRows(Direction d, float x, float y, float z, float extent, DirectX::XMFLOAT4X4 &rows)
{
    using namespace DirectX;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(extent) || extent < 12 || extent > 48)
        return false;
    const XMVECTOR direction=XMVectorSet(d.x,d.y,d.z,0),center=XMVectorSet(x,y,z,1);
    const XMVECTOR eye=XMVectorAdd(center,XMVectorScale(direction,extent));
    const XMVECTOR up=std::abs(d.y)<.95f?XMVectorSet(0,1,0,0):XMVectorSet(1,0,0,0);
    const XMMATRIX matrix=XMMatrixMultiply(XMMatrixLookToLH(eye,XMVectorNegate(direction),up),
        XMMatrixOrthographicLH(2*extent,2*extent,.05f,2*extent));
    XMStoreFloat4x4(&rows,XMMatrixTranspose(matrix));
    return true;
}
}
