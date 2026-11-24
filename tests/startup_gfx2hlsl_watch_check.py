"""gfx2hlsl.py pending scan and --watch on synthetic request files (pure Python, no Windows, no GPU).

A request is consumed only once its ready marker (.a or .rtv) exists and it has no published .txt.
In watch mode a request whose .fail is newer than its marker is not retried until the marker is
rewritten; the one-shot --pending scan still retries it. The --watch process must translate a new
request while it runs and must not rewrite a failure on later passes. A --pending scan with four
parallel workers must print the same results and write the same failures as one worker.
"""
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 'startup'))
import gfx2hlsl  # noqa: E402

failures = []


def expect(label, got, want):
    if got != want:
        failures.append('%s: got %s, want %s' % (label, got, want))


def touch(path, data=b'', mtime=None):
    path.write_bytes(data)
    if mtime is not None:
        os.utime(path, (mtime, mtime))


def scan(directory, skip_failed):
    return gfx2hlsl.pending_keys(str(directory), skip_failed)


def pending_run(directory, jobs):
    env = dict(os.environ, X2W_TRANSLATE_JOBS=str(jobs))
    out = subprocess.run([sys.executable, '-B', str(ROOT / 'tools' / 'startup' / 'gfx2hlsl.py'), '--pending', str(directory)],
                         capture_output=True, text=True, timeout=120, env=env)
    fails = {p.name: p.read_bytes() for p in Path(directory).glob('*.fail')}
    return out.returncode, sorted(out.stdout.splitlines()), fails


with tempfile.TemporaryDirectory() as tmp:
    d = Path(tmp)
    now = time.time()
    # 1: complete request; 2: already published; 3: no ready marker yet (negative control);
    # 4: failed after its marker was written; 5: marker rewritten after the failure; 6: RTV variant.
    for key in (1, 2, 4, 5):
        for ext in ('.b', '.root'):
            touch(d / ('req_%016x%s' % (key, ext)))
    touch(d / ('req_%016x.a' % 1))
    touch(d / ('req_%016x.a' % 2))
    touch(d / ('%016x.txt' % 2))
    touch(d / ('req_%016x.b' % 3))
    touch(d / ('req_%016x.root' % 3))
    touch(d / ('req_%016x.a' % 4), mtime=now - 10)
    touch(d / ('%016x.fail' % 4), mtime=now - 5)
    touch(d / ('%016x.fail' % 5), mtime=now - 10)
    touch(d / ('req_%016x.a' % 5), mtime=now - 5)
    touch(d / ('req_%016x.rtv' % 6))
    expect('watch pass', scan(d, True), [1, 5, 6])
    expect('one-shot pass', scan(d, False), [1, 4, 5, 6])

# Parallel --pending must give the same results as one worker: eight invalid RTV requests fail alike.
with tempfile.TemporaryDirectory() as one, tempfile.TemporaryDirectory() as many:
    for d in (one, many):
        for key in range(8):
            touch(Path(d) / ('req_%016x.rtv' % (0x100 + key)), b'not an rtv request %d' % key)
    serial, parallel = pending_run(one, 1), pending_run(many, 4)
    expect('parallel --pending output and failures', parallel, serial)
    expect('every request answered', len(serial[1]), 8)

with tempfile.TemporaryDirectory() as tmp:
    d = Path(tmp)
    watch = subprocess.Popen([sys.executable, '-B', str(ROOT / 'tools' / 'startup' / 'gfx2hlsl.py'),
                              '--watch', str(d), '0.1'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.5)
        # Written while the watcher runs; an invalid RTV request fails without a GPU or compiler.
        touch(d / ('req_%016x.rtv' % 7), b'not an rtv request')
        fail = d / ('%016x.fail' % 7)
        deadline = time.time() + 20
        while not fail.exists() and time.time() < deadline and watch.poll() is None:
            time.sleep(0.1)
        expect('watcher wrote a failure for the new request', fail.exists(), True)
        first = fail.stat().st_mtime_ns if fail.exists() else None
        time.sleep(1.0)
        expect('failure left unchanged by later passes', fail.stat().st_mtime_ns if fail.exists() else None, first)
        expect('watcher still running', watch.poll(), None)
    finally:
        watch.kill()
        out, err = watch.communicate(timeout=20)
    lines = [line for line in out.splitlines() if line.strip()]
    expect('watcher output lines', len(lines), 1)
    if err.strip():
        failures.append('watcher stderr: %r' % err.strip()[-500:])

for item in failures:
    print('FAIL', item)
if failures:
    sys.exit(1)
print('startup_gfx2hlsl_watch_check passed')
