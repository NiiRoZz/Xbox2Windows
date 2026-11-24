// Console allocation answer for a small 2D texture asked with a 4 KiB alignment (docs/startup-runtime.md).
//
// A console executable asks the allocation-info entries with Alignment 0x1000 for a texture that is not a
// render or depth target and whose top level, padded to 4 KiB blocks, holds at most 0x80007 bits over all
// array slices. It keeps the answer only when the returned alignment is 0x1000 and otherwise asks again
// with alignment 0. Desktop D3D12 answers such a texture with 64 KiB; the console sizes it in 4 KiB blocks.
//
// The size is the GFX10 4 KiB thin-block layout as AMD addrlib computes it (Gfx10Lib, Mesa 25.1.0): each
// level is padded to whole blocks until the first level that fits a tail of half a block's width and a
// block's height while at most 8 levels remain; that level and every smaller one share one block.
//
// Scope, any other description keeps the desktop answer: a 2D texture with layout UNKNOWN, Alignment
// 0x1000, one sample, one array slice, power-of-two width and height (so block-compressed mip sizes are the
// same in texels and in blocks), MipLevels 0 (full chain) to the full chain, no render-target or depth
// flag, a format whose bits per texel are 4, 8, 16, 32, 64 or 128 (small_texture_4k_bits), and the top
// level within the executable's 0x80007-bit bound.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace x2w_startup {
namespace small_texture_4k_detail {
// 4 KiB thin block in texels for a bit depth; {0, 0} for other depths. Block-compressed formats count
// their 4x4 texel blocks (8 or 16 bytes) as elements.
struct Block { std::uint64_t width, height; };
inline Block block(unsigned bits)
{
    switch (bits) {
    case 4: return {128, 64};    // 8-byte blocks, 32 x 16 elements
    case 8: return {64, 64};     // 8-bit texels, or 16-byte blocks at 16 x 16 elements
    case 16: return {64, 32};
    case 32: return {32, 32};
    case 64: return {32, 16};
    case 128: return {16, 16};
    default: return {0, 0};
    }
}
inline bool power_of_two(std::uint64_t v) { return v && !(v & (v - 1)); }
}  // namespace small_texture_4k_detail

// Bits per texel of the formats answered; 0 for every other format.
inline unsigned small_texture_4k_bits(unsigned format)
{
    const unsigned f = format;
    if (f >= 1 && f <= 4) return 128;                                        // R32G32B32A32
    if (f >= 9 && f <= 22) return 64;                                        // R16G16B16A16, R32G32, R32G8X24
    if ((f >= 23 && f <= 47) || f == 67 || (f >= 87 && f <= 93)) return 32;  // R10G10B10A2 .. R24G8, R9G9B9E5, B8G8R8A8
    if ((f >= 48 && f <= 59) || f == 85 || f == 86 || f == 115) return 16;   // R8G8, R16, B5G6R5, B5G5R5A1, B4G4R4A4
    if (f >= 60 && f <= 65) return 8;                                        // R8, A8
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) return 4;              // BC1, BC4
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) return 8;  // BC2, BC3, BC5, BC6H, BC7
    return 0;
}

// Bytes of the 4 KiB-block layout of a single-slice 2D texture (no scope checks).
inline std::uint64_t small_texture_4k_bytes(std::uint64_t width, std::uint64_t height, unsigned mips, unsigned bits)
{
    const auto b = small_texture_4k_detail::block(bits);
    std::uint64_t blocks = 0;
    for (unsigned m = 0; m < mips; ++m) {
        const std::uint64_t w = (width >> m) ? (width >> m) : 1, h = (height >> m) ? (height >> m) : 1;
        if (mips > 1 && w <= b.width / 2 && h <= b.height && mips - m <= 8) return (blocks + 1) * 0x1000;
        blocks += (w + b.width - 1) / b.width * ((h + b.height - 1) / b.height);
    }
    return blocks * 0x1000;
}

// Replaces info with the console answer and returns true for a description in scope; false leaves it.
inline bool small_texture_4k(const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_ALLOCATION_INFO& info)
{
    using namespace small_texture_4k_detail;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Alignment != 0x1000 || d.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
        d.SampleDesc.Count != 1 || d.DepthOrArraySize != 1 || !power_of_two(d.Width) || !power_of_two(d.Height))
        return false;
    if (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) return false;
    const unsigned bits = small_texture_4k_bits(d.Format);
    const Block b = block(bits);
    if (!b.width) return false;
    unsigned full = 0;
    for (std::uint64_t e = d.Width > d.Height ? d.Width : d.Height; e; e >>= 1) ++full;
    const unsigned mips = d.MipLevels ? d.MipLevels : full;
    if (mips > full) return false;
    // The executable's bound: the top level padded to whole blocks, in bits, at most 0x80007.
    const std::uint64_t padded = (d.Width + b.width - 1) / b.width * b.width * ((d.Height + b.height - 1) / b.height * b.height);
    if (padded * bits > 0x80007) return false;
    info = {small_texture_4k_bytes(d.Width, d.Height, mips, bits), 0x1000};
    return true;
}

}  // namespace x2w_startup
