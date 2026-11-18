#!/usr/bin/env python3
"""Loopback SSE emitter for Meong perf runs.

Content mode (the default) streams --bytes of content in --delta-byte pieces
as small OpenAI-compatible chat.completion.chunk events, then a usage chunk
and [DONE].  The frame shape is deliberately minimal: Meong only reads
choices[0].delta and usage, so the benchmark measures client work rather than
mock bookkeeping.

Tool mode (--tool) streams one tool round instead: a read_file call opens with
id+name, its arguments arrive as --frag-byte fragments, then a finish frame, a
usage chunk and [DONE].  The defaults stream 48 KiB of arguments in 776
fragment deltas (779 data frames with the opening, finish and usage frames).
Only the opening frame carries a non-empty id/name; an argument-only fragment
is what jdelta_scan() accepts on its fast path, so a fragment that gains an
id/name silently measures the slow path instead.  Keep that split, and quote
the printed frame count beside any instruction count.

The round needs a TUI run: --oneshot never sends tools and ignores tool_calls.
--path points read_file at a file outside the working directory, so Meong asks
to allow it unless --yolo is set.  A missing --path only turns the tool result
into an error; the scan work is the same.  A request whose messages already
carry a role:"tool" message is answered with a short text turn so the round
completes instead of looping on tool calls; a tool turn that does not carry
exactly one read_file result for call_1 is refused with HTTP 400, because a
bench that accepts a broken fragment join grades it green.
"""
import argparse
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

STREAM_MODEL = "bench/stream"
TOOL_MODEL = "bench/tool"
TOOL_CALL_ID = "call_1"
TOOL_NAME = "read_file"
TOOL_DEFAULT_PATH = "/tmp/toolbench.txt"
# Opening fragment sizes: providers rarely split cleanly on the first few
# deltas.  Everything after this prefix is exactly --frag bytes.
TOOL_FRAG_PREFIX = (3, 11, 1, 7, 19, 5, 13, 2)


def build_payload(total, delta, tag):
    if len(tag) != 1:
        raise SystemExit("--tag must be one byte")
    frame = (b'data: {"choices":[{"delta":{"content":"' + tag * delta +
             b'"}}]}\n\n')
    full = total // delta
    rem = total % delta
    parts = [frame * full]
    if rem:
        parts.append(b'data: {"choices":[{"delta":{"content":"' +
                     tag * rem + b'"}}]}\n\n')
    events = full + (1 if rem else 0)
    parts.append(('data: {"choices":[],"usage":{"prompt_tokens":1,'
                  '"completion_tokens":%d}}\n\n' % events).encode())
    parts.append(b"data: [DONE]\n\n")
    return b"".join(parts), events


def tool_frame(model, delta, finish=None):
    obj = {
        "id": "chatcmpl-bench",
        "object": "chat.completion.chunk",
        "created": 1,
        "model": model,
        "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
    }
    body = json.dumps(obj, separators=(",", ":")).encode()
    return b"data: " + body + b"\n\n"


def build_tool_payload(pad, frag, tag, path):
    if len(tag) != 1:
        raise SystemExit("--tag must be one byte")
    args = ('{"path":' + json.dumps(path) + ',"pad":"' +
            tag.decode() * pad + '"}')
    parts = [
        tool_frame(TOOL_MODEL, {
            "tool_calls": [{
                "index": 0,
                "id": TOOL_CALL_ID,
                "type": "function",
                "function": {"name": TOOL_NAME, "arguments": ""},
            }]
        }),
    ]
    pieces = []
    off = 0
    while off < len(args):
        n = frag
        if len(pieces) < len(TOOL_FRAG_PREFIX):
            n = TOOL_FRAG_PREFIX[len(pieces)]
        n = min(n, len(args) - off)
        piece = args[off:off + n]
        pieces.append(piece)
        off += n
        parts.append(tool_frame(TOOL_MODEL, {
            "tool_calls": [{"index": 0, "function": {"arguments": piece}}]
        }))
    if "".join(pieces) != args:
        raise SystemExit("tool fragments lost arguments")
    parts.append(tool_frame(TOOL_MODEL, {}, "tool_calls"))
    parts.append((
        b'data: {"choices":[],"usage":{"prompt_tokens":10,'
        b'"completion_tokens":%d}}\n\n' % len(pieces)))
    parts.append(b"data: [DONE]\n\n")
    return b"".join(parts), len(args), len(pieces)


def build_tool_followup():
    parts = [
        tool_frame(TOOL_MODEL, {"content": "tool said: ok"}),
        tool_frame(TOOL_MODEL, {}, "stop"),
        b'data: {"choices":[],"usage":{"prompt_tokens":12,'
        b'"completion_tokens":3}}\n\n',
        b"data: [DONE]\n\n",
    ]
    return b"".join(parts)


def classify_turn(raw):
    """Return (kind, problem) for a tool-mode request.

    kind is "round" for the opening request, "followup" once the client has
    sent the tool result, and "bad" when the turn carries tool state that the
    harness cannot grade.  A broken fragment join shows up here as the wrong
    number of tool messages, not as a silent second round.
    """
    try:
        req = json.loads(raw)
    except ValueError:
        return "round", None
    if not isinstance(req, dict):
        return "round", None
    msgs = req.get("messages")
    if not isinstance(msgs, list):
        return "round", None
    got = [m for m in msgs
           if isinstance(m, dict) and m.get("role") == "tool"]
    has_calls = any(
        isinstance(m, dict) and m.get("role") == "assistant" and
        m.get("tool_calls")
        for m in msgs
    )
    if not got and not has_calls:
        return "round", None
    if len(got) != 1:
        return "bad", "expected 1 tool message, got %d" % len(got)
    m = got[0]
    if m.get("tool_call_id") != TOOL_CALL_ID:
        return "bad", "tool_call_id must be %r, got %r" % (
            TOOL_CALL_ID, m.get("tool_call_id"))
    if m.get("name") != TOOL_NAME:
        return "bad", "tool name must be %r, got %r" % (
            TOOL_NAME, m.get("name"))
    if not m.get("content"):
        return "bad", "tool content must be non-empty"
    return "followup", None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    payload = b""
    followup = b""
    model = STREAM_MODEL
    tools = False
    block = 65536

    def log_message(self, fmt, *args):
        pass

    def _json(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        raw = self.rfile.read(n) if n else b""
        if self.path.rstrip("/") != "/v1/chat/completions":
            self._json(404, b'{"error":{"message":"not found"}}')
            return
        payload = self.payload
        if self.followup:
            kind, problem = classify_turn(raw)
            if problem:
                body = json.dumps(
                    {"error": {"message": "bench: " + problem}},
                    separators=(",", ":")).encode()
                self._json(400, body)
                return
            if kind == "followup":
                payload = self.followup
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            p = payload
            for i in range(0, len(p), self.block):
                chunk = p[i:i + self.block]
                self.wfile.write(b"%x\r\n" % len(chunk))
                self.wfile.write(chunk)
                self.wfile.write(b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        entry = {"id": self.model, "object": "model", "owned_by": "bench"}
        if self.tools:
            entry["capabilities"] = {"supports_function_calling": True}
        body = json.dumps({"object": "list", "data": [entry]},
                          separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


class Server(HTTPServer):
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--port-file")
    ap.add_argument("--bytes", type=int, default=1 << 20)
    ap.add_argument("--delta", type=int, default=64)
    ap.add_argument("--tag", default="x")
    ap.add_argument("--block", type=int, default=65536)
    ap.add_argument("--tool", action="store_true",
                    help="stream one tool round instead of content")
    ap.add_argument("--pad", type=int,
                    help="tool mode: argument padding bytes (default 49152)")
    ap.add_argument("--frag", type=int,
                    help="tool mode: fragment bytes (default 64)")
    ap.add_argument("--path",
                    help="tool mode: read_file path (default %s)"
                         % TOOL_DEFAULT_PATH)
    args = ap.parse_args()
    if args.block <= 0:
        ap.error("--block must be positive")
    tag = args.tag.encode()
    if args.tool:
        pad = 49152 if args.pad is None else args.pad
        frag = 64 if args.frag is None else args.frag
        path = TOOL_DEFAULT_PATH if args.path is None else args.path
        if pad < 0:
            ap.error("--pad must be non-negative")
        if frag <= 0:
            ap.error("--frag must be positive")
        if not path:
            ap.error("--path must not be empty")
        payload, args_len, nfrags = build_tool_payload(pad, frag, tag, path)
        Handler.payload = payload
        Handler.followup = build_tool_followup()
        Handler.model = TOOL_MODEL
        Handler.tools = True
    else:
        if args.pad is not None or args.frag is not None or \
                args.path is not None:
            ap.error("--pad/--frag/--path only apply with --tool")
        if args.delta <= 0:
            ap.error("--delta must be positive")
        payload, events = build_payload(args.bytes, args.delta, tag)
        Handler.payload = payload
        Handler.model = STREAM_MODEL
    Handler.block = args.block
    srv = Server(("127.0.0.1", args.port), Handler)
    port = srv.server_address[1]
    if args.port_file:
        tmp = args.port_file + ".tmp"
        with open(tmp, "w") as f:
            f.write(str(port))
        os.replace(tmp, args.port_file)
    if args.tool:
        print("bench_sse: tool round, args %d bytes, %d fragments "
              "(%d frames), port %d"
              % (args_len, nfrags, nfrags + 3, port),
              file=sys.stderr, flush=True)
    else:
        print("bench_sse: %d bytes, %d events, port %d"
              % (args.bytes, events, port), file=sys.stderr, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
