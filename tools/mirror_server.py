#!/usr/bin/env python3
"""Serve the NiteKernel driver store over HTTP.

QEMU's user-mode networking exposes the host as 10.0.2.2, so a guest that
boots with -netdev user can fetch drivers from http://10.0.2.2:8080/payloads/.

Usage: mirror_server.py <driver_store_dir> [port]
"""
import http.server
import os
import socketserver
import sys


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "build/drivers"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    root = os.path.abspath(root)
    if not os.path.isdir(root):
        raise SystemExit("no such directory: %s" % root)

    handler = lambda *a, **k: http.server.SimpleHTTPRequestHandler(*a, directory=root, **k)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("0.0.0.0", port), handler) as httpd:
        print("driver mirror serving %s on http://0.0.0.0:%d" % (root, port), flush=True)
        httpd.serve_forever()


if __name__ == "__main__":
    main()
