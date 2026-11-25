"""Summarise X2W_AUDIO_TRACE captures (src/startup_stubs/audio_trace.h, docs/audio-roadmap.md Phase 0).

    python -B tools/startup/audio_trace.py <prefix>            # summary of <prefix>.*.bin
    python -B tools/startup/audio_trace.py <prefix> --dump xapu:3 --limit 40
    python -B tools/startup/audio_trace.py --self-test

Each file is a sequence of 256-byte records; the first is a header with the QPC frequency and image
bases. Return addresses are printed relative to the main image (+0x...) or as "other". The summary
prints counts, timing and field histograms only, never audio data.
"""
from __future__ import annotations

import argparse
import collections
import glob
import statistics
import struct
import sys
import tempfile
from pathlib import Path

RECORD = struct.Struct("<QIBBH6QqQ176s")
assert RECORD.size == 256
MAGIC = 0x52544158

NAMES = {
    1: {0: "AcpHalAllocateShapeContexts", 1: "AcpHalCreate", 3: "ApuAlloc", 4: "ApuCreateHeap", 5: "ApuFree",
        6: "ApuIsVirtualAddressValid", 7: "ApuMapApuAddress", 8: "ApuMapVirtualAddress", 0x40: "AcpHal::slot0(connect)",
        0x42: "AcpHal::slot2(submit)", 0x80: "xma-context-snapshot"},
    2: {0: "XDspActivate", 1: "XDspConnectWithMaximumStreamLimit", 2: "XDspDeactivate", 3: "XDspDisconnect",
        4: "XDspSubmitCommand"},
    3: {0: "XApuConnect", 1: "XApuDequeueResult", 2: "XApuDisconnect", 3: "XApuEnqueueCommand", 4: "XApuGetResultCount",
        0x83: "xapu-decode-packet"},
}
MODULES = {"acphal": 1, "chad": 2, "xapu": 3}


class Rec:
    __slots__ = ("qpc", "thread", "module", "function", "args", "result", "ret", "payload")

    def __init__(self, raw: bytes):
        qpc, self.thread, self.module, self.function, n, *rest = RECORD.unpack(raw)
        self.qpc = qpc
        self.args = rest[:6]
        self.result = rest[6]
        self.ret = rest[7]
        self.payload = rest[8][:n]


def load(path: Path, out=sys.stdout):
    data = path.read_bytes()
    if len(data) < RECORD.size:
        raise ValueError(f"{path}: no header")
    if len(data) % RECORD.size:
        print(f"note: {path.name}: {len(data) % RECORD.size} trailing bytes ignored (run still writing?)", file=out)
    header = Rec(data[: RECORD.size])
    if header.function != 0xFF or header.args[3] != MAGIC:
        raise ValueError(f"{path}: not an audio trace")
    records = [Rec(data[i: i + RECORD.size]) for i in range(RECORD.size, len(data) - RECORD.size + 1, RECORD.size)]
    return header, records


def u32(b: bytes, off: int) -> int | None:
    return struct.unpack_from("<I", b, off)[0] if len(b) >= off + 4 else None


def where(ret: int, image: int) -> str:
    return f"+0x{ret - image:x}" if image <= ret < image + 0x40000000 else "other"


def gaps_ms(times: list[float]) -> str:
    if len(times) < 2:
        return "-"
    d = [b - a for a, b in zip(times, times[1:])]
    return f"median {statistics.median(d):.2f} ms, p95 {sorted(d)[int(0.95 * (len(d) - 1))]:.2f} ms, max {max(d):.2f} ms"


def summarise(prefix: str, out=sys.stdout) -> int:
    files = sorted(glob.glob(prefix + ".*.bin"))
    if not files:
        print(f"no files match {prefix}.*.bin", file=out)
        return 1
    loaded = [(Path(f), *load(Path(f), out)) for f in files]
    origin = min(h.qpc for _, h, _ in loaded)
    for path, header, records in loaded:
        freq, image = header.args[0], header.args[1]
        ms = lambda r: (r.qpc - origin) * 1000.0 / freq  # noqa: E731
        print(f"\n== {path.name}: {len(records)} records", file=out)
        by_fn = collections.defaultdict(list)
        for r in records:
            by_fn[(r.module, r.function)].append(r)
        for (module, function), rs in sorted(by_fn.items()):
            name = NAMES.get(module, {}).get(function, f"fn{function:#x}")
            t = [ms(r) for r in rs]
            results = collections.Counter(r.result & 0xFFFFFFFF for r in rs)
            rets = collections.Counter(where(r.ret, image) for r in rs)
            threads = len({r.thread for r in rs})
            print(f"  {name}: {len(rs)} calls, {t[0] / 1000:.2f}-{t[-1] / 1000:.2f} s, {threads} thread(s), gaps {gaps_ms(t)}",
                  file=out)
            print(f"    results {dict((f'{k:#x}', v) for k, v in results.most_common(4))}; "
                  f"callers {dict(rets.most_common(4))}", file=out)
            if (module, function) == (3, 3):  # XApuEnqueueCommand: type at +0, slot at +4
                kinds = collections.defaultdict(list)
                for r, tt in zip(rs, t):
                    kinds[u32(r.payload, 0)].append((r, tt))
                for kind, items in sorted(kinds.items(), key=lambda kv: (kv[0] is None, kv[0])):
                    # The caller passes three arguments; r9 is not an argument and is not reported.
                    slots = {u32(r.payload, 4) for r, _ in items}
                    extra = ""
                    if kind == 3:  # decode command, read from its builder: frames at +0x24, input bytes at +0x28
                        frames = collections.Counter(u32(r.payload, 0x24) for r, _ in items)
                        sizes = [u32(r.payload, 0x28) or 0 for r, _ in items]
                        extra = f", frames {dict(frames.most_common(3))}, input bytes {min(sizes)}-{max(sizes)}"
                    print(f"    command type {kind}: {len(items)}, {len(slots)} distinct slots, "
                          f"gaps {gaps_ms([tt for _, tt in items])}{extra}", file=out)
            if (module, function) == (3, 0x83):  # decode packets: args slot, bytes, frames; payload = packet head
                sizes = [r.args[1] & 0xFFFFFFFF for r in rs]
                first = collections.Counter(r.payload[0] for r in rs if r.payload)
                print(f"    packet bytes {min(sizes)}-{max(sizes)}, frames {dict(collections.Counter(r.args[2] for r in rs))}, "
                      f"first byte {dict((f'{k:#04x}', v) for k, v in first.most_common(6))}", file=out)
            if (module, function) == (1, 0x42):  # HAL submit: type is args[1]
                kinds = collections.Counter(r.args[1] & 0xFFFFFFFF for r in rs)
                nonzero = [u32(r.payload, 0) for r in rs]
                print(f"    submit types {dict(kinds)}; nonzero XMA contexts first {nonzero[0]}, max {max(n or 0 for n in nonzero)}, "
                      f"last {nonzero[-1]}", file=out)
            if (module, function) == (1, 0x80):
                print(f"    {len({r.args[0] for r in rs})} context snapshot(s)", file=out)
            if (module, function) == (1, 3):
                sizes = collections.Counter((r.args[2] & 0xFFFFFFFF, r.args[3] & 0xFFFFFFFF, r.args[4] & 0xFFFFFFFF) for r in rs)
                print(f"    (size, align, flags): {dict((f'{s:#x},{a:#x},{f:#x}', n) for (s, a, f), n in sizes.most_common(6))}",
                      file=out)
    return 0


def dump(prefix: str, spec: str, limit: int, out=sys.stdout) -> int:
    module_name, _, fn = spec.partition(":")
    module, function = MODULES[module_name], int(fn, 0)
    shown = 0
    for f in sorted(glob.glob(prefix + ".*.bin")):
        header, records = load(Path(f))
        for r in records:
            if (r.module, r.function) != (module, function):
                continue
            args = " ".join(f"{a:#x}" for a in r.args)
            print(f"{(r.qpc - header.qpc) * 1000.0 / header.args[0]:10.3f} ms t{r.thread} {where(r.ret, header.args[1])} "
                  f"args [{args}] -> {r.result & 0xFFFFFFFF:#x} payload {r.payload.hex(' ')}", file=out)
            shown += 1
            if shown >= limit:
                return 0
    return 0


def self_test() -> int:
    import io

    def rec(qpc, module, function, args=(0,) * 6, result=0, ret=0, payload=b""):
        return RECORD.pack(qpc, 7, module, function, len(payload), *args, result, ret, payload.ljust(176, b"\0"))

    image = 0x140000000
    with tempfile.TemporaryDirectory() as d:
        prefix = str(Path(d) / "t")
        body = rec(0, 0, 0xFF, (1000, image, 0, MAGIC, 0, 0))
        for i in range(4):
            body += rec(10 * i, 3, 3, (1, 0, 0, 0x100 * i, 0, 0), 0, image + 0x1234, struct.pack("<II", 1 + i % 2, i))
        decode = bytearray(0x40)
        struct.pack_into("<II", decode, 0, 3, 7)
        struct.pack_into("<II", decode, 0x24, 960, 321)
        body += rec(50, 3, 3, ret=image + 0x1234, payload=bytes(decode))
        body += rec(51, 3, 0x83, (7, 321, 960, 0, 0, 0), 0, image + 0x1234, bytes((0xFC, 0xFF, 0xFE)))
        Path(prefix + ".xapu.dll.bin").write_bytes(body + b"\0" * 5)
        hal = rec(0, 0, 0xFF, (1000, image, 0, MAGIC, 0, 0)) + rec(5, 1, 0x42, (0, 5, 0, 0, 0, 0), 0, 1,
                                                                   struct.pack("<IIQ", 3, 350, 9))
        Path(prefix + ".AcpHal.dll.bin").write_bytes(hal)
        out = io.StringIO()
        assert summarise(prefix, out) == 0
        text = out.getvalue()
        assert "XApuEnqueueCommand: 5 calls" in text, text
        assert "frames {960: 1}, input bytes 321-321" in text, text
        assert "packet bytes 321-321, frames {960: 1}, first byte {'0xfc': 1}" in text, text
        assert "command type 1: 2, 2 distinct slots" in text, text
        assert "r9" not in text, text
        assert "callers {'+0x1234': 5}" in text, text
        assert "submit types {5: 1}; nonzero XMA contexts first 3" in text, text
        assert "trailing bytes ignored" in text, text
        out = io.StringIO()
        dump(prefix, "xapu:3", 1, out)
        assert out.getvalue().count("\n") == 1 and "+0x1234" in out.getvalue(), out.getvalue()
        Path(prefix + ".bad.bin").write_bytes(b"\0" * 256)
        try:
            summarise(prefix, io.StringIO())
            raise AssertionError("a file without the magic must be refused")
        except ValueError:
            pass
    print("audio_trace self-test passed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix", nargs="?")
    ap.add_argument("--dump", metavar="MODULE:FN", help="print raw records, e.g. xapu:3 (modules: acphal, chad, xapu)")
    ap.add_argument("--limit", type=int, default=50)
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.prefix:
        ap.error("prefix required")
    return dump(a.prefix, a.dump, a.limit) if a.dump else summarise(a.prefix)


if __name__ == "__main__":
    sys.exit(main())
