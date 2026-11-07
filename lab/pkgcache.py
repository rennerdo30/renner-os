#!/usr/bin/env python3
"""Caching pacman mirror for the test lab.

Listens on 127.0.0.1 only. QEMU guests reach it as http://10.0.2.2:<port>:

    Server = http://10.0.2.2:8079/$repo/os/$arch

- Package files (*.pkg.tar.*, and their .sig files) are immutable per file
  name, so they are cached forever. A miss is streamed to the client and
  written to the cache at the same time, so pacman never waits for a full
  download before the first byte.
- Repository databases (*.db, *.files and their signatures) are always
  fetched fresh, so updates stay visible.
- With --files, the repo's install/ and system/ folders are served under
  /renner/, so a guest can run the real `curl ... | bash` installer.
- pacman checks every package signature itself, so the cache cannot
  smuggle anything in.

The server stops itself after --minutes, or on GET /_lab/quit?token=<token>.
"""

from __future__ import annotations

import argparse
import os
import re
import secrets
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# Checked after URL-decoding. ':' appears in package names with an epoch
# (wpa_supplicant-2:2.12-1); it is stored as %3A because Windows forbids ':'.
SAFE_PATH = re.compile(r"^/[A-Za-z0-9._+@~:-]+(/[A-Za-z0-9._+@~:-]+)*$")
PKG_FILE = re.compile(r"\.pkg\.tar\.(zst|xz|gz)(\.sig)?$")
CHUNK = 1 << 20


class Cache:
    def __init__(self, root: Path, upstream: str, token: str, log):
        self.root = root
        self.upstream = upstream.rstrip("/")
        self.token = token
        self.log = log
        self.stats = {"hit": 0, "miss": 0, "pass": 0, "error": 0, "file": 0}
        self.files: Path | None = None
        self.lock = threading.Lock()
        self.stop = threading.Event()

    def count(self, kind: str) -> None:
        with self.lock:
            self.stats[kind] += 1


SOURCE_DIRS = ("installer", "install", "system")


def source_tar(repo: Path) -> bytes:
    """The parts of the repo the build VM needs, without build output."""
    import io
    import tarfile

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tar:
        for top in SOURCE_DIRS:
            base = repo / top
            for path in sorted(base.rglob("*")):
                rel = path.relative_to(repo)
                if "target" in rel.parts or "__pycache__" in rel.parts or not path.is_file():
                    continue
                tar.add(path, arcname=rel.as_posix(), recursive=False)
    return buf.getvalue()


def make_handler(cache: Cache):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "renner-pkgcache"

        def log_message(self, fmt, *args):  # quiet default logging
            pass

        def note(self, kind: str, size: int | str = "") -> None:
            cache.count(kind)
            cache.log(f"{time.strftime('%H:%M:%S')} {kind.upper():5} {self.path} {size}")

        def do_HEAD(self):
            self.handle_get(head=True)

        def do_GET(self):
            self.handle_get(head=False)

        def handle_get(self, head: bool):
            path = urllib.parse.unquote(self.path.split("?", 1)[0])
            if path == "/_lab/quit":
                ok = self.path.endswith(f"token={cache.token}")
                self.send_simple(200 if ok else 403, b"bye\n" if ok else b"no\n")
                if ok:
                    cache.stop.set()
                return
            if path == "/_lab/stats":
                body = (str(cache.stats) + "\n").encode()
                self.send_simple(200, body)
                return
            if not SAFE_PATH.match(path) or ".." in path:
                self.send_simple(400, b"bad path\n")
                return
            if path == "/renner/source.tar" and cache.files:  # build VM: the repo as a tar
                self.serve_bytes(source_tar(cache.files), head)
                self.note("file", "source.tar")
                return
            if path.startswith("/artifacts/"):  # build results uploaded by the build VM
                local = cache.root.parent / "artifacts" / path[len("/artifacts/"):]
                if local.is_file():
                    self.serve_file(local, head)
                    self.note("file", local.stat().st_size)
                else:
                    self.send_simple(404, b"not found\n")
                return
            if path.startswith("/renner/"):  # the repo's installer files, for curl | bash tests
                rel = path[len("/renner/"):]
                local = cache.files / rel if cache.files else None
                if local and rel.split("/", 1)[0] in ("install", "system") and local.is_file():
                    self.serve_file(local, head)
                    self.note("file", local.stat().st_size)
                else:
                    self.send_simple(404, b"not found\n")
                return

            local = cache.root / path.lstrip("/").replace(":", "%3A")
            if PKG_FILE.search(path) and local.is_file():
                self.serve_file(local, head)
                self.note("hit", local.stat().st_size)
                return
            # Only whole package downloads are cached; ranged requests pass through.
            cacheable = bool(PKG_FILE.search(path)) and "Range" not in self.headers
            self.proxy(path, local if cacheable else None, head)

        def do_PUT(self):
            # Lab only: the build VM uploads a built artifact (one flat name).
            name = urllib.parse.unquote(self.path.split("?", 1)[0])
            if not name.startswith("/_lab/artifacts/") or not re.fullmatch(r"[A-Za-z0-9._-]+", name[16:]):
                self.send_simple(400, b"bad artifact name\n")
                return
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 512 * 1024 * 1024:
                self.send_simple(400, b"bad length\n")
                return
            target = cache.root.parent / "artifacts" / name[16:]
            target.parent.mkdir(parents=True, exist_ok=True)
            tmp = target.with_name(f".{target.name}.part")
            left = length
            with open(tmp, "wb") as out:
                while left > 0:
                    chunk = self.rfile.read(min(CHUNK, left))
                    if not chunk:
                        break
                    out.write(chunk)
                    left -= len(chunk)
            if left:
                tmp.unlink(missing_ok=True)
                self.send_simple(400, b"short upload\n")
                return
            os.replace(tmp, target)
            self.send_simple(201, b"stored\n")
            self.note("file", f"upload {target.name} {length}")

        def serve_bytes(self, data: bytes, head: bool):
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            if not head:
                self.wfile.write(data)

        def send_simple(self, code: int, body: bytes):
            self.send_response(code)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def serve_file(self, local: Path, head: bool):
            size = local.stat().st_size
            start, end = 0, size - 1
            rng = self.headers.get("Range", "")
            m = re.match(r"bytes=(\d+)-(\d*)$", rng)
            if m:
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else size - 1
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            else:
                self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            if head:
                return
            with open(local, "rb") as fh:
                fh.seek(start)
                left = end - start + 1
                while left > 0:
                    chunk = fh.read(min(CHUNK, left))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)

        def proxy(self, path: str, store: Path | None, head: bool):
            url = cache.upstream + urllib.parse.quote(path, safe="/+@~")
            req = urllib.request.Request(url, method="HEAD" if head else "GET")
            for name in ("Range", "If-Modified-Since", "If-None-Match"):
                if name in self.headers:
                    req.add_header(name, self.headers[name])
            req.add_header("User-Agent", "renner-pkgcache")
            try:
                resp = urllib.request.urlopen(req, timeout=30)
            except urllib.error.HTTPError as err:
                self.send_response(err.code)
                for name in ("Last-Modified", "ETag"):
                    if err.headers.get(name):
                        self.send_header(name, err.headers[name])
                self.send_header("Content-Length", "0")
                self.end_headers()
                # 304 (unchanged db) and 404 on a non-package file (pacman asks
                # for *.db.sig, which Arch does not publish) are normal.
                normal = err.code == 304 or (err.code == 404 and not PKG_FILE.search(path))
                self.note("pass" if normal else "error", err.code)
                return
            except OSError as err:
                self.send_simple(502, f"upstream: {err}\n".encode())
                self.note("error", str(err))
                return

            with resp:
                self.send_response(resp.status)
                for name in ("Content-Length", "Content-Type", "Content-Range",
                             "Last-Modified", "ETag", "Accept-Ranges"):
                    if resp.headers.get(name):
                        self.send_header(name, resp.headers[name])
                self.end_headers()
                if head:
                    self.note("pass", "HEAD")
                    return
                tmp = None
                if store is not None and resp.status == 200:
                    store.parent.mkdir(parents=True, exist_ok=True)
                    tmp = store.with_name(f".{store.name}.{secrets.token_hex(4)}.part")
                out = open(tmp, "wb") if tmp else None
                total = 0
                try:
                    while chunk := resp.read(CHUNK):
                        if out:
                            out.write(chunk)
                        self.wfile.write(chunk)
                        total += len(chunk)
                    if out:
                        out.close()
                        out = None
                        expected = resp.headers.get("Content-Length")
                        if expected is None or int(expected) == total:
                            os.replace(tmp, store)
                            tmp = None
                finally:
                    if out:
                        out.close()
                    if tmp and tmp.exists():
                        tmp.unlink()
                self.note("miss" if store is not None else "pass", total)

    return Handler


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--root", type=Path, default=Path("E:/AgentData/renneros-lab/pkgcache"))
    p.add_argument("--upstream", default="https://geo.mirror.pkgbuild.com")
    p.add_argument("--port", type=int, default=8079)
    p.add_argument("--minutes", type=int, default=120)
    p.add_argument("--token", default=os.environ.get("RENNER_PKGCACHE_TOKEN", ""))
    p.add_argument("--files", type=Path, help="repo checkout served read-only under /renner/ (install/, system/)")
    a = p.parse_args()

    a.root.mkdir(parents=True, exist_ok=True)
    for stale in a.root.rglob(".*.part"):  # leftovers from an interrupted run
        stale.unlink(missing_ok=True)
    log_file = open(a.root / "access.log", "a", buffering=1)
    log_lock = threading.Lock()

    def log(line: str) -> None:
        with log_lock:
            log_file.write(line + "\n")

    cache = Cache(a.root, a.upstream, a.token or secrets.token_hex(16), log)
    cache.files = a.files.resolve() if a.files else None
    server = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(cache))
    server.daemon_threads = True

    threading.Thread(target=server.serve_forever, daemon=True).start()
    log(f"== {time.ctime()} serving {a.root} from {a.upstream} on 127.0.0.1:{a.port} for {a.minutes} min")
    try:
        cache.stop.wait(timeout=a.minutes * 60)  # time limit or /_lab/quit
    finally:
        server.shutdown()
        server.server_close()
        log(f"== {time.ctime()} stopped; {cache.stats}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
