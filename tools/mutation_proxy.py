#!/usr/bin/env python3
"""Raw TCP pass-through proxy for the Phase 4 mutation gate.

Forwards bytes unchanged in both directions and writes one raw request and one
raw response file per connection. The harness parses HTTP/SSE after the run;
the proxy never rewrites the request body.

Usage:
    python3 tools/mutation_proxy.py --upstream 127.0.0.1:8899 \
        --log /tmp/run/proxy --port-file /tmp/run/proxy.port
"""
import argparse
import os
import select
import socket
import socketserver
import threading


class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, upstream, log_dir):
        super().__init__(addr, Handler)
        self.upstream = upstream
        self.log_dir = log_dir
        self.counter = 0
        self.lock = threading.Lock()

    def next_id(self):
        with self.lock:
            self.counter += 1
            return self.counter


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        n = self.server.next_id()
        req_path = os.path.join(self.server.log_dir, "conn-%04d.req" % n)
        resp_path = os.path.join(self.server.log_dir, "conn-%04d.resp" % n)
        client = self.request
        try:
            upstream = socket.create_connection(self.server.upstream, timeout=10)
        except OSError:
            return
        req = open(req_path, "wb")
        resp = open(resp_path, "wb")
        try:
            while True:
                ready, _, _ = select.select([client, upstream], [], [], 15)
                if not ready:
                    break
                for s in ready:
                    data = s.recv(65536)
                    if not data:
                        return
                    if s is client:
                        req.write(data)
                        req.flush()
                        upstream.sendall(data)
                    else:
                        resp.write(data)
                        resp.flush()
                        client.sendall(data)
        finally:
            req.close()
            resp.close()
            try:
                upstream.close()
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--port-file", required=True)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()
    host, port = args.upstream.rsplit(":", 1)
    os.makedirs(args.log, exist_ok=True)
    srv = Proxy((args.host, 0), (host, int(port)), args.log)
    tmp = args.port_file + ".tmp"
    with open(tmp, "w") as f:
        f.write(str(srv.server_address[1]))
    os.replace(tmp, args.port_file)
    print("proxy listening on %d -> %s:%s" % (srv.server_address[1], host, port),
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
