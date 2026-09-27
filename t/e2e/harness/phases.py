"""Phases of the e2e suite: seeding, nginx configs, load, the correctness
verifier, consistency checks, starvation probes and chaos actions."""

import glob
import json
import os
import re
import shutil
import signal
import subprocess
import threading
import time

from lib import (BUCKETS, CACHE_PORT, DATA, HARNESS, ORIGIN_PORT, RESULTS,
                 Client, KeySet, Nginx, children, log, md5_path, percentiles,
                 rng)


# -- configuration -----------------------------------------------------------

def mb(n):
    return "%dm" % n


def cache_conf(cfg, meta, index_size=None, max_size=None, reconcile=None,
               http_extra=None, throttle="10ms", walk_budget=None):
    files = meta["files"]
    # room for what is on disk now (fills of earlier phases included) and as
    # many fills again as the seed: nginx alerts when its keys zone is full,
    # and the index must not run full outside the small-index test
    room = int(scan()["files"] * 1.3) + files
    video_share = max(meta["video_permille"], 1) / 1000.0
    kz_img = max(16, int(room / 8000) + 16)
    kz_vid = max(16, int(room * video_share * 3 / 8000) + 16)
    idx = index_size or cfg.index_size or mb(
        max(16, int(room * 320 / 1048576) + 16))
    max_size = max_size or cfg.max_size
    extra = "\n    ".join(cfg.http_extra + (http_extra or []))
    loader = ("loader_files=20000 loader_sleep=1ms loader_threshold=500ms "
              "manager_files=5000 manager_sleep=10ms manager_threshold=500ms "
              "inactive=30d use_temp_path=off")

    return """
daemon off;
user root;
worker_processes %(workers)d;
worker_rlimit_nofile 65536;
error_log logs/error.log info;
pid logs/nginx.pid;
%(main_extra)s

events {
    worker_connections 16384;
}

http {
    access_log off;
    keepalive_requests 1000000;
    keepalive_timeout 120s;
    sendfile on;

    proxy_cache_path %(data)s/cache/images levels=1:2 keys_zone=images:%(kz_img)dm
                     %(loader)s %(max_size)s;
    proxy_cache_path %(data)s/cache/videos levels=1:2 keys_zone=videos:%(kz_vid)dm
                     %(loader)s;

    cache_purge_background_queue on;
    cache_purge_queue_size       16384;
    cache_purge_batch_size       1024;
    cache_purge_throttle_ms      %(throttle)s;
    %(walk_budget)s
    cache_purge_index            %(idx)s;
    cache_purge_index_reconcile  %(reconcile)s;
    cache_purge_response_type    text;
    %(extra)s

    upstream origin {
        server 127.0.0.1:%(origin)d;
        keepalive 256;
    }

    server {
        listen %(port)d reuseport backlog=65535;

        proxy_http_version 1.1;
        proxy_set_header   Connection "";
        proxy_cache_key    "$uri$is_args$args";
        proxy_cache_valid  200 30d;
        # off: a request waiting for the lock polls every 500 ms, which
        # would show up in the latency probes as a stall of the worker
        proxy_cache_lock   off;
        add_header X-Cache $upstream_cache_status always;

        location /cdn/ {
            proxy_pass        http://origin;
            proxy_cache       images;
            proxy_cache_purge PURGE from 127.0.0.1;
        }

        location /video/ {
            proxy_pass        http://origin;
            proxy_cache       videos;
            proxy_cache_purge PURGE from 127.0.0.1;
        }

        location /purgeall/images {
            proxy_pass        http://origin;
            proxy_cache       images;
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }

        location = /status {
            stub_status;
        }
    }
}
""" % {"workers": cfg.workers, "main_extra": "\n".join(cfg.main_extra),
       "data": DATA, "kz_img": kz_img, "kz_vid": kz_vid, "loader": loader,
       "max_size": ("max_size=%s" % max_size) if max_size else "",
       "idx": idx, "reconcile": reconcile or cfg.reconcile, "extra": extra,
       "throttle": throttle,
       "walk_budget": ("cache_purge_walk_budget %s;" % walk_budget)
       if walk_budget else "",
       "origin": ORIGIN_PORT, "port": CACHE_PORT}


def origin_conf(cfg):
    return """
daemon off;
user root;
worker_processes %d;
worker_rlimit_nofile 65536;
error_log logs/error.log warn;
pid logs/nginx.pid;

events {
    worker_connections 16384;
}

http {
    access_log off;
    sendfile on;
    tcp_nopush on;
    keepalive_requests 1000000;
    open_file_cache off;

    map $uri $blob {
        "~/s(?<b>[0-9]+)\\.[a-z0-9]+$"  /blobs/$b;
        default                        /blobs/0;
    }

    server {
        listen 127.0.0.1:%d backlog=65535;
        root %s/origin;

        location / {
            add_header Cache-Control "public, max-age=31536000" always;
            try_files /keys$uri $blob =404;
        }
    }
}
""" % (max(2, cfg.workers // 2), ORIGIN_PORT, DATA)


# -- seeding -----------------------------------------------------------------

def scan(prefixes=(), zones=("images", "videos"), timeout=3600):
    cmd = ["cachegen", "scan", "--threads", "16"]
    for z in zones:
        cmd += ["--dir", os.path.join(DATA, "cache", z)]
    for p in prefixes:
        cmd += ["--prefix", p]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError("cachegen scan failed: " + r.stderr)
    return json.loads(r.stdout)


def ensure_seed(cfg, res):
    want = {"files": cfg.files, "bytes_target": int(cfg.gb * 1e9),
            "sparse": 1 if cfg.sparse else 0, "seed": cfg.seed,
            "variants": cfg.variants, "tenants": cfg.tenants}
    meta_path = os.path.join(DATA, "seed.json")
    meta = None

    if os.path.exists(meta_path) and not cfg.reseed:
        with open(meta_path) as f:
            meta = json.load(f)
        if any(meta.get(k) != v for k, v in want.items()):
            log("seed: parameters changed, reseeding")
            meta = None
        else:
            have = scan()["files"]
            if have < 0.9 * cfg.files or have > 3 * cfg.files:
                log("seed: %d files on disk for a seed of %d, reseeding"
                    % (have, cfg.files))
                meta = None
            else:
                res.add("seed", True, {"reused": True, "files_on_disk": have,
                                       "gb": round(meta["bytes"] / 1e9, 2),
                                       "sparse": bool(meta["sparse"])})

    if meta is None:
        t0 = time.time()
        shutil.rmtree(os.path.join(DATA, "cache"), ignore_errors=True)
        if os.path.exists(meta_path):
            os.unlink(meta_path)
        cmd = ["cachegen", "seed", "--out", DATA, "--files", str(cfg.files),
               "--bytes", str(int(cfg.gb * 1e9)), "--seed", str(cfg.seed),
               "--threads", str(cfg.seed_threads),
               "--variants", str(cfg.variants),
               "--tenants", str(cfg.tenants)]
        if cfg.sparse:
            cmd.append("--sparse")
        log("seed: " + " ".join(cmd))
        r = subprocess.run(cmd)
        with open(meta_path) as f:
            meta = json.load(f)
        ok = r.returncode == 0 and meta["errors"] == 0
        res.add("seed", ok, {"reused": False, "files": meta["files"],
                             "gb": round(meta["bytes"] / 1e9, 2),
                             "sparse": bool(meta["sparse"]),
                             "video_permille": meta["video_permille"],
                             "seconds": round(time.time() - t0, 1),
                             "errors": meta["errors"]})
        if not ok:
            raise RuntimeError("seeding failed")

    # origin blobs: one sparse file per size bucket
    blobs = os.path.join(DATA, "origin", "blobs")
    os.makedirs(blobs, exist_ok=True)
    for b, size in enumerate(BUCKETS):
        p = os.path.join(blobs, str(b))
        if not os.path.exists(p) or os.path.getsize(p) != size:
            with open(p, "wb") as f:
                f.write(("BLOB %d\n" % b).encode())
                f.truncate(size)
    shutil.rmtree(os.path.join(DATA, "origin", "keys"), ignore_errors=True)
    return meta


# -- nginx lifecycle ---------------------------------------------------------

def start_origin(cfg):
    o = Nginx("origin", origin_conf(cfg))
    if o.alive():
        return o
    o.start(ORIGIN_PORT)
    return o


def start_cache(cfg, meta, **kw):
    n = Nginx("cache", cache_conf(cfg, meta, **kw))
    rc, err = n.test_conf()
    if rc != 0:
        raise RuntimeError("cache nginx -t failed:\n" + err)
    n.mark = n.log.size()
    n.start(CACHE_PORT)
    return n


# one line per cache zone: every zone has an index of its own
BUILD_DONE = re.compile(
    r'key index build of "([^"]+)" complete: (\d+) file\(s\) read, '
    r'(\d+) purged, (\d+) entries, in (\d+)ms')

INDEXED_ZONES = ("images", "videos")


def wait_build(ngx, since, timeout, zones=INDEXED_ZONES):
    """Wait until the last index build of every zone has completed; returns
    their numbers, summed (build_ms: the slowest zone)."""
    t0 = time.time()
    while True:
        last = {}
        for m in BUILD_DONE.finditer(ngx.log.read(since)):
            last[m.group(1)] = m      # a build restarted by chaos: the last
        if all(z in last for z in zones):
            break
        if time.time() - t0 >= timeout or not ngx.alive():
            return None
        time.sleep(0.2)
    ms = [last[z] for z in zones]
    return {"files_read": sum(int(m.group(2)) for m in ms),
            "purged": sum(int(m.group(3)) for m in ms),
            "entries": sum(int(m.group(4)) for m in ms),
            "build_ms": max(int(m.group(5)) for m in ms)}


def wait_loader(ngx, since, timeout):
    """nginx logs one "http file cache: <path> ..." line per zone when its
    cache loader is done."""
    t0 = time.time()
    while True:
        text = ngx.log.read(since)
        if all(("http file cache: %s/cache/%s " % (DATA, z)) in text
               for z in ("images", "videos")):
            return round(time.time() - t0, 2)
        if time.time() - t0 >= timeout:
            return None
        time.sleep(0.2)


# -- tracked keys and the correctness verifier -------------------------------

class Tracked:
    """Keys the verifier owns: nobody else GETs or purges them.  Their origin
    files carry a version on the first line."""

    def __init__(self, groups=8, per_group=4, video_groups=2):
        self.groups = []
        for g in range(groups):
            keys = ["/cdn/trk/g%d/k%d/s0.jpg" % (g, k)
                    for k in range(per_group)]
            self.groups.append(("/cdn/trk/g%d/" % g, keys, 12000 + 997 * g))
        for g in range(video_groups):
            keys = ["/video/trk/v%d/k%d/s6.mp4" % (g, k) for k in range(2)]
            self.groups.append(("/video/trk/v%d/" % g, keys, 1500000))
        self.version = {}
        self.lock = threading.Lock()

    def write(self, key, version, size):
        path = os.path.join(DATA, "origin", "keys", key.lstrip("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        head = ("VERSION %d KEY %s\n" % (version, key)).encode()
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(head)
            f.write(b"x" * max(0, size - len(head)))
        os.rename(tmp, path)
        with self.lock:
            self.version[key] = version

    def init_all(self):
        """Version 1 at the origin, and no cached copy left over from an
        earlier run on the same data (nginx is not running yet)."""
        for _, keys, size in self.groups:
            for k in keys:
                self.write(k, 1, size)
                zone = "videos" if k.startswith("/video/") else "images"
                try:
                    os.unlink(md5_path(zone, k))
                except OSError:
                    pass


def body_version(body):
    line = body.split(b"\n", 1)[0]
    if line.startswith(b"VERSION "):
        try:
            return int(line.split()[1])
        except ValueError:
            return None
    return None


class Verifier:
    """Core invariant: after a purge is acknowledged, the purged content is
    never served again -- at once for 200, within `bound` seconds for 202."""

    def __init__(self, tracked, bound=30.0, allow_notfound=False,
                 chaos=False):
        self.tracked = tracked
        self.bound = bound
        self.allow_notfound = allow_notfound
        self.chaos = chaos
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.stats = {"cycles": 0, "skipped_not_cached": 0,
                      "purge_status": {}, "modes": {}, "http_errors": 0,
                      "queued_waits": [], "retries_429": 0,
                      "overpurged": 0}
        self.violations = []
        self.nviolations = 0
        self.threads = []

    def _count(self, name, key):
        with self.lock:
            d = self.stats[name]
            d[str(key)] = d.get(str(key), 0) + 1

    def violation(self, kind, **kw):
        kw["kind"] = kind
        kw["t"] = round(time.time(), 3)
        with self.lock:
            self.nviolations += 1
            if len(self.violations) < 200:
                self.violations.append(kw)
                log("VIOLATION %s" % json.dumps(kw))

    def get(self, c, key):
        """(status, version, x-cache); retries connection errors"""
        for _ in range(50 if self.chaos else 3):
            st, h, body = c.request("GET", key, retries=2)
            if st is None or (self.chaos and st in (500, 502, 503, 504)):
                with self.lock:
                    self.stats["http_errors"] += 1
                time.sleep(0.1)
                continue
            return st, body_version(body), h.get("x-cache")
        return None, None, None

    def purge(self, c, path):
        """(status, retried).  retried: a connection broke first, so an
        earlier attempt may have done the work (a 404/412 after that is no
        sign of a missing entry)."""
        errors0 = c.errors
        for _ in range(200):
            st, _, _ = c.request("PURGE", path, retries=2)
            if st is None:
                with self.lock:
                    self.stats["http_errors"] += 1
                time.sleep(0.1)
                continue
            if st == 429:
                with self.lock:
                    self.stats["retries_429"] += 1
                time.sleep(0.1)
                continue
            return st, c.errors != errors0
        return None, True

    def cycle(self, c, r, prefix, keys, size):
        key = r.choice(keys)
        mode = r.choice(["exact", "exact", "asset", "group"])
        v = self.tracked.version[key]

        # cached with the current version, and a HIT
        st, ver, xc = self.get(c, key)
        if st != 200:
            self.violation("get_failed", key=key, status=st)
            return
        if ver != v:
            self.violation("stale_before_cycle", key=key, want=v, got=ver,
                           x_cache=xc)
            # counted once: clear it so the next cycles test afresh
            self.purge(c, key)
            time.sleep(0.2)
            return
        st, ver, xc = self.get(c, key)
        if st != 200 or xc != "HIT" or ver != v:
            with self.lock:
                self.stats["skipped_not_cached"] += 1
            return

        # the origin moves on; the cache still has the old version
        self.tracked.write(key, v + 1, size)
        st, ver, xc = self.get(c, key)
        if st != 200 or ver != v:
            with self.lock:
                self.stats["skipped_not_cached"] += 1
            return

        if mode == "exact":
            path = key
        elif mode == "asset":
            path = key.rsplit("/", 1)[0] + "/*"
        else:
            path = prefix + "*"

        t0 = time.time()
        st, retried = self.purge(c, path)
        self._count("purge_status", st)
        self._count("modes", mode)
        with self.lock:
            self.stats["cycles"] += 1

        if st == 200:
            st2, ver, xc = self.get(c, key)
            if ver != v + 1:
                self.violation("stale_after_200", key=key, purge=path,
                               want=v + 1, got=ver, status=st2, x_cache=xc)
            return

        if st in (404, 412):
            st2, ver, xc = self.get(c, key)
            if ver == v:
                self.violation("stale_after_notfound", key=key, purge=path,
                               status=st, got=ver, x_cache=xc)
            elif self.chaos and not retried:
                # Purges are carried out at least once: after a kill -9, a
                # reload or an index reset, one may run again (re-queued) or
                # late (a build's list), and remove what was cached since.
                # That costs a refill, never serves stale content -- the
                # checks above stay strict.
                with self.lock:
                    self.stats["overpurged"] += 1
            elif not self.allow_notfound and not retried:
                self.violation("notfound_after_hit", key=key, purge=path,
                               status=st, x_cache=xc)
            return

        if st == 202:
            deadline = t0 + self.bound
            while True:
                st2, ver, xc = self.get(c, key)
                if ver == v + 1:
                    with self.lock:
                        self.stats["queued_waits"].append(time.time() - t0)
                    st3, ver3, xc3 = self.get(c, key)
                    if ver3 != v + 1:
                        self.violation("regressed_after_202", key=key,
                                       purge=path, got=ver3, x_cache=xc3)
                    return
                if time.time() > deadline:
                    self.violation("stale_after_202_bound", key=key,
                                   purge=path, bound_s=self.bound, got=ver,
                                   x_cache=xc)
                    return
                time.sleep(0.05)

        self.violation("purge_unexpected_status", key=key, purge=path,
                       status=st)

    def run_group(self, prefix, keys, size, seed):
        c = Client()
        r = rng(seed)
        while not self.stop.is_set():
            try:
                self.cycle(c, r, prefix, keys, size)
            except Exception as e:        # a harness bug must not pass
                self.violation("verifier_exception", error=repr(e))
                time.sleep(0.5)
        c.close()

    def start(self):
        # each phase starts clean: an exact purge (synchronous in every index
        # state) of every key, so a purge lost in an earlier phase is not
        # counted again here
        c = Client()
        for _, keys, _ in self.tracked.groups:
            for k in keys:
                for _ in range(50):
                    st, _, _ = c.request("PURGE", k, retries=5)
                    if st in (200, 404, 412):
                        break
                    time.sleep(0.1)
        c.close()
        for n, (prefix, keys, size) in enumerate(self.tracked.groups):
            t = threading.Thread(target=self.run_group,
                                 args=(prefix, keys, size, 1000 + n),
                                 daemon=True)
            t.start()
            self.threads.append(t)

    def finish(self):
        self.stop.set()
        for t in self.threads:
            t.join(self.bound + 60)
        waits = self.stats.pop("queued_waits")
        self.stats["queued_wait"] = percentiles(waits)
        self.stats["violations"] = self.nviolations
        return self.stats


# -- purge traffic -----------------------------------------------------------

# every path the purge driver sent (wildcards without their "*"): purges
# carried out in the background can still land after the load stopped
SENT_PURGES = []


class PurgeDriver:
    """FileCdnService-shaped purges against the seeded key set."""

    def __init__(self, ks, rate, threads=4, big_every=10.0, chaos=False):
        self.ks = ks
        self.rate = rate
        self.nthreads = threads
        self.big_every = big_every
        self.chaos = chaos
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.status = {}
        self.sent = 0
        self.bad = []
        self.threads = []
        self.last_big = time.time()

    def pick(self, r):
        ks = self.ks
        now = time.time()
        with self.lock:
            if now - self.last_big >= self.big_every:
                self.last_big = now
                t = r.randrange(ks.tenants)
                # assets of tenant t whose id starts with digit d
                return "big", "/cdn/t%d/a%d*" % (t, r.randrange(1, 10))
        x = r.random()
        if x < 0.40:
            return "exact", ks.key(ks.random_seeded(r))
        if x < 0.70:
            a = r.randrange(ks.assets())
            return "asset", ks.asset_prefix(a) + "*"
        # match nothing: an asset id beyond the key set
        a = ks.assets() * 10 + r.randrange(1 << 30)
        return "nothing", "/cdn/t%d/a%d/*" % (a % ks.tenants, a)

    def run(self, seed):
        c = Client()
        r = rng(seed)
        interval = self.nthreads / float(self.rate)
        nxt = time.time()
        while not self.stop.is_set():
            kind, path = self.pick(r)
            SENT_PURGES.append(path.rstrip("*"))
            st, _, _ = c.request("PURGE", path, retries=1)
            with self.lock:
                self.sent += 1
                d = self.status.setdefault(kind, {})
                d[str(st)] = d.get(str(st), 0) + 1
                if st is None and not self.chaos:
                    self.bad.append((kind, path, st))
                elif st is not None and st >= 500:
                    self.bad.append((kind, path, st))
            nxt += interval
            delay = nxt - time.time()
            if delay > 0:
                time.sleep(delay)
            else:
                nxt = time.time()
        c.close()

    def start(self):
        for n in range(self.nthreads):
            t = threading.Thread(target=self.run, args=(2000 + n,),
                                 daemon=True)
            t.start()
            self.threads.append(t)

    def finish(self):
        self.stop.set()
        for t in self.threads:
            t.join(60)
        return {"sent": self.sent, "status": self.status,
                "bad": len(self.bad), "bad_sample": self.bad[:10]}


# -- latency prober ------------------------------------------------------------

class Prober:
    """GETs of cached small objects over keep-alive connections, one request
    at a time per connection: how long does a request wait for a worker?"""

    def __init__(self, keys, conns=16):
        self.keys = keys
        self.conns = conns
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.samples = []
        self.errors = 0
        self.non200 = 0
        self.threads = []

    def warm(self):
        c = Client()
        for k in self.keys:
            c.request("GET", k, retries=3)
        c.close()

    def run(self, seed):
        c = Client(timeout=30)
        r = rng(seed)
        local = []
        while not self.stop.is_set():
            k = r.choice(self.keys)
            t0 = time.perf_counter()
            st, _, _ = c.request("GET", k)
            dt = time.perf_counter() - t0
            if st is None:
                with self.lock:
                    self.errors += 1
                time.sleep(0.01)
                continue
            if st != 200:
                with self.lock:
                    self.non200 += 1
            local.append(dt)
            time.sleep(0.002)
        with self.lock:
            self.samples.extend(local)
        c.close()

    def start(self):
        self.stop.clear()
        self.samples = []
        self.errors = 0
        self.non200 = 0
        self.threads = []
        for n in range(self.conns):
            t = threading.Thread(target=self.run, args=(3000 + n,),
                                 daemon=True)
            t.start()
            self.threads.append(t)

    def finish(self):
        self.stop.set()
        for t in self.threads:
            t.join(60)
        d = percentiles(self.samples)
        d["errors"] = self.errors
        d["non200"] = self.non200
        return d


HOT_TENANTS = 4      # probe keys live in tenants 0..3; bulk purges spare them


def hot_keys(ks, n=256):
    """small seeded images of tenants < HOT_TENANTS: hits that cost the
    worker little"""
    r = rng(7)
    out = set()
    tries = 0
    while len(out) < n and tries < n * 10000:
        tries += 1
        i = ks.random_seeded(r)
        if ks.zone(i) == "images" and (i % ks.variants) % 6 in (0, 1) \
                and (i // ks.variants) % ks.tenants < HOT_TENANTS:
            out.add(ks.key(i))
    return sorted(out)


# -- wrk -----------------------------------------------------------------------

class Wrk:
    def __init__(self, cfg, ks, duration):
        self.cmd = ["wrk", "-t", str(cfg.wrk_threads), "-c",
                    str(cfg.wrk_conns), "-d", "%ds" % duration,
                    "--timeout", "10s", "-s",
                    os.path.join(HARNESS, "load.lua"),
                    "http://127.0.0.1:%d" % CACHE_PORT, "--",
                    str(ks.files), str(ks.variants), str(ks.tenants),
                    str(ks.permille), str(cfg.miss_permille)]
        self.result = None
        self.thread = None

    def run(self):
        r = subprocess.run(self.cmd, capture_output=True, text=True)
        for line in r.stdout.splitlines():
            if line.startswith("E2E_JSON "):
                self.result = json.loads(line[9:])
        if self.result is None:
            self.result = {"error": "no result", "rc": r.returncode,
                           "stdout": r.stdout[-2000:],
                           "stderr": r.stderr[-2000:]}
        else:
            d = self.result["duration_us"] / 1e6
            self.result["rps"] = round(self.result["requests"] / d, 1)
            self.result["mbytes_s"] = round(self.result["bytes"] / d / 1e6, 1)

    def start(self):
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def finish(self, timeout=None):
        self.thread.join(timeout)
        return self.result


# -- consistency ---------------------------------------------------------------

def fill_unseeded(ks, r, assets=6):
    """GET every variant of a few unseeded assets, so nginx itself stores
    them (index entries recorded by the header filter, not the build)."""
    c = Client()
    prefixes = []
    for _ in range(assets):
        a = ks.random_image_asset(r, seeded=False)
        ok = 0
        for v in range(ks.variants):
            st, _, _ = c.request("GET", ks.key(a * ks.variants + v),
                                 retries=5)
            ok += st == 200
        prefixes.append(ks.asset_prefix(a))
    c.close()
    return prefixes


def consistency(ks, name, res, r, siblings=True, timeout=120, extra=None,
                chaos=False):
    """Purge prefixes and check the disk: nothing matching may remain (every
    stored file is reachable by a wildcard purge), and a sibling prefix is
    left alone."""
    prefixes = fill_unseeded(ks, r)
    n = ks.assets()
    for _ in range(8):
        prefixes.append(ks.asset_prefix(r.randrange(n)))
    va = ks.random_video_asset(r)
    if va is not None:
        prefixes.append(ks.asset_prefix(va))
    t = r.randrange(ks.tenants)
    prefixes.append("/cdn/t%d/a%d" % (t, r.randrange(1, 10)))  # big
    prefixes += extra or []

    sib = []
    for _ in range(100000):
        if len(sib) == 4:
            break
        p = ks.asset_prefix(r.randrange(n))
        if not any(p.startswith(q) or q.startswith(p)
                   for q in prefixes + SENT_PURGES):
            sib.append(p)

    before = scan(prefixes + sib)["matches"]
    c = Client()
    status = {}
    still = {}
    for p in prefixes:
        st = None
        for _ in range(100):
            st, _, _ = c.request("PURGE", p + "*", retries=5)
            if st != 429 and st is not None:
                break
            time.sleep(0.1)
        status[p] = st
        # "nothing matched" while files were there: are they still?  Gone
        # means another purge (one still running, say) got there first;
        # still there means the purge missed them
        if st in (404, 412) and before[p] > 0:
            still[p] = scan([p])["matches"][p]
    c.close()

    t0 = time.time()
    remaining = None
    delay = 0.2
    while time.time() - t0 < timeout:
        after = scan(prefixes + sib)["matches"]
        remaining = {p: after[p] for p in prefixes if after[p] != 0}
        if not remaining:
            break
        time.sleep(delay)
        delay = min(delay * 2, 5)

    sib_changed = {p: (before[p], after[p]) for p in sib
                   if before[p] != after[p]}
    bad_status = {p: s for p, s in status.items()
                  if s not in (200, 202, 404, 412)}
    # a prefix with files before must not answer "nothing matched"
    notfound = {p: still[p] for p in still if still[p] > 0}
    raced = {p: before[p] for p in still if still[p] == 0}
    ok = not remaining and not bad_status and not notfound and (
        not siblings or not sib_changed)
    notes = []
    if remaining:
        notes.append("files left on disk after purge: %s" % remaining)
    if notfound:
        notes.append("purge said not found but files existed: %s" % notfound)
    if siblings and sib_changed:
        notes.append("sibling prefixes changed: %s" % sib_changed)
    res.add(name, ok, {"prefixes": len(prefixes),
                       "files_before": sum(before[p] for p in prefixes),
                       "seconds_to_empty": round(time.time() - t0, 2),
                       "status": sorted(set(str(s) for s in status.values())),
                       "siblings_checked": siblings,
                       "notfound_raced": raced,
                       "sibling_files": sum(before[p] for p in sib)},
            notes)
    return ok


# -- error log audit -------------------------------------------------------------

ALWAYS_BAD = [re.compile(r"exited on signal (11|6|7|4|8)"),
              re.compile(r"AddressSanitizer")]
CHAOS_ALLOW = [r"worker process \d+ exited on signal 9",
               r"cache (manager|loader) process \d+ exited on signal 9",
               r"shared memory zone \".*\" was locked by \d+",
               r"died while updating the (key index|purge queue)"]
SMALL_INDEX_ALLOW = [r'key index build of "[^"]+" failed',
                     r'key index of "[^"]+" incomplete',
                     r"could not plan the key index build",
                     r"cache_purge_index zone is full"]


# nginx's cache loader stats every file it lists (ngx_walk_tree) and logs a
# file deleted in between -- by a purge, or the cache manager -- at crit
LOADER_RACE = r"stat\(\) \"%s/cache/.*\" failed \(2: No such file or directory\)" % DATA


def audit(ngx, since, name, res, allow=()):
    text = ngx.log.read(since)
    allow_re = [re.compile(a) for a in list(allow) + [LOADER_RACE]]
    bad, allowed = [], {}
    for line in text.splitlines():
        if any(r.search(line) for r in ALWAYS_BAD):
            bad.append(line)
            continue
        if not re.search(r"\[(emerg|alert|crit)\]", line):
            continue
        hit = next((a for a in allow_re if a.search(line)), None)
        if hit is None:
            bad.append(line)
        else:
            allowed[hit.pattern] = allowed.get(hit.pattern, 0) + 1
    asan = sorted(glob.glob(os.path.join(RESULTS, "asan*")))
    notes = ["unexpected: " + l[:400] for l in bad[:20]]
    if asan:
        notes.append("AddressSanitizer reports: %s" % asan)
    res.add(name, not bad and not asan,
            {"unexpected": len(bad), "allowed": allowed,
             "asan_reports": len(asan)}, notes)
    return not bad and not asan


# -- chaos actions -----------------------------------------------------------------

def kill_random_worker(ngx, r):
    w = ngx.workers()
    if not w:
        return None
    pid = r.choice(w)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        return None
    return pid


def gdb_kill_in_index_change(ngx, symbol, timeout=90):
    """Stop a worker inside a change of the key index (the busy flag up, the
    index lock held) and kill -9 it there.  Breaks on the call that lowers
    the flag again: at its entry the flag is still up."""
    workers = ngx.workers()
    if not workers:
        return {"result": "no workers"}
    pid = workers[0]
    script = [
        "set pagination off",
        "set confirm off",
        "break %s if $rsi == 0" % symbol,
        "continue",
        "bt 3",
        "shell kill -9 %d" % pid,
    ]
    cmd = ["gdb", "-p", str(pid), "-batch", "-nx"]
    for s in script:
        cmd += ["-ex", s]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout)
        out = r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode(errors="replace") if isinstance(
            e.stdout, bytes) else (e.stdout or "")
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
        return {"result": "timeout", "pid": pid, "out": out[-1500:]}
    if "Function \"%s\" not defined" % symbol in out or \
            "No symbol table" in out:
        return {"result": "no symbol", "pid": pid, "out": out[-1500:]}
    hit = ("Breakpoint 1," in out) or ("breakpoint 1," in out.lower())
    return {"result": "killed in change" if hit else "no hit", "pid": pid,
            "out": out[-1500:]}


def durability_kill_drainer(cache, tracked, res, name, bound=240.0):
    """A purge answered 202 must survive kill -9 of the worker carrying it
    out.  Run while wildcards are walked (a failed index): the walk takes
    seconds, so killing every worker a second after the 202 hits it in
    flight."""
    c = Client()
    prefix, keys, size = tracked.groups[0]
    probe_prefix, probe_keys, probe_size = tracked.groups[1]

    def cached(key):
        v = tracked.version[key]
        for _ in range(3):
            st, h, body = c.request("GET", key, retries=5)
        return st == 200 and h.get("x-cache") == "HIT" \
            and body_version(body) == v

    def wait_version(key, want, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            st, _, body = c.request("GET", key, retries=5)
            if body_version(body) == want:
                return round(time.time() - t0, 2)
            time.sleep(0.2)
        return None

    # exact purges are synchronous: a clean start whatever came before
    pk = probe_keys[0]
    for k in (pk, keys[0]):
        c.request("PURGE", k, retries=5)

    # the queue drained up to now: a purge of another key took effect
    if not cached(pk):
        res.add(name, False, {"setup": "probe key not cached"})
        return
    pv = tracked.version[pk] + 1
    tracked.write(pk, pv, probe_size)
    c.request("PURGE", pk.rsplit("/", 1)[0] + "/*", retries=5)
    drained = wait_version(pk, pv, bound)

    key = keys[0]
    if drained is None or not cached(key):
        res.add(name, False, {"setup": "queue did not drain or key not "
                                       "cached", "drain_s": drained})
        return
    v = tracked.version[key] + 1
    tracked.write(key, v, size)
    st, _, _ = c.request("PURGE", key.rsplit("/", 1)[0] + "/*", retries=5)
    if st != 202:
        res.add(name, True, {"purge_status": st},
                ["purge answered %s, not queued: nothing to test" % st],
                skipped=True)
        return
    killed = []
    for at in (1.0, 5.0):
        time.sleep(at - (1.0 if at > 1.0 else 0.0))
        for pid in cache.workers():
            try:
                os.kill(pid, signal.SIGKILL)
                killed.append(pid)
            except OSError:
                pass
    fresh = wait_version(key, v, bound)
    c.close()
    ok = fresh is not None
    res.add(name, ok, {"purge_status": st, "workers_killed": len(killed),
                       "drain_s": drained, "fresh_after_s": fresh,
                       "bound_s": bound},
            [] if ok else ["the queued purge was lost: stale content still "
                           "served %ds after the 202" % bound])


def external_delete(ks, r, files=200):
    """Delete cache files and one whole level-2 directory behind nginx."""
    removed = 0
    for _ in range(files):
        i = ks.random_seeded(r)
        try:
            os.unlink(md5_path(ks.zone(i), ks.key(i)))
            removed += 1
        except OSError:
            pass
    d = os.path.join(DATA, "cache", "images", "%x" % r.randrange(16),
                     "%02x" % r.randrange(256))
    n = len(os.listdir(d)) if os.path.isdir(d) else 0
    shutil.rmtree(d, ignore_errors=True)
    return {"files_removed": removed, "dir_removed": d, "dir_files": n}


def key_in_dir(ks, d, r, tries=2000000):
    """A seeded image key whose file lives in level-2 directory d"""
    import hashlib
    l1, l2 = d.rstrip("/").split("/")[-2:]
    for _ in range(tries):
        i = ks.random_seeded(r)
        if ks.zone(i) != "images":
            continue
        k = ks.key(i)
        h = hashlib.md5(k.encode()).hexdigest()
        if h[31] == l1 and h[29:31] == l2:
            return k
    return None
