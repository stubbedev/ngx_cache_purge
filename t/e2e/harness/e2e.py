#!/usr/bin/env python3
"""ngx_cache_purge end-to-end suite.

    e2e.py --profile quick|scale|chaos [options]

See t/e2e/README.md.  Exit status 0 when every phase passed.
"""

import argparse
import os
import re
import shutil
import signal
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib import DATA, RESULTS, Client, KeySet, Results, log, rng  # noqa
import phases as P  # noqa


PROFILES = {
    "quick": dict(files=80000, gb=3.0, workers=4, duration=45,
                  purge_rate=200, build_timeout=300, max_runtime=1800),
    "scale": dict(files=2500000, gb=200.0, workers=8, duration=180,
                  purge_rate=400, build_timeout=3600, max_runtime=6 * 3600),
    "chaos": dict(files=150000, gb=5.0, workers=4, duration=90,
                  purge_rate=200, build_timeout=600, max_runtime=3600),
}


def env(name, default=None):
    return os.environ.get("E2E_" + name, default)


def parse():
    a = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawTextHelpFormatter)
    a.add_argument("--profile", default=env("PROFILE", "quick"),
                   choices=sorted(PROFILES))
    a.add_argument("--files", type=int, default=env("FILES"))
    a.add_argument("--gb", type=float, default=env("GB"))
    a.add_argument("--real", action="store_true",
                   default=env("SPARSE", "1") == "0",
                   help="write real body bytes (default: sparse files)")
    a.add_argument("--seed", type=int, default=int(env("SEED", "1")))
    a.add_argument("--seed-threads", type=int,
                   default=int(env("SEED_THREADS", "16")))
    a.add_argument("--reseed", action="store_true",
                   default=env("RESEED", "0") == "1")
    a.add_argument("--variants", type=int, default=8)
    a.add_argument("--tenants", type=int, default=64)
    a.add_argument("--workers", type=int, default=env("WORKERS"))
    a.add_argument("--duration", type=int, default=env("DURATION"),
                   help="seconds of mixed load")
    a.add_argument("--wrk-threads", type=int,
                   default=int(env("WRK_THREADS", "4")))
    a.add_argument("--wrk-conns", type=int,
                   default=int(env("WRK_CONNS", "64")))
    a.add_argument("--miss-permille", type=int,
                   default=int(env("MISS_PERMILLE", "50")),
                   help="share of GETs for unseeded keys (fills), per mille")
    a.add_argument("--purge-rate", type=int, default=env("PURGE_RATE"),
                   help="purges per second during mixed load")
    a.add_argument("--index-size", default=env("INDEX_SIZE"),
                   help="cache_purge_index (default: ~480 bytes per file)")
    a.add_argument("--max-size", default=env("MAX_SIZE"),
                   help="max_size= of the images zone")
    a.add_argument("--reconcile", default=env("RECONCILE", "10m"))
    a.add_argument("--http-extra", action="append",
                   default=[s for s in env("HTTP_EXTRA", "").split(";;")
                            if s.strip()],
                   help="extra http{} directive, e.g. "
                        "'cache_purge_thread_pool default;' (repeatable; "
                        "env E2E_HTTP_EXTRA, ';;'-separated)")
    a.add_argument("--main-extra", action="append",
                   default=[s for s in env("MAIN_EXTRA", "").split(";;")
                            if s.strip()],
                   help="extra main-level directive, e.g. "
                        "'thread_pool default threads=16;'")
    a.add_argument("--p99-bound-ms", type=float,
                   default=float(env("P99_BOUND_MS", "50")))
    a.add_argument("--p99-factor", type=float,
                   default=float(env("P99_FACTOR", "5")))
    a.add_argument("--build-timeout", type=int, default=env("BUILD_TIMEOUT"))
    a.add_argument("--max-runtime", type=int, default=env("MAX_RUNTIME"))
    a.add_argument("--gdb-symbol",
                   default=env("GDB_SYMBOL", "ngx_http_cache_purge_index_busy"))
    a.add_argument("--keep-data", action="store_true",
                   help="leave nginx-written files; default keeps the seed")
    cfg = a.parse_args()

    prof = PROFILES[cfg.profile]
    for k, v in prof.items():
        if getattr(cfg, k, None) in (None, ""):
            setattr(cfg, k, v)
    for k in ("files", "workers", "duration", "purge_rate", "build_timeout",
              "max_runtime"):
        setattr(cfg, k, int(getattr(cfg, k)))
    cfg.gb = float(cfg.gb)
    cfg.sparse = not cfg.real
    cfg.http_extra = [s.strip() for s in cfg.http_extra]
    cfg.main_extra = [s.strip() for s in cfg.main_extra]
    return cfg


def watchdog(cfg, res):
    def fire():
        time.sleep(cfg.max_runtime)
        res.add("watchdog", False, {"max_runtime_s": cfg.max_runtime},
                ["the run exceeded --max-runtime and was aborted"])
        print(res.write(), flush=True)
        os._exit(3)
    threading.Thread(target=fire, daemon=True).start()


def latency_ok(cfg, base, got, allow_errors=False):
    bound = max(cfg.p99_bound_ms, cfg.p99_factor * base.get("p99_ms", 0))
    ok = got.get("n", 0) > 0 and got.get("p99_ms", 1e9) <= bound
    if not allow_errors:
        ok = ok and got.get("errors", 0) == 0 and got.get("non200", 0) == 0
    return ok, bound


def save_logs(*ngxs):
    for n in ngxs:
        if n is None:
            continue
        d = os.path.join(RESULTS, "logs", n.name)
        os.makedirs(d, exist_ok=True)
        for f in ("error.log", "stderr.log"):
            src = os.path.join(n.prefix, "logs", f)
            if os.path.exists(src):
                shutil.copy(src, os.path.join(d, f))
        src = os.path.join(n.prefix, "conf", "nginx.conf")
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, "nginx.conf"))


def fresh_run_dir():
    shutil.rmtree(os.path.join(DATA, "run"), ignore_errors=True)


# -- the mixed load phase, shared by the profiles ----------------------------

def mixed_load(cfg, ks, tracked, res, name, chaos_fn=None, bound=30.0,
               allow_notfound=False, chaos=False, duration=None,
               purge_rate=None):
    duration = duration or cfg.duration
    ver = P.Verifier(tracked, bound=bound, allow_notfound=allow_notfound,
                     chaos=chaos)
    pd = P.PurgeDriver(ks, purge_rate or cfg.purge_rate, chaos=chaos)
    wrk = P.Wrk(cfg, ks, duration)
    prober = P.Prober(P.hot_keys(ks, 128), conns=4)

    wrk.start()
    pd.start()
    ver.start()
    prober.start()
    events = chaos_fn(duration) if chaos_fn else None
    w = wrk.finish(duration + 120)
    pd_res = pd.finish()
    lat = prober.finish()
    v = ver.finish()

    werr = (w or {}).get("errors", {})
    notes = []
    ok = w is not None and "error" not in w and w.get("requests", 0) > 0
    if not chaos:
        ok = ok and sum(werr.values()) == 0
    elif werr.get("status", 0):
        ok = False
        notes.append("non-2xx GET responses under chaos: %d" % werr["status"])
    if pd_res["bad"]:
        ok = False
        notes.append("bad purge answers: %s" % pd_res["bad_sample"])
    if v["violations"] or v["cycles"] == 0:
        ok = False
        notes += ["violation: %s" % x for x in ver.violations[:20]]
    if v["cycles"] == 0:
        notes.append("the verifier completed no cycle")

    details = {"wrk": w, "purges": pd_res, "verifier": v,
               "probe_latency_under_load": lat}
    if events is not None:
        details["chaos_events"] = events
    res.add(name, ok, details, notes)
    return ok


# -- quick / scale ------------------------------------------------------------

def run_standard(cfg, res):
    r = rng(cfg.seed)
    meta = P.ensure_seed(cfg, res)
    ks = KeySet(meta)
    fresh_run_dir()
    origin = P.start_origin(cfg)
    tracked = P.Tracked()
    tracked.init_all()
    cache = None

    try:
        # 1. start + initial index build
        disk0 = P.scan()["files"]
        cache = P.start_cache(cfg, meta)
        t0 = time.time()
        b = P.wait_build(cache, cache.mark, cfg.build_timeout)
        # nginx starts its cache loader 60 s after the master: not waited for
        loader = P.wait_loader(cache, cache.mark, 0)
        ok = b is not None and b["files_read"] == disk0 \
            and b["entries"] == b["files_read"]
        res.add("index build", ok, {"disk_files": disk0, "build": b,
                                    "wall_s": round(time.time() - t0, 2),
                                    "cache_loader_s": loader},
                [] if ok else ["build numbers do not match the disk"])
        if b is None:
            return

        # 2. idle baseline latency
        prober = P.Prober(P.hot_keys(ks), conns=16)
        prober.warm()
        prober.start()
        time.sleep(5)
        base = prober.finish()
        res.add("latency: idle baseline", base["n"] > 0
                and base["errors"] == 0 and base["non200"] == 0, base)

        # 3. mixed load + purges + the correctness verifier
        mixed_load(cfg, ks, tracked, res, "mixed load + purges + verifier")

        # 4. disk / index consistency
        P.consistency(ks, "consistency after load", res, r,
                      timeout=120 if cfg.profile == "quick" else 900)

        # 5. restart: the build from disk, and latency during it.  The probe
        # keys are cached first, so no fill adds files during the build.
        prober.warm()
        cache.stop()
        disk1 = P.scan()["files"]
        cache = P.start_cache(cfg, meta)
        prober.start()
        b = P.wait_build(cache, cache.mark, cfg.build_timeout)
        time.sleep(1)
        lat = prober.finish()
        ok = b is not None and b["files_read"] == disk1 \
            and b["entries"] == b["files_read"]
        res.add("index rebuild after restart", ok,
                {"disk_files": disk1, "build": b})
        lok, bound = latency_ok(cfg, base, lat)
        res.add("latency: during index build", lok,
                dict(lat, bound_ms=bound, build_ms=(b or {}).get("build_ms")))

        # 6. bulk background purge: whole tenants, latency meanwhile
        tenants = r.sample(range(P.HOT_TENANTS, ks.tenants), 8)
        prefixes = ["/cdn/t%d/" % t for t in tenants]
        before = P.scan(prefixes)["matches"]
        prober.start()
        c = Client()
        status = [c.request("PURGE", p + "*", retries=3)[0] for p in prefixes]
        c.close()
        t1 = time.time()
        left = None
        while time.time() - t1 < (120 if cfg.profile == "quick" else 1800):
            left = sum(P.scan(prefixes)["matches"].values())
            if left == 0:
                break
            time.sleep(1)
        lat = prober.finish()
        ok = left == 0 and all(s in (200, 202) for s in status)
        res.add("bulk background purge", ok,
                {"files": sum(before.values()), "status": status,
                 "seconds": round(time.time() - t1, 2), "left": left})
        lok, bound = latency_ok(cfg, base, lat)
        res.add("latency: during bulk purge", lok, dict(lat, bound_ms=bound))

        # 7. purge_all of the images zone
        before = P.scan(zones=("images",))["files"]
        vbefore = P.scan(zones=("videos",))["files"]
        prober.start()
        c = Client()
        st = c.request("PURGE", "/purgeall/images", retries=3)[0]
        t1 = time.time()
        hot = len(prober.keys)
        while time.time() - t1 < (120 if cfg.profile == "quick" else 1800):
            if P.scan(zones=("images",))["files"] <= hot:
                break
            time.sleep(1)
        lat = prober.finish()
        # the prober refilled its keys meanwhile: once more, then nothing
        st2 = c.request("PURGE", "/purgeall/images", retries=3)[0]
        left = None
        while time.time() - t1 < (180 if cfg.profile == "quick" else 2400):
            left = P.scan(zones=("images",))["files"]
            if left == 0:
                break
            time.sleep(0.5)
        c.close()
        vafter = P.scan(zones=("videos",))["files"]
        ok = st in (200, 202) and st2 in (200, 202) and left == 0 \
            and vafter == vbefore
        res.add("purge_all images zone", ok,
                {"files": before, "status": [st, st2], "left": left,
                 "videos_untouched": vafter == vbefore,
                 "seconds": round(time.time() - t1, 2)})
        lok, bound = latency_ok(cfg, base, lat)
        res.add("latency: during purge_all", lok, dict(lat, bound_ms=bound))

        P.audit(cache, 0, "error log audit", res)

    finally:
        if cache is not None:
            cache.stop()
        origin.stop()
        save_logs(cache, origin)


# -- chaos ---------------------------------------------------------------------

def wait_index_idle(cache, since, timeout):
    """No index build running: in every zone, the last "building" has a
    "complete" (or a failure) after it."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        text = cache.log.read(since)
        last = {}
        for m in re.finditer(r'building the key index of "([^"]+)"', text):
            last[m.group(1)] = m.end()
        if all(re.search(r'key index build of "%s" (complete|failed)'
                         % re.escape(z), text[pos:])
               for z, pos in last.items()):
            return True
        time.sleep(0.5)
    return False


def run_chaos(cfg, res):
    r = rng(cfg.seed)
    meta = P.ensure_seed(cfg, res)
    ks = KeySet(meta)
    fresh_run_dir()
    origin = P.start_origin(cfg)
    tracked = P.Tracked()
    tracked.init_all()
    cache = None
    allow = list(P.CHAOS_ALLOW)

    try:
        # 1. kills and a reload while the index is being built -- slowed
        # down so that they land in the middle of it; afterwards a reload
        # to the normal settings keeps the index
        cache = P.start_cache(cfg, meta, reconcile="10s", throttle="40ms",
                              walk_budget="1ms")
        mark = cache.mark
        events = []
        for _ in range(3):
            time.sleep(r.uniform(0.02, 0.4))
            events.append(("kill", P.kill_random_worker(cache, r)))
        cache.reload()
        events.append(("reload", None))
        time.sleep(r.uniform(0.05, 0.3))
        events.append(("kill", P.kill_random_worker(cache, r)))
        b = P.wait_build(cache, mark, cfg.build_timeout)
        idle = wait_index_idle(cache, mark, cfg.build_timeout)
        text = cache.log.read(mark)
        res.add("chaos: index build under kill -9 and reload",
                b is not None and idle and cache.alive(),
                {"build": b, "events": events,
                 "builds_started": text.count("building the key index"),
                 "units_reclaimed": len(re.findall(r"handed out again",
                                                   text))})
        rmark = cache.log.size()
        cache.reload(P.cache_conf(cfg, meta, reconcile="10s"))
        time.sleep(2)
        rebuilt = "building the key index" in cache.log.read(rmark)
        res.add("chaos: reload keeps the index", not rebuilt,
                {"rebuilt_after_reload": rebuilt})

        # 2. mixed load with kills, reloads, external deletes, gdb
        gdb_info = {}
        ext = {}

        def chaos_loop(duration):
            ev = []
            t0 = time.time()
            next_kill = t0 + r.uniform(1, 3)
            next_reload = t0 + r.uniform(8, 14)
            did_ext = did_gdb = did_reload_pass = False
            while time.time() - t0 < duration:
                now = time.time()
                el = now - t0
                if now >= next_kill:
                    ev.append((round(el, 1), "kill",
                               P.kill_random_worker(cache, r)))
                    next_kill = now + r.uniform(1.5, 4)
                if now >= next_reload:
                    cache.reload()
                    ev.append((round(el, 1), "reload", None))
                    next_reload = now + r.uniform(8, 14)
                if not did_reload_pass and el > duration * 0.2:
                    did_reload_pass = True
                    t = r.randrange(ks.tenants)
                    P.SENT_PURGES.append("/cdn/t%d/" % t)
                    st = Client().request("PURGE", "/cdn/t%d/*" % t,
                                          retries=3)[0]
                    cache.reload()
                    ev.append((round(el, 1), "big purge + reload", st))
                if not did_ext and el > duration * 0.3:
                    did_ext = True
                    ext.update(P.external_delete(ks, r))
                    ev.append((round(el, 1), "external delete", ext))
                if not did_gdb and el > duration * 0.5:
                    did_gdb = True
                    gmark = cache.log.size()
                    g = P.gdb_kill_in_index_change(cache, cfg.gdb_symbol)
                    g["mark"] = gmark
                    gdb_info.update(g)
                    ev.append((round(el, 1), "gdb kill in index change",
                               g["result"]))
                    next_kill = time.time() + 5
                time.sleep(0.1)
            return ev

        mixed_load(cfg, ks, tracked, res,
                   "chaos: mixed load + purges + verifier under kills",
                   chaos_fn=chaos_loop, bound=90.0, chaos=True)

        # the gdb kill: a reset, then a rebuild
        if gdb_info.get("result") == "no symbol":
            res.add("chaos: kill -9 inside an index change", True,
                    {"gdb": gdb_info}, ["symbol %s not found" %
                                        cfg.gdb_symbol], skipped=True)
        else:
            gm = gdb_info.get("mark", 0)
            alert = cache.log.wait(r'died while updating the key index '
                                   r'of "([^"]+)"', since=gm, timeout=60)
            rebuilt = None
            if alert:
                # only the index of that zone is reset and rebuilt
                rebuilt = P.wait_build(cache, gm + alert.end(),
                                       cfg.build_timeout,
                                       zones=(alert.group(1),))
            ok = gdb_info.get("result") == "killed in change" \
                and alert is not None and rebuilt is not None
            res.add("chaos: kill -9 inside an index change", ok,
                    {"gdb": {k: v for k, v in gdb_info.items()
                             if k != "out"},
                     "reset_alert": alert is not None, "rebuild": rebuilt},
                    [] if ok else ["gdb output: " +
                                   gdb_info.get("out", "")[-800:]])

        # 3. quiesce, then the disk must agree with the purges
        idle = wait_index_idle(cache, mark, cfg.build_timeout)
        res.add("chaos: index settles", idle and cache.alive(),
                {"master_alive": cache.alive()})
        P.consistency(ks, "chaos: consistency after chaos", res, r,
                      timeout=300, chaos=True)

        # files removed behind nginx's back come back on the next GET
        k = P.key_in_dir(ks, ext.get("dir_removed", ""), r) \
            if ext.get("dir_removed") else None
        if k:
            c = Client()
            st, h, _ = c.request("GET", k, retries=5)
            c.close()
            path = os.path.join(ext["dir_removed"],
                                __import__("hashlib").md5(
                                    k.encode()).hexdigest())
            ok = st == 200 and os.path.exists(path)
            res.add("chaos: removed directory is refilled", ok,
                    {"key": k, "status": st, "x_cache": h.get("x-cache"),
                     "file_back": os.path.exists(path)})

        P.audit(cache, 0, "chaos: error log audit (kills, reloads, gdb)",
                res, allow)
        cache.stop()

        # 4. an index too small to hold the cache: walks, still correct
        # every cache zone has an index of its own, each at least 8 pages
        cache = P.start_cache(cfg, meta, index_size="64k videos=32k",
                              reconcile="10s")
        m = cache.log.wait(r'key index build of "[^"]+" failed|could not '
                           r'plan the key index build|key index of "[^"]+" '
                           r'incomplete',
                           since=cache.mark, timeout=cfg.build_timeout)
        res.add("small index: build fails as expected", m is not None,
                {"log": m.group(0) if m else None})
        mixed_load(cfg, ks, tracked, res,
                   "small index: load + purges + verifier (walk fallback)",
                   bound=300.0, duration=max(20, cfg.duration // 3),
                   purge_rate=max(20, cfg.purge_rate // 4))
        P.consistency(ks, "small index: consistency", res, r, timeout=600)
        P.durability_kill_drainer(
            cache, tracked, res,
            "small index: queued purge survives kill -9 of all workers")
        P.audit(cache, cache.mark, "small index: error log audit", res,
                allow + P.SMALL_INDEX_ALLOW)
        cache.stop()

        # 5. cache manager evictions (max_size below the data) + reconcile
        imgs = P.scan(zones=("images",))
        cap = max(64 << 20, int(imgs["bytes"] * 0.4))
        cache = P.start_cache(cfg, meta, max_size="%dm" % (cap >> 20),
                              reconcile="5s")
        b = P.wait_build(cache, cache.mark, cfg.build_timeout)
        # evictions and reconcile both wait for nginx's cache loader, which
        # starts 60 s after the master
        loader = P.wait_loader(cache, cache.mark, 300)
        log("evictions: cache loader done after %ss" % loader)
        mixed_load(cfg, ks, tracked, res,
                   "evictions: load + purges + verifier",
                   bound=60.0, allow_notfound=True,
                   duration=max(30, cfg.duration // 2))
        # without load the cache manager gets the zone under max_size, and
        # reconcile passes drop the entries of what it evicted
        t1 = time.time()
        after = P.scan(zones=("images",))
        while after["bytes"] > cap * 1.05 and time.time() - t1 < 600:
            time.sleep(2)
            after = P.scan(zones=("images",))
        rmark = cache.log.size()
        m = cache.log.wait(r'index of "[^"]+" reconciled: checked \d+, dropped \d+',
                           since=rmark, timeout=60)
        dropped = sum(int(x) for x in re.findall(
            r'index of "[^"]+" reconciled: checked \d+, dropped (\d+)',
            cache.log.read(cache.mark)))
        res.add("evictions: cache manager evicted, index reconciled",
                after["bytes"] <= cap * 1.05 and m is not None
                and dropped > 0,
                {"images_bytes_before": imgs["bytes"],
                 "images_bytes_after": after["bytes"], "max_size": cap,
                 "seconds_to_max_size": round(time.time() - t1, 1),
                 "entries_dropped_by_reconcile": dropped,
                 "cache_loader_s": loader, "build": b})
        P.consistency(ks, "evictions: consistency", res, r, siblings=False,
                      timeout=300)
        P.audit(cache, cache.mark, "evictions: error log audit", res)

    finally:
        if cache is not None:
            cache.stop()
        origin.stop()
        save_logs(cache, origin)


def main():
    cfg = parse()
    os.makedirs(RESULTS, exist_ok=True)
    res = Results(cfg.profile)
    watchdog(cfg, res)
    log("profile %s: files=%d gb=%.1f sparse=%s workers=%d duration=%ds "
        "http_extra=%s main_extra=%s sanitize=%s"
        % (cfg.profile, cfg.files, cfg.gb, cfg.sparse, cfg.workers,
           cfg.duration, cfg.http_extra, cfg.main_extra,
           open("/usr/local/nginx/SANITIZE").read().strip()
           if os.path.exists("/usr/local/nginx/SANITIZE") else "?"))
    try:
        if cfg.profile == "chaos":
            run_chaos(cfg, res)
        else:
            run_standard(cfg, res)
    except Exception as e:
        import traceback
        traceback.print_exc()
        res.add("harness", False, {"exception": repr(e)},
                traceback.format_exc().splitlines()[-8:])
    print(res.write(), flush=True)
    sys.exit(0 if res.ok else 1)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(143))
    main()
