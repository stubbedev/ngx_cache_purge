#!/usr/bin/env python3
import http.client, os, random, sys, threading, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import tor
threads = os.environ.get("MODE", "threads") == "threads"
tor.start(tor.conf("cache_purge_index 64m;", workers=12, threads=threads, zones=[("za","levels=1:2","64m")]))
tor.wait_log('build of "za" complete')
stop = time.time() + float(os.environ.get("DUR", "60"))
bad = []; stats = {"checks": 0, "inserts": 0, "nomatch": 0}
lk = threading.Lock()
def inserter(tid):
    c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30); i = 0
    while time.time() < stop:
        i += 1
        try: tor.req("GET", "/za/ins/%d/%d" % (tid, i), c=c)
        except Exception: c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30)
        with lk: stats["inserts"] += 1
def nomatch():
    c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30)
    while time.time() < stop:
        try: st, _ = tor.req("PURGE", "/za/none/%d*" % random.randrange(10**9), c=c)
        except Exception: c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30); continue
        if st != 412: bad.append("nomatch purge -> %d" % st)
        with lk: stats["nomatch"] += 1
def checker(tid):
    c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30); i = 0
    while time.time() < stop:
        i += 1; k = "/za/chk/%d/%d/" % (tid, i)
        try:
            st, xc = tor.req("GET", k + "obj", c=c)
            st2, _ = tor.req("PURGE", k + "*", c=c)
            st3, xc3 = tor.req("GET", k + "obj", c=c)
        except Exception as e:
            c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=30); continue
        if xc != "MISS" or st2 != 200 or xc3 != "MISS":
            bad.append("%s: GET %s purge %d then %s" % (k, xc, st2, xc3))
        with lk: stats["checks"] += 1
ts = [threading.Thread(target=inserter, args=(i,)) for i in range(16)] + \
     [threading.Thread(target=nomatch) for _ in range(8)] + \
     [threading.Thread(target=checker, args=(i,)) for i in range(16)]
[t.start() for t in ts]; [t.join() for t in ts]
print(stats, "violations:", len(bad)); print("\n".join(bad[:10]))
tor.logscan("seq"); print("FAIL" if (bad or tor.FAIL) else "PASS", tor.FAIL)
tor.stop()
