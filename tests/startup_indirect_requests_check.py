"""A console pipeline used only through ExecuteIndirect (list slot 61) still writes its translation request.

usage: startup_indirect_requests_check.py <startup_stream_consumer> <warp|hardware>

The consumer builds the stream pipeline (slot 86, with an input layout so its request key differs), then a
device slot 57 pipeline from the same shaders and root. "slot57-indirect" binds it and records one
ExecuteIndirect draw, which the stand-in refuses (no desktop pipeline); the request must still appear with the
slot-57 shaders and root, once. "slot57-bound" binds it and records nothing: the negative control, which must
leave only the stream pipeline's request.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from startup_stream_draw_check import pixel, vertex


def requests(consumer, backend, directory, mode, runs=1):
    cache = directory / mode
    cache.mkdir()
    vs, ps, root = (directory / name for name in ("vs", "ps", "root"))
    vs.write_bytes(vertex())
    ps.write_bytes(pixel(False))
    env = dict(os.environ, X2W_D3D12_PERMISSIVE="1", X2W_D3D12_LIST_NOOP="1", X2W_D3D12_RENDER="1",
               X2W_SHADER_CACHE=str(cache), X2W_TEST_INPUT_LAYOUT="1")
    env.setdefault("X2W_D3D12_DEBUG", "0")
    for _ in range(runs):
        result = subprocess.run([consumer, vs, ps, root, backend, directory / "unused", mode], env=env,
                                capture_output=True, text=True)
        if backend == "hardware" and result.returncode == 77:
            print("No D3D12 feature-level 12 hardware adapter")
            sys.exit(77)
        if result.returncode:
            raise RuntimeError(f"{mode}: consumer failed on {backend}: {result.returncode}\n{result.stderr[-2000:]}")
    found = {}
    for marker in sorted(cache.glob("req_*.a")):
        key = marker.name[4:-2]
        found[key] = {ext: (cache / f"req_{key}{ext}").read_bytes() if (cache / f"req_{key}{ext}").exists() else None
                      for ext in (".a", ".b", ".root", ".ia")}
    return found, vs.read_bytes(), ps.read_bytes(), root.read_bytes()


def check(consumer, backend):
    with tempfile.TemporaryDirectory() as temp:
        bound, *_ = requests(consumer, backend, Path(temp), "slot57-bound")
        assert len(bound) == 1, f"control: {len(bound)} requests, expected only the stream pipeline's"
        stream_key, stream = next(iter(bound.items()))
        assert stream[".ia"] is not None, "control: the stream request has no input layout"

    with tempfile.TemporaryDirectory() as temp:
        # Two runs into one cache: the request is written once, the second run finds it.
        found, vs, ps, root = requests(consumer, backend, Path(temp), "slot57-indirect", runs=2)
        assert stream_key in found, "the stream pipeline's request is missing"
        extra = {key: files for key, files in found.items() if key != stream_key}
        assert len(extra) == 1, f"{len(extra)} indirect-only requests, expected 1"
        files = next(iter(extra.values()))
        # Device slot 57 records the vertex program as a and the pixel program as b.
        assert files[".a"] == vs and files[".b"] == ps and files[".root"] == root and files[".ia"] is None, \
            "the indirect-only request does not carry the slot-57 shaders and root"
        leftovers = list(Path(temp).rglob("*.tmp"))
        assert not leftovers, f"temporary request files left: {leftovers}"
    print(f"startup_indirect_requests: PASS on {backend} (control: 1 request; indirect-only: 1 more request, "
          f"slot-57 shaders and root, written once over 2 runs)")
    return 0


if __name__ == "__main__":
    sys.exit(check(*sys.argv[1:]))
