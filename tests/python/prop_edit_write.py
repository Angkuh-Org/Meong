#!/usr/bin/env python3
"""Deterministic property/differential test for Meong's edit and write tools.

Fixed seed, structured generators, a fresh temp dir per case, and a Python
bytes reference. The real C tool runs through tests/c/prop_harness.c. Nothing
outside the temp root is written. No third-party dependency.

Usage:
    MEONG_HARNESS=/path/to/prop_harness python3 tests/python/prop_edit_write.py
Env:
    MEONG_PROP_EDIT   edit cases (default 2000)
    MEONG_PROP_WRITE  write cases (default 800)
"""
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

HARNESS = os.environ.get("MEONG_HARNESS")
if not HARNESS or not os.path.exists(HARNESS):
    print("MEONG_HARNESS must point to the prop_harness binary", file=sys.stderr)
    sys.exit(2)

ROOT = tempfile.mkdtemp(prefix="meong-prop.", dir=os.environ.get("TMPDIR", "/tmp"))
os.umask(0o022)
NUL_PATH = []

TOKENS = [
    b"alpha", b"beta", b"\n", b"\r\n", b"\r", b"\t", b" ", b'"', b"\\",
    b"{", b"}", b"\x01", b"\x7f", "€".encode(), "😀".encode(), b"\x00",
]
PLAIN = [t for t in TOKENS if t != b"\x00"]


def gen_data(rng, ntok):
    return b"".join(rng.choice(PLAIN) for _ in range(ntok))


def run(tool, cwd, args):
    p = os.path.join(ROOT, "args.json")
    with open(p, "wb") as f:
        f.write(json.dumps(args).encode())
    r = subprocess.run([HARNESS, tool, cwd, p], capture_output=True)
    lines = r.stdout.split(b"\n")
    rc = int(lines[0].split(b"=")[1])
    err = lines[1][4:].decode("utf-8", "replace")
    out = lines[2][4:].decode("utf-8", "replace")
    return rc, err, out


def stat_of(p):
    st = os.lstat(p)
    return (st.st_ino, st.st_size, st.st_mode & 0o7777, st.st_mtime_ns)


def temp_residue(d):
    return [n for n in os.listdir(d) if n.startswith(".mng.") or n.endswith(".mng")]


def newdir(tag):
    return tempfile.mkdtemp(prefix=tag + ".", dir=ROOT)


def case_edit(rng, results):
    toks = [rng.choice(PLAIN) for _ in range(rng.randint(1, 20))]
    i = rng.randrange(len(toks))
    j = rng.randrange(i + 1, min(len(toks), i + 8) + 1)
    old = b"".join(toks[i:j])
    data = b"".join(toks)
    mode = rng.choice(["unique", "unique", "unique", "dup", "missing", "replace_all", "overlap"])
    if mode == "overlap":
        t = rng.choice([b"a", b"alpha", b"\r\n", b"\x01"])
        k = rng.randint(2, 3)
        old = t * k
        data = t * (k + 1) + b"z"
    if mode == "dup":
        data = data + old + data
    if mode == "missing":
        old = old + b"ZZZ"
    new = rng.choice([b"", b"X", b"".join(rng.choice(PLAIN) for _ in range(3))])
    if new == old:
        new = new + b"Y"
    d = newdir("e")
    path = os.path.join(d, "f.txt")
    with open(path, "wb") as f:
        f.write(data)
    cnt = data.count(old)
    rc, err, out = run("edit", d, {"path": "f.txt",
                                   "old_string": old.decode("utf-8", "surrogateescape"),
                                   "new_string": new.decode("utf-8", "surrogateescape"),
                                   "replace_all": mode == "replace_all"})
    after_bytes = open(path, "rb").read()
    binary = b"\x00" in data[:8192]
    ok, why = True, ""
    if binary:
        ok = rc != 0 and after_bytes == data and "binary" in err
        why = "binary refusal"
    elif mode == "missing" or cnt == 0:
        ok = rc != 0 and after_bytes == data and "not found" in err
        why = "missing -> no write"
    elif cnt > 1 and mode != "replace_all":
        ok = rc != 0 and after_bytes == data and "matches" in err
        why = "dup -> no write"
    else:
        want = data.replace(old, new)
        ok = rc == 0 and after_bytes == want
        if mode == "replace_all" and cnt > 1:
            ok = ok and ("x%d" % cnt) in out
        why = "exact replacement"
        if rc == 0 and not ok and after_bytes != want:
            for k in range(min(len(want), len(after_bytes))):
                if want[k] != after_bytes[k]:
                    why += " (first diff at %d)" % k
                    break
    if temp_residue(d):
        ok = False
        why += " + temp residue"
    results[("edit", mode, ok)] = results.get(("edit", mode, ok), 0) + 1
    if not ok:
        results.setdefault("failures", []).append(
            dict(tool="edit", mode=mode, data=data, old=old, new=new, rc=rc, err=err,
                 out=out, got=after_bytes,
                 want=(data.replace(old, new) if not binary else data), why=why))
    shutil.rmtree(d)


def case_write(rng, results):
    content = gen_data(rng, rng.randint(0, 8))
    mode = rng.choice(["create", "create", "overwrite", "refuse", "identical", "cap"])
    d = newdir("w")
    path = os.path.join(d, "t.bin")
    pre = None
    if mode not in ("create", "cap"):
        pre = gen_data(rng, rng.randint(1, 8))
        with open(path, "wb") as f:
            f.write(pre)
        os.chmod(path, rng.choice([0o600, 0o640, 0o644, 0o755]))
    before = stat_of(path) if pre is not None else None
    args = {"path": "t.bin", "content": content.decode("utf-8", "surrogateescape")}
    if mode == "overwrite":
        args["overwrite"] = True
    if mode == "identical":
        args["content"] = pre.decode("utf-8", "surrogateescape")
        args["overwrite"] = True
    if mode == "cap":
        args["content"] = "A" * 24577
    rc, err, out = run("write", d, args)
    exists = os.path.exists(path)
    got = open(path, "rb").read() if exists else None
    ok, why = True, ""
    if mode == "create":
        ok = rc == 0 and got == content and (os.stat(path).st_mode & 0o7777) == 0o644
        why = "create exact + 0644"
    elif mode == "cap":
        ok = rc != 0 and not exists and "content too large" in err
        why = "24 KiB cap refuses, no file"
    elif mode == "refuse":
        ok = rc != 0 and got == pre and stat_of(path) == before and "already exists" in err
        why = "refuse keeps bytes+inode+mtime"
    elif mode == "identical":
        ok = rc == 0 and got == pre and stat_of(path) == before and out.startswith("OK: unchanged")
        why = "identical is a no-op"
    else:
        ok = rc == 0 and got == content and (os.stat(path).st_mode & 0o7777) == before[2]
        why = "overwrite exact + mode preserved"
    if temp_residue(d):
        ok = False
        why += " + temp residue"
    results[("write", mode, ok)] = results.get(("write", mode, ok), 0) + 1
    if not ok:
        results.setdefault("failures", []).append(
            dict(tool="write", mode=mode, content=content, pre=pre, rc=rc, err=err,
                 out=out, got=got, before=before, why=why))
    shutil.rmtree(d)


def case_resolver(rng, results):
    d = newdir("r")
    outside = newdir("o")
    open(os.path.join(outside, "secret"), "wb").write(b"secret\n")
    open(os.path.join(d, "real"), "wb").write(b"x\n")
    os.symlink(os.path.join(outside, "secret"), os.path.join(d, "link"))
    rc, err, out = run("edit", d, {"path": "link", "old_string": "secret", "new_string": "public"})
    ok = rc != 0 and "symlink" in err and open(os.path.join(outside, "secret"), "rb").read() == b"secret\n"
    results[("resolver", "symlink leaf", ok)] = results.get(("resolver", "symlink leaf", ok), 0) + 1
    os.symlink(outside, os.path.join(d, "up"))
    rc, err, out = run("edit", d, {"path": "up/secret", "old_string": "secret", "new_string": "public"})
    ok = rc != 0 and open(os.path.join(outside, "secret"), "rb").read() == b"secret\n"
    results[("resolver", "symlink parent", ok)] = results.get(("resolver", "symlink parent", ok), 0) + 1
    rc, err, out = run("write", d, {"path": "a/../b.txt", "content": "v"})
    ok = rc == 0 and os.path.exists(os.path.join(d, "b.txt")) and not os.path.exists(os.path.join(d, "a"))
    results[("resolver", ".. collapse", ok)] = results.get(("resolver", ".. collapse", ok), 0) + 1
    rc, err, out = run("write", d, {"path": "nul.txt\u0000/../../escape", "content": "v"})
    NUL_PATH.append((rc, err, out, os.path.exists(os.path.join(d, "nul.txt"))))
    shutil.rmtree(d)
    shutil.rmtree(outside)


def main():
    n_edit = int(os.environ.get("MEONG_PROP_EDIT", "2000"))
    n_write = int(os.environ.get("MEONG_PROP_WRITE", "800"))
    rng = random.Random(20261121)
    results = {}
    t0 = time.time()
    for _ in range(n_edit):
        case_edit(rng, results)
    t1 = time.time()
    for _ in range(n_write):
        case_write(rng, results)
    t2 = time.time()
    case_resolver(rng, results)
    t3 = time.time()
    print("edit  %5d cases %6.2fs (%.1f ms/case)" % (n_edit, t1 - t0, 1000 * (t1 - t0) / n_edit))
    print("write %5d cases %6.2fs (%.1f ms/case)" % (n_write, t2 - t1, 1000 * (t2 - t1) / n_write))
    print("resolver    4 cases %6.3fs" % (t3 - t2))
    groups = {}
    for k, n in results.items():
        if k == "failures":
            continue
        tool, mode, ok = k
        g = groups.setdefault((tool, mode), [0, 0])
        g[0 if ok else 1] += n
    for k in sorted(groups):
        print("%-10s %-14s pass=%4d fail=%d" % (k[0], k[1], groups[k][0], groups[k][1]))
    fails = results.get("failures", [])
    print("failures:", len(fails))
    for f in fails[:4]:
        print(json.dumps({k: (v.decode("utf-8", "backslashreplace") if isinstance(v, bytes) else v)
                          for k, v in f.items()}, indent=1)[:1500])
    print("nul path:", NUL_PATH)
    shutil.rmtree(ROOT, ignore_errors=True)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
