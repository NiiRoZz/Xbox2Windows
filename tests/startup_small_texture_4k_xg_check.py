"""XG slots 2 and 8 for a small texture given the 4 KiB allocation answer (src/startup_stubs/small_texture_4k.h).

A 32x32 R8G8B8A8 texture with layout UNKNOWN gets 0x1000 bytes at 4 KiB alignment from the allocation-info
entries, while its desktop linear footprint is 0x1f80 bytes (32 rows at a 256-byte pitch). Slot 2 must
report 0x1000 at 0x1000, and slot 8 must copy into a 0x1000-byte allocation followed by a no-access guard
page without touching the guard: rows inside the allocation are written, rows past it are not.

usage: startup_small_texture_4k_xg_check.py <xg_xs.dll> [--mutate no-limit]
--mutate no-limit asks with an explicit 64 KiB alignment, which the 4 KiB answer does not cover: slot 8 then
needs the whole footprint, cannot write it in front of the guard page, and the check fails (negative control).
"""
import ctypes as ct
import struct
import sys

from startup_xg_bc7_layout_check import COPY_INTO, DESC, HRESULT, LAYOUT, LAYOUT_BYTES, PTR, RELEASE

MEM_COMMIT, MEM_RESERVE, MEM_RELEASE = 0x1000, 0x2000, 0x8000
PAGE_NOACCESS, PAGE_READWRITE = 0x01, 0x04
WIDTH = HEIGHT = 32
ROW_BYTES, DESKTOP_PITCH = WIDTH * 4, 256


def main(path, mutate):
    kernel = ct.WinDLL("kernel32", use_last_error=True)
    kernel.VirtualAlloc.restype = PTR
    kernel.VirtualAlloc.argtypes = (PTR, ct.c_size_t, ct.c_uint32, ct.c_uint32)
    kernel.VirtualProtect.argtypes = (PTR, ct.c_size_t, ct.c_uint32, ct.POINTER(ct.c_uint32))
    kernel.VirtualFree.argtypes = (PTR, ct.c_size_t, ct.c_uint32)
    create = ct.WinDLL(path).XGCreateTextureComputer
    create.argtypes = (PTR, ct.POINTER(PTR), ct.c_uint64, ct.c_uint64)
    create.restype = HRESULT

    alignment = 0x10000 if mutate else 0
    desc = ct.create_string_buffer(DESC.pack(3, alignment, WIDTH, HEIGHT, 1, 1, 28, 1, 0, 0, 0))
    computer = PTR()
    assert create(desc, ct.byref(computer), 0, 0) == 0 and computer
    slots = ct.cast(computer, ct.POINTER(ct.POINTER(PTR)))[0]
    block = None
    try:
        layout = ct.create_string_buffer(LAYOUT_BYTES)
        assert LAYOUT(slots[2])(computer, layout) == 0
        total, align = struct.unpack_from("<QQ", layout.raw, 0)
        print(f"slot 2: total 0x{total:x} alignment 0x{align:x}")
        expected = (0x10000, 0x10000) if mutate else (0x1000, 0x1000)
        assert (total, align) == expected, (hex(total), hex(align))

        block = kernel.VirtualAlloc(None, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
        assert block
        old = ct.c_uint32()
        assert kernel.VirtualProtect(block + 0x1000, 0x1000, PAGE_NOACCESS, ct.byref(old))
        ct.memset(block, 0xA5, 0x1000)
        linear = bytes((y * 7 + x) & 0xFF for y in range(HEIGHT) for x in range(ROW_BYTES))
        source = ct.create_string_buffer(linear, len(linear))
        hr = COPY_INTO(slots[8])(computer, block, 0, 0, source, ROW_BYTES, 0)
        print(f"slot 8 into 0x1000 bytes before a guard page: 0x{hr & 0xffffffff:08x}")
        assert hr == 0, hex(hr & 0xffffffff)
        memory = ct.string_at(block, 0x1000)
        rows_inside = 0x1000 // DESKTOP_PITCH
        for y in range(rows_inside):
            got = memory[y * DESKTOP_PITCH:y * DESKTOP_PITCH + ROW_BYTES]
            assert got == linear[y * ROW_BYTES:(y + 1) * ROW_BYTES], f"row {y}"
        print(f"startup_small_texture_4k_xg: PASS ({rows_inside} rows written inside the allocation, guard page untouched)")
    finally:
        if block:
            kernel.VirtualFree(block, 0, MEM_RELEASE)
        RELEASE(slots[1])(computer)


if __name__ == "__main__":
    args = sys.argv[1:]
    mutate = args[1:] == ["--mutate", "no-limit"]
    if not args or (len(args) > 1 and not mutate):
        sys.exit("usage: startup_small_texture_4k_xg_check.py <xg_xs.dll> [--mutate no-limit]")
    main(args[0], mutate)
