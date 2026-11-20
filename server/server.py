#!/usr/bin/env python3
"""
PageDB web server.

  browser  <--HTTP/JSON-->  server.py  <--stdin/stdout lines-->  pagedb engine (C++)

It uses only the Python standard library (no pip install needed).
The engine process is kept alive between requests; if it dies (e.g. the "crash" button)
the server restarts it, and the engine performs crash recovery on start-up.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
DATA = os.path.join(ROOT, "data")
RESULTS = os.path.join(ROOT, "results")
EXE = "pagedb.exe" if os.name == "nt" else "pagedb"
ENGINE_PATH = os.path.join(ROOT, "engine", "build", EXE)
SOURCES = ["slotted_page", "disk_manager", "log_manager", "replacers", "buffer_pool_manager", "btree", "sql",
           "database", "executor", "simulator", "protocol", "main"]


def try_build():
    """Compile the engine with g++ if it has not been built yet."""
    print("Engine not found - trying to build it with g++ ...")
    os.makedirs(os.path.dirname(ENGINE_PATH), exist_ok=True)
    cmd = ["g++", "-std=gnu++14", "-O2", "-I", os.path.join(ROOT, "engine", "include")]
    cmd += [os.path.join(ROOT, "engine", "src", s + ".cpp") for s in SOURCES]
    cmd += ["-o", ENGINE_PATH]
    try:
        subprocess.check_call(cmd)
        return True
    except Exception as e:  # noqa
        print("Build failed:", e)
        return False


class Engine:
    """Owns the C++ engine process and serialises access to it."""

    def __init__(self, db_path, policy, frames):
        self.db_path, self.policy, self.frames = db_path, policy, frames
        self.lock = threading.Lock()
        self.proc = None
        self.banner = {}
        self.start()

    def start(self):
        args = [ENGINE_PATH, "--serve", "--db", self.db_path, "--policy", self.policy, "--frames", str(self.frames)]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, encoding="utf-8", errors="replace", bufsize=1)
        line = self.proc.stdout.readline()
        if not line:
            err = self.proc.stderr.read()
            raise RuntimeError("engine failed to start: " + err)
        self.banner = json.loads(line)
        if self.banner.get("event") == "error":
            raise RuntimeError("engine error: " + self.banner.get("error", "?"))

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def call(self, command):
        """Send one command line, return the parsed JSON reply."""
        command = command.replace("\r", " ").replace("\n", " ")
        with self.lock:
            restarted = False
            if not self.alive():
                self.start()
                restarted = True
            try:
                self.proc.stdin.write(command + "\n")
                self.proc.stdin.flush()
                line = self.proc.stdout.readline()
            except (BrokenPipeError, OSError):
                line = ""
            if not line:  # engine died while handling the command
                self.proc.wait()
                self.start()
                return {"ok": False, "error": "the engine process died and was restarted (crash recovery ran)",
                        "restarted": True, "recovery": self.banner.get("recovery")}
            reply = json.loads(line)
            if restarted:
                reply["restarted"] = True
                reply["recovery"] = self.banner.get("recovery")
            return reply

    def crash(self):
        """Kill the engine WITHOUT letting it flush anything, then restart it (recovery runs)."""
        with self.lock:
            if not self.alive():
                self.start()
            try:
                self.proc.stdin.write("crash\n")
                self.proc.stdin.flush()
                self.proc.stdout.readline()
            except (BrokenPipeError, OSError):
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.start()
            return {"ok": True, "crashed": True, "recovery": self.banner.get("recovery")}

    def reset(self):
        with self.lock:
            if self.alive():
                try:
                    self.proc.stdin.write("quit\n")
                    self.proc.stdin.flush()
                    self.proc.wait(timeout=5)
                except Exception:  # noqa
                    self.proc.kill()
            for suffix in ("", ".wal", ".trace.tmp", ".bench.tmp"):
                try:
                    os.remove(self.db_path + suffix)
                except OSError:
                    pass
            self.start()
            return {"ok": True, "message": "database deleted and recreated"}

    def shutdown(self):
        with self.lock:
            if self.alive():
                try:
                    self.proc.stdin.write("quit\n")
                    self.proc.stdin.flush()
                    self.proc.wait(timeout=5)
                except Exception:  # noqa
                    self.proc.kill()


ENGINE = None
MIME = {".html": "text/html; charset=utf-8", ".js": "application/javascript; charset=utf-8",
        ".css": "text/css; charset=utf-8", ".json": "application/json", ".svg": "image/svg+xml", ".png": "image/png"}


class Handler(BaseHTTPRequestHandler):
    server_version = "PageDB/1.0"

    def log_message(self, fmt, *args):  # keep the console quiet
        pass

    # ---------------------------------------------------------------- helpers
    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except ValueError:
            return {}

    # ---------------------------------------------------------------- routing
    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        p = url.path
        try:
            if p == "/api/pool":
                return self.send_json(ENGINE.call("pool"))
            if p == "/api/disk":
                return self.send_json(ENGINE.call("disk"))
            if p == "/api/tables":
                return self.send_json(ENGINE.call("tables"))
            if p == "/api/info":
                r = ENGINE.call("info")
                r["startup"] = ENGINE.banner
                return self.send_json(r)
            if p == "/api/tree":
                return self.send_json(ENGINE.call("tree " + q.get("table", "")))
            if p == "/api/page":
                return self.send_json(ENGINE.call("page " + q.get("id", "0")))
            if p == "/api/wal":
                return self.send_json(ENGINE.call("wal " + q.get("n", "40")))
            if p.startswith("/api/"):
                return self.send_json({"ok": False, "error": "unknown endpoint"}, 404)
            return self.static(p)
        except Exception as e:  # noqa
            return self.send_json({"ok": False, "error": str(e)}, 500)

    def do_POST(self):
        p = urlparse(self.path).path
        b = self.read_body()
        try:
            if p == "/api/sql":
                return self.send_json(ENGINE.call("sql " + str(b.get("sql", ""))))
            if p == "/api/config":
                return self.send_json(ENGINE.call("config %s %s" % (b.get("policy", "LRU"), int(b.get("frames", 16)))))
            if p == "/api/seed":
                return self.send_json(ENGINE.call("seed %d" % int(b.get("n", 200))))
            if p == "/api/flush":
                return self.send_json(ENGINE.call("flush"))
            if p == "/api/checkpoint":
                return self.send_json(ENGINE.call("checkpoint"))
            if p == "/api/crash":
                return self.send_json(ENGINE.crash())
            if p == "/api/reset":
                return self.send_json(ENGINE.reset())
            if p == "/api/trace":
                refs = ",".join(str(int(x)) for x in b.get("refs", []))
                return self.send_json(ENGINE.call("trace %s %d %s" % (b.get("policy", "LRU"), int(b.get("frames", 3)), refs)))
            if p == "/api/belady":
                refs = ",".join(str(int(x)) for x in b.get("refs", []))
                return self.send_json(ENGINE.call("belady " + refs))
            if p == "/api/bench":
                cmd = "bench %d %d %d %s %s %s" % (
                    int(b.get("pages", 1000)), int(b.get("ops", 20000)), int(b.get("write_pct", 20)),
                    ",".join(str(int(x)) for x in b.get("sizes", [8, 16, 32, 64, 128, 256])),
                    ",".join(str(x) for x in b.get("workloads", ["seq", "random", "zipf", "hotcold"])),
                    ",".join(str(x) for x in b.get("policies", ["FIFO", "LRU", "CLOCK", "OPT"])))
                r = ENGINE.call(cmd)
                if r.get("ok"):
                    os.makedirs(RESULTS, exist_ok=True)
                    with open(os.path.join(RESULTS, "bench_latest.json"), "w") as f:
                        json.dump(r, f, indent=1)
                return self.send_json(r)
            return self.send_json({"ok": False, "error": "unknown endpoint"}, 404)
        except (ValueError, TypeError) as e:
            return self.send_json({"ok": False, "error": "bad request: %s" % e}, 400)
        except Exception as e:  # noqa
            return self.send_json({"ok": False, "error": str(e)}, 500)

    def static(self, path):
        if path in ("", "/"):
            path = "/index.html"
        full = os.path.normpath(os.path.join(WEB, path.lstrip("/")))
        if not full.startswith(WEB) or not os.path.isfile(full):
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"not found")
            return
        with open(full, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", MIME.get(os.path.splitext(full)[1], "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)


def main():
    global ENGINE
    ap = argparse.ArgumentParser(description="PageDB web server")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--policy", default="LRU", choices=["FIFO", "LRU", "CLOCK"])
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--db", default=os.path.join(DATA, "pagedb.db"))
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    os.makedirs(DATA, exist_ok=True)
    if not os.path.isfile(ENGINE_PATH) and not try_build():
        print("\nCould not find or build the engine. Run build.bat (Windows) or ./build.sh first.")
        sys.exit(1)
    ENGINE = Engine(args.db, args.policy, args.frames)
    rec = ENGINE.banner.get("recovery", {})
    if rec.get("ran"):
        print("Previous run did not shut down cleanly -> crash recovery ran: redo %s, undo %s" % (rec["redo_applied"], rec["undo_applied"]))
    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = "http://127.0.0.1:%d/" % args.port
    print("PageDB is running at", url, "  (Ctrl+C to stop)")
    if not args.no_browser:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        print("\nshutting down (clean checkpoint) ...")
        ENGINE.shutdown()
        httpd.server_close()


if __name__ == "__main__":
    main()
