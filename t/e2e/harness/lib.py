"""Shared pieces of the e2e harness: key set, nginx control, HTTP, logs."""

import http.client
import json
import os
import random
import re
import signal
import socket
import subprocess
import threading
import time

DATA = os.environ.get("E2E_DATA", "/data")
RESULTS = os.environ.get("E2E_RESULTS", "/results")
HARNESS = os.path.dirname(os.path.abspath(__file__))

CACHE_PORT = 8080
ORIGIN_PORT = 8081
ZONES = ("images", "videos")

# origin blob sizes per size bucket (s<bucket> in the key)
BUCKETS = [8 << 10, 16 << 10, 32 << 10, 64 << 10, 128 << 10, 256 << 10,
           1 << 20, 2 << 20, 4 << 20, 8 << 20]


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


# -- key set (mirror of tools/cachegen.c and load.lua) -----------------------

class KeySet:
    def __init__(self, meta):
        self.files = meta["files"]
        self.variants = meta["variants"]
        self.tenants = meta["tenants"]
        self.permille = meta["video_permille"]

    def is_video_asset(self, a):
        return (a * 7919) % 1000 < self.permille

    def key(self, i):
        a, v = divmod(i, self.variants)
        t = a % self.tenants
        if self.is_video_asset(a):
            return "/video/t%d/a%d/s%d.mp4?r=%d" % (t, a, 6 + v % 4, v)
        return "/cdn/t%d/a%d/s%d.jpg?w=%d" % (t, a, v % 6, 160 * (v + 1))

    def zone(self, i):
        return "videos" if self.is_video_asset(i // self.variants) else "images"

    def assets(self):
        return (self.files + self.variants - 1) // self.variants

    def asset_prefix(self, a):
        t = a % self.tenants
        if self.is_video_asset(a):
            return "/video/t%d/a%d/" % (t, a)
        return "/cdn/t%d/a%d/" % (t, a)

    def asset_files(self, a):
        """seeded files of asset a"""
        lo = a * self.variants
        return max(0, min(self.files, lo + self.variants) - lo)

    def random_seeded(self, rng):
        return rng.randrange(self.files)

    def random_image_asset(self, rng, seeded=True):
        n = self.assets()
        while True:
            a = rng.randrange(n) if seeded else n + rng.randrange(n * 4 + 1)
            if not self.is_video_asset(a):
                return a

    def random_video_asset(self, rng):
        n = self.assets()
        if self.permille == 0:
            return None
        for _ in range(100000):
            a = rng.randrange(n)
            if self.is_video_asset(a):
                return a
        return None


def md5_path(zone, key):
    import hashlib
    h = hashlib.md5(key.encode()).hexdigest()
    return os.path.join(DATA, "cache", zone, h[31], h[29:31], h)


# -- processes ---------------------------------------------------------------

def read_proc(pid, name):
    try:
        with open("/proc/%d/%s" % (pid, name), "rb") as f:
            return f.read()
    except OSError:
        return b""


def children(ppid):
    """(pid, title) of the processes whose parent is ppid"""
    out = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        pid = int(d)
        st = read_proc(pid, "stat").decode(errors="replace")
        if not st:
            continue
        try:
            fields = st[st.rindex(")") + 2:].split()
        except ValueError:
            continue
        if int(fields[1]) != ppid or fields[0] == "Z":
            continue
        title = read_proc(pid, "cmdline").replace(b"\0", b" ").decode(
            errors="replace").strip()
        out.append((pid, title))
    return out


class Nginx:
    """One nginx master (daemon off) run as a child of the harness."""

    def __init__(self, name, conf):
        self.name = name
        self.prefix = os.path.join(DATA, "run", name)
        self.conf = conf
        self.proc = None
        self.error_log = os.path.join(self.prefix, "logs", "error.log")
        self.log = LogWatch(self.error_log)

    def write_conf(self):
        for d in ("logs", "conf", "temp"):
            os.makedirs(os.path.join(self.prefix, d), exist_ok=True)
        with open(os.path.join(self.prefix, "conf", "nginx.conf"), "w") as f:
            f.write(self.conf)

    def test_conf(self):
        self.write_conf()
        r = subprocess.run(["nginx", "-t", "-p", self.prefix + "/", "-c",
                            "conf/nginx.conf"], capture_output=True,
                           text=True)
        return r.returncode, r.stderr

    def start(self, port, timeout=60):
        self.write_conf()
        out = open(os.path.join(self.prefix, "logs", "stderr.log"), "ab")
        self.proc = subprocess.Popen(
            ["nginx", "-p", self.prefix + "/", "-c", "conf/nginx.conf"],
            stdout=out, stderr=out, start_new_session=True)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("%s exited at start (%s), see %s"
                                   % (self.name, self.proc.returncode,
                                      self.error_log))
            try:
                socket.create_connection(("127.0.0.1", port), 0.5).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("%s did not listen on %d" % (self.name, port))

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    @property
    def pid(self):
        return self.proc.pid

    def signal(self, sig):
        if self.alive():
            os.kill(self.proc.pid, sig)

    def reload(self, conf=None):
        if conf is not None:
            self.conf = conf
            self.write_conf()
        self.signal(signal.SIGHUP)

    def processes(self):
        return children(self.proc.pid) if self.alive() else []

    def workers(self):
        return [p for p, t in self.processes()
                if "worker process" in t and "shutting down" not in t]

    def stop(self, timeout=60):
        if not self.alive():
            return
        self.signal(signal.SIGQUIT)
        try:
            self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            log("%s: graceful stop timed out, SIGTERM" % self.name)
            self.signal(signal.SIGTERM)
            try:
                self.proc.wait(15)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
        # leftover workers of a killed master
        for p, _ in children(self.proc.pid):
            try:
                os.kill(p, signal.SIGKILL)
            except OSError:
                pass


class LogWatch:
    """Follow an error log; wait for lines after a mark."""

    def __init__(self, path):
        self.path = path

    def size(self):
        try:
            return os.path.getsize(self.path)
        except OSError:
            return 0

    def read(self, since=0):
        try:
            with open(self.path, "rb") as f:
                f.seek(since)
                return f.read().decode(errors="replace")
        except OSError:
            return ""

    def wait(self, regex, since=0, timeout=60, alive=None):
        r = re.compile(regex)
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = r.search(self.read(since))
            if m:
                return m
            if alive is not None and not alive():
                return None
            time.sleep(0.1)
        return None


# -- HTTP --------------------------------------------------------------------

class Client:
    """A keep-alive HTTP/1.1 client; reconnects on errors."""

    def __init__(self, port=CACHE_PORT, timeout=30):
        self.port = port
        self.timeout = timeout
        self.conn = None
        self.errors = 0

    def request(self, method, path, retries=0, body_limit=None):
        """(status, headers, body) or (None, {}, b"") after connection
        errors.  retries: extra attempts on connection errors."""
        for attempt in range(retries + 1):
            try:
                if self.conn is None:
                    self.conn = http.client.HTTPConnection(
                        "127.0.0.1", self.port, timeout=self.timeout)
                self.conn.request(method, path)
                r = self.conn.getresponse()
                body = r.read() if body_limit is None else r.read(body_limit)
                if body_limit is not None:
                    # drain the rest to keep the connection usable
                    while r.read(1 << 20):
                        pass
                headers = {k.lower(): v for k, v in r.getheaders()}
                if headers.get("connection", "").lower() == "close":
                    self.close()
                return r.status, headers, body
            except (OSError, http.client.HTTPException):
                self.errors += 1
                self.close()
                if attempt < retries:
                    time.sleep(0.05 * (attempt + 1))
        return None, {}, b""

    def close(self):
        if self.conn is not None:
            try:
                self.conn.close()
            except Exception:
                pass
            self.conn = None


def percentiles(samples):
    if not samples:
        return {"n": 0}
    s = sorted(samples)

    def p(q):
        return round(s[min(len(s) - 1, int(q * len(s)))] * 1000, 3)

    return {"n": len(s), "p50_ms": p(0.50), "p90_ms": p(0.90),
            "p99_ms": p(0.99), "p999_ms": p(0.999),
            "max_ms": round(s[-1] * 1000, 3)}


# -- results -----------------------------------------------------------------

class Results:
    def __init__(self, profile):
        self.profile = profile
        self.phases = []
        self.started = time.time()

    def add(self, name, ok, details=None, notes=None, skipped=False):
        entry = {"phase": name,
                 "result": "SKIP" if skipped else ("PASS" if ok else "FAIL"),
                 "details": details or {}, "notes": notes or []}
        self.phases.append(entry)
        log("[%s] %s %s" % (entry["result"], name,
                            json.dumps(details or {}, sort_keys=True)[:2000]))
        for n in notes or []:
            log("    - " + n)
        self.write()
        return ok

    @property
    def ok(self):
        return all(p["result"] != "FAIL" for p in self.phases)

    def write(self):
        os.makedirs(RESULTS, exist_ok=True)
        doc = {"profile": self.profile, "ok": self.ok,
               "seconds": round(time.time() - self.started, 1),
               "phases": self.phases}
        with open(os.path.join(RESULTS, "summary.json"), "w") as f:
            json.dump(doc, f, indent=2, sort_keys=True)
        lines = ["e2e profile %s: %s (%.0fs)" % (
            self.profile, "PASS" if self.ok else "FAIL",
            time.time() - self.started)]
        for p in self.phases:
            lines.append("  %-4s %s" % (p["result"], p["phase"]))
            for k, v in sorted(p["details"].items()):
                lines.append("         %s: %s" % (k, json.dumps(v)))
            for n in p["notes"]:
                lines.append("         - %s" % n)
        with open(os.path.join(RESULTS, "summary.txt"), "w") as f:
            f.write("\n".join(lines) + "\n")
        return "\n".join(lines)


def rng(seed=None):
    return random.Random(seed if seed is not None else time.time_ns())
