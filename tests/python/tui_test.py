#!/usr/bin/env python3
"""Drive the ncurses UI through a pty against the mock gateway."""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

URL = sys.argv[1]
BIN = os.environ.get("MEONG_BIN", os.path.abspath("./build/meong"))
ENV = os.environ.copy()
ENV.setdefault("TERM", "xterm-256color")
ENV.setdefault("LANG", "C.UTF-8")
ENV.setdefault("LC_ALL", "C.UTF-8")

ANSI = re.compile(
    rb"\x1b(?:\[[0-9;?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\)|[()][A-Za-z0-9]|[=>])"
)
CTRL = re.compile(rb"[\x00-\x08\x0b\x0c\x0e-\x1f]")


def clean(data: bytes) -> bytes:
    return CTRL.sub(b"", ANSI.sub(b"", data))


class Session:
    def __init__(self, args, env_overrides=None):
        self.buf = b""
        self.status = None
        env = dict(ENV)
        if env_overrides:
            env.update(env_overrides)
        pid, fd = pty.fork()
        if pid == 0:
            fcntl.ioctl(
                0, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 110, 0, 0)
            )
            os.execvpe(BIN, [BIN] + args, env)
        self.pid = pid
        self.fd = fd

    def read(self, timeout=0.1):
        try:
            ready, _, _ = select.select([self.fd], [], [], timeout)
        except InterruptedError:
            return
        if not ready:
            return
        try:
            data = os.read(self.fd, 65536)
        except OSError:
            return
        if data:
            self.buf += data
            if len(self.buf) > 3_000_000:
                self.buf = self.buf[-1_500_000:]

    def wait_for(self, needle, timeout=10.0):
        if isinstance(needle, str):
            needle = needle.encode()
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in clean(self.buf):
                return True
            self.read(0.05)
        return False

    def mark(self):
        return len(self.buf)

    def wait_since(self, mark, needle, timeout=8.0):
        if isinstance(needle, str):
            needle = needle.encode()
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in clean(self.buf[mark:]):
                return True
            self.read(0.05)
        return False

    def wait_picker_highlight(self, timeout=5.0):
        pattern = re.compile(rb"\x1b\[[0-9;]*7m")
        deadline = time.time() + timeout
        while time.time() < deadline:
            idx = self.buf.rfind(b"choose a model")
            if idx >= 0 and pattern.search(self.buf[idx:]):
                return True
            self.read(0.05)
        return False

    def send(self, data):
        os.write(self.fd, data)

    def wait_exit(self, timeout=6.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid == self.pid:
                self.status = status
                return True
            self.read(0.05)
        return False

    def close(self):
        if self.status is None:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass
        try:
            os.close(self.fd)
        except OSError:
            pass

    def dump(self):
        text = clean(self.buf).decode("utf-8", "replace")
        return text[-8000:]


def fail(msg, *sessions):
    print(f"FAIL: {msg}", file=sys.stderr)
    for i, s in enumerate(sessions):
        print(f"--- session {i} tail ---", file=sys.stderr)
        print(s.dump(), file=sys.stderr)
    raise SystemExit(1)


def plain_session(model, needle, label, done_marker=None):
    s = Session(["--url", URL, "--model", model, "--no-mouse"])
    try:
        if not s.wait_for("ask anything"):
            fail(f"{label}: chat screen never appeared", s)
        s.send(b"hi\r")
        if not s.wait_for(needle, 10):
            fail(f"{label}: expected text never rendered", s)
        if done_marker and not s.wait_for(done_marker, 8):
            fail(f"{label}: stream never finished", s)
        s.send(b"/quit\r")
        if not s.wait_exit(6):
            fail(f"{label}: TUI did not exit", s)
        if s.status != 0:
            fail(f"{label}: exit status {s.status >> 8}", s)
        print(f"ok - tui {label}")
    finally:
        s.close()


def main():
    s = Session(["--url", URL, "--model", "mock/split", "--no-mouse"])
    try:
        if not s.wait_for("ask anything"):
            fail("the chat screen never appeared", s)
        s.send(b"hello\r")
        if not s.wait_for("ctx 42", 12):
            fail("stream never finished", s)
        s.send(b"/help\r")
        if not s.wait_for("commands", 5):
            fail("help overlay did not open", s)
        s.send(b"x")
        time.sleep(0.3)
        s.send(b"\t")
        if not s.wait_for("choose a model", 5):
            fail("model picker did not open", s)
        s.send(b"plain")
        time.sleep(0.3)
        s.send(b"\r")
        if not s.wait_for("model mock/plain", 5):
            fail("model selection did not apply", s)
        s.send(b"/quit\r")
        if not s.wait_exit(6):
            fail("TUI did not exit on /quit", s)
        if s.status != 0:
            fail(f"TUI exit status {s.status >> 8}", s)
        print("ok - tui stream, help, picker, quit")
    finally:
        s.close()

    s = Session(["--url", URL, "--model", "mock/slow", "--no-mouse"])
    try:
        if not s.wait_for("ask anything"):
            fail("slow chat screen never appeared", s)
        s.send(b"go\r")
        if not s.wait_for("slow", 8):
            fail("slow stream never produced a token", s)
        s.send(b"\x03")
        if not s.wait_for("stopped", 5):
            fail("ctrl-c did not cancel the generation", s)
        s.send(b"/quit\r")
        if not s.wait_exit(6):
            fail("TUI did not exit after cancel", s)
        if s.status != 0:
            fail(f"TUI exit status after cancel {s.status >> 8}", s)
        print("ok - tui ctrl-c cancel")
    finally:
        s.close()

    s = Session(["--url", URL, "--model", "mock/slow", "--no-mouse"])
    try:
        if not s.wait_for("ask anything"):
            fail("queue chat screen never appeared", s)
        s.send(b"first\r")
        if not s.wait_for("generating", 8):
            fail("queue: slow stream never started", s)
        s.send(b"second\r")
        if not s.wait_for("queue ↓", 5):
            fail("queue: queued prompt is not shown", s)
        if not s.wait_for("◑ second", 12):
            fail("queue: queued prompt was not sent", s)
        s.send(b"/quit\r")
        if not s.wait_exit(8):
            fail("queue TUI did not exit", s)
        if s.status != 0:
            fail(f"queue TUI exit status {s.status >> 8}", s)
        print("ok - tui queue while streaming")
    finally:
        s.close()

    s = Session(["--url", URL, "--model", "mock/slow", "--no-mouse"])
    try:
        if not s.wait_for("ask anything"):
            fail("cancel queue chat screen never appeared", s)
        s.send(b"go\r")
        if not s.wait_for("generating", 8):
            fail("cancel queue: slow stream never started", s)
        s.send(b"second\r")
        if not s.wait_for("queue ↓", 5):
            fail("cancel queue: queued prompt is not shown", s)
        s.send(b"\x03")
        if not s.wait_for("stopped", 5):
            fail("cancel queue: ctrl-c did not cancel", s)
        if not s.wait_for("queue ↓", 3):
            fail("cancel queue: queue vanished after cancel", s)
        s.send(b"\r")
        if not s.wait_for("◑ second", 12):
            fail("cancel queue: enter did not send the queued prompt", s)
        s.send(b"/quit\r")
        if not s.wait_exit(8):
            fail("cancel queue TUI did not exit", s)
        if s.status != 0:
            fail(f"cancel queue TUI exit status {s.status >> 8}", s)
        print("ok - tui ctrl-c keeps the queue for the next enter")
    finally:
        s.close()

    s = Session(["--url", URL, "--model", "mock/slowerror", "--no-mouse"])
    try:
        if not s.wait_for("ask anything"):
            fail("failing queue chat screen never appeared", s)
        s.send(b"first\r")
        if not s.wait_for("generating", 8):
            fail("failing queue: stream never started", s)
        s.send(b"second\r")
        if not s.wait_for("queue ↓", 5):
            fail("failing queue: queued prompt is not shown", s)
        if not s.wait_for("mock queue failure", 12):
            fail("failing queue: the error never arrived", s)
        time.sleep(0.3)
        for _ in range(10):
            s.read(0.05)
        tail = clean(s.buf)
        tail = tail[tail.rfind(b"mock queue failure"):]
        if b"generating" in tail:
            fail("failing queue: failed reply drained the queue", s)
        s.send(b"\x03")
        if not s.wait_exit(6):
            fail("failing queue TUI did not exit", s)
        if s.status != 0:
            fail(f"failing queue TUI exit status {s.status >> 8}", s)
        print("ok - tui failed reply keeps the queue")
    finally:
        s.close()

    plain_session("mock/error200", "mock says no", "200 json error")
    plain_session("mock/json200", "pong from mock", "raw json fallback", "ctx 12")

    s = Session(["--url", URL, "--model", "mock/huge", "--no-mouse"])
    try:
        if not s.wait_for("ask anything"):
            fail("huge chat screen never appeared", s)
        s.send(b"first\r")
        if not s.wait_for("ctx 42", 15):
            fail("huge reply never finished", s)
        s.send(b"second\r")
        if not s.wait_for("trimmed ok", 10):
            fail("context cap did not send the trim note", s)
        s.send(b"/quit\r")
        if not s.wait_exit(6):
            fail("huge TUI did not exit", s)
        if s.status != 0:
            fail(f"huge TUI exit status {s.status >> 8}", s)
        print("ok - tui context trim")
    finally:
        s.close()

    s = Session(["--url", URL, "--model", "mock/plain", "--no-mouse"])
    try:
        if not s.wait_for("ask anything"):
            fail("commands chat screen never appeared", s)
        s.send(b"/system foo\r")
        if not s.wait_for("system prompt set (3 bytes)", 5):
            fail("system set note missing", s)
        s.send(b"/system\r")
        if not s.wait_for("system: foo", 5):
            fail("system show note missing", s)
        s.send(b"/system off\r")
        if not s.wait_for("system prompt cleared", 5):
            fail("system clear note missing", s)
        s.send(b"hi\r")
        if not s.wait_for("pong from mock", 10):
            fail("basic reply missing", s)
        if not s.wait_for("ctx 42", 10):
            fail("basic reply never finished", s)
        mark = s.mark()
        s.send(b"\x10")
        s.send(b"\r")
        if not s.wait_since(mark, "pong from mock", 10):
            fail("history recall did not resend the prompt", s)
        mark = s.mark()
        s.send(b"/clear\r")
        if not s.wait_since(mark, "talk to anything behind PAIG", 5):
            fail("/clear did not return to the splash", s)
        mark = s.mark()
        s.send(b"/retry\r")
        if not s.wait_since(mark, "nothing to retry", 5):
            fail("/retry did not report an empty conversation", s)
        s.send(b"/quit\r")
        if not s.wait_exit(6):
            fail("commands TUI did not exit", s)
        if s.status != 0:
            fail(f"commands TUI exit status {s.status >> 8}", s)
        print("ok - tui system, clear, retry, history")
    finally:
        s.close()

    s = Session(
        ["--url", URL, "--model", "mock/split", "--no-mouse"],
        env_overrides={"LC_ALL": "C", "LANG": "C"},
    )
    try:
        if not s.wait_for("ask anything"):
            fail("C-locale chat screen never appeared", s)
        if b"+---" not in clean(s.buf):
            fail("C locale did not fall back to ASCII chrome", s)
        s.send(b"/quit\r")
        if not s.wait_exit(6):
            fail("C-locale TUI did not exit", s)
        if s.status != 0:
            fail(f"C-locale TUI exit status {s.status >> 8}", s)
        print("ok - tui C locale falls back to ascii")
    finally:
        s.close()

    s = Session(
        [
            "--url",
            URL,
            "--model",
            "mock/split",
            "--ascii",
            "--no-color",
            "--no-mouse",
        ]
    )
    try:
        if not s.wait_for("ask anything"):
            fail("ascii chat screen never appeared", s)
        if b"+---" not in clean(s.buf):
            fail("ascii borders missing", s)
        s.send(b"\t")
        if not s.wait_for("choose a model"):
            fail("ascii model picker did not open", s)
        if not s.wait_picker_highlight():
            fail("no-color picker selection is not highlighted", s)
        s.send(b"\r")
        time.sleep(0.2)
        s.send(b"/quit\r")
        if not s.wait_exit(6):
            fail("ascii TUI did not exit", s)
        if s.status != 0:
            fail(f"ascii TUI exit status {s.status >> 8}", s)
        print("ok - tui ascii/no-color picker")
    finally:
        s.close()


if __name__ == "__main__":
    main()
