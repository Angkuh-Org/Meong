#!/usr/bin/env python3
"""Tiny PAIG-shaped mock for Meong's offline tests."""
import argparse
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODELS = [
    "mock/plain",
    "mock/split",
    "mock/slow",
    "mock/huge",
    "mock/json200",
    "mock/slowerror",
    "mock/error",
    "mock/error200",
]
CHUNKS = [3, 11, 1, 7, 19, 5, 13, 2]


def sse_frame(obj):
    return b"data: " + json.dumps(obj, separators=(",", ":")).encode() + b"\n\n"


def chunk_obj(model, delta, finish=None):
    return {
        "id": "chatcmpl-mock",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": model,
        "choices": [
            {
                "index": 0,
                "delta": delta,
                "finish_reason": finish,
            }
        ],
    }


def usage_obj(model):
    return {
        "id": "chatcmpl-mock",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": model,
        "choices": [],
        "usage": {
            "prompt_tokens": 42,
            "completion_tokens": 7,
            "total_tokens": 49,
        },
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _json(self, status, payload):
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        if self.path == "/health":
            self._json(200, {"ok": True, "service": "mock-paig"})
            return
        if self.path.rstrip("/") == "/v1/models":
            data = [
                {"id": m, "object": "model", "created": 1, "owned_by": "mock"}
                for m in MODELS
            ]
            self._json(200, {"object": "list", "data": data})
            return
        self._json(404, {"error": {"message": "not found"}})

    def _chunk(self, data):
        self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
        self.wfile.flush()

    def _stream_simple(self, model, text):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        payload = b"".join(
            [
                sse_frame(chunk_obj(model, {"content": text})),
                sse_frame(usage_obj(model)),
                b"data: [DONE]\n\n",
            ]
        )
        try:
            self._chunk(payload)
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _stream(self, model, body):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache, no-transform")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        if model == "mock/split":
            frames = [
                sse_frame(
                    chunk_obj(model, {"reasoning_content": "thinking a little "})
                ),
                sse_frame(chunk_obj(model, {"content": "Hello, "})),
                sse_frame(chunk_obj(model, {"content": "Meong 🌊🐟"})),
                sse_frame(
                    chunk_obj(
                        model,
                        {"content": " done.\n\n```c\nint x = 1;\n```\n"},
                        "stop",
                    )
                ),
                sse_frame(usage_obj(model)),
                b"data: [DONE]\n\n",
            ]
            payload = b"".join(frames)
        elif model == "mock/slow":
            try:
                req = json.loads(body)
                second = any(
                    m.get("role") == "user" and m.get("content") == "second"
                    for m in req.get("messages", [])
                )
            except (ValueError, TypeError):
                second = False
            if second:
                frames = [
                    sse_frame(chunk_obj(model, {"content": "queued reply"})),
                    sse_frame(usage_obj(model)),
                    b"data: [DONE]\n\n",
                ]
            else:
                frames = [
                    sse_frame(chunk_obj(model, {"content": "slow "})),
                    sse_frame(chunk_obj(model, {"content": "token "})),
                    sse_frame(chunk_obj(model, {"content": "stream"})),
                    sse_frame(usage_obj(model)),
                    b"data: [DONE]\n\n",
                ]
            payload = b"".join(frames)
        elif model == "mock/slowerror":
            frames = [
                sse_frame(chunk_obj(model, {"content": "slow "})),
                sse_frame(chunk_obj(model, {"content": "error "})),
                b'data: {"error":{"message":"mock queue failure"}}\n\n',
            ]
            payload = b"".join(frames)
        else:
            payload = b"".join(
                [
                    sse_frame(chunk_obj(model, {"role": "assistant", "content": ""})),
                    sse_frame(chunk_obj(model, {"content": "pong from mock"})),
                    sse_frame(usage_obj(model)),
                    b"data: [DONE]\n\n",
                ]
            )

        try:
            if model in ("mock/slow", "mock/slowerror"):
                for frame in frames:
                    self._chunk(frame)
                    time.sleep(0.50)
            else:
                i = 0
                n = 0
                while i < len(payload):
                    size = CHUNKS[n % len(CHUNKS)]
                    self._chunk(payload[i : i + size])
                    i += size
                    n += 1
                    time.sleep(0.002)
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        if self.path.rstrip("/") != "/v1/chat/completions":
            self._json(404, {"error": {"message": "not found"}})
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        try:
            req = json.loads(raw)
        except json.JSONDecodeError:
            self._json(400, {"error": {"message": "bad json"}})
            return
        model = req.get("model", "")
        stream = bool(req.get("stream"))

        if model == "mock/huge":
            msgs = req.get("messages", [])
            trimmed = any(
                m.get("role") == "system"
                and "Earlier turns" in m.get("content", "")
                for m in msgs
            )
            second = any(
                m.get("role") == "user" and m.get("content") == "second"
                for m in msgs
            )
            blob = any(
                m.get("role") == "assistant"
                and len(m.get("content", "")) > 1000
                for m in msgs
            )
            content = (
                "trimmed ok" if trimmed and second and not blob else "x" * 260000
            )
            if stream:
                self._stream_simple(model, content)
                return
            self._json(
                200,
                {
                    "id": "chatcmpl-mock",
                    "object": "chat.completion",
                    "model": model,
                    "choices": [
                        {
                            "index": 0,
                            "message": {
                                "role": "assistant",
                                "content": content,
                            },
                            "finish_reason": "stop",
                        }
                    ],
                    "usage": {
                        "prompt_tokens": 42,
                        "completion_tokens": 7,
                    },
                },
            )
            return

        if model == "mock/error":
            self._json(
                429,
                {
                    "type": "error",
                    "error": {
                        "type": "MockLimit",
                        "message": "mock says no",
                    },
                },
            )
            return
        if model == "mock/error200":
            self._json(
                200,
                {
                    "type": "error",
                    "error": {"type": "MockError", "message": "mock says no"},
                },
            )
            return
        if stream and model != "mock/json200":
            self._stream(model, raw)
            return
        self._json(
            200,
            {
                "id": "chatcmpl-mock",
                "object": "chat.completion",
                "created": int(time.time()),
                "model": model,
                "choices": [
                    {
                        "index": 0,
                        "message": {
                            "role": "assistant",
                            "content": "pong from mock",
                        },
                        "finish_reason": "stop",
                    }
                ],
                "usage": {
                    "prompt_tokens": 12,
                    "completion_tokens": 3,
                    "total_tokens": 15,
                },
            },
        )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--port-file")
    args = ap.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    port = server.server_address[1]
    if args.port_file:
        tmp = args.port_file + ".tmp"
        with open(tmp, "w") as f:
            f.write(str(port))
        os.replace(tmp, args.port_file)
    print(f"mock listening on {port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
