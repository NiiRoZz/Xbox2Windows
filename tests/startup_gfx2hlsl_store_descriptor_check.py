"""Compute buffer store of tools/startup/gfx2hlsl.py after a descriptor word was modified (Refs #2274).

  (no arguments)                                  translation controls, pure Python
  binary <llvm-mc>                                reassemble the fixture sources and compare them with the words below
  dispatch <dxc> <offscreen_check> <warp|hardware>
                                                  translate, compile with DXC cs_6_0, dispatch and compare every word
  control <r-offset|k-dropped-store> <dxc> <offscreen_check> <warp|hardware>
                                                  the same dispatch with a mutated translation, which must mismatch

The store reads its four descriptor words from the registers it names. In the entry block (before the first branch
or branch target) every instruction has run exactly once and in program order, so the tracked words of those four
registers are what the store reads. There the store is lowered when all four are the words an s_load returned for
one descriptor, even though the descriptor carries a modification mark from an earlier state of the registers or
from a copy in other registers:

- case R modifies words 1 and 3 and overwrites word 2, reloads all four words, and stores one word at an immediate
  offset of 8 bytes into 28-byte records;
- case K copies the descriptor to s[20:23], modifies words 1, 2 and 3 of the copy, and stores two words through the
  loaded words in s[16:19] at an immediate offset of 4 bytes into 12-byte records.

Both stores are the plain store of #2182 with the same register values, so no descriptor field is interpreted.
A store through a word that is itself modified, overwritten or misplaced stays refused, and past the entry block
any mark on the descriptor refuses the store as before.

The dispatch uses the runner and root signature of startup_gfx2hlsl_compute_check.py (root SRV t0, root UAV u0,
16 root constants), so the descriptor fetch is not exercised on the GPU: the read-back checks the emitted store of
the two programs, and the controls below check which programs translate. All 320 words are compared, so the words
next to each stored record must keep the sentinel.
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import startup_gfx2hlsl_compute_check as C  # noqa: E402

G = C.G
KEY16 = (('table', 1), 16)
REFUSAL = 'buffer store through a modified descriptor'

SOURCE_R = """\
s_mov_b32 s10, s6
s_mov_b32 s11, 0
s_load_dwordx8 s[12:19], s[10:11], 0x0
s_waitcnt lgkmcnt(0)
s_and_b32 s17, s17, 0xffff
s_mov_b32 s18, 4
s_or_b32 s19, s19, 0x30000000
s_load_dwordx4 s[16:19], s[10:11], 0x10
s_waitcnt lgkmcnt(0)
s_lshl_b32 s20, s7, 3
v_add_nc_u32 v3, s20, v0
v_lshlrev_b32 v7, 2, v3
buffer_load_dword v8, v7, s[12:15], 0 offen
v_mul_u32_u24 v4, 28, v3
v_mul_lo_u32 v10, v3, s5
v_add_nc_u32 v10, s4, v10
s_waitcnt vmcnt(0)
v_mul_lo_u32 v8, v8, 33
v_xor_b32 v8, v8, v10
buffer_store_dword v8, v4, s[16:19], 0 offen offset:8
s_endpgm
"""
WORDS_R = bytes.fromhex(
    '06038abe' '80038bbe' '05030cf4000000fa' '7fc08cbf' '11ff1187ffff0000' '840392be' '13ff138800000030'
    '050408f4100000fa' '7fc08cbf' '0783148f' '1400064a' '82060e34' '001030e007080380' '9c060816'
    '0a0069d5030b0000' '0414144a' '703f8cbf' '080069d508430100' '0815103a' '081070e004080480' '000081bf')

SOURCE_K = """\
s_mov_b32 s10, s6
s_mov_b32 s11, 0
s_load_dwordx8 s[12:19], s[10:11], 0x0
s_waitcnt lgkmcnt(0)
s_mov_b64 s[20:21], s[16:17]
s_mov_b64 s[22:23], s[18:19]
s_and_b32 s21, s21, 0xc000ffff
s_mul_i32 s22, s22, 12
s_or_b32 s23, s23, 0x30000000
v_mad_u32_u24 v3, v1, 5, v0
s_lshl_b32 s24, s8, 1
s_add_u32 s24, s24, s7
s_mul_i32 s24, s24, 10
v_add_nc_u32 v3, s24, v3
v_lshlrev_b32 v7, 2, v3
buffer_load_dword v5, v7, s[12:15], 0 offen
v_mul_u32_u24 v4, 12, v3
v_mul_lo_u32 v6, v3, s5
v_add_nc_u32 v6, s4, v6
s_waitcnt vmcnt(0)
v_xor_b32 v5, v5, v6
v_xor_b32 v6, 0x5a000000, v3
buffer_store_dwordx2 v[5:6], v4, s[16:19], 0 offen offset:4
s_endpgm
"""
WORDS_K = bytes.fromhex(
    '06038abe' '80038bbe' '05030cf4000000fa' '7fc08cbf' '100494be' '120496be' '15ff1587ffff00c0' '168c1693'
    '17ff178800000030' '030043d5010b0104' '0881188f' '18071880' '188a1893' '1806064a' '82060e34'
    '001030e007050380' '8c060816' '060069d5030b0000' '040c0c4a' '703f8cbf' '050d0a3a' 'ff060c3a0000005a'
    '041074e004050480' '000081bf')

CASES = {
    # name: (source, words, rsrc2, group size, group counts)
    'R': (SOURCE_R, WORDS_R, C.USER7 | C.TGID_Y | C.tidig(1), (8, 1, 1), (1, 5, 1)),
    'K': (SOURCE_K, WORDS_K, C.USER7 | C.TGID_X | C.TGID_Z | C.tidig(2), (5, 2, 1), (2, 1, 3)),
}

# Single instructions of the pure-Python controls, recorded from llvm-mc 15. A branch operand is a count of dwords
# from the next instruction (65529 is -7, 65520 is -16).
LINES = {
    's_mov_b32 s10, s6': '06038abe',
    's_mov_b32 s11, 0': '80038bbe',
    's_load_dwordx8 s[12:19], s[10:11], 0x0': '05030cf4000000fa',
    's_waitcnt lgkmcnt(0)': '7fc08cbf',
    'v_lshlrev_b32 v4, 2, v0': '82000834',
    'v_mov_b32 v5, v0': '00030a7e',
    'buffer_store_dword v5, v4, s[16:19], 0 offen': '001070e004050480',
    'buffer_store_dword v5, v4, s[20:23], 0 offen': '001070e004050580',
    's_endpgm': '000081bf',
    's_and_b32 s16, s16, 0xffff': '10ff1087ffff0000',
    's_and_b32 s17, s17, 0xffff': '11ff1187ffff0000',
    's_and_b32 s18, s18, 0xffff': '12ff1287ffff0000',
    's_and_b32 s19, s19, 0xffff': '13ff1387ffff0000',
    's_mov_b32 s16, 4': '840390be',
    's_mov_b32 s17, 4': '840391be',
    's_mov_b32 s18, 4': '840392be',
    's_mov_b32 s19, 4': '840393be',
    's_or_b32 s19, s19, 0x30000000': '13ff138800000030',
    's_load_dwordx4 s[16:19], s[10:11], 0x10': '050408f4100000fa',
    's_load_dwordx2 s[16:17], s[10:11], 0x10': '050404f4100000fa',
    's_load_dwordx2 s[18:19], s[10:11], 0x18': '850404f4180000fa',
    's_mov_b64 s[20:21], s[16:17]': '100494be',
    's_mov_b64 s[22:23], s[18:19]': '120496be',
    's_and_b32 s21, s21, 0xc000ffff': '15ff1587ffff00c0',
    's_and_b32 s20, s20, 0xffff': '14ff1487ffff0000',
    's_mov_b32 s20, s17': '110394be',
    's_mov_b32 s17, s20': '140391be',
    's_mov_b32 s17, s18': '120391be',
    's_cmp_lt_u32 s7, 4': '07840abf',
    's_add_u32 s24, s24, 1': '18811880',
    's_cmp_lt_u32 s24, 4': '18840abf',
    's_cbranch_scc1 2': '020085bf',
    's_cbranch_scc1 3': '030085bf',
    's_cbranch_scc1 65529': 'f9ff85bf',
    's_cbranch_scc1 65520': 'f0ff85bf',
}
HEAD = ('s_mov_b32 s10, s6', 's_mov_b32 s11, 0', 's_load_dwordx8 s[12:19], s[10:11], 0x0', 's_waitcnt lgkmcnt(0)',
        'v_lshlrev_b32 v4, 2, v0', 'v_mov_b32 v5, v0')  # 7 dwords
STORE = 'buffer_store_dword v5, v4, s[16:19], 0 offen'
MODIFY = 's_and_b32 s17, s17, 0xffff'
RELOAD = ('s_load_dwordx4 s[16:19], s[10:11], 0x10', 's_waitcnt lgkmcnt(0)')  # 3 dwords
RELOAD8 = ('s_load_dwordx8 s[12:19], s[10:11], 0x0', 's_waitcnt lgkmcnt(0)')
COPY = ('s_mov_b64 s[20:21], s[16:17]', 's_mov_b64 s[22:23], s[18:19]')
BRANCH = 's_cmp_lt_u32 s7, 4'

# Entry block: the four registers hold loaded words, the descriptor is marked. Each was refused before.
ACCEPTED = {
    'reload': (MODIFY, 's_or_b32 s19, s19, 0x30000000', *RELOAD, STORE),
    'reload by the wide load': (MODIFY, *RELOAD8, STORE),
    'modified copy': (*COPY, 's_and_b32 s21, s21, 0xc000ffff', STORE),
    'store through the copy, source modified after': (*COPY, MODIFY, STORE.replace('s[16:19]', 's[20:23]')),
    'word restored from a copy': ('s_mov_b32 s20, s17', MODIFY, 's_mov_b32 s17, s20', STORE),
}
# The entry block ends at the first branch: the store before it translates, the store after it is refused.
BOUNDARY = (MODIFY, *RELOAD, STORE, BRANCH, 's_cbranch_scc1 2', STORE)

# Entry block: one of the four registers does not hold its loaded word. Each stays refused, whatever the marks.
WORD_REFUSED = {
    **{'word %d modified after the reload' % k: (MODIFY, *RELOAD, 's_and_b32 s%d, s%d, 0xffff' % (16 + k, 16 + k), STORE)
       for k in range(4)},
    **{'word %d overwritten after the reload' % k: (MODIFY, *RELOAD, 's_mov_b32 s%d, 4' % (16 + k), STORE)
       for k in range(4)},
    'reload of words 0-1 only': (MODIFY, 's_or_b32 s19, s19, 0x30000000', 's_load_dwordx2 s[16:17], s[10:11], 0x10',
                                 's_waitcnt lgkmcnt(0)', STORE),
    # A two-word load at byte 0x18 is tracked as its own load, not as words 2-3 of the descriptor at byte 0x10.
    'reload of words 2-3 only': ('s_and_b32 s19, s19, 0xffff', 's_load_dwordx2 s[18:19], s[10:11], 0x18',
                                 's_waitcnt lgkmcnt(0)', STORE),
    'loaded word in the wrong register': (MODIFY, *RELOAD, 's_mov_b32 s17, s18', STORE),
    'store through the modified copy': (*COPY, 's_and_b32 s21, s21, 0xc000ffff', STORE.replace('s[16:19]', 's[20:23]')),
    'restored from a modified copy': (MODIFY, 's_mov_b32 s20, s17', 's_mov_b32 s17, s20', STORE),
}

# Past the entry block: by program order the four registers hold loaded words, but another path can reach the store.
# name: (refused with the mark, the same program with the marking instruction made harmless, which translates)
PATH_REFUSED = {
    # The branch skips the reload, so the store can run with the modified word 1.
    'skipped reload': ((MODIFY, BRANCH, 's_cbranch_scc1 3', *RELOAD, STORE),
                       (BRANCH, 's_cbranch_scc1 3', *RELOAD, STORE)),
    # The branch skips the modification of a copy: neither path changes s[16:19], and the mark still refuses.
    'skipped modification of a copy': (
        ('s_mov_b32 s20, s17', BRANCH, 's_cbranch_scc1 2', 's_and_b32 s20, s20, 0xffff', STORE),
        (BRANCH, 's_cbranch_scc1 2', 's_and_b32 s20, s20, 0xffff', STORE)),
    # The store is not a branch target, but it follows the first branch.
    'store after the first branch': ((MODIFY, *RELOAD, BRANCH, 's_cbranch_scc1 2', STORE),
                                     ('s_and_b32 s20, s20, 0xffff', *RELOAD, BRANCH, 's_cbranch_scc1 2', STORE)),
    # A branch to +0 leaves no entry block at all.
    'branch to the first instruction': ((MODIFY, *RELOAD, STORE, BRANCH, 's_cbranch_scc1 65520'),
                                        ('s_and_b32 s20, s20, 0xffff', *RELOAD, STORE, BRANCH, 's_cbranch_scc1 65520')),
}
# The store is a loop head: the second pass stores through the word the loop body modified. Refused with the mark of
# the modification before the loop; no variant without that mark is pinned.
LOOP = (MODIFY, *RELOAD, STORE, MODIFY, 's_add_u32 s24, s24, 1', 's_cmp_lt_u32 s24, 4', 's_cbranch_scc1 65529')


def code(*lines):
    return b''.join(bytes.fromhex(LINES[line]) for line in (*HEAD, *lines, 's_endpgm'))


def pipeline(body, refusals=None):
    return G.ComputePipeline(code(*body), C.root_blob(), C.CASES['A'][2], (2, 3, 4), refusals=refusals)


def translate(name):
    _, words, rsrc2, local, _ = CASES[name]
    return G.ComputePipeline(words, C.root_blob(), rsrc2, local)


class sticky_mark:
    """The guard before this change: a mark on the descriptor refuses the store in the entry block too."""

    def __enter__(self):
        G.Stage.past_entry = property(lambda stage: True, lambda stage, value: None)

    def __exit__(self, *_exc):
        del G.Stage.past_entry


def check_controls():
    plain = pipeline((STORE,))
    plain_hlsl, plain_manifest = plain.hlsl_cs(), plain.manifest()
    assert plain.modified == set() and plain_manifest.splitlines()[-1] == 'uav 0 table 1 16', plain_manifest
    for name, body in ACCEPTED.items():
        pipe = pipeline(body)
        # The same store as the plain program: no instruction above the store emits anything.
        assert pipe.hlsl_cs() == plain_hlsl and pipe.manifest() == plain_manifest, (name, pipe.hlsl_cs())
        # The mark is still recorded for the descriptor, so the indexed-load refusal keeps seeing it.
        assert pipe.modified == {KEY16}, (name, pipe.modified)
        with sticky_mark():
            C.refused(REFUSAL, lambda body=body: pipeline(body).hlsl_cs())
    for guard, stores in ((None, 1), (sticky_mark(), 0)):
        refusals = []
        if guard:
            with guard:
                pipe = pipeline(BOUNDARY, refusals)
        else:
            pipe = pipeline(BOUNDARY, refusals)
        assert [G.normalize_refusal(reason) for reason, _ in refusals] == [REFUSAL] * (2 - stores), refusals
        assert pipe.hlsl_cs().count('u0.Store(a, v5);') == stores, pipe.hlsl_cs()
    for name in CASES:
        pipe = translate(name)
        assert pipe.modified == {KEY16} and pipe.manifest().splitlines()[-1] == 'uav 0 table 1 16', (name, pipe.manifest())
        with sticky_mark():
            C.refused(REFUSAL, lambda name=name: translate(name).hlsl_cs())
    r, k = translate('R').hlsl_cs().splitlines(), translate('K').hlsl_cs().splitlines()
    assert '    a = (v4 + 8) & 0xfffffffcu;' in r and '    u0.Store(a, v8);' in r, r
    assert '    a = (v4 + 4) & 0xfffffffcu;' in k and '    u0.Store2(a, uint2(v5, v6));' in k, k

    for name, body in WORD_REFUSED.items():
        C.refused('buffer store through', lambda body=body: pipeline(body).hlsl_cs())
        if not name.startswith('word 0 overwritten'):  # that register no longer holds a descriptor word at all
            C.refused(REFUSAL, lambda body=body: pipeline(body).hlsl_cs())

    for name, (body, unmarked) in PATH_REFUSED.items():
        C.refused(REFUSAL, lambda body=body: pipeline(body).hlsl_cs())
        hlsl = pipeline(unmarked).hlsl_cs()
        assert hlsl.count('u0.Store(a, v5);') == 1, (name, hlsl)
    C.refused(REFUSAL, lambda: pipeline(LOOP).hlsl_cs())

    # A refused store leaves the translation as if the instruction were absent: same HLSL and manifest, no UAV.
    body = PATH_REFUSED['skipped reload'][0]
    refusals = []
    pipe = pipeline(body, refusals)
    assert [G.normalize_refusal(reason) for reason, _ in refusals] == [REFUSAL], refusals
    absent = pipeline(body[:-1])
    assert pipe.uavs == {} and pipe.hlsl_cs() == absent.hlsl_cs() and pipe.manifest() == absent.manifest(), pipe.hlsl_cs()
    assert 'RWByteAddressBuffer' not in pipe.hlsl_cs() and 'uav ' not in pipe.manifest(), pipe.manifest()

    # A Stage without the entry-block tracking (a unit probe) proves nothing: the mark refuses.
    probe = G.Stage.__new__(G.Stage)
    probe.kind, probe.sym = 'cs', {16 + k: ('desc', ('table', 1), 16, k) for k in range(4)}
    probe.pipe = type('Pipe', (), {'modified': {KEY16}})()
    C.refused(REFUSAL, lambda: probe.op_buffer_store_dword(['v5', 'v4', 's[16:19]', '0'], ['offen']))
    print('%d entry-block stores through loaded words translate and are refused by the previous guard; %d word and '
          '%d path refusals hold' % (len(ACCEPTED) + len(CASES) + 1, len(WORD_REFUSED), len(PATH_REFUSED) + 2))


def check_binary(llvm_mc):
    for name, (source, words, *_rest) in CASES.items():
        built = C.assemble(llvm_mc, source)
        assert built == words, '%s: assembled %s, recorded %s' % (name, built.hex(), words.hex())
    for line, words in LINES.items():
        built = C.assemble(llvm_mc, line + '\n').hex()
        assert built == words, '%s: assembled %s, recorded %s' % (line, built, words)
    print('fixture words match llvm-mc')


def expected(name):
    out = [C.SENTINEL] * C.WORDS
    _, _, _, local, groups = CASES[name]
    for gz in range(groups[2]):
        for gy in range(groups[1]):
            for gx in range(groups[0]):
                for ty in range(local[1]):
                    for tx in range(local[0]):
                        if name == 'R':
                            i = gy * 8 + tx
                            out[7 * i + 2] = ((C.input_word(i) * 33) & C.MASK) ^ ((C.KEY + i * C.STRIDE) & C.MASK)
                        else:
                            i = (gx + 2 * gz) * 10 + tx + 5 * ty
                            out[3 * i + 1] = C.input_word(i) ^ ((C.KEY + i * C.STRIDE) & C.MASK)
                            out[3 * i + 2] = 0x5a000000 ^ i
    return out


# control -> (case, text the translation must contain once, replacement)
CONTROLS = {
    'r-offset': ('R', '    a = (v4 + 8) & 0xfffffffcu;\n', '    a = (v4 + 0) & 0xfffffffcu;\n'),
    'k-dropped-store': ('K', '    u0.Store2(a, uint2(v5, v6));\n', ''),
}


def run_case(name, dxc, runner, adapter, directory, control=None):
    hlsl = translate(name).hlsl_cs()
    if control:
        _, old, new = CONTROLS[control]
        assert hlsl.count(old) == 1, (control, hlsl)
        hlsl = hlsl.replace(old, new)
    src, cso, case = (directory / ('%s.%s' % (name, ext)) for ext in ('hlsl', 'cso', 'case'))
    src.write_text(hlsl, encoding='ascii')
    subprocess.run([dxc, '-T', 'cs_6_0', '-E', 'main', '-Fo', str(cso), str(src)], check=True)
    groups = CASES[name][4]
    constants = [C.KEY, C.STRIDE] + [0] * 14
    case.write_bytes(struct.pack('<I', groups[0] | groups[1] << 8 | groups[2] << 16)
                     + struct.pack('<%dI' % C.WORDS, *expected(name)) + struct.pack('<16I', *constants))
    return subprocess.run([runner, adapter, '--compute-store-case', str(cso), str(case)], capture_output=True, text=True)


def check_dispatch(dxc, runner, adapter, control=None):
    runner = str(Path(runner).resolve())
    out = HERE.parent / 'out'
    out.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        for name in ((CONTROLS[control][0],) if control else CASES):
            run = run_case(name, dxc, runner, adapter, Path(tmp), control)
            print(run.stdout, end='')
            if run.returncode == 77:
                return 77
            if control:
                if run.returncode != 1 or 'expected=' not in run.stderr:
                    raise AssertionError('control %s: not rejected (exit %d) %s' % (control, run.returncode, run.stderr))
                print('control %s: rejected at %s' % (control, run.stderr.strip().splitlines()[0]))
            elif run.returncode or run.stderr:
                raise AssertionError('case %s: %s' % (name, run.stderr or 'exit %d' % run.returncode))
            else:
                stored = sum(word != C.SENTINEL for word in expected(name))
                print('case %s: all %d words match (%d stored, %d sentinels)' % (name, C.WORDS, stored, C.WORDS - stored))
    return 0


if __name__ == '__main__':
    if len(sys.argv) == 1:
        check_controls()
    elif len(sys.argv) == 3 and sys.argv[1] == 'binary':
        check_binary(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == 'dispatch':
        sys.exit(check_dispatch(*sys.argv[2:]))
    elif len(sys.argv) == 6 and sys.argv[1] == 'control':
        sys.exit(check_dispatch(*sys.argv[3:], control=sys.argv[2]))
    else:
        sys.exit(__doc__)
