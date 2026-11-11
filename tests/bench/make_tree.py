#!/usr/bin/env python3
"""Generates benchmark data for bench_transfer (tests/bench_transfer.cpp).

  make_tree.py <dest> [--files 15645] [--dirs 3656] [--seed 1] [--big-mb 1536] [--no-tree] [--no-big]

<dest>/tree: a tree resembling a source checkout with build outputs: --dirs folders (nested up to
7 deep, a few wide "obj"-like folders), --files files with sizes skewed small (about 85% of 1-30 KB,
12% of 30-100 KB, 3% of 100 KB-5 MB; about 0.8-1 GB in all with the defaults).
<dest>/big.bin: one file of --big-mb MB, for bulk throughput.
Deterministic for a given seed. The content is random (nothing on the way compresses).
"""
import argparse
import os
import random
import sys

WORDS = ["src", "core", "net", "ui", "platform", "include", "tests", "obj", "x64", "Release", "Debug",
         "generated", "moc", "autogen", "resources", "icons", "third_party", "lib", "build", "cmake",
         "docs", "tools", "scripts", "assets", "widgets", "detail", "impl", "win", "linux", "mac"]
EXTS = [".cpp", ".h", ".obj", ".pdb", ".txt", ".json", ".cmake", ".png", ".ilk", ".tlog", ".d", ".o", ".js", ".py"]


def make_dirs(rng, count):
    dirs = ["tree"]
    depth = {"tree": 0}
    while len(dirs) < count:
        # Prefer recent folders as parents (deep chains) but keep some breadth.
        parent = dirs[-1 - int(rng.expovariate(1 / 40)) % len(dirs)] if rng.random() < 0.7 else rng.choice(dirs)
        if depth[parent] >= 7:
            parent = rng.choice(dirs[: max(1, len(dirs) // 4)])
        name = "%s_%d" % (rng.choice(WORDS), len(dirs))
        path = parent + "/" + name
        dirs.append(path)
        depth[path] = depth[parent] + 1
    return dirs


def size_of(rng):
    r = rng.random()
    if r < 0.85:
        return int(1024 * (30 ** rng.random()))           # 1-30 KB, log-uniform
    if r < 0.97:
        return int(30 * 1024 + rng.random() * 70 * 1024)  # 30-100 KB
    return int(100 * 1024 * (50 ** rng.random()))         # 100 KB-5 MB, log-uniform


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dest")
    ap.add_argument("--files", type=int, default=15645)
    ap.add_argument("--dirs", type=int, default=3656)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--big-mb", type=int, default=1536)
    ap.add_argument("--no-tree", action="store_true")
    ap.add_argument("--no-big", action="store_true")
    a = ap.parse_args()
    rng = random.Random(a.seed)
    pool = os.urandom(8 * 1024 * 1024)  # random bytes, sliced at random offsets
    if not a.no_tree:
        dirs = make_dirs(rng, a.dirs)
        for d in dirs:
            os.makedirs(os.path.join(a.dest, d), exist_ok=True)
        total = 0
        for i in range(a.files):
            d = dirs[rng.randrange(len(dirs))]
            n = size_of(rng)
            name = "file_%05d%s" % (i, rng.choice(EXTS))
            with open(os.path.join(a.dest, d, name), "wb") as f:
                left = n
                while left > 0:
                    k = min(left, len(pool) // 2)
                    off = rng.randrange(len(pool) - k + 1)
                    f.write(pool[off:off + k])
                    left -= k
            total += n
        print("tree: %d files, %d folders, %.1f MB" % (a.files, len(dirs), total / 1e6))
    if not a.no_big:
        with open(os.path.join(a.dest, "big.bin"), "wb") as f:
            for i in range(a.big_mb // 4):
                f.write(pool[(i * 4097) % (4 * 1024 * 1024):][: 4 * 1024 * 1024].ljust(4 * 1024 * 1024, b"\0"))
        print("big.bin: %d MB" % a.big_mb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
