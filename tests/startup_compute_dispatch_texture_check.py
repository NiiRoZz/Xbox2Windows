"""Translated compute programs with 2D texture views, dispatched through the stand-in's command list (slot 14).

usage: startup_compute_dispatch_texture_check.py <startup_stream_consumer> <dxc> <warp|hardware>

Texture output: the consumer creates the image store program of startup_compute_image_store_d16_check.py as a
device slot 67 pipeline, fills a 32x8 RGBA16_FLOAT texture with sentinel halves, creates an unordered access view of
it at table byte 64 (device slot 19), records Dispatch(2, 1, 1) and copies the texture to a readback buffer. Four
runs share one cache:

1. before translation: refused with "no translation", every half keeps its sentinel;
2. after gfx2hlsl.py --pending with DXC: the 1024 halves equal mip 0 of the fixture's CPU reference (64 changed);
3. texture-store-none (no Dispatch recorded): every half keeps its sentinel;
4. texture-store-denied (a texture without unordered access, whose view the stand-in does not create): refused
   with one logged reason, every half keeps its sentinel.
Exit 77 when the adapter is not available.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

import startup_compute_image_store_d16_check as S
import startup_gfx2hlsl_compute_check as C
from startup_compute_dispatch_check import lines
from startup_compute_request_check import DATA, prepare
from startup_compute_translate_check import state_a
from startup_stream_check import gfx2hlsl as G


def run(directory, consumer, env, mode, layout):
    out = directory / 'out.bin'
    out.unlink(missing_ok=True)
    args = [consumer, directory / 'vertex', directory / 'pixel', directory / 'root', 'slot67',
            directory / 'program', directory / 'data', directory / 'state', mode]
    result = subprocess.run(args, env=dict(env, X2W_TEST_DISPATCH_OUT=str(out)), capture_output=True, text=True)
    if result.returncode == 77:
        print(result.stdout, end='')
        sys.exit(77)
    assert result.returncode == 0, f'{mode}: consumer exit {result.returncode}\n{result.stderr[-3000:]}'
    return list(struct.unpack(layout, out.read_bytes())), result


def translate(cache, dxc, key):
    out = subprocess.run([sys.executable, '-B', G.__file__, '--pending', cache], check=True, capture_output=True,
                         text=True, env=dict(os.environ, X2W_DXC=dxc)).stdout
    assert f'{key:016x}: ok' in out, out


def clean(result):
    assert not lines(result, 'refused'), result.stderr[-3000:]
    errors = [line for line in lines(result, 'x2w-d3d12-debug ') if ' sev=0 ' in line or ' sev=1 ' in line]
    assert not errors, errors[0]
    layer = lines(result, 'x2w-d3d12 debug layer: ')
    return 'enabled, no error message' if layer and '0x00000000' in layer[0] else 'unavailable'


def store(directory, consumer, dxc, env):
    cache, base = prepare(directory)
    env = dict(base, **env)
    (directory / 'compute_root').write_bytes(C.root_blob(3, table_range=True))
    env['X2W_TEST_COMPUTE_ROOT'] = str(directory / 'compute_root')
    assert S.fixture.GROUP == (4, 2, 1) and S.fixture.GROUPS == 2, 'the consumer hard-codes Dispatch(2, 1, 1)'
    (directory / 'program').write_bytes(S.fixture.WORDS)
    (directory / 'data').write_bytes(DATA)
    (directory / 'state').write_bytes(state_a(S.fixture.GROUP, S.RSRC2))
    reference = S.fixture.reference()
    halves = 32 * 8 * 4  # mip 0 comes first in both halves of the reference
    untouched, expected = reference[:halves], reference[len(reference) // 2:][:halves]
    assert sum(a != b for a, b in zip(untouched, expected)) == 64
    layout = '<%dH' % halves

    got, result = run(directory, consumer, env, 'texture-store', layout)
    print(result.stdout, end='')
    assert got == untouched, 'a Dispatch without a translation wrote the texture'
    assert len(lines(result, 'Dispatch(2, 1, 1) refused: no translation')) == 1, result.stderr[-3000:]
    key = int(lines(result, 'x2w-draw compute request')[0].split()[3], 16)
    translate(cache, dxc, key)

    got, result = run(directory, consumer, env, 'texture-store', layout)
    wrong = [i for i in range(halves) if got[i] != expected[i]]
    assert not wrong, 'half=%d got=%04x expected=%04x (%d halves differ)\n%s' % (
        wrong[0], got[wrong[0]], expected[wrong[0]], len(wrong), result.stderr[-3000:])
    assert len(lines(result, 'Dispatch(2, 1, 1) translated')) == 1, result.stderr[-3000:]
    layer = clean(result)

    got, result = run(directory, consumer, env, 'texture-store-none', layout)
    assert got == untouched and not lines(result, 'Dispatch('), 'the texture changed without a Dispatch'

    got, result = run(directory, consumer, env, 'texture-store-denied', layout)
    assert got == untouched, 'a refused Dispatch wrote the texture'
    refused = lines(result, 'refused: ')
    assert len(refused) == 1 and 'an output texture view was not created on the desktop' in refused[0], result.stderr[-3000:]
    print(f'texture output: all {halves} halves match after translation (64 changed); untouched before translation, '
          f'without a Dispatch and for a refused view; debug layer: {layer}')


def main(consumer, dxc, adapter):
    env = dict(X2W_D3D12_LIST_NOOP='1', X2W_D3D12_DEBUG='1')  # guest lists; debug-layer messages go to the log
    if adapter == 'hardware':
        env['X2W_TEST_DISPATCH_HARDWARE'] = '1'
    with tempfile.TemporaryDirectory() as temporary:
        store(Path(temporary) / 'store', consumer, dxc, env)
    print(f'startup_compute_dispatch_texture: PASS on {adapter}')


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[3] not in ('warp', 'hardware'):
        sys.exit(__doc__)
    main(*sys.argv[1:])
