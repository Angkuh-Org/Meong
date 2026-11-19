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
    "mock/markdown",
    "mock/slow",
    "mock/huge",
    "mock/json200",
    "mock/slowerror",
    "mock/error",
    "mock/error200",
    "mock/reasononly",
    "mock/tinyctx",
    "mock/flaky",
    "mock/flaky-tmux",
    "mock/cache",
    "mock/nocache",
    "mock/json200cache",
    "mock/tools",
    "mock/tools-frag",
    "mock/tools-multi",
    "mock/tools-session",
    "mock/tools-think",
    "mock/tools-think-slow",
]
FLAKY = {}
FLAKY_LOCK = threading.Lock()
TITLE_HITS = {"count": 0}
TITLE_LOCK = threading.Lock()
CAPABILITIES = {
    "mock/split": {
        "context_window": 131072,
        "max_output_tokens": 65536,
        "supports_function_calling": True,
        "supports_reasoning": False,
        "supports_prompt_caching": True,
        "input_cost_per_token": 0,
        "output_cost_per_token": 0,
        "estimated": True,
    },
    "mock/tinyctx": {
        "context_window": 100,
    },
    "mock/tools": {
        "context_window": 131072,
        "max_output_tokens": 65536,
        "supports_function_calling": True,
        "supports_reasoning": False,
        "supports_prompt_caching": True,
        "input_cost_per_token": 0,
        "output_cost_per_token": 0,
        "estimated": True,
    },
    "mock/tools-frag": {
        "context_window": 131072,
        "max_output_tokens": 65536,
        "supports_function_calling": True,
        "supports_reasoning": False,
        "supports_prompt_caching": True,
        "input_cost_per_token": 0,
        "output_cost_per_token": 0,
        "estimated": True,
    },
    "mock/tools-multi": {
        "context_window": 131072,
        "max_output_tokens": 65536,
        "supports_function_calling": True,
        "supports_reasoning": False,
        "supports_prompt_caching": True,
        "input_cost_per_token": 0,
        "output_cost_per_token": 0,
        "estimated": True,
    },
    "mock/tools-think": {
        "context_window": 131072,
        "max_output_tokens": 65536,
        "supports_function_calling": True,
        "supports_reasoning": True,
        "supports_prompt_caching": True,
        "input_cost_per_token": 0,
        "output_cost_per_token": 0,
        "estimated": True,
    },
    "mock/tools-think-slow": {
        "context_window": 131072,
        "max_output_tokens": 65536,
        "supports_function_calling": True,
        "supports_reasoning": True,
        "supports_prompt_caching": True,
        "input_cost_per_token": 0,
        "output_cost_per_token": 0,
        "estimated": True,
    },
}
CAPABILITIES["mock/tools-session"] = dict(CAPABILITIES["mock/tools-multi"])
CHUNKS = [3, 11, 1, 7, 19, 5, 13, 2]
TOOL_CALL_ID = "call_1"
TOOL_NAME = "read_file"
TOOL_ARGS = '{"path":"/proj/version.txt"}'
TOOL_OK_ARGS = '{"path":"notes.md","start_line":1,"end_line":3}'
TOOL_DIR_ID = "call_2"
TOOL_DIR_NAME = "list_dir"
TOOL_DIR_ARGS = '{"path":"."}'
TOOL_OTHER_ARGS = '{"path":"/proj/other.txt"}'
TOOL_CONTRACT = {
    "mock/tools": [(TOOL_CALL_ID, TOOL_NAME)],
    "mock/tools-frag": [(TOOL_CALL_ID, TOOL_NAME)],
    "mock/tools-multi": [
        (TOOL_CALL_ID, TOOL_NAME),
        (TOOL_DIR_ID, TOOL_DIR_NAME),
    ],
    "mock/tools-session": [
        (TOOL_CALL_ID, TOOL_NAME),
        (TOOL_DIR_ID, TOOL_NAME),
    ],
    "mock/tools-think": [(TOOL_CALL_ID, TOOL_NAME)],
    "mock/tools-think-slow": [(TOOL_CALL_ID, TOOL_NAME)],
}
# mock/tools-frag ships TOOL_ARGS split into exactly these three deltas; the
# later two carry neither `index` nor `id`, so a client that only joins by
# index or id sees three calls, not one. See tool_accum.c.
TOOL_ARG_FRAGS = ['{"path":', '"/proj/', 'version.txt"}']


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


def usage_dict(model, prompt=42, completion=7):
    usage = {
        "prompt_tokens": prompt,
        "completion_tokens": completion,
        "total_tokens": prompt + completion,
    }
    if model in ("mock/cache", "mock/json200cache"):
        usage["prompt_tokens_details"] = {
            "cached_tokens": 30 if prompt == 42 else 8
        }
    return usage


def usage_obj(model):
    return {
        "id": "chatcmpl-mock",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": model,
        "choices": [],
        "usage": usage_dict(model),
    }


def is_title_request(req):
    for m in req.get("messages") or []:
        if m.get("role") == "system" and "write a title for this chat" in (
            m.get("content") or ""
        ).lower():
            return True
    return False


def tool_result_error(req, model):
    """Check every role:"tool" message against the model's call contract.

    Returns None when the tool turn is well formed, else a reason string that
    do_POST serves as HTTP 400 so a broken client fails loudly instead of
    silently passing the TUI test.
    """
    want = TOOL_CONTRACT.get(model) or [(TOOL_CALL_ID, TOOL_NAME)]
    msgs = req.get("messages") or []
    got = [m for m in msgs if m.get("role") == "tool"]
    has_calls = any(
        m.get("role") == "assistant" and m.get("tool_calls") for m in msgs
    )
    if not got and not has_calls:
        return None
    if len(got) != len(want):
        return "expected %d tool messages, got %d" % (len(want), len(got))
    for m, (call_id, name) in zip(got, want):
        if m.get("tool_call_id") != call_id:
            return "tool_call_id must be %r, got %r" % (
                call_id,
                m.get("tool_call_id"),
            )
        if m.get("name") != name:
            return "tool name must be %r, got %r" % (name, m.get("name"))
        if not m.get("content"):
            return "tool content must be non-empty"
    return None


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
        if self.path.rstrip("/") == "/stats":
            with TITLE_LOCK:
                self._json(200, {"title_hits": TITLE_HITS["count"]})
            return
        if self.path.rstrip("/") == "/v1/models":
            data = []
            for m in MODELS:
                entry = {
                    "id": m,
                    "object": "model",
                    "created": 1,
                    "owned_by": "mock",
                }
                if m in CAPABILITIES:
                    entry["capabilities"] = CAPABILITIES[m]
                data.append(entry)
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
        elif model == "mock/markdown":
            chunks = [
                "# Head",
                "ing\n\nThis is **bo",
                "ld**, *it",
                "alic* and `co",
                "de`.\n\n- one\n- t",
                "wo\n\n1. first\n2. second\n- [ ] todo\n- [x] done\n",
                "~~gone~~ text\n\n> quoted\n\n| name | age |\n|---",
                "|---|\n| meong | 9 |\n\n```c\n",
                "int x = 1;\n```\n\n[Docs](https://example.com)\n",
            ]
            frames = [
                sse_frame(chunk_obj(model, {"content": text})) for text in chunks
            ]
            frames += [
                sse_frame(chunk_obj(model, {}, "stop")),
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
        elif model == "mock/reasononly":
            frames = [
                sse_frame(
                    chunk_obj(model, {"reasoning_content": "thinking a little "})
                ),
                sse_frame(chunk_obj(model, {"reasoning_content": "with no answer"})),
                sse_frame(usage_obj(model)),
                b"data: [DONE]\n\n",
            ]
            payload = b"".join(frames)
        elif model in TOOL_CONTRACT:
            try:
                req = json.loads(body)
                answered = any(
                    m.get("role") == "tool" for m in req.get("messages") or []
                )
            except (ValueError, TypeError):
                answered = False
            if answered:
                text = ""
                for m in req.get("messages") or []:
                    if m.get("role") == "tool":
                        c = m.get("content")
                        text = c if isinstance(c, str) else json.dumps(c)
                final = (
                    "done with tools"
                    if model in ("mock/tools-think", "mock/tools-think-slow")
                    else "tool said: " + text
                )
                frames = [
                    sse_frame(chunk_obj(model, {"content": final})),
                    sse_frame(chunk_obj(model, {}, "stop")),
                    sse_frame(usage_obj(model)),
                    b"data: [DONE]\n\n",
                ]
            elif model in ("mock/tools-think", "mock/tools-think-slow"):
                frames = [
                    sse_frame(
                        chunk_obj(
                            model, {"reasoning_content": "thinking a little "}
                        )
                    ),
                    sse_frame(
                        chunk_obj(model, {"reasoning_content": "with tools"})
                    ),
                    sse_frame(
                        chunk_obj(
                            model,
                            {
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": TOOL_CALL_ID,
                                        "type": "function",
                                        "function": {
                                            "name": TOOL_NAME,
                                            "arguments": TOOL_OK_ARGS,
                                        },
                                    }
                                ]
                            },
                        )
                    ),
                    sse_frame(chunk_obj(model, {}, "tool_calls")),
                    sse_frame(usage_obj(model)),
                    b"data: [DONE]\n\n",
                ]
            elif model == "mock/tools-multi":
                frames = [
                    sse_frame(
                        chunk_obj(
                            model,
                            {
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": TOOL_CALL_ID,
                                        "type": "function",
                                        "function": {
                                            "name": TOOL_NAME,
                                            "arguments": TOOL_OK_ARGS,
                                        },
                                    },
                                    {
                                        "index": 1,
                                        "id": TOOL_DIR_ID,
                                        "type": "function",
                                        "function": {
                                            "name": TOOL_DIR_NAME,
                                            "arguments": TOOL_DIR_ARGS,
                                        },
                                    },
                                ]
                            },
                        )
                    ),
                    sse_frame(chunk_obj(model, {}, "tool_calls")),
                    sse_frame(usage_obj(model)),
                    b"data: [DONE]\n\n",
                ]
            elif model == "mock/tools-session":
                frames = [
                    sse_frame(
                        chunk_obj(
                            model,
                            {
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": TOOL_CALL_ID,
                                        "type": "function",
                                        "function": {
                                            "name": TOOL_NAME,
                                            "arguments": TOOL_ARGS,
                                        },
                                    },
                                    {
                                        "index": 1,
                                        "id": TOOL_DIR_ID,
                                        "type": "function",
                                        "function": {
                                            "name": TOOL_NAME,
                                            "arguments": TOOL_OTHER_ARGS,
                                        },
                                    },
                                ]
                            },
                        )
                    ),
                    sse_frame(chunk_obj(model, {}, "tool_calls")),
                    sse_frame(usage_obj(model)),
                    b"data: [DONE]\n\n",
                ]
            elif model == "mock/tools":
                frames = [
                    sse_frame(
                        chunk_obj(
                            model,
                            {
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": TOOL_CALL_ID,
                                        "type": "function",
                                        "function": {
                                            "name": TOOL_NAME,
                                            "arguments": TOOL_ARGS,
                                        },
                                    }
                                ]
                            },
                        )
                    ),
                    sse_frame(chunk_obj(model, {}, "tool_calls")),
                    sse_frame(usage_obj(model)),
                    b"data: [DONE]\n\n",
                ]
            else:
                frames = [
                    sse_frame(
                        chunk_obj(
                            model,
                            {
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": TOOL_CALL_ID,
                                        "type": "function",
                                        "function": {
                                            "name": TOOL_NAME,
                                            "arguments": TOOL_ARG_FRAGS[0],
                                        },
                                    }
                                ]
                            },
                        )
                    ),
                    # No index and no id on purpose: only a client that
                    # appends an unattributed fragment to the previous call
                    # folds this in; an index/id-keyed one opens a new slot.
                    sse_frame(
                        chunk_obj(
                            model,
                            {
                                "tool_calls": [
                                    {"function": {"arguments": TOOL_ARG_FRAGS[1]}}
                                ]
                            },
                        )
                    ),
                    sse_frame(
                        chunk_obj(
                            model,
                            {
                                "tool_calls": [
                                    {"function": {"arguments": TOOL_ARG_FRAGS[2]}}
                                ]
                            },
                        )
                    ),
                    sse_frame(chunk_obj(model, {}, "tool_calls")),
                    sse_frame(usage_obj(model)),
                    b"data: [DONE]\n\n",
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
            if model in (
                "mock/slow",
                "mock/slowerror",
                "mock/tools-think-slow",
            ):
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

        if not stream and is_title_request(req):
            with TITLE_LOCK:
                TITLE_HITS["count"] += 1
            # PAIG's opencode free route forces stream:true; answer with SSE
            # even though the request body asks for false.
            self._stream_simple(model, "mock chat title")
            return

        if model in TOOL_CONTRACT:
            err = tool_result_error(req, model)
            if err:
                self._json(
                    400,
                    {"error": {"message": "mock %s: %s" % (model, err)}},
                )
                return

        if model in ("mock/flaky", "mock/flaky-tmux"):
            with FLAKY_LOCK:
                attempt = FLAKY.get(model, 0) + 1
                FLAKY[model] = attempt
            if attempt == 1:
                self._json(
                    429,
                    {
                        "type": "error",
                        "error": {
                            "type": "MockFlaky",
                            "message": "mock flaky failure",
                        },
                    },
                )
                return
            if stream:
                self._stream_simple(model, "recovered after retry")
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
                                "content": "recovered after retry",
                            },
                            "finish_reason": "stop",
                        }
                    ],
                    "usage": {
                        "prompt_tokens": 12,
                        "completion_tokens": 3,
                    },
                },
            )
            return

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
        if stream and model not in ("mock/json200", "mock/json200cache"):
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
                "usage": usage_dict(model, 12, 3),
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
