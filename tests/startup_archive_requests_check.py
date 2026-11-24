"""archive_requests.py on a synthetic archive (pure Python, no Windows, no GPU, no title files).

Programs follow the measured layout: a 36-byte header, s_inst_prefetch, code, s_code_end padding to
64 bytes, each on a 256-byte step of a zlib stream that starts on a 64-byte boundary. Each pixel
program directly followed by a vertex program must become one request per root, keyed exactly as
the runtime keys it. Controls: a pixel program followed by another pixel program, a vertex program
with no pixel program before it, a program that runs to the end of its stream, a valid pair in a
stream that is not on a 64-byte boundary, and a run with no root signature must write nothing. A
pair split between two consecutive zlib streams must be carved whole. A second run must write
nothing new.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / 'tools' / 'startup' / 'archive_requests.py'
sys.path.insert(0, str(ROOT / 'tools' / 'startup'))
import gfx2hlsl  # noqa: E402

CODE_END = bytes.fromhex('00009fbf')
failures = []


def expect(label, got, want):
    if got != want:
        failures.append('%s: got %r, want %r' % (label, got, want))


def program(stage, serial, blocks=1):
    target = {'vs': 12, 'ps': 0}[stage]  # exp pos0 / exp mrt0, then s_endpgm
    header = b'\x00\x00\x80\xd5' + struct.pack('<I', serial) + bytes(28)
    body = header + gfx2hlsl.PREFETCH + struct.pack('<3I', 0xf800080f | target << 4, 0, 0xbf810000)
    size = 0x40 * blocks
    return body + CODE_END * ((size - len(body)) // 4)


def stream(layout, length=0x1000):
    """layout: [(offset, program)], offsets on 256-byte steps from 0x2c as in the measured streams."""
    data = bytearray(length)
    for at, blob in layout:
        data[at:at + len(blob)] = blob
    return bytes(data)


PS1, VS1 = program('ps', 1), program('vs', 2, blocks=2)
PS2, PS3, VS3, VS4 = program('ps', 3), program('ps', 4), program('vs', 5), program('vs', 6)
PS_TAIL, VS_TAIL = program('ps', 7), program('vs', 12, blocks=2)  # VS_TAIL ends where the stream ends
PS5, VS5 = program('ps', 8), program('vs', 9)
PS6, VS6 = program('ps', 10), program('vs', 11)

first = stream([(0x02c, PS1), (0x12c, VS1), (0x22c, PS2), (0x32c, PS3), (0x42c, VS3), (0x52c, VS4),
                (0x62c, PS_TAIL), (0x72c, VS_TAIL)], length=0x72c + len(VS_TAIL))
second = stream([(0x02c, PS5), (0x12c, VS5)])
misaligned = stream([(0x02c, PS6), (0x12c, VS6)])

# A pair split between two zlib streams, the second starting on the 64-byte boundary right after the
# first, must be carved whole.
PS8, VS8 = program('ps', 13), program('vs', 14, blocks=2)
split = stream([(0x02c, PS8), (0x12c, VS8)], length=0x300)

archive = bytearray()
for blob, aligned in ((first, True), (misaligned, False), (second, True), (b'gap', True)):
    while len(archive) % 64 != (0 if aligned else 32):
        archive.append(0)
    archive += zlib.compress(blob, 9) if blob != b'gap' else bytes(64)  # the gap keeps `split` apart
for piece in (split[:0x150], split[0x150:]):
    while len(archive) % 64:
        archive.append(0)
    archive += zlib.compress(piece, 9)
roots = [b'DXBC-root-one', b'DXBC-root-two']
want = {gfx2hlsl.pipeline_key(a, b, r): (a, b, r)
        for a, b in ((PS1, VS1), (PS3, VS3), (PS5, VS5), (PS8, VS8)) for r in roots}

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'archive.bin').write_bytes(archive)
    rootdir = tmp / 'roots'
    rootdir.mkdir()
    for i, r in enumerate(roots + roots[:1]):  # a duplicate root must not double the requests
        (rootdir / ('req_%016x.root' % i)).write_bytes(r)
    cache = tmp / 'cache'

    def run(*extra):
        return subprocess.run([sys.executable, '-B', str(TOOL), str(tmp / 'archive.bin'), str(cache), *extra],
                              capture_output=True, text=True, timeout=120)

    missing = run('--roots', str(tmp / 'absent'))
    expect('missing roots path: refused', missing.returncode != 0 and 'not found' in missing.stderr, True)
    cache.mkdir()  # an existing cache without any req_*.root
    empty = run()
    expect('no roots: refused with a message', empty.returncode != 0 and 'no root signature' in empty.stderr, True)
    expect('no roots: requests written', sorted(cache.glob('req_*')), [])

    out = run('--roots', str(rootdir))
    expect('run exit status', out.returncode, 0)
    got = {int(p.name[4:20], 16) for p in cache.glob('req_*.a')}
    expect('request keys', sorted(got), sorted(want))
    for key, (a, b, r) in want.items():
        name = cache / ('req_%016x' % key)
        parts = tuple(name.with_suffix(ext).read_bytes() if name.with_suffix(ext).exists() else None
                      for ext in ('.a', '.b', '.root'))
        expect('request %016x parts' % key, parts, (a, b, r))
    expect('no temporary files left', sorted(p.name for p in cache.glob('*.tmp')), [])
    expect('summary', 'pairs=4' in out.stdout and 'written=8' in out.stdout and 'streams=3' in out.stdout, True)

    again = run('--roots', str(rootdir))
    expect('second run', 'written=0' in again.stdout and 'existing=8' in again.stdout, True)
    if failures:
        print(out.stdout, out.stderr, again.stdout, sep='\n')

for item in failures:
    print('FAIL', item)
if failures:
    sys.exit(1)
print('startup_archive_requests_check passed')
