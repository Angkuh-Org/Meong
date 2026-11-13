#!/usr/bin/env python3
"""Loopback SSE emitter for Meong perf runs.

Streams a configurable number of content bytes as small OpenAI-compatible
chat.completion.chunk events, then a usage chunk and [DONE].  The frame shape
is deliberately minimal: Meong only reads choices[0].delta and usage, so the
benchmark measures client work rather than mock bookkeeping.
"""
import argparse
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


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


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    payload = b""
    block = 65536

    def log_message(self, fmt, *args):
        pass

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        if n:
            self.rfile.read(n)
        if self.path.rstrip("/") != "/v1/chat/completions":
            body = b'{"error":{"message":"not found"}}'
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            p = self.payload
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
        body = b'{"object":"list","data":[{"id":"bench/stream"}]}'
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
    args = ap.parse_args()
    if args.delta <= 0:
        ap.error("--delta must be positive")
    payload, events = build_payload(args.bytes, args.delta,
                                    args.tag.encode())
    Handler.payload = payload
    Handler.block = args.block
    srv = Server(("127.0.0.1", args.port), Handler)
    port = srv.server_address[1]
    if args.port_file:
        tmp = args.port_file + ".tmp"
        with open(tmp, "w") as f:
            f.write(str(port))
        os.replace(tmp, args.port_file)
    print("bench_sse: %d bytes, %d events, port %d"
          % (args.bytes, events, port), file=sys.stderr, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
