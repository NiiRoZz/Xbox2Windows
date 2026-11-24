// Checks src/startup_stubs/small_texture_4k.h without a device: the 4 KiB answers for small 2D textures
// asked with a 4 KiB alignment, and the descriptions it leaves alone.
//
//   startup_small_texture_4k_check                            literal sizes and refused descriptions
//   startup_small_texture_4k_check --mutate whole-width-tail  expectations of a rule whose mip tail is a
//                                                             whole block wide (negative control: fails)
//   startup_small_texture_4k_check --addrlib                  every accepted shape against AMD addrlib's
//                                                             GFX10 4 KiB layout; 77 when built without it
//
// The literal sizes are worked by hand from the 4 KiB blocks (128x64 texels at 4 bits, 64x64 at 8, 64x32 at
// 16, 32x32 at 32, 32x16 at 64, 16x16 at 128) and the tail rule; the --addrlib run reproduces all of them.
// addrlib: Mesa 25.1.0 (5c142e46f3f6e752ed745fd48912ebb8fad67145) src/amd/addrlib, MIT, fetched by
// tools/fetch_addrlib.py; nothing is vendored.
#include "startup_stubs/small_texture_4k.h"

#include <cstdio>
#include <cstring>

namespace {
int failures;

D3D12_RESOURCE_DESC texture(unsigned format, unsigned width, unsigned height, unsigned mips = 1, UINT64 alignment = 0x1000)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = alignment;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(mips);
    desc.Format = static_cast<DXGI_FORMAT>(format);
    desc.SampleDesc.Count = 1;
    return desc;
}

const D3D12_RESOURCE_ALLOCATION_INFO desktop{0x10000, 0x10000};

void answered(const char* name, const D3D12_RESOURCE_DESC& desc, unsigned long long expected)
{
    D3D12_RESOURCE_ALLOCATION_INFO info = desktop;
    const bool changed = x2w_startup::small_texture_4k(desc, info);
    if (!changed || info.SizeInBytes != expected || info.Alignment != 0x1000) {
        std::printf("FAIL %s: changed %d size 0x%llx alignment 0x%llx, expected 0x%llx at 0x1000\n", name, changed,
                    static_cast<unsigned long long>(info.SizeInBytes), static_cast<unsigned long long>(info.Alignment), expected);
        ++failures;
    }
}

void kept(const char* name, const D3D12_RESOURCE_DESC& desc)
{
    D3D12_RESOURCE_ALLOCATION_INFO info = desktop;
    if (x2w_startup::small_texture_4k(desc, info) || info.SizeInBytes != desktop.SizeInBytes || info.Alignment != desktop.Alignment) {
        std::printf("FAIL %s: the desktop answer was changed to 0x%llx at 0x%llx\n", name,
                    static_cast<unsigned long long>(info.SizeInBytes), static_cast<unsigned long long>(info.Alignment));
        ++failures;
    }
}

int literal(bool whole_width_tail)
{
    // whole_width_tail: what a tail one block wide (instead of half) would answer.
    const auto rule = [whole_width_tail](unsigned long long real, unsigned long long mutated) { return whole_width_tail ? mutated : real; };
    answered("32x16 R8G8B8A8", texture(28, 32, 16), 0x1000);
    answered("128x4 R16G16B16A16 (desktop at 4 KiB answers 0x2000)", texture(11, 128, 4), 0x4000);
    answered("4x4 BC1", texture(71, 4, 4), 0x1000);
    answered("128x128 R16 float", texture(54, 128, 128), 0x8000);
    answered("256x256 BC7, the 64 KiB bound", texture(98, 256, 256), 0x10000);
    answered("16x16 R32G32B32A32 float", texture(2, 16, 16), 0x1000);
    answered("128x128 BC1 6 mips: 2 blocks + tail", texture(71, 128, 128, 6), 0x3000);
    answered("64x128 BC1 5 mips: 2 blocks + tail", texture(71, 64, 128, 5), 0x3000);
    answered("64x64 BC7 5 mips: 1 block + tail", texture(98, 64, 64, 5), rule(0x2000, 0x1000));
    answered("128x64 BC7 5 mips: 2 + 1 + tail", texture(98, 128, 64, 5), rule(0x4000, 0x3000));
    answered("256x64 BC1 5 mips: 2 + 1 + tail", texture(71, 256, 64, 5), rule(0x4000, 0x3000));
    answered("128x32 BC1 4 mips: 1 + tail", texture(71, 128, 32, 4), rule(0x2000, 0x1000));
    answered("128x128 R8G8B8A8 8 mips: 16 + 4 + 1 + tail", texture(28, 128, 128, 8), rule(0x16000, 0x15000));
    answered("1024x1 R8 full chain (MipLevels 0): 16+8+4+2+1 + tail", texture(61, 1024, 1, 0), 0x20000);

    // Refused: the desktop answer stays.
    kept("alignment 0", texture(28, 32, 16, 1, 0));
    kept("alignment 64 KiB", texture(28, 32, 16, 1, 0x10000));
    D3D12_RESOURCE_DESC d = texture(28, 32, 16);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    kept("render target", d);
    d = texture(40, 32, 16);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    kept("depth target", d);
    kept("256x128 R8G8B8A8, over the 64 KiB bound", texture(28, 256, 128));
    kept("512x256 BC7, over the bound", texture(98, 512, 256));
    d = texture(28, 32, 16);
    d.DepthOrArraySize = 2;
    kept("array of 2", d);
    kept("48x16 R8G8B8A8, width not a power of two", texture(28, 48, 16));
    kept("R32G32B32 float, 96 bits", texture(6, 32, 16));
    kept("32x16 with 7 mips, more than the chain", texture(28, 32, 16, 7));
    d = texture(28, 32, 16);
    d.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    kept("64 KiB layout", d);
    d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x102);
    kept("console layout code", d);
    d = texture(28, 32, 16);
    d.SampleDesc.Count = 4;
    kept("4 samples", d);
    d = texture(28, 32, 16);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    kept("3D texture", d);
    if (failures) {
        std::printf("startup_small_texture_4k: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("startup_small_texture_4k: PASS (14 sizes, 14 refused descriptions)\n");
    return 0;
}
}  // namespace

#ifndef XBOX2WINDOWS_HAVE_ADDRLIB
int addrlib_check()
{
    std::puts("startup_small_texture_4k --addrlib: SKIP (built without addrlib; run tools/fetch_addrlib.py and configure with "
              "-DXBOX2WINDOWS_ADDRLIB_DIR=<mesa>/src/amd/addrlib)");
    return 77;
}
#else
#include <cstdlib>

#include "addrinterface.h"

namespace {
VOID* ADDR_API alloc_sys_mem(const ADDR_ALLOCSYSMEM_INPUT* in) { return std::malloc(in->sizeInBytes); }
ADDR_E_RETURNCODE ADDR_API free_sys_mem(const ADDR_FREESYSMEM_INPUT* in)
{
    std::free(in->pVirtAddr);
    return ADDR_OK;
}
}  // namespace

// Every power-of-two shape and mip count the helper accepts, for one format of each bit depth, against
// Addr2ComputeSurfaceInfo with ADDR_SW_4KB_S on Mesa's "navi21" (gfx10.3) configuration, the one
// xg_swizzle_select_check uses: family 0x8F, revision 0x28, GB_ADDR_CONFIG 0x444.
int addrlib_check()
{
    ADDR_CREATE_INPUT create{};
    ADDR_CREATE_OUTPUT created{};
    create.size = sizeof create;
    created.size = sizeof created;
    create.chipEngine = 0x0000000D;  // CIASICIDGFXENGINE_ARCTICISLAND
    create.chipFamily = 0x8F;
    create.chipRevision = 0x28;
    create.callbacks.allocSysMem = alloc_sys_mem;
    create.callbacks.freeSysMem = free_sys_mem;
    create.regValue.gbAddrConfig = 0x40 | 4u | (4u << 8);
    if (AddrCreate(&create, &created) != ADDR_OK) return 1;
    struct { unsigned dxgi; AddrFormat addr; const char* name; } formats[] = {
        {71, ADDR_FMT_BC1, "BC1"}, {98, ADDR_FMT_BC7, "BC7"}, {61, ADDR_FMT_8, "R8"}, {54, ADDR_FMT_16, "R16"},
        {28, ADDR_FMT_8_8_8_8, "R8G8B8A8"}, {11, ADDR_FMT_16_16_16_16, "R16G16B16A16"}, {2, ADDR_FMT_32_32_32_32, "R32G32B32A32"}};
    unsigned compared = 0, mismatches = 0;
    for (const auto& f : formats)
        for (unsigned w = 1; w <= 4096; w <<= 1)
            for (unsigned h = 1; h <= 4096; h <<= 1)
                for (unsigned mips = 1; mips <= 13; ++mips) {
                    D3D12_RESOURCE_ALLOCATION_INFO info = desktop;
                    if (!x2w_startup::small_texture_4k(texture(f.dxgi, w, h, mips), info)) continue;
                    ADDR2_COMPUTE_SURFACE_INFO_INPUT in{};
                    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out{};
                    in.size = sizeof in;
                    out.size = sizeof out;
                    in.flags.texture = 1;
                    in.swizzleMode = ADDR_SW_4KB_S;
                    in.resourceType = ADDR_RSRC_TEX_2D;
                    in.format = f.addr;
                    in.width = w;
                    in.height = h;
                    in.numSlices = 1;
                    in.numMipLevels = mips;
                    in.numSamples = 1;
                    in.numFrags = 1;
                    ++compared;
                    if (Addr2ComputeSurfaceInfo(created.hLib, &in, &out) != ADDR_OK || out.surfSize != info.SizeInBytes ||
                        out.baseAlign != info.Alignment) {
                        if (++mismatches <= 12)
                            std::printf("FAIL %s %ux%u %u mips: helper 0x%llx at 0x%llx, addrlib 0x%llx at 0x%x\n", f.name, w, h, mips,
                                        static_cast<unsigned long long>(info.SizeInBytes),
                                        static_cast<unsigned long long>(info.Alignment),
                                        static_cast<unsigned long long>(out.surfSize), out.baseAlign);
                    }
                }
    AddrDestroy(created.hLib);
    std::printf("startup_small_texture_4k --addrlib: %u shapes compared, %u mismatches (Mesa 25.1.0 addrlib, SW_4KB_S)\n", compared,
                mismatches);
    return compared && !mismatches ? 0 : 1;
}
#endif

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--addrlib") == 0) return addrlib_check();
    const bool mutate = argc > 2 && std::strcmp(argv[1], "--mutate") == 0 && std::strcmp(argv[2], "whole-width-tail") == 0;
    if (argc > 1 && !mutate) return 2;
    return literal(mutate);
}
