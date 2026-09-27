# End-to-end suite

A real nginx (built from source with the module, `--with-threads`) in a
container, in front of a second nginx acting as origin, over a cache seeded
straight onto disk in nginx's own file format. It checks what users see and
what ends up on disk, not internals.

```bash
t/e2e/run.sh quick          # ~80k files, 3 GB (sparse), a few minutes
t/e2e/run.sh chaos          # kill -9, reloads, gdb, disk tampering, small index, evictions
t/e2e/run.sh scale          # 2.5M files, 200 GB (sparse by default)

E2E_SANITIZE=asan t/e2e/run.sh chaos        # AddressSanitizer build
NGINX_VERSION=1.31.6 t/e2e/run.sh quick     # another nginx
E2E_GB=400 E2E_FILES=5000000 E2E_SPARSE=0 t/e2e/run.sh scale   # real bytes
E2E_HTTP_EXTRA='cache_purge_thread_pool default;' \
E2E_MAIN_EXTRA='thread_pool default threads=16;' t/e2e/run.sh quick
```

Results go to `t/e2e/results/<time>-<profile>/`: `summary.txt` (human),
`summary.json`, and the nginx logs and configs. The exit status is 0 only
when every phase passed. Data lives in `t/e2e/.data/<profile>` (`E2E_DATA_DIR`) and is
reused while the seed parameters match and at least 90% of the seeded files
are still there.

## Layout

| Path | What |
|---|---|
| `Dockerfile` | ubuntu 24.04, nginx 1.30.5 + module, `SANITIZE=asan` option, wrk, gdb |
| `tools/cachegen.c` | `seed`: writes cache files (real `ngx_http_file_cache_header_t`, crc32, md5 paths, levels=1:2), multi-threaded, sparse or real bytes. `scan`: counts files and KEY prefix matches on disk. `key`: one key of the set. |
| `harness/e2e.py` | profiles and phases |
| `harness/phases.py` | nginx configs, verifier, purge driver, latency prober, consistency, audit, chaos actions |
| `harness/lib.py` | key set, nginx process control, HTTP client, results |
| `harness/load.lua` | wrk GET traffic: hits over the seeded keys, misses (fills) over unseeded ones |

The key set is a formula shared by all three languages: file `i` is variant
`i % 8` of asset `i / 8`, tenant `asset % 64`; a few assets are videos
(`/video/t<t>/a<a>/s<b>.mp4?r=<v>`, zone `videos`), the rest images
(`/cdn/t<t>/a<a>/s<b>.jpg?w=<w>`, zone `images`). Keys beyond the seeded
count are misses the origin can still serve (size-bucket blobs); keys under
`/cdn/trk/` and `/video/trk/` belong to the verifier alone and have per-key
origin files with a version on the first line.

## Phases and what they assert

* **seed / index build** -- the first build reads exactly the files on disk
  and indexes each once (`files read == entries == files on disk`).
* **latency: idle baseline** -- 16 keep-alive connections GETting cached
  small objects, one request at a time.
* **mixed load + purges + verifier** -- wrk hits and misses (fills, i.e.
  writes), FileCdnService-shaped purges (exact, per asset, match nothing,
  periodic large wildcards), and the verifier. Fails on any wrk error, any
  5xx purge answer, or any verifier violation.
* **verifier** (the core invariant) -- for its own keys: cached and a HIT,
  origin bumped, still the old version, then PURGE (exact, asset wildcard or
  group wildcard). After `200` the next GET must be the new version; after
  `202` the new version must appear within the bound and never regress;
  `404/412` for a key just seen as a HIT is a violation (unless evictions are
  expected, or the purge had to be retried after a broken connection).
* **consistency** -- stores a few unseeded assets through nginx, picks random
  asset, video and large prefixes, counts matching files on disk, purges them
  and polls the disk until nothing matches (every stored file is reachable
  through the index); sibling prefixes must be untouched; a purge that says
  "nothing matched" while files existed is a failure.
* **index rebuild after restart** -- same numbers as the disk again.
* **latency: during index build / bulk purge / purge_all** -- the probe's p99
  must stay under `max(--p99-bound-ms, --p99-factor x baseline p99)`
  (default `max(50 ms, 5x)`), with no errors: the module's background work
  must not starve request handling.
* **bulk background purge** -- 8 whole tenants; the disk must empty.
* **purge_all images zone** -- the zone empties, the videos zone is untouched.
* **error log audit** -- no `[emerg]`, `[alert]` or `[crit]` beyond what a
  phase causes on purpose, no crash signals, no ASan report.

`chaos` adds: kill -9 of workers and a reload during the index build; the
mixed load under repeated kill -9 and reloads, a large purge followed at once
by a reload, cache files and a whole level-2 directory deleted behind nginx's
back (it must refill), and gdb stopping a worker inside a change of the key
index (at the call that lowers the busy flag) and killing it there -- the
reset alert and a rebuild must follow, and the verifier and consistency
checks must still pass. Then an index far too small (build fails, purges walk,
still correct) and `max_size` below the data (cache manager evictions,
reconcile passes, still correct). With the failed index it also sends a
wildcard (202, carried out by a walk) and kill -9s every worker a second
later: the purge must still take effect.

The cache lock is off in the test config: a request waiting for it polls
every 500 ms, which the latency probes would report as a stalled worker.

## Knobs

`--files --gb --real --workers --duration --wrk-threads --wrk-conns
--miss-permille --purge-rate --index-size --max-size --reconcile
--http-extra --main-extra --p99-bound-ms --p99-factor --build-timeout
--max-runtime --gdb-symbol --reseed`; each also as `E2E_<NAME>` (see
`e2e.py --help`). `E2E_SPARSE=0` writes real bytes: 200 GB then really is
200 GB on disk.

Notes: nginx starts its cache loader 60 s after the master, so zone
accounting (and with it evictions and reconcile) only starts then; builds of
the key index do not wait for it. Sparse files have their full size as far as
nginx is concerned (`max_size`, `Content-Length`), only the disk is spared.

## Torture scenarios

`t/e2e/torture.sh` runs targeted fault injection against a small nginx in the
same image, each scenario with the thread pool and inline, and checks the
disk after every purge:

| Scenario | What it does |
|---|---|
| `s_tiny_index` | an index zone far too small: the build fails, wildcards walk, still correct |
| `s_gdb_crash` | gdb stops a worker inside an index change and kills it: reset, rebuild |
| `s_external_rm` | cache files and level directories deleted behind nginx's back (stale directory fds) |
| `s_levels` | `levels=1:2:2`, `levels=2` and no levels, across a restart |
| `s_emfile` | a descriptor limit of 60: the build fails and retries with fewer files open, walks retry their directories |
| `s_purge_all_vary` | Vary variants (two md5s per key) and purge_all |
| `s_keys` | keys around the 512-byte index truncation, case folding, over-long wildcards (400) |
| `s_reload_storm` | 25 reloads during a build under load |
| `s_many_zones` | 12 cache zones, one purged, the others untouched |
| `s_kill_after_202` | kill -9 of every worker right after a queued purge: it is carried out anyway |
| `s_reset_during_build` | purges during a build, then an index reset: they are carried out after the rebuild |
| `s_reload_while_locked` | a worker frozen holding the index / queue lock, a reload, then kill -9: the master must not hang |

`t/e2e/torture.sh --seq` hammers the lock-free reads: 12 workers, new keys
inserted all the time, and GET -> wildcard purge -> GET cycles that must never
see a purge answered "not found" for a key just stored.

```bash
t/e2e/torture.sh                         # all of them (~20 min)
t/e2e/torture.sh s_gdb_crash s_emfile    # some
MODE=threads t/e2e/torture.sh            # one mode
E2E_SANITIZE=asan t/e2e/torture.sh       # AddressSanitizer build
DUR=120 t/e2e/torture.sh --seq
```
