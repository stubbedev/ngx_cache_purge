#!/usr/bin/env python3
"""Fault-injection torture scenarios for ngx_cache_purge (dev container)."""
import http.client, os, random, re, shutil, signal, subprocess, sys, threading, time

N = os.environ.get("TOR_NGINX", "/usr/local/nginx/sbin/nginx")
N_DEFAULT = N
N_NOAIO = "/usr/local/nginx-noaio/sbin/nginx"      # threads, no file AIO
R = "/tmp/tor"
SRC = os.environ.get("TOR_SRC", "/module/ngx_cache_purge_module.c")
SHIM = "/tmp/tor_shim.so"
ENV = {}                # extra environment for nginx (LD_PRELOAD...)
FAIL = []

def log(*a): print(*a, flush=True)

def conf(extra_http="", zones=None, workers=4, main="", threads=True, rlimit=65536, conns=4096, loc_extra="", locs_extra=""):
    zones = zones or [("za", "levels=1:2", "20m"), ("zb", "levels=1:2", "10m")]
    zc = "\n".join("    proxy_cache_path %s/%s %s keys_zone=%s:%s inactive=60m use_temp_path=off;" % (R, z, lv, z, sz) for z, lv, sz in zones)
    locs = "\n".join("""        location /%s/ { proxy_pass http://127.0.0.1:8081; proxy_cache %s; proxy_cache_key "$uri$is_args$args"; proxy_cache_valid 200 1h; proxy_cache_purge PURGE from 127.0.0.1; add_header X-Cache $upstream_cache_status; %s }""" % (z, z, loc_extra) for z, _, _ in zones)
    locs += "\n" + locs_extra
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
    T_START[0] = time.time()
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
    env = dict(os.environ, **ENV)
    with open(R + "/start.out", "w+") as fh:
        rc = subprocess.call([N, "-c", R + "/nginx.conf"] + list(args), stdout=fh, stderr=fh, env=env)
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
            # regular files only: opening a planted FIFO would block
            if not os.path.isfile(os.path.join(root, f)) or os.path.islink(os.path.join(root, f)): continue
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

def line_of(*texts):
    """the line of the module source holding the first of texts found once:
    breakpoints follow the code, not a line number (an older source has the
    older text)"""
    lines = open(SRC).read().split("\n")
    for t in texts:
        t, off = t if isinstance(t, tuple) else (t, 0)     # (text, lines after)
        hits = [i + 1 for i, l in enumerate(lines) if t in l]
        if len(hits) == 1: return "%s:%d" % (os.path.basename(SRC), hits[0] + off)
    raise SystemExit("source line not found: %r" % (texts,))

def shim(**faults):
    """build the fault shim once, and set the faults: eio=<dir part>,
    eloop=<name>, slow=<usec>; a fault set to None is removed"""
    if not os.path.exists(SHIM):
        subprocess.check_call(["gcc", "-shared", "-fPIC", "-O2", "-o", SHIM,
                               os.path.join(os.path.dirname(__file__), "tor_shim.c"), "-ldl"])
    for k, v in faults.items():
        f = "%s/fault.%s" % (R, k)
        if v is None:
            try: os.unlink(f)
            except OSError: pass
        else:
            open(f, "w").write(str(v))

def preload(on):
    if on:
        ENV["LD_PRELOAD"] = SHIM
        # the shim comes before the ASan runtime in an ASan build
        ENV["ASAN_OPTIONS"] = os.environ.get("ASAN_OPTIONS", "") + ":verify_asan_link_order=0"
    else:
        ENV.pop("LD_PRELOAD", None); ENV.pop("ASAN_OPTIONS", None)

def gdb_bg(pid, *cmds):
    args = ["gdb", "-p", str(pid), "-batch"]
    for c in cmds: args += ["-ex", c]
    return subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def wait_file(f, t=30):
    end = time.time() + t
    while time.time() < end:
        if os.path.exists(f): return True
        time.sleep(0.05)
    return False

T_START = [0.0]

def crashed(since=0):
    """a crash signal in the log, or an ASan report written since start()"""
    txt = open(R + "/error.log").read()[since:]
    asan = [f for f in os.listdir("/tmp") if f.startswith("asan")
            and os.path.getmtime("/tmp/" + f) >= T_START[0]]
    return bool(re.search(r"signal (11|6|7|8)|AddressSanitizer", txt)) or bool(asan)

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


# ------------------------------------------------ regression scenarios (#2)
#
# Each fix of the audit, positive (what must still work) and negative (the
# failure it prevents).  Run against the build before the fixes, the
# negatives fail.

def mode(threads): return "threads" if threads else "inline"

def gdb_release(g, t=15):
    """end a gdb that may still wait in "continue": SIGINT returns it to
    its script (which detaches), so no breakpoint is left in the worker"""
    if g.poll() is None:
        try: os.kill(g.pid, signal.SIGINT)
        except ProcessLookupError: pass
    try: g.wait(t)
    except subprocess.TimeoutExpired: g.kill()

def s_index_reset_window(threads):
    """A worker reads the index root without the lock (header filter) while
    another resets the index.  The reset used to publish NULL before the new
    root: a reader between its NULL check and its read crashed.

    w3 is stopped in index_record just before it reads the root again (its
    NULL check passed); w1 dies inside an index change; w2 (or w1's
    successor) then resets and is stopped inside the reset, after the pool
    was wiped; w3 goes on and reads."""
    name = "index-reset-window(%s)" % mode(threads)
    log("==", name)
    start(conf("cache_purge_index 32m;", workers=3, threads=threads))
    wait_log('build of "za" complete')
    fill("za", "i/a", 200)
    for f in ("reader", "resetter", "done"):
        try: os.unlink("%s/%s" % (R, f))
        except OSError: pass
    w1, w2, w3 = workers()[:3]
    reset_at = line_of("sh = ngx_http_cache_purge_index_create(shpool, generation);")
    read_at = line_of(("if (sh == NULL || sh->state == NGX_CACHE_PURGE_INDEX_FAILED)", -2),
                      "if (ngx_http_cache_purge_index_peek(ix)->state")
    wait_done = "shell while [ ! -f %s/done ]; do sleep 0.1; done" % R
    # w3: stop before the read; go on once a reset is under way
    g3 = gdb_bg(w3, "break " + read_at, "continue", "shell touch %s/reader" % R,
                "shell while [ ! -f %s/resetter ] && [ ! -f %s/done ]; do sleep 0.1; done" % (R, R),
                "delete", "detach")
    # w2: stop inside a reset until the end
    resetters = [gdb_bg(w2, "break " + reset_at, "continue",
                        "shell touch %s/resetter" % R, wait_done, "delete", "detach")]
    time.sleep(2)
    stopflag = [False]
    def load(prefix):
        c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=5); i = 0
        while not stopflag[0]:
            i += 1
            try: req("GET", "/za/i/%s%d" % (prefix, i), c=c)
            except Exception: c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=5)
    ts = [threading.Thread(target=load, args=("r%d-" % k,)) for k in range(12)]
    [t.start() for t in ts]
    # the reader first, then the crash inside a change
    ok_reader = wait_file(R + "/reader", 30)
    g1 = gdb_bg(w1, "break ngx_http_cache_purge_index_busy if busy == 1",
                "continue", "finish", "shell kill -9 %s" % w1)
    try: g1.wait(30)
    except subprocess.TimeoutExpired: gdb_release(g1)
    # w1's successor may be the one to reset
    time.sleep(0.3)
    for w in workers():
        if w not in (w2, w3):
            resetters.append(gdb_bg(w, "break " + reset_at, "continue",
                                    "shell touch %s/resetter" % R, wait_done, "delete", "detach"))
    ok_reset = wait_file(R + "/resetter", 30)
    time.sleep(2)                       # w3 reads inside the reset
    mark_crash = crashed()
    open(R + "/done", "w").close()
    stopflag[0] = True
    for g in [g3] + resetters: gdb_release(g)
    [t.join() for t in ts]
    log("  reader stopped: %s, reset stopped: %s" % (ok_reader, ok_reset))
    if not (ok_reader and ok_reset): FAIL.append(name + ": setup: breakpoints not hit")
    if mark_crash or crashed():
        FAIL.append(name + ": a worker crashed reading the index during a reset")
    # positive: the index is rebuilt and purges work
    if not wait_log(r'died while updating the key index[\s\S]*build of "za" complete', 60):
        FAIL.append(name + ": no rebuild after the reset")
    purge_and_check("za", "i/", name)
    logscan(name, allow=[r"exited on signal 9", r"died while updating", r"was locked by"])
    stop()

def s_queue_reset_cut(threads):
    """A process killed inside a queue reset, between wiping the pool and
    creating the queue, left no queue: every purge 429 until a restart."""
    name = "queue-reset-cut(%s)" % mode(threads)
    log("==", name)
    start(conf("", workers=1, threads=threads))
    fill("za", "c/a", 300)
    # 1: the worker dies holding the queue lock mid-change
    w = workers()[0]
    g = gdb_bg(w, "rbreak ^ngx_http_cache_purge_queue_lane", "continue", "shell kill -9 %s" % w)
    time.sleep(1.5)
    try: req("PURGE", "/za/c/a1*")
    except Exception: pass
    try: g.wait(30)
    except subprocess.TimeoutExpired: g.kill()
    time.sleep(1)
    # 2: the next one dies inside the reset, before the queue is created
    w = workers()[0]
    g = gdb_bg(w, "break " + line_of("queue = ngx_http_cache_purge_queue_create(cmcf, shpool, generation);"),
               "continue", "shell kill -9 %s" % w)
    time.sleep(1.5)
    try: req("PURGE", "/za/c/a2*")
    except Exception: pass
    try: g.wait(30)
    except subprocess.TimeoutExpired: g.kill()
    time.sleep(1)
    if not wait_log("died while updating the purge queue", 5):
        FAIL.append(name + ": setup: no reset"); stop(); return
    # negative: purges are queued again, not refused
    codes = []
    for i in range(3, 8):
        for _ in range(20):
            try: codes.append(req("PURGE", "/za/c/a%d*" % i)[0]); break
            except Exception: time.sleep(0.2)
    log("  purges after the cut reset:", codes)
    if any(c == 429 for c in codes) or len(codes) < 5:
        FAIL.append(name + ": queue refuses purges after a reset cut short: %s" % codes)
    # positive: they are carried out
    for i in range(3, 8):
        if not wait_gone("za", "c/a%d" % i, 30):
            FAIL.append("%s: purge c/a%d* not carried out" % (name, i))
    logscan(name, allow=[r"exited on signal 9", r"died while (updating|resetting)", r"was locked by"])
    stop()

def s_reclaim(threads):
    """A purge whose pass kills its worker every time.  It used to go back
    at the head of the queue at once: a crash loop, and the rest of its
    batch never done.  Now: backoff, alone, dropped after 3."""
    name = "reclaim(%s)" % mode(threads)
    log("==", name)
    # a slow drainer: the purges are queued before a pass takes them
    start(conf("", workers=1, threads=threads).replace("cache_purge_throttle_ms 10ms", "cache_purge_throttle_ms 300ms"))
    fill("za", "p/a", 200)
    fill("za", "q/a", 200)
    q_before = disk("za", "q/")
    # a function, not a line: pass_start is inlined into a function GCC
    # splits, and a line breakpoint misses the copy that runs; walk_start is
    # called with the batch already taken
    at = "ngx_http_cache_purge_walk_start"
    cond = '$_memeq(((ngx_http_cache_purge_pass_item_t *) pass->items.elts)[0].key.data, "/za/p/", 6)'
    kills = []
    queued = None
    for _ in range(6):
        ws = workers()
        if not ws: time.sleep(0.2); continue
        w = ws[0]
        try: os.unlink(R + "/hit")
        except OSError: pass
        g = gdb_bg(w, "break %s if %s" % (at, cond), "continue",
                   "shell touch %s/hit" % R, "shell kill -9 %s" % w)
        time.sleep(1.5)                  # attached, breakpoint set
        if queued is None:
            # same lane, one batch: p (poison) first
            queued = (req("PURGE", "/za/p/*")[0], req("PURGE", "/za/q/*")[0])
        if wait_file(R + "/hit", 20):
            kills.append(time.time())
            try: g.wait(10)
            except subprocess.TimeoutExpired: gdb_release(g)
            time.sleep(0.3)
        else:
            gdb_release(g)
            break
    gaps = [round(b - a, 1) for a, b in zip(kills, kills[1:])]
    log("  queued: %s, poison passes killed: %d, gaps %s" % (queued, len(kills), gaps))
    # negative: no crash loop, it is given up
    if len(kills) != 3:
        FAIL.append("%s: poison purge ran %d times, expected 3 then dropped" % (name, len(kills)))
    if not wait_log(r'purge of .* key "/za/p/\*" dropped: 3 workers died', 10):
        FAIL.append(name + ": no 'dropped' line for the poison purge")
    if len(gaps) >= 2 and gaps[1] < 1.5:
        FAIL.append("%s: no backoff between reclaims (gaps %s)" % (name, gaps))
    # positive: its batch mate is carried out, the poison's files are left
    if not wait_gone("za", "q/", 30):
        FAIL.append("%s: the batch mate q/* was never carried out (%d of %d left)" % (name, disk("za", "q/"), q_before))
    if disk("za", "p/") == 0:
        FAIL.append(name + ": the dropped purge was carried out after all")
    logscan(name, allow=[r"exited on signal 9", r"dropped: 3 workers died", r"was locked by"])
    stop()

def queued_conf(threads, size, throttle, extra=""):
    # no index: every wildcard is a queued walk; one per pass, slowly
    return conf("cache_purge_queue_size %d; %s" % (size, extra), workers=1, threads=threads) \
        .replace("cache_purge_throttle_ms 10ms", "cache_purge_throttle_ms %s" % throttle) \
        .replace("cache_purge_batch_size 100", "cache_purge_batch_size 1")

def s_resize_reload(threads):
    """A reload that changes cache_purge_queue_size gets a new queue zone;
    the purges queued in the old one were silently dropped."""
    name = "resize-reload(%s)" % mode(threads)
    log("==", name)
    for new_size, expect in ((2048, "grow"), (64, "same"), (4, "shrink")):
        start(queued_conf(threads, 64, "3s"))
        for i in range(10): fill("za", "r%d/a" % i, 20, 4)
        codes = [req("PURGE", "/za/r%d/*" % i)[0] for i in range(10)]
        mark = len(open(R + "/error.log").read())
        open(R + "/nginx.conf", "w").write(queued_conf(threads, new_size, "10ms"))
        run_nginx("-s", "reload")
        wait_log(r"start worker process", 10, since=mark)
        time.sleep(1)
        txt = open(R + "/error.log").read()[mark:]
        m = re.search(r"purge queue resized: (\d+) queued purge\(s\) moved to the new queue, (\d+) lost", txt)
        left = [i for i in range(10) if not wait_gone("za", "r%d/" % i, 20)]
        log("  %s (64 -> %d): queued %s, log %s, not carried out %s"
            % (expect, new_size, set(codes), m.groups() if m else None, left))
        if set(codes) != {202}: FAIL.append("%s/%s: not all queued: %s" % (name, expect, codes))
        if expect == "grow":
            # positive: all of them move, and are carried out
            if not m or int(m.group(2)) != 0: FAIL.append("%s/grow: no 'moved, 0 lost' line" % name)
            if left: FAIL.append("%s/grow: purges lost across the resize: %s" % (name, left))
        elif expect == "same":
            # the zone is kept as it is: nothing to move, nothing lost
            if m: FAIL.append("%s/same: a same-size reload moved the queue" % name)
            if left: FAIL.append("%s/same: purges lost across the reload: %s" % (name, left))
        else:
            # negative: what does not fit is reported, the rest carried out
            if not m or int(m.group(1)) + int(m.group(2)) < 9 or int(m.group(2)) == 0:
                FAIL.append("%s/shrink: loss not reported (%s)" % (name, m.groups() if m else None))
            elif len(left) > int(m.group(2)) + 1:
                FAIL.append("%s/shrink: more lost (%d) than reported (%s)" % (name, len(left), m.group(2)))
        logscan(name, allow=[r"purge queue resized: .* lost \(queue full\)"])
        stop()

def s_purge_all_vs_wildcard(threads):
    """purge_all whose request key equals a queued wildcard of the same
    cache was taken for that wildcard: 202, but only the prefix purged."""
    name = "purge_all-vs-wildcard(%s)" % mode(threads)
    log("==", name)
    loc = """        location /all/ { proxy_pass http://127.0.0.1:8081; proxy_cache za; proxy_cache_key "/za/x/*"; proxy_cache_purge PURGE purge_all from 127.0.0.1; }"""
    start(conf("", workers=1, threads=threads, locs_extra=loc)
          .replace("cache_purge_throttle_ms 10ms", "cache_purge_throttle_ms 2s"))
    fill("za", "x/a", 100); fill("za", "y/a", 100)
    st1, _ = req("PURGE", "/za/x/*")         # key "/za/x/*"
    st2, _ = req("PURGE", "/all/anything")   # purge_all, key "/za/x/*" too
    log("  wildcard -> %d, purge_all -> %d" % (st1, st2))
    # positive: the wildcard's files go; negative: so does everything else
    if not wait_gone("za", "x/", 30): FAIL.append(name + ": wildcard not carried out")
    if not wait_gone("za", "", 30):
        FAIL.append("%s: purge_all swallowed by an equal-key wildcard (%d files left)" % (name, disk("za", "")))
    logscan(name)
    stop()

def cache_files(zone):
    """regular files under zone (not planted FIFOs or symlinks)"""
    return [os.path.join(r, f) for r, _, fs in os.walk(R + "/" + zone) for f in fs
            if os.path.isfile(os.path.join(r, f)) and not os.path.islink(os.path.join(r, f))]

def key_of(path):
    d = open(path, "rb").read(4096)
    i = d.find(b"\nKEY: ")
    return d[i + 6:d.index(b"\n", i + 6)].decode()

def s_bad_files(threads):
    """Files in the cache tree that are no cache files of this nginx: their
    bytes at the key offset were indexed as keys and matched by walks.  And
    a name that openat() refuses with ELOOP (swapped for a symlink since
    readdir) failed the whole build unit, again and again."""
    name = "bad-files(%s)" % mode(threads)
    log("==", name)
    for indexed in (True, False):
        sub = name + ("/index" if indexed else "/walk")
        start(conf("cache_purge_index 32m;" if indexed else "", workers=2, threads=threads))
        if indexed: wait_log('build of "za" complete')
        fill("za", "f/a", 300)
        stop()
        real = sorted(cache_files("za"))
        def where(nm):
            # the directory its name (an md5) maps to with levels=1:2: the
            # index and the walk slots delete by that path
            d = os.path.join(R, "za", nm[-1], nm[-3:-1])
            os.makedirs(d, exist_ok=True)
            return os.path.join(d, nm)
        def plant(nm, data):
            open(where(nm), "wb").write(data); return where(nm)
        src = open(real[0], "rb").read()
        # another cache version, same key: not ours
        foreign = plant("f" * 32, bytes([src[0] ^ 0x7f]) + src[1:])
        # junk, and a file too short for a header
        junk = plant("e" * 32, os.urandom(3000))
        short = plant("d" * 32, b"\nKEY: /za/f/a1\n")
        # "KEY:" missing where the key goes
        nokey = plant("c" * 32, src.replace(b"\nKEY: ", b"\nXEY: ", 1))
        # a FIFO and a symlink out of the cache: skipped, never opened
        fifo = where("b" * 32); os.mkfifo(fifo)
        os.makedirs(R + "/outside", exist_ok=True)
        outside = R + "/outside/target"; open(outside, "wb").write(src)
        link = where("a" * 32); os.symlink(outside, link)
        # owned like nginx's own files: a match must be able to delete them
        import pwd
        nb = pwd.getpwnam("nobody")
        for root, dirs, files in os.walk(R + "/za"):
            for x in dirs + files:
                os.lchown(os.path.join(root, x), nb.pw_uid, nb.pw_gid)
        # openat() of this real file fails with ELOOP
        eloop = os.path.basename(real[1])
        shim(eloop=eloop); preload(True)
        restart_keep()
        t0 = time.time()
        if indexed:
            ok = wait_log(r'---- restart[\s\S]*build of "za" complete', 30)
            txt = open(R + "/error.log").read().split("---- restart")[-1]
            log("  %s: build complete %s in %.1fs" % (sub, ok, time.time() - t0))
            # negative: none of these fails the build (ELOOP included)
            if not ok or re.search(r'build of "za" failed|could not be read', txt):
                FAIL.append(sub + ": the build failed over planted files / ELOOP")
        # positive: the real files of the prefix go; negative: the planted
        # ones stay, the target outside too
        st, _ = req("PURGE", "/za/f/*")
        # (disk() would count the planted files that carry a real key)
        end = time.time() + 30
        while True:
            left_real = [f for f in real if os.path.exists(f) and os.path.basename(f) != eloop]
            if not left_real or time.time() > end: break
            time.sleep(0.3)
        survivors = [os.path.basename(f) for f in (foreign, junk, short, nokey, fifo, link, outside) if os.path.lexists(f)]
        log("  %s: purge -> %d, real files left %d/%d, planted left %d/7"
            % (sub, st, len(left_real), len(real) - 1, len(survivors)))
        if left_real: FAIL.append("%s: %d real files not purged" % (sub, len(left_real)))
        if len(survivors) != 7:
            FAIL.append("%s: planted files matched as cache entries: %s" % (sub, sorted(set("abcdef") - {s[0] for s in survivors})))
        preload(False); shim(eloop=None)
        # nginx's own cache loader reports the short one
        logscan(sub, allow=[r'cache file .* is too small'])
        stop()

def s_readdir_eio(threads):
    """readdir() failing (EIO) was taken for the end of the directory: a
    build marked its unit complete with files unindexed, and purge_all said
    200 over a cache it had not read."""
    name = "readdir-eio(%s)" % mode(threads)
    log("==", name)
    # the index build
    start(conf("cache_purge_index 32m;", workers=2, threads=threads))
    wait_log('build of "za" complete')
    fill("za", "e/a", 600)
    stop()
    lv1 = sorted(os.listdir(R + "/za"))[0]
    shim(eio="/za/%s" % lv1); preload(True)
    restart_keep()
    time.sleep(4)
    txt = open(R + "/error.log").read().split("---- restart")[-1]
    # negative: with a directory unreadable the build is not complete
    if 'build of "za" complete' in txt:
        FAIL.append(name + ": build complete with a directory it could not read")
    shim(eio=None)
    # positive: once readable, the retry completes and a wildcard reaches
    # the files of that directory too
    if not wait_log(r'---- restart[\s\S]*build of "za" complete', 90):
        FAIL.append(name + ": build never completed after the fault")
    purge_and_check("za", "e/", name)
    logscan(name, allow=[r"build of .* failed|could not be read|readdir|Input/output error"])
    preload(False)
    stop()
    # purge_all without the queue: a synchronous walk
    sub = name + "/sync-purge_all"
    loc = """        location /all/ { proxy_pass http://127.0.0.1:8081; proxy_cache za; proxy_cache_key $uri; proxy_cache_purge PURGE purge_all from 127.0.0.1; }"""
    c = conf("", workers=1, threads=threads, locs_extra=loc).replace("cache_purge_background_queue on;", "cache_purge_background_queue off;")
    start(c)
    fill("za", "s/a", 300)
    lv1 = sorted(os.listdir(R + "/za"))[0]
    shim(eio="/za/%s" % lv1); preload(True)
    restart_keep()
    st_fault, _ = req("PURGE", "/all/x")
    shim(eio=None)
    st_ok, _ = req("PURGE", "/all/x")
    log("  %s: under EIO -> %d, after -> %d, files left %d" % (sub, st_fault, st_ok, disk("za", "")))
    # negative: not 200 over an unread directory; positive: 200 and empty
    if st_fault == 200: FAIL.append(sub + ": 200 although a directory could not be read")
    if st_ok != 200 or disk("za", "") != 0: FAIL.append(sub + ": purge_all did not empty the cache")
    logscan(sub, allow=[r"readdir|Input/output error|could not"])
    preload(False)
    stop()

def s_vary_walk(threads):
    """cache_purge_vary_aware: the variants of a key of 511 bytes or more
    were left; a temp file of a fill in progress was deleted."""
    name = "vary-walk(%s)" % mode(threads)
    log("==", name)
    start(conf("cache_purge_vary_aware on;", workers=1, threads=threads))
    keys = {"short": "/za/v/short", "long": "/za/v/" + "L" * 600}
    for k in keys.values():
        for v in "abc": req("GET", k, {"X-V": v})
    req("GET", "/za/w/other", {"X-V": "a"})
    def files_of(k):
        return [f for f in cache_files("za") if len(os.path.basename(f)) == 32 and key_of(f) == k]
    counts = {n: len(files_of(k)) for n, k in keys.items()}
    log("  variant files:", counts)
    if min(counts.values()) < 2: FAIL.append(name + ": setup: no variants on disk")
    # a temp file of a fill in progress, with the long key's KEY: line
    lf = files_of(keys["long"])[0]
    tmp = lf + ".0000000042"
    shutil.copy(lf, tmp)
    for n, k in keys.items():
        st, _ = req("PURGE", k, {"X-V": "a"})
        left = len(files_of(k))
        log("  %s: purge -> %d, variants left %d" % (n, st, left))
        # positive: every variant goes, whatever the key length
        if st != 200 or left: FAIL.append("%s/%s: %d variants left" % (name, n, left))
    # negative: the temp file and another key stay
    if not os.path.exists(tmp): FAIL.append(name + ": a temp file (fill in progress) was deleted")
    if not files_of("/za/w/other"): FAIL.append(name + ": another key's file was deleted")
    logscan(name)
    stop()

def s_inline_budget(threads):
    """With a thread pool, a read that could not be posted (this worker's
    tasks in flight) ran inline without the walk budget: up to 1024 files
    read on the event loop.  A slow disk makes that a stall."""
    if not threads: return
    name = "inline-budget(threads)"
    log("==", name)
    # one thread, a queue of one: with slow reads the pool is full, the
    # build's posts fail and its reads run inline
    # levels=1: 16 units of ~1250 files, so one inline read can be long
    c = conf("cache_purge_index 64m; cache_purge_walk_budget 5ms;", workers=1, threads=True,
             zones=[("za", "levels=1", "20m")],
             main="thread_pool tp threads=1 max_queue=1;").replace("cache_purge_thread_pool default tasks=4;", "cache_purge_thread_pool tp tasks=4;")
    start(c)
    wait_log('build of "za" complete')
    fill("za", "b/a", 20000, par=32)
    shim(slow=1500); preload(True)          # 1.5 ms per key read
    restart_keep()
    stopflag = [False]; worst = [0.0]
    def probe():
        c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30)
        while not stopflag[0]:
            t = time.time()
            try: req("GET", "/za/b/a1", c=c)
            except Exception: c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30)
            worst[0] = max(worst[0], time.time() - t)
            time.sleep(0.01)
    def unlinks():
        # indexed purges during the build keep the single task busy
        c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30); i = 0
        while not stopflag[0]:
            i += 1
            try: req("PURGE", "/za/b/a%d*" % (100 + i % 5000), c=c)
            except Exception: c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30)
    ts = [threading.Thread(target=probe)] + [threading.Thread(target=unlinks) for _ in range(4)]
    [t.start() for t in ts]
    done = wait_log(r'---- restart[\s\S]*build of "za" complete', 300)
    stopflag[0] = True; [t.join() for t in ts]
    shim(slow=None); preload(False)
    log("  build complete %s, worst GET %.0f ms" % (done, worst[0] * 1000))
    # positive: the build completes; negative: no long event loop stall
    if not done: FAIL.append(name + ": build never completed")
    if worst[0] > 0.4: FAIL.append("%s: a GET waited %.0f ms behind an inline read" % (name, worst[0] * 1000))
    logscan(name)
    stop()

def s_aio_threads(threads):
    """aio threads on a build without file AIO: ngx_http_file_cache_open
    answers NGX_AGAIN; an exact purge answered 500 and purged nothing."""
    global N
    name = "aio-threads(%s)" % mode(threads)
    log("==", name)
    for binary in (N_NOAIO, N_DEFAULT):
        sub = "%s/%s" % (name, "no-file-aio" if binary == N_NOAIO else "file-aio")
        if not os.path.exists(binary): FAIL.append(sub + ": binary missing"); continue
        N = binary
        start(conf("", workers=2, threads=threads, loc_extra="aio threads;"))
        codes = []
        for i in range(20):
            k = "/za/t/a%d" % i
            codes.append((req("GET", k)[1], req("PURGE", k)[0], req("PURGE", k)[0], req("GET", k)[1]))
        log("  %s: %s" % (sub, sorted(set(codes))))
        # positive: a cached key purged (200), then MISS; negative: a key
        # not cached answers not-found, never 500
        bad = [c for c in codes if c != ("MISS", 200, 412, "MISS")]
        if bad: FAIL.append("%s: %s" % (sub, sorted(set(bad))))
        logscan(sub)
        stop()
    N = N_DEFAULT

if __name__ == "__main__":
    only = sys.argv[1:]
    for f in [s_tiny_index, s_gdb_crash, s_external_rm, s_levels, s_emfile, s_purge_all_vary, s_keys, s_reload_storm, s_many_zones, s_kill_after_202, s_reset_during_build, s_reload_while_locked,
              s_index_reset_window, s_queue_reset_cut, s_reclaim, s_resize_reload, s_purge_all_vs_wildcard,
              s_bad_files, s_readdir_eio, s_vary_walk, s_inline_budget, s_aio_threads]:
        for th in ((True, False) if not os.environ.get("MODE") else (os.environ["MODE"] == "threads",)):
            if only and f.__name__ not in only: continue
            try: f(th)
            except SystemExit as e: FAIL.append("%s: %s" % (f.__name__, e)); stop()
            except Exception as e: FAIL.append("%s: exception %r" % (f.__name__, e)); stop()
            preload(False); N = N_DEFAULT
    log("\nFAILURES:" if FAIL else "\nALL PASSED")
    for x in FAIL: log(" -", x)
    sys.exit(1 if FAIL else 0)
