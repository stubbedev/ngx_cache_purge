#!/usr/bin/env python3
"""Fault-injection torture scenarios for ngx_cache_purge (dev container)."""
import http.client, os, random, re, shutil, signal, subprocess, sys, threading, time

N = os.environ.get("TOR_NGINX", "/usr/local/nginx/sbin/nginx")
R = "/tmp/tor"
FAIL = []

def log(*a): print(*a, flush=True)

def conf(extra_http="", zones=None, workers=4, main="", threads=True, rlimit=65536, conns=4096):
    zones = zones or [("za", "levels=1:2", "20m"), ("zb", "levels=1:2", "10m")]
    zc = "\n".join("    proxy_cache_path %s/%s %s keys_zone=%s:%s inactive=60m use_temp_path=off;" % (R, z, lv, z, sz) for z, lv, sz in zones)
    locs = "\n".join("""        location /%s/ { proxy_pass http://127.0.0.1:8081; proxy_cache %s; proxy_cache_key "$uri$is_args$args"; proxy_cache_valid 200 1h; proxy_cache_purge PURGE from 127.0.0.1; add_header X-Cache $upstream_cache_status; }""" % (z, z) for z, _, _ in zones)
    t = "cache_purge_thread_pool default tasks=4;" if threads else ""
    return f"""worker_processes {workers};
error_log {R}/error.log notice;
pid {R}/nginx.pid;
worker_rlimit_nofile {rlimit};
{main}
events {{ worker_connections {conns}; }}
http {{
    access_log off;
{zc}
    cache_purge_background_queue on;
    cache_purge_throttle_ms 10ms;
    cache_purge_batch_size 100;
    cache_purge_index_sync_limit 64;
    {t}
    {extra_http}
    server {{
        listen 8080 reuseport;
{locs}
    }}
    server {{ listen 8081; location / {{ add_header Vary X-V; return 200 "body $uri $http_x_v\\n"; }} }}
}}
"""

def start(c):
    subprocess.run(["pkill", "-9", "nginx"], capture_output=True)
    time.sleep(0.3)
    shutil.rmtree(R, ignore_errors=True); os.makedirs(R)
    open(R + "/nginx.conf", "w").write(c)
    rc, err = run_nginx()
    if rc: raise SystemExit("start failed: " + err)
    time.sleep(0.5)

def run_nginx(*args):
    """start (or signal) nginx; a daemon keeps inherited pipes open, so its
    output goes to a file"""
    with open(R + "/start.out", "w+") as fh:
        rc = subprocess.call([N, "-c", R + "/nginx.conf"] + list(args), stdout=fh, stderr=fh)
        fh.seek(0)
        return rc, fh.read()

def restart_keep(c=None):
    """stop and start nginx on the same cache dirs"""
    run_nginx("-s", "stop"); time.sleep(1)
    if c: open(R + "/nginx.conf", "w").write(c)
    open(R + "/error.log", "a").write("---- restart\n")
    rc, err = run_nginx()
    if rc: raise SystemExit("restart failed: " + err)
    time.sleep(0.5)

def stop():
    run_nginx("-s", "quit"); time.sleep(1)
    subprocess.run(["pkill", "-9", "nginx"], capture_output=True)

def req(m, u, h=None, c=None):
    own = c is None
    c = c or http.client.HTTPConnection("127.0.0.1", 8080, timeout=30)
    c.request(m, u, headers=h or {}); r = c.getresponse(); r.read()
    if own: c.close()
    return r.status, r.getheader("X-Cache")

def fill(zone, prefix, n, par=16):
    def w(i0):
        c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30)
        for i in range(i0, n, par):
            for att in range(5):
                try: req("GET", "/%s/%s%d" % (zone, prefix, i), c=c); break
                except Exception: c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30)
    ts = [threading.Thread(target=w, args=(i,)) for i in range(par)]
    [t.start() for t in ts]; [t.join() for t in ts]

def disk(zone, pre):
    pre = ("/%s/%s" % (zone, pre)).encode(); n = 0
    for root, _, files in os.walk(R + "/" + zone):
        for f in files:
            if len(f) != 32: continue
            try:
                with open(os.path.join(root, f), "rb") as fh: d = fh.read(4096)
            except OSError: continue
            i = d.find(b"\nKEY: ")
            if i >= 0 and d[i + 6:].lower().startswith(pre): n += 1
    return n

def wait_gone(zone, pre, t=30):
    end = time.time() + t
    while time.time() < end:
        if disk(zone, pre) == 0: return True
        time.sleep(0.5)
    return False

def purge_and_check(zone, pre, name):
    before = disk(zone, pre)
    st, _ = req("PURGE", "/%s/%s*" % (zone, pre))
    ok = wait_gone(zone, pre)
    log("  %s: purge %s* -> %d, %d files before, gone=%s" % (name, pre, st, before, ok))
    if not ok: FAIL.append("%s: files left under %s after purge (%d)" % (name, pre, disk(zone, pre)))
    return ok

def logscan(name, allow=()):
    txt = open(R + "/error.log").read()
    bad = [l for l in txt.splitlines() if re.search(r"\[(alert|crit|emerg)\]|signal (11|6|7|8)|AddressSanitizer", l)
           and not any(re.search(a, l) for a in allow)]
    for l in bad[:10]: log("   LOG:", l[:220])
    if bad: FAIL.append("%s: %d unexpected alert/crit lines" % (name, len(bad)))
    return txt

def wait_log(pat, t=60, since=0):
    # since: an offset into the log
    end = time.time() + t
    while time.time() < end:
        if re.search(pat, open(R + "/error.log").read()[since:]): return True
        time.sleep(0.2)
    return False

# ---------------------------------------------------------------- scenarios

def s_tiny_index(threads):
    name = "tiny-index(%s)" % ("threads" if threads else "inline")
    log("==", name)
    start(conf("cache_purge_index 96k; cache_purge_index_reconcile 2s;", threads=threads))
    wait_log("build of \"za\" complete")
    fill("za", "t/a", 3000)
    fill("zb", "u/b", 500)
    if not wait_log("incomplete|build of .* failed"): FAIL.append(name + ": index never reported full")
    for p in ["t/a1", "t/a2", "u/b1"]:
        purge_and_check("za" if p.startswith("t") else "zb", p, name)
    purge_and_check("za", "t/", name)
    logscan(name, allow=[r"cache_purge_index zone is full|incomplete|build of .* failed|could not plan"])
    stop()

def s_gdb_crash(threads):
    name = "gdb-crash-in-change(%s)" % ("threads" if threads else "inline")
    log("==", name)
    start(conf("cache_purge_index 32m;", workers=2, threads=threads))
    wait_log("build of \"za\" complete")
    fill("za", "g/a", 300)
    pids = subprocess.run("pgrep -a nginx | grep 'worker process' | cut -d' ' -f1", shell=True, capture_output=True, text=True).stdout.split()
    victim = pids[0]
    gdbcmd = ["gdb", "-p", victim, "-batch",
              "-ex", "break ngx_http_cache_purge_index_busy if busy == 1",
              "-ex", "continue", "-ex", "finish",
              "-ex", "shell kill -9 %s" % victim]
    g = subprocess.Popen(gdbcmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    # new keys make some worker (maybe the victim) insert
    th = threading.Thread(target=fill, args=("za", "g/new", 400)); th.start()
    try: g.wait(30)
    except subprocess.TimeoutExpired: g.kill()
    th.join()
    if not wait_log("died while updating the key index", 10):
        FAIL.append(name + ": no reset after a crash inside a change")
    wait_log("build of \"za\" complete[\\s\\S]*build of \"za\" complete", 60)
    fill("za", "g/a", 300)
    purge_and_check("za", "g/a1", name)
    purge_and_check("za", "g/new", name)
    purge_and_check("za", "g/", name)
    logscan(name, allow=[r"exited on signal 9", r"died while updating", r"was locked by"])
    stop()

def s_external_rm(threads):
    name = "external-rm(%s)" % ("threads" if threads else "inline")
    log("==", name)
    start(conf("cache_purge_index 32m;", threads=threads))
    wait_log("build of \"za\" complete")
    fill("za", "x/a", 2000)
    purge_and_check("za", "x/a1", name)        # warms the level dir fds
    for d in os.listdir(R + "/za")[:6]:
        shutil.rmtree(os.path.join(R, "za", d), ignore_errors=True)
    for f in [os.path.join(r, x) for r, _, fs in os.walk(R + "/za") for x in fs][:200]:
        try: os.unlink(f)
        except OSError: pass
    fill("za", "x/a", 2000)                    # recreates dirs and files
    purge_and_check("za", "x/a2", name)
    purge_and_check("za", "x/", name)
    logscan(name)
    stop()

def s_levels(threads):
    name = "levels(%s)" % ("threads" if threads else "inline")
    log("==", name)
    zones = [("z3", "levels=1:2:2", "10m"), ("z0", "", "10m"), ("z1", "levels=2", "10m")]
    start(conf("cache_purge_index 32m; cache_purge_index_refresh 2s;", zones=zones, threads=threads))
    for z in ["z3", "z0", "z1"]:
        wait_log('build of "%s" complete' % z)
        fill(z, "l/a", 800)
    # restart: the build reads what is on disk
    restart_keep()
    for z in ["z3", "z0", "z1"]:
        wait_log('---- restart[\\s\\S]*build of "%s" complete' % z)
        purge_and_check(z, "l/a1", name)
        purge_and_check(z, "l/", name)
    logscan(name)
    stop()

def s_emfile(threads):
    name = "fd-exhaustion(%s)" % ("threads" if threads else "inline")
    log("==", name)
    Z = [("za", "levels=1", "20m"), ("zb", "levels=1:2", "10m")]
    start(conf("cache_purge_index 32m;", zones=Z, workers=2, threads=threads, rlimit=65536))
    wait_log("build of \"za\" complete")
    fill("za", "e/a", 6000)
    # rebuild on restart with a tiny descriptor limit
    restart_keep(conf("cache_purge_index 32m;", zones=Z, workers=2, threads=threads, rlimit=int(os.environ.get("RL","90")), conns=20).replace("tasks=4", "tasks=16"))
    time.sleep(3)
    txt = open(R + "/error.log").read().split("---- restart")[-1]
    complete = "build of \"za\" complete" in txt
    failed = bool(re.search(r"build of \"za\" failed|could not be read", txt))
    log("  build complete=%s failed=%s" % (complete, failed))
    if complete and failed: FAIL.append(name + ": build both complete and failed")
    for p in ["e/a1", "e/a2", "e/"]:
        purge_and_check("za", p, name)
    if failed:
        # the build is retried soon, with fewer descriptors at a time
        if not wait_log('build of \"za\" failed[\\s\\S]*build of \"za\" complete', 60):
            FAIL.append(name + ": the failed build never completed on retry")
    logscan(name, allow=[r"could not be read|build of .* failed|Too many open files|incomplete|could not read .* completely"])
    stop()

def s_purge_all_vary(threads):
    name = "vary+purge_all(%s)" % ("threads" if threads else "inline")
    log("==", name)
    start(conf("cache_purge_index 32m;", threads=threads))
    wait_log("build of \"za\" complete")
    for i in range(300):
        for v in "abc":
            req("GET", "/za/v/a%d" % i, {"X-V": v})
    n = disk("za", "") ; log("  files with variants:", n)
    purge_and_check("za", "v/a1", name)
    purge_and_check("za", "", name)
    logscan(name)
    stop()


def s_keys(threads):
    name = "key-edges(%s)" % ("threads" if threads else "inline")
    log("==", name)
    start(conf("cache_purge_index 32m;", threads=threads))
    wait_log("build of \"za\" complete")
    # keys around the 512-byte index truncation, and case folding
    base = "/za/k/"
    for L in (500, 509, 510, 511, 512, 513, 600, 2000):
        u = base + "L%d-" % L
        u = u + "x" * (L - len(u))
        st, _ = req("GET", u)
        if st != 200: FAIL.append("%s: GET len %d -> %d" % (name, L, st))
    req("GET", "/za/k/MiXeD/Case")
    # a prefix at the truncation boundary still reaches the long keys
    p511 = (base + "L2000-" + "x" * 600)[:511]
    st, _ = req("PURGE", p511 + "*"); log("  purge 511-byte prefix ->", st)
    if st not in (200, 202): FAIL.append("%s: 511-byte wildcard -> %d" % (name, st))
    st, _ = req("PURGE", (base + "x" * 600) + "*"); log("  purge >512-byte prefix ->", st)
    if st not in (400,): FAIL.append("%s: over-long wildcard should be 400, got %d" % (name, st))
    st, _ = req("PURGE", "/za/K/mixed/*"); log("  purge case-folded ->", st)
    if st not in (200, 202): FAIL.append("%s: case-insensitive wildcard -> %d" % (name, st))
    purge_and_check("za", "k/", name)
    logscan(name)
    stop()

def s_reload_storm(threads):
    name = "reload-storm(%s)" % ("threads" if threads else "inline")
    log("==", name)
    start(conf("cache_purge_index 32m;", workers=4, threads=threads))
    wait_log("build of \"za\" complete")
    fill("za", "r/a", 4000)
    # restart so a build runs, then reload over and over while it does and
    # while purges and fills go on
    restart_keep()
    stopflag = [False]
    def load():
        c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30)
        while not stopflag[0]:
            try:
                req("GET", "/za/r/a%d" % random.randrange(4000), c=c)
                if random.random() < 0.05: req("PURGE", "/za/r/a%d*" % random.randrange(400), c=c)
            except Exception: c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30)
    ts = [threading.Thread(target=load) for _ in range(8)]
    [t.start() for t in ts]
    for i in range(25):
        run_nginx("-s", "reload")
        time.sleep(random.uniform(0.05, 0.4))
    time.sleep(3)
    stopflag[0] = True; [t.join() for t in ts]
    if not wait_log('---- restart[\\s\\S]*build of "za" complete', 60):
        FAIL.append(name + ": build never completed through the reloads")
    purge_and_check("za", "r/a1", name)
    purge_and_check("za", "r/", name)
    logscan(name)
    stop()

def s_many_zones(threads):
    name = "many-zones(%s)" % ("threads" if threads else "inline")
    log("==", name)
    zones = [("m%d" % i, "levels=1:2", "2m") for i in range(12)]
    start(conf("cache_purge_index 64m m3=1m;", zones=zones, threads=threads))
    for z, _, _ in zones:
        wait_log('build of "%s" complete' % z)
    ths = [threading.Thread(target=fill, args=(z, "q/a", 300, 4)) for z, _, _ in zones]
    [t.start() for t in ths]; [t.join() for t in ths]
    for z, _, _ in zones:
        purge_and_check(z, "q/a1", name)
    purge_and_check("m3", "q/", name)
    purge_and_check("m7", "", name)
    # the others untouched
    if disk("m5", "q/") == 0: FAIL.append(name + ": purge of one zone emptied another")
    logscan(name)
    stop()


def workers():
    return subprocess.run("pgrep -a nginx | grep 'worker process' | cut -d' ' -f1", shell=True, capture_output=True, text=True).stdout.split()

def s_kill_after_202(threads):
    name = "kill-after-202(%s)" % ("threads" if threads else "inline")
    log("==", name)
    # no index: every wildcard is a queued walk, answered 202
    start(conf("cache_purge_walk_budget 1ms;", workers=2, threads=threads).replace("cache_purge_throttle_ms 10ms", "cache_purge_throttle_ms 50ms"))
    fill("za", "k/a", 3000)
    for round_ in range(6):
        st, _ = req("PURGE", "/za/k/a%d*" % round_)
        if st != 202: FAIL.append("%s: expected 202, got %d" % (name, st))
        # the drainer dies mid-pass, several times over
        for _ in range(3):
            time.sleep(random.uniform(0.02, 0.3))
            for p in workers():
                try: os.kill(int(p), signal.SIGKILL)
                except ProcessLookupError: pass
    for round_ in range(6):
        if not wait_gone("za", "k/a%d" % round_, 90):
            FAIL.append("%s: purge k/a%d* lost after kill -9 (%d files left)" % (name, round_, disk("za", "k/a%d" % round_)))
    log("  left: " + str([disk("za", "k/a%d" % r) for r in range(6)]))
    logscan(name, allow=[r"exited on signal 9", r"was locked by", r"died while updating"])
    stop()

def s_reset_during_build(threads):
    name = "reset-during-build(%s)" % ("threads" if threads else "inline")
    log("==", name)
    c = conf("cache_purge_index 64m; cache_purge_walk_budget 2ms;", workers=2, threads=threads).replace("tasks=4", "tasks=1")
    start(c)
    wait_log('build of "za" complete')
    fill("za", "b/a", 20000, par=32)
    restart_keep()
    # purges while the build runs: registered in the build's list
    for i in range(10):
        st, _ = req("PURGE", "/za/b/a%d*" % i)
        log("  purge during build ->", st)
    # a worker dies inside an index change: the index (and its list) resets
    pids = workers()
    g = subprocess.Popen(["gdb", "-p", pids[0], "-batch", "-ex", "break ngx_http_cache_purge_index_busy if busy == 1",
                          "-ex", "continue", "-ex", "finish", "-ex", "shell kill -9 %s" % pids[0]],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    th = threading.Thread(target=fill, args=("za", "b/new", 300)); th.start()
    try: g.wait(30)
    except subprocess.TimeoutExpired: g.kill()
    th.join()
    reset = wait_log("died while updating the key index", 5)
    log("  reset happened:", reset)
    for i in range(10):
        if not wait_gone("za", "b/a%d" % i, 120):
            FAIL.append("%s: purge b/a%d* lost across the reset (%d left)" % (name, i, disk("za", "b/a%d" % i)))
    logscan(name, allow=[r"exited on signal 9", r"was locked by", r"died while updating"])
    stop()


def s_reload_while_locked(threads):
    """A worker holds a shm lock when it is killed, and the master is asked
    to reload before it reaps it: the reload must not wait for that lock."""
    name = "reload-while-locked(%s)" % ("threads" if threads else "inline")
    log("==", name)
    for sym, trigger in (("break ngx_http_cache_purge_index_busy if busy == 1", "fill"),
                         ("rbreak ^ngx_http_cache_purge_queue_lane", "purge")):
        start(conf("cache_purge_index 32m;", workers=1, threads=threads))
        wait_log('build of "za" complete')
        fill("za", "w/a", 200)
        victim = workers()[0]
        g = subprocess.Popen(["gdb", "-p", victim, "-batch", "-ex", sym,
                              "-ex", "continue", "-ex", "shell touch /tmp/tor/stopped",
                              "-ex", "shell sleep 3600"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2)
        stopflag = [False]
        def load():
            c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=5)
            i = 0
            while not stopflag[0] and not os.path.exists(R + "/stopped"):
                i += 1
                try:
                    if trigger == "fill": req("GET", "/za/w/new%d" % i, c=c)
                    else: req("PURGE", "/za/w/*", c=c)   # > sync limit: queued
                except Exception: c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=5)
        ts = [threading.Thread(target=load) for _ in range(4)]
        [t.start() for t in ts]
        end = time.time() + 30
        while time.time() < end and not os.path.exists(R + "/stopped"): time.sleep(0.1)
        stopflag[0] = True; [t.join() for t in ts]
        if not os.path.exists(R + "/stopped"):
            FAIL.append("%s/%s: breakpoint never hit" % (name, trigger)); g.kill(); stop(); continue
        # the victim is frozen holding the lock: reload first, then kill it
        mark = len(open(R + "/error.log").read())
        run_nginx("-s", "reload")
        time.sleep(0.5)
        os.kill(int(victim), signal.SIGKILL)
        g.kill()
        ok = wait_log(r"reconfiguring[\s\S]*start worker process", 15, since=mark)
        if not ok: FAIL.append("%s/%s: the master did not finish the reload" % (name, trigger))
        st = None
        for _ in range(50):
            try: st, _ = req("GET", "/za/w/a1"); break
            except Exception: time.sleep(0.2)
        if st != 200: FAIL.append("%s/%s: not serving after the reload (%s)" % (name, trigger, st))
        purge_and_check("za", "w/", name + "/" + trigger)
        logscan(name, allow=[r"exited on signal 9", r"was locked by", r"died while updating"])
        stop()
        try: os.unlink(R + "/stopped")
        except OSError: pass

if __name__ == "__main__":
    only = sys.argv[1:]
    for f in [s_tiny_index, s_gdb_crash, s_external_rm, s_levels, s_emfile, s_purge_all_vary, s_keys, s_reload_storm, s_many_zones, s_kill_after_202, s_reset_during_build, s_reload_while_locked]:
        for th in ((True, False) if not os.environ.get("MODE") else (os.environ["MODE"] == "threads",)):
            if only and f.__name__ not in only: continue
            try: f(th)
            except SystemExit as e: FAIL.append("%s: %s" % (f.__name__, e)); stop()
            except Exception as e: FAIL.append("%s: exception %r" % (f.__name__, e)); stop()
    log("\nFAILURES:" if FAIL else "\nALL PASSED")
    for x in FAIL: log(" -", x)
    sys.exit(1 if FAIL else 0)
