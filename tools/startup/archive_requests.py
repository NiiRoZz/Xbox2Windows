#!/usr/bin/env python3
"""Write startup shader requests from a shader archive, without running the title (docs/startup-runtime.md).

    archive_requests.py <archive> <shader cache> [--roots <dir or file> ...]

The archive stores native programs in zlib streams that start on 64-byte boundaries. Each program
begins with a 36-byte header followed by s_inst_prefetch and is padded in 64-byte blocks. In the
measured archive a pipeline's vertex program directly follows its pixel program, so each pixel
program followed by a vertex program in the same stream becomes one request (a = pixel, b = vertex,
the order the runtime records). Root signatures were not found in the archive; they are read from
existing req_*.root files (by default those already in the cache), and every pair is written once
per distinct root, so each key matches what the runtime would compute for that combination.
Translate the result with gfx2hlsl.py --pending <shader cache>.
"""
import argparse
import glob
import mmap
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx2hlsl  # noqa: E402

HEADER = 0x24          # bytes before s_inst_prefetch
HEADER_TAG = b'\x80\xd5'  # bytes 2..3 of the first header word in every measured program
BLOCK = 0x40
STEP = 0x100           # programs start on 256-byte steps of the stream
WINDOW = 0x20000       # compressed bytes offered to one stream; a 64 KiB stream needs far less
ENDPGM = bytes.fromhex('000081bf')  # s_endpgm


def complete(program):
    """True when the carved bytes hold an s_endpgm; an all-zero block inside a program cuts it short."""
    return any(program[at:at + 4] == ENDPGM for at in range(HEADER, len(program) - 3, 4))


def next_step(at, program):
    """Stream offset of the 256-byte step right after the program."""
    return at + -(-len(program) // STEP) * STEP


def is_program(data, at):
    return (at >= 0 and data[at + 2:at + 4] == HEADER_TAG
            and data[at + HEADER:at + HEADER + 4] == gfx2hlsl.PREFETCH)


def carve(data):
    """Yield (offset, program) in stream order; a program that runs past the stream is skipped."""
    hit = data.find(gfx2hlsl.PREFETCH)
    while hit >= 0:
        at = hit - HEADER
        if at % 4 == 0 and is_program(data, at):
            end = at + BLOCK
            # 64-byte blocks until an all-zero block or the next program on a 256-byte step
            while end + BLOCK <= len(data) and any(data[end:end + BLOCK]) and not (
                    (end - at) % STEP == 0 and is_program(data, end)):
                end += BLOCK
            # a program that reaches the end of the joined stream may be cut off, so it is skipped
            if end + BLOCK <= len(data):
                yield at, data[at:end]
        hit = data.find(gfx2hlsl.PREFETCH, hit + 4)


def chunks(view):
    """Yield (archive offset, end offset, inflated bytes) for each complete zlib stream on a 64-byte boundary."""
    at = 0
    while at < len(view) - 1:
        cmf, flg = view[at], view[at + 1]
        if cmf == 0x78 and (cmf << 8 | flg) % 31 == 0:
            inflater = zlib.decompressobj()
            window = view[at:at + WINDOW]
            try:
                data = inflater.decompress(window)
            except zlib.error:
                data = b''
            if inflater.eof and data:
                end = at + len(window) - len(inflater.unused_data)
                yield at, end, data
                at = (end + 63) & ~63
                continue
        at += 64


def streams(archive):
    """Yield (archive offset, inflated bytes); a zlib stream that starts on the 64-byte boundary right
    after the previous one continues it, so a program split between the two is carved whole."""
    with open(archive, 'rb') as file:
        if os.fstat(file.fileno()).st_size == 0:
            return
        with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as view:
            start, joined, expected = None, [], None
            for at, end, data in chunks(view):
                if at != expected and joined:
                    yield start, b''.join(joined)
                    joined = []
                if not joined:
                    start = at
                joined.append(data)
                expected = (end + 63) & ~63
            if joined:
                yield start, b''.join(joined)


def read_roots(paths):
    roots = []
    for path in paths:
        if not os.path.exists(path):
            sys.exit('root signature path not found: %s' % path)
        files = sorted(glob.glob(os.path.join(path, 'req_*.root'))) if os.path.isdir(path) else [path]
        for name in files:
            blob = open(name, 'rb').read()
            if blob and blob not in roots:
                roots.append(blob)
    return roots


def write_request(cache, key, a, b, root):
    """Writes .b and .root, then .a last: the scanner treats .a as the ready marker."""
    name = os.path.join(cache, 'req_%016x' % key)
    if os.path.exists(name + '.a'):
        return False
    for ext, blob in (('.b', b), ('.root', root), ('.a', a)):
        temporary = '%s%s.%d.tmp' % (name, ext, os.getpid())
        with open(temporary, 'wb') as out:
            out.write(blob)
        os.replace(temporary, name + ext)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('archive')
    parser.add_argument('cache')
    parser.add_argument('--roots', nargs='+', help='req_*.root files or directories (default: the cache)')
    args = parser.parse_args()
    roots = read_roots(args.roots or [args.cache])
    if not roots:
        sys.exit('no root signature found: run the title once with a shader cache, or pass --roots')
    os.makedirs(args.cache, exist_ok=True)
    # streams counts joined runs of consecutive zlib streams
    counts = dict(streams=0, programs=0, incomplete=0, ps=0, vs=0, other=0, undecodable=0, pairs=0,
                  written=0, existing=0)
    for _, data in streams(args.archive):
        counts['streams'] += 1
        previous = None  # (stage, offset, program) of the program carved just before
        for at, program in carve(data):
            counts['programs'] += 1
            if not complete(program):
                counts['incomplete'] += 1
                previous = None
                continue
            try:
                stage = gfx2hlsl.classify(program)
            except gfx2hlsl.Unsupported:
                stage = 'undecodable'
            counts[stage] += 1
            # directly following: the vertex program starts on the 256-byte step after the pixel program
            if stage == 'vs' and previous and previous[0] == 'ps' and at == next_step(previous[1], previous[2]):
                counts['pairs'] += 1
                for root in roots:
                    key = gfx2hlsl.pipeline_key(previous[2], program, root)
                    counts['written' if write_request(args.cache, key, previous[2], program, root) else 'existing'] += 1
            previous = (stage, at, program)
    counts['roots'] = len(roots)
    print(' '.join('%s=%d' % item for item in counts.items()))


if __name__ == '__main__':
    main()
