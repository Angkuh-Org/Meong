#!/usr/bin/env python3
"""Phase 4 preflight F0-F4 for the mutation gate.

F0 identity, F1 catalogue, F2 streaming text, F3 1 KiB write exact SHA,
F4 2 KiB edit exact SHA. Streaming only; the paid routes are never selected.
F3/F4 force the tool choice because this is a transport/fragmentation check,
not a model-choice row.

Usage:
    python3 tools/mutation_preflight.py --model opencode-go/beam-free
"""
import argparse
import codecs
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.request
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("MEONG_BIN", os.path.join(ROOT, "build", "meong"))
PAIG = os.path.join(os.path.dirname(ROOT), "PAIG")
MODEL = "opencode-go/beam-free"
URL = "http://127.0.0.1:8899"
TOOLS_SHA = "73c4337c64dc556d655cc45aecae6f10e12bdf3c32a951dccbaf176ed0245c15"
TOOLS_BYTES = 2861
POLICY_SHA = "ab27852d088a02f0f3b0ae326d32b9bc6014991a566c4577c49853e87291ad79"
POLICY_BYTES = 359
OUT = os.path.join(ROOT, "docs", "research", "phase4",
                   "preflight-2026-11-21.json")


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def extract_c_string(path, name):
    src = open(path, "r", encoding="utf-8").read()
    i = src.index("=", src.index(name + "[]")) + 1
    parts = []
    while i < len(src):
        c = src[i]
        if c == '"':
            i += 1
            buf = []
            esc = False
            while i < len(src):
                c = src[i]
                if esc:
                    buf.append(c)
                    esc = False
                elif c == "\\":
                    buf.append(c)
                    esc = True
                elif c == '"':
                    break
                else:
                    buf.append(c)
                i += 1
            parts.append("".join(buf))
            i += 1
        elif c == ";":
            break
        else:
            i += 1
    return codecs.decode("".join(parts), "unicode_escape").encode("utf-8")


def long_line():
    seed = b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" \\ { }'
    assert len(seed) == 69
    line = (seed * 30)[:2040] + b"\t" + "€".encode() + "😀".encode()
    assert len(line) == 2048
    return line


def get_json(url, timeout=30):
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read()
        return r.status, dict(r.headers), json.loads(raw)


def post_stream(url, body, timeout=180):
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        url.rstrip("/") + "/v1/chat/completions", data=data,
        headers={"Content-Type": "application/json",
                 "Accept": "text/event-stream"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        status = r.status
        headers = dict(r.headers)
        ctype = headers.get("Content-Type", "")
        frames = 0
        text = []
        reasoning = []
        tools = {}
        usage = None
        finish = None
        first_text = None
        first_keys = None
        frag_count = 0
        max_frag = 0
        for raw in r:
            line = raw.decode("utf-8", "replace").strip()
            if not line.startswith("data:"):
                continue
            payload = line[5:].strip()
            if payload == "[DONE]":
                break
            try:
                obj = json.loads(payload)
            except json.JSONDecodeError:
                continue
            frames += 1
            if obj.get("usage"):
                usage = obj["usage"]
            for ch in obj.get("choices", []) or []:
                if ch.get("finish_reason"):
                    finish = ch["finish_reason"]
                delta = ch.get("delta", {}) or {}
                if delta.get("content"):
                    if first_text is None:
                        first_text = time.time() - t0
                    text.append(delta["content"])
                if delta.get("reasoning_content"):
                    reasoning.append(delta["reasoning_content"])
                if delta.get("reasoning"):
                    reasoning.append(delta["reasoning"])
                for tc in delta.get("tool_calls", []) or []:
                    frag_count += 1
                    idx = tc.get("index", 0)
                    slot = tools.setdefault(idx, {"id": None, "name": None,
                                                  "args": []})
                    if tc.get("id"):
                        slot["id"] = tc["id"]
                    if first_keys is None:
                        first_keys = sorted(tc.keys())
                    fn = tc.get("function", {}) or {}
                    if fn.get("name"):
                        slot["name"] = fn["name"]
                    frag = fn.get("arguments", "") or ""
                    max_frag = max(max_frag, len(frag))
                    if frag:
                        slot["args"].append(frag)
        return {
            "status": status,
            "content_type": ctype,
            "frames": frames,
            "text": "".join(text),
            "reasoning": "".join(reasoning),
            "tools": tools,
            "usage": usage,
            "finish": finish,
            "first_text_s": first_text,
            "first_fragment_keys": first_keys,
            "fragments": frag_count,
            "max_fragment": max_frag,
            "latency_s": round(time.time() - t0, 3),
        }


def f0():
    pid = None
    pid_path = os.path.expanduser("~/.paig/paig.pid")
    if os.path.exists(pid_path):
        pid = open(pid_path).read().strip()
    exe = None
    paig_sha = None
    if pid and os.path.exists("/proc/%s/exe" % pid):
        exe = os.readlink("/proc/%s/exe" % pid)
        paig_sha = sha256_file(exe)
    tree = {}
    for cmd in (["rev-parse", "HEAD"], ["status", "--porcelain"],
                ["diff", "--stat"]):
        try:
            r = subprocess.run(["git", "-C", PAIG] + cmd, capture_output=True,
                               text=True, timeout=10)
            tree[cmd[0]] = r.stdout.strip()
        except Exception as e:
            tree[cmd[0]] = "ERROR: %s" % e
    tools = extract_c_string(os.path.join(ROOT, "src", "tools", "tools.c"),
                             "TOOLS_JSON")
    policy = extract_c_string(os.path.join(ROOT, "src", "chat", "chat.c"),
                              "TOOL_POLICY")
    meong = {"path": BIN, "bytes": os.path.getsize(BIN),
             "sha256": sha256_file(BIN)}
    try:
        ver = subprocess.run([BIN, "--version"], capture_output=True,
                             text=True, timeout=5).stdout.strip()
    except Exception as e:
        ver = "ERROR: %s" % e
    out = {
        "meong": meong, "meong_version": ver,
        "paig_pid": pid, "paig_exe": exe, "paig_sha256": paig_sha,
        "paig_tree": tree,
        "tools_sha256": sha256_bytes(tools), "tools_bytes": len(tools),
        "tools_match": sha256_bytes(tools) == TOOLS_SHA and len(tools) == TOOLS_BYTES,
        "policy_sha256": sha256_bytes(policy), "policy_bytes": len(policy),
        "policy_match": sha256_bytes(policy) == POLICY_SHA and len(policy) == POLICY_BYTES,
    }
    out["ok"] = bool(out["tools_match"] and out["policy_match"] and paig_sha)
    return out, tools, policy


def f1(url):
    status, headers, body = get_json(url.rstrip("/") + "/v1/models")
    data = body.get("data", body if isinstance(body, list) else [])
    entry = next((m for m in data if m.get("id") == MODEL), None)
    norm = json.loads(json.dumps(body))
    for m in norm.get("data", norm if isinstance(norm, list) else []):
        if isinstance(m, dict):
            m["created"] = 0
    if isinstance(norm, dict) and isinstance(norm.get("data"), list):
        norm["data"] = sorted(norm["data"], key=lambda m: m.get("id", ""))
    nh = sha256_bytes(json.dumps(norm, sort_keys=True,
                                 separators=(",", ":")).encode())
    caps = (entry or {}).get("capabilities", {}) or {}
    out = {
        "status": status,
        "entries": len(data),
        "model": entry,
        "function_calling": caps.get("supports_function_calling"),
        "estimated": caps.get("estimated"),
        "normalized_sha256": nh,
        "ok": bool(status == 200 and entry and
                   caps.get("supports_function_calling") is True),
    }
    return out


def f2(url):
    body = {"model": MODEL, "stream": True, "max_tokens": 128,
            "messages": [{"role": "user",
                          "content": "Reply with the single word: pong"}]}
    r = post_stream(url, body, 60)
    r["ok"] = (r["status"] == 200 and
               "pong" in (r["text"] + " " + r["reasoning"]).lower())
    return r


def f3(url, tools, policy):
    content = "A" * 1024
    prompt = ("Use the `write` tool to create `preflight-write.txt` with "
              "content exactly the following 1,024 characters, copied "
              "byte-for-byte. Do not add a newline. Content:\n" + content)
    body = {"model": MODEL, "stream": True, "max_tokens": 2048,
            "messages": [{"role": "system", "content": policy.decode()},
                         {"role": "user", "content": prompt}],
            "tools": json.loads(tools),
            "tool_choice": {"type": "function",
                            "function": {"name": "write"}},
            "stream_options": {"include_usage": True}}
    r = post_stream(url, body, 180)
    slot = r["tools"].get(0, {})
    args = {}
    try:
        args = json.loads("".join(slot.get("args", [])))
    except Exception:
        pass
    r["tool_name"] = slot.get("name")
    r["args"] = args
    r["expected_sha256"] = sha256_bytes(content.encode())
    r["got_sha256"] = sha256_bytes(args.get("content", "").encode())
    r["ok"] = (r["status"] == 200 and slot.get("name") == "write" and
               args.get("content") == content)
    return r


def f4(url, tools, policy):
    line = b"A" * 2048
    prompt = ("Use the `edit` tool on `edit-long.txt` to replace the entire "
              "first line with the single line `REPLACED`. `old_string` must "
              "be exactly 2,048 capital A characters, copied byte-for-byte; "
              "do not include a trailing newline. The line is:\n" + line.decode())
    body = {"model": MODEL, "stream": True, "max_tokens": 8192,
            "messages": [{"role": "system", "content": policy.decode()},
                         {"role": "user", "content": prompt}],
            "tools": json.loads(tools),
            "tool_choice": {"type": "function",
                            "function": {"name": "edit"}},
            "stream_options": {"include_usage": True}}
    r = post_stream(url, body, 180)
    slot = r["tools"].get(0, {})
    args = {}
    try:
        args = json.loads("".join(slot.get("args", [])))
    except Exception:
        pass
    old = args.get("old_string", "")
    r["tool_name"] = slot.get("name")
    r["args"] = args
    r["expected_old_sha256"] = sha256_bytes(line)
    r["got_old_sha256"] = sha256_bytes(old.encode())
    r["old_bytes"] = len(old.encode())
    r["ok"] = (r["status"] == 200 and slot.get("name") == "edit" and
               old == line.decode() and args.get("new_string") == "REPLACED")
    return r


def main():
    global MODEL
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--url", default=URL)
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--skip-live", action="store_true")
    args = ap.parse_args()
    MODEL = args.model
    result = {"schema": "meong.mutation.preflight.v1",
              "utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
              "model": MODEL, "url": args.url}
    f0out, tools, policy = f0()
    result["f0"] = f0out
    print("F0 identity:", "ok" if f0out["ok"] else "FAIL",
          "meong", f0out["meong"]["bytes"], f0out["meong"]["sha256"][:12],
          "paig", (f0out["paig_sha256"] or "?")[:12],
          "tools", f0out["tools_bytes"], f0out["tools_sha256"][:12],
          "policy", f0out["policy_bytes"], f0out["policy_sha256"][:12])
    if args.skip_live:
        result["ok"] = f0out["ok"]
        json.dump(result, open(args.out, "w"), indent=1, sort_keys=True)
        return 0 if result["ok"] else 1
    f1out = f1(args.url)
    result["f1"] = f1out
    print("F1 catalogue:", "ok" if f1out["ok"] else "FAIL",
          "entries", f1out["entries"],
          "function_calling", f1out["function_calling"],
          "normalized", f1out["normalized_sha256"][:12])
    f2out = f2(args.url)
    result["f2"] = f2out
    print("F2 stream:", "ok" if f2out["ok"] else "FAIL",
          "status", f2out["status"], "frames", f2out["frames"],
          "text", repr(f2out["text"][:40]),
          "reasoning", repr(f2out["reasoning"][:40]),
          "latency", f2out["latency_s"])
    f3out = f3(args.url, tools, policy)
    result["f3"] = f3out
    print("F3 write 1 KiB:", "ok" if f3out["ok"] else "FAIL",
          "status", f3out["status"], "frags", f3out["fragments"],
          "max", f3out["max_fragment"], "sha", f3out["got_sha256"][:12])
    f4out = f4(args.url, tools, policy)
    result["f4"] = f4out
    print("F4 edit 2 KiB:", "ok" if f4out["ok"] else "FAIL",
          "status", f4out["status"], "frags", f4out["fragments"],
          "max", f4out["max_fragment"], "old", f4out["old_bytes"],
          "sha", f4out["got_old_sha256"][:12])
    costs = []
    for key in ("f2", "f3", "f4"):
        u = result[key].get("usage") or {}
        oc = u.get("opencode") or {}
        costs.append(oc.get("cost", u.get("cost", 0)))
    result["cost_ok"] = all(c in (0, None) for c in costs)
    result["costs"] = costs
    result["ok"] = bool(f0out["ok"] and f1out["ok"] and f2out["ok"] and
                        f3out["ok"] and f4out["ok"] and result["cost_ok"])
    json.dump(result, open(args.out, "w"), indent=1, sort_keys=True)
    print("cost_ok", result["cost_ok"], "costs", costs)
    print("PREFLIGHT", "ok" if result["ok"] else "FAIL", "->", args.out)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
