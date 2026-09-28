# ngx_cache_purge

An nginx module that adds cache purge support for `FastCGI`, `proxy`, `SCGI`,
and `uWSGI` caches. A purge operation removes the cached entry whose key
matches the purge request. This repository maintains a fork of
[FRiCKLE/ngx_cache_purge](https://github.com/FRiCKLE/ngx_cache_purge), with
compatibility updates, bug fixes, and additional purge features.

See the [release notes](https://github.com/nginx-modules/ngx_cache_purge/releases)
for changes and [distribution packages](#distribution-packages) for packaged builds.

---

## Contents

- [Features](#features)
- [Compatibility](#compatibility)
- [Distribution packages](#distribution-packages)
- [Build from source](#build-from-source)
- [Directives](#directives)
- [Partial key purge](#partial-key-purge)
- [How a wildcard purge runs](#how-a-wildcard-purge-runs)
- [Sample configurations](#sample-configurations)
- [Performance tuning](#performance-tuning)
- [Monitoring and debugging](#monitoring-and-debugging)
- [Troubleshooting](#troubleshooting)
- [Testing](#testing)
- [Migration](#migration)
- [Security](#security)
- [License](#license)
- [Technical and historical notes](#technical-and-historical-notes)

---

## Features

- **Inline purge** — dedicated HTTP method (`PURGE`) with IP access control
- **Separate purge location** — 3-arg `proxy_cache_purge zone key` syntax for
  regex-captured key purging without a `proxy_pass`
- **Wildcard / partial purge** — a trailing `*` purges every key with that
  prefix
- **`purge_all`** — removes every entry in the cache zone in one request
- **Background queue** — wildcard and `purge_all` purges are answered `202`
  and carried out by coalesced, time-sliced walks; queued purges survive
  worker crashes and reloads
- **Key index** — optional per-zone shared-memory index of every cached key,
  so a wildcard purge costs what it matches instead of a directory walk
- **Thread pool offload** — optional: reading keys and unlinking files
  happens in an nginx thread pool, keeping the event loop free
- **Vary-aware purge** — after an exact-key purge, removes all filesystem
  variants (gzip, Vary header) sharing the same cache key
- **Response types** — `html` (default), `json`, `xml`, `text`
- **No dependencies** — nginx and libc only

---

## Compatibility

| nginx  | status     |
|--------|------------|
| 1.20.x | ✓ tested   |
| 1.26.x | ✓ tested   |
| 1.28.x | ✓ tested   |
| 1.29.x | ✓ tested   |

CI runs the Test::Nginx suites against the versions above. The key index,
thread pool and end-to-end suites have also been run by hand against 1.28.3,
1.30.4, 1.30.5 and 1.31.6, including under AddressSanitizer. Older releases
back to 1.7.9 compile but are not covered by CI.

---

### Distribution packages

The package and ports definitions below reference this fork. They are maintained
by their respective downstream projects; versions and available features vary
by release and repository branch.

| System | Package or port | Packaging source / details |
| --- | --- | --- |
| Debian | [`libnginx-mod-http-cache-purge`](https://packages.debian.org/trixie/libnginx-mod-http-cache-purge) | [Package tracker](https://tracker.debian.org/pkg/libnginx-mod-http-cache-purge); [unstable package](https://packages.debian.org/sid/libnginx-mod-http-cache-purge) |
| Ubuntu 26.04 LTS | [`libnginx-mod-http-cache-purge`](https://packages.ubuntu.com/resolute/libnginx-mod-http-cache-purge) | Universe; check the package for your Ubuntu release |
| Arch Linux | [`nginx-mod-cache_purge`](https://archlinux.org/packages/extra/x86_64/nginx-mod-cache_purge/) | Official Extra repository; [PKGBUILD](https://gitlab.archlinux.org/archlinux/packaging/packages/nginx-mod-cache_purge/-/blob/main/PKGBUILD) |
| Arch Linux AUR | [`nginx-mainline-mod-cache_purge`](https://aur.archlinux.org/packages/nginx-mainline-mod-cache_purge) | User-maintained build recipe for `nginx-mainline`; [PKGBUILD](https://aur.archlinux.org/cgit/aur.git/tree/PKGBUILD?h=nginx-mainline-mod-cache_purge) |
| Alpine Linux | [`nginx-mod-http-cache-purge`](https://pkgs.alpinelinux.org/package/edge/main/x86/nginx-mod-http-cache-purge) | [APKBUILD](https://gitlab.alpinelinux.org/alpine/aports/-/blob/master/main/nginx/APKBUILD); select your branch and architecture on the package site |
| FreeBSD / NGINX | [`www/nginx-module-cache-purge`](https://cgit.freebsd.org/ports/tree/www/nginx-module-cache-purge) | Module sources for the NGINX ports; [Makefile](https://cgit.freebsd.org/ports/tree/www/nginx-module-cache-purge/Makefile) |
| FreeBSD / Angie | [`www/angie-module-cache-purge`](https://cgit.freebsd.org/ports/tree/www/angie-module-cache-purge) | Dynamic module for Angie; [Makefile](https://cgit.freebsd.org/ports/tree/www/angie-module-cache-purge/Makefile) |

Older packages with the same name may use a different upstream. Debian switched
to this fork in version `1:2.5.3-1`; Ubuntu 24.04's published package is still
based on 2.3. Check your package's source and changelog when upgrading.

Use a module build compatible with your installed NGINX package. Distribution
modules may depend on a specific NGINX version or ABI, and 2.x packages do not
provide the background queue directives introduced in 3.x.

---

### Build from source

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Dynamic module:

```bash
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

`cache_purge_thread_pool` needs nginx built with `--with-threads`. Everything
else works on a default build.

---

## Directives

### `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off | <method> [purge_all] from all | <cidr> ...
         proxy_cache_purge <zone> "<key_expression>"
Default: —
Context: http, server, location
```

**Inline form** (`from …`) — intercepts the named HTTP method on a proxy
location and purges the matching cache entry. `on` is a shorthand for method
`PURGE`; `off` disables purging. Optionally restrict to a list of CIDR ranges
or use `from all` to allow from any address. Adding `purge_all` before `from`
empties the entire cache zone regardless of the request URI.

**Separate-location form** (two arguments) — use inside a dedicated purge
location (typically a regex location capturing the cache key). Looks up the
cache zone by name and purges the compiled key expression. This form is
incompatible with `proxy_cache` and `proxy_pass` in the same location.


### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off | <method> [purge_all] from all | <cidr> ...
         fastcgi_cache_purge <zone> "<key_expression>"
Default: —
Context: http, server, location
```

Equivalent to `proxy_cache_purge` but for FastCGI cache zones configured with
`fastcgi_cache` / `fastcgi_cache_path`.


### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off | <method> [purge_all] from all | <cidr> ...
         scgi_cache_purge <zone> "<key_expression>"
Default: —
Context: http, server, location
```

Equivalent to `proxy_cache_purge` but for SCGI cache zones configured with
`scgi_cache` / `scgi_cache_path`.


### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off | <method> [purge_all] from all | <cidr> ...
         uwsgi_cache_purge <zone> "<key_expression>"
Default: —
Context: http, server, location
```

Equivalent to `proxy_cache_purge` but for uWSGI cache zones configured with
`uwsgi_cache` / `uwsgi_cache_path`.


### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type html | json | xml | text
Default: html
Context: http, server, location
```

Sets the `Content-Type` and body format of purge responses. Has no effect on
cache-miss responses (412 / 404), which are generated by nginx's built-in
error-page renderer.


### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off
Default: off
Context: http
```

When enabled, wildcard and `purge_all` purge requests are enqueued and return
`202 Accepted` immediately; a per-worker background timer drains the queue in
batches. Has no effect on exact-key purges, which are always synchronous. When
disabled, all purges are processed synchronously in the request handler.

A queued purge stays in shared memory until it has been carried out: if the
worker running it dies (crash, `kill -9`), it is queued again after a backoff
and from then on run on its own, so a purge that kills its worker cannot take
the rest of its batch down with it; after three such deaths it is dropped
(`crit`). A reload or graceful stop hands it back too, and a reload that
changes `cache_purge_queue_size` moves the queued purges into the resized
queue (`notice`; `alert` with the number lost if the new queue is too small). A walk that cannot read
some cache directories (out of file descriptors, I/O errors) walks them again,
in rounds further apart, and queues its purges again if they still fail.

Purges are carried out at least once: after a crash, a reload or an index
reset one may run again or late, and then also remove what was cached after
it was acknowledged. That costs a refill from the upstream; it never leaves
stale content behind. The exceptions are logged: a process dying in the
middle of a change to the queue itself resets the queue (`alert`, with the
number of purges lost); a purge that cannot be queued again for lack of
shared memory is dropped (`crit`); and so is one older than
`cache_purge_queue_timeout`, if set (`error`).


### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>
Default: 1024
Context: http
```

Maximum number of entries the background queue can hold. Each slot occupies
roughly 1–2 KB of shared memory (2048 slots ≈ 3 MB). When the queue is full,
new wildcard / `purge_all` purge requests are answered `429 Too Many
Requests` -- never walked synchronously in the request worker. Duplicates of a
queued purge are recognised by hash and not queued twice. Only meaningful when
`cache_purge_background_queue on`.


### `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>
Default: 10
Context: http
```

How many queued purges of one cache are carried out together, by a single
walk of its directory (or, with `cache_purge_index`, a single pass over the
index). All patterns are matched at once -- one binary search per file over
the sorted, prefix-free set -- so a pass costs about the same for one purge as
for thousands; set it as high as `cache_purge_queue_size`. Only meaningful
when `cache_purge_background_queue on`.


### `cache_purge_walk_budget`

```
Syntax:  cache_purge_walk_budget <time>
Default: 20ms
Context: http
```

How long a background walk (and a key index build) runs before it yields to
the event loop. A walk resumes on the next tick, `cache_purge_throttle_ms`
later; a build too, or with `cache_purge_thread_pool` as soon as the pending
events have been handled (its disk work happens in the pool). `0` walks a
whole directory in one tick.


### `cache_purge_queue_timeout`

```
Syntax:  cache_purge_queue_timeout <time>
Default: 0
Context: http
```

Queued purges older than this are dropped (and logged). `0`: never -- a
dropped purge leaves stale content.


### `cache_purge_index`

```
Syntax:  cache_purge_index <size> [<zone>=<size> ...]
Default: 0 (off)
Context: http
Requires: cache_purge_background_queue on
```

Keeps every key stored in any cache in shared-memory indexes, so that a
wildcard or `purge_all` purge is a range lookup plus exact deletes instead of
a directory walk: its cost follows the files it matches, not the size of the
cache.

Every cache zone gets an index of its own, in a shared memory zone of its
own with its own lock: fills and purges of one cache never wait for
another's. `<size>` is the total; `<zone>=<size>` sets the share of a
`keys_zone`, and the rest is divided among the other zones in proportion to
their `keys_zone` sizes (which bound how many files each can hold). For
example `cache_purge_index 1g videos=64m;` gives `videos` 64 MB and the other
zones the remaining 960 MB between them.

* Entries are recorded when an upstream response is about to be stored,
  subrequests (slices, background updates) and both Vary file names
  included, and checked again when the request ends: an entry without a file
  is dropped, a stored file without an entry gets one.
* After a (re)start the index is built from the files on disk, in parallel
  by all workers (a reload keeps it). Wildcards during the build are applied
  to what is indexed and to the rest as the walk reaches it, and answered
  `202`; each is also queued to run again once the build is over, so that
  it is not lost if the index is reset during the build.
* A build that cannot read a cache directory (out of file descriptors, I/O
  errors) fails rather than leave files out; it is retried after 10 s, with
  fewer files open at a time.
* Size it at ~170-260 bytes per cached file (the key length decides). If it
  runs full, a `crit` line is logged and wildcards fall back to the queued
  walks until the index has been reconciled and rebuilt.
* Lookups that find what they look for -- a wildcard that matches nothing, a
  fill or revalidation of a key already indexed -- do not take the index
  lock: they read optimistically and check a sequence counter afterwards.
* The queue and the index are locked with their slab pool's mutex, which the
  master releases when a worker dies; a worker that dies in the middle of a
  change resets the zone (the index is rebuilt from disk) rather than leave
  it inconsistent.

Answers: `200` purged (up to `cache_purge_index_sync_limit` files, deleted
before the answer), `202` more than that (the rest in the background) or
during a build, `404`/`412` nothing matched, `429` could not be queued, `400`
a wildcard key too long (512 bytes) to match.


### `cache_purge_index_sync_limit`

```
Syntax:  cache_purge_index_sync_limit <number>
Default: 1024
Context: http
```

Matching files an indexed wildcard deletes before answering; more are deleted
in the background and the answer is `202`. 1 to 1048576.


### `cache_purge_index_reconcile`

```
Syntax:  cache_purge_index_reconcile <time>
Default: 10m
Context: http
```

How often entries of files the cache manager expired or evicted are dropped
(a walk of the index in memory, not of the disk; it waits for the cache
loader after a start). A pass is spread over half of this interval, a few
entries at a time, rather than run in one burst. Also how often a failed
index is retried. Must be greater than 0.


### `cache_purge_index_refresh`

```
Syntax:  cache_purge_index_refresh <time>
Default: 0 (off)
Context: http
```

How often the cache directories are walked, gently, for files the index does
not know -- restored or copied in from outside, or lost by a process killed
at the wrong moment. The walk only adds entries, so it is always safe; it
logs a `warn` with the count when it had to add any.

A refresh only reads the directories that changed since the previous walk
began: their ctime moves with every file created, renamed or deleted in
them (and with `utimes()`, so restoring a directory with old mtimes does not
hide it). Every sixth refresh reads all of them regardless.


### `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool off | <name> [tasks=<n>]
Default: off
Context: http
Requires: nginx built with --with-threads
```

Carries out the disk work of purges and index walks -- reading the keys of
cache files, unlinking them -- in the nginx thread pool `<name>` (see
[`thread_pool`](https://nginx.org/en/docs/ngx_core_module.html#thread_pool);
`default` exists without being declared), so that the event loop keeps
serving requests while it happens. Shared memory is never touched from a
thread: the bookkeeping stays on the event loop, in short slices.

* A wildcard answered from the index waits for its unlinks in the pool; the
  worker serves other requests meanwhile.
* An index build keeps `<n>` directory reads in flight per worker (default
  4), a queued walk `<n>` per drainer.
* At most `2 × <n>` operations per worker are in the pool at once; beyond
  that, and when the pool's queue is full, the work is done inline.

Without it the same work runs inline in slices of `cache_purge_walk_budget`,
with the event loop getting its turn between slices.

### `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>
Default: 10ms
Context: http
```

Interval between background processing ticks. Accepts any nginx time value:
`10ms`, `500ms`, `1s`, `2s 500ms`; must be greater than 0. Only meaningful
when `cache_purge_background_queue on`.

Increase on constrained or spinning-disk storage; decrease on NVMe.

**Time-unit note:** a bare integer with no suffix (e.g. `10`) is interpreted
as milliseconds — matching the `_ms` directive name — and a startup warning is
logged. Use an explicit `ms` suffix (`10ms`) to silence the warning. This
differs from most nginx time directives, where a bare integer means seconds.


### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off
Default: on
Context: http
```

Controls the HTTP status code returned when a purge request targets an entry
that is not in the cache:

| value | status on miss            |
|-------|---------------------------|
| `on`  | `412 Precondition Failed` |
| `off` | `404 Not Found`           |

Default is `on` (412) for backwards compatibility with earlier releases.
Set to `off` to return `404 Not Found`, the correct HTTP status for a
resource that does not exist (RFC 9110 §15.5.5).


### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off
Default: off
Context: http
```

When `on`, an exact-key purge walks the cache directory after deleting the
primary file and removes any remaining files that carry the same `KEY:` string.
This covers all `Vary` and `gzip_vary` variants of a cached response, which
are stored at different filesystem paths but share one logical key. The byte
immediately after the key string in each file is verified to be `\n`,
preventing false matches against keys that share only a common prefix.

Disabled by default because it adds a full, synchronous cache directory walk
per exact-key purge (with `cache_purge_index` too). Enable it when `gzip_vary on` or `Vary:` headers cause multiple cache
files to be created for a single logical entry. Wildcard and `purge_all`
purges do not need this option — the walk they already perform catches every
variant regardless.

---

## Partial key purge

When the exact cache key is not known — for example because it includes cookie
values or query parameters — append `*` to the key to request a prefix match:

```
PURGE /images/header*
```

Rules:

- `*` is only special as the last character of the key. Anywhere else it is a
  literal `*`. There are no other glob characters.
- Matching is a case-insensitive prefix match on the full cache key.
- Put the variable part (usually `$uri`) at the **end** of `proxy_cache_key`,
  or the prefix will not line up with the stored keys.
- A key of just `*` matches everything in the zone, like `purge_all`.
- A wildcard key longer than 512 bytes is answered `400`.

---

## How a wildcard purge runs

What happens to a wildcard or `purge_all` purge depends on the directives in
effect:

| Configuration | What happens | Answer |
|---|---|---|
| no background queue | the cache directory is walked in the request handler | `200` when done |
| `cache_purge_background_queue on` | queued; worker 0 walks the cache once for up to `cache_purge_batch_size` queued purges, in slices of `cache_purge_walk_budget` | `202` |
| `+ cache_purge_index` | range lookup in the zone's index; up to `cache_purge_index_sync_limit` files are deleted before answering, the rest in the background | `200`, or `202` for more |
| `+ cache_purge_thread_pool` | as above, with key reads and unlinks in the thread pool | as above |

While the index is being built (after a start, or after it was reset), a
wildcard is applied to what is indexed so far and to the rest as the build
reaches it, and is answered `202`. If the index fills up, a `crit` line is
logged and wildcards fall back to queued walks until it has been rebuilt.

Exact-key purges always run synchronously and never walk the cache, unless
`cache_purge_vary_aware on`.

---

## Sample configurations

### Basic — inline purge method

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

```bash
curl -X PURGE http://localhost/images/logo.png
# 200 — entry purged
# 412 — entry not in cache  (or 404 with cache_purge_legacy_status off)
# 403 — client IP not in the allowed list
```

### Basic — separate purge location

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location / {
            proxy_pass      http://backend;
            proxy_cache     main;
            proxy_cache_key "$host$uri$is_args$args";
        }

        # Capture the cache key from the URI and pass it to the 3-arg form.
        # No proxy_pass or proxy_cache required in this location.
        location ~ ^/purge(/.*) {
            allow             127.0.0.1;
            deny              all;
            proxy_cache_purge main "$host$1$is_args$args";
        }
    }
}
```

### Background queue enabled

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;
    cache_purge_queue_size        2048;
    cache_purge_batch_size        20;
    cache_purge_throttle_ms       10ms;

    server {
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

### Purge all entries

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location /content {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
    }
}
```

```bash
curl -X PURGE http://localhost/anything
# 200 — entire cache zone cleared
```

### JSON responses with per-location override

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        cache_purge_response_type json;   # default for all locations in this server
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from all;
        }

        location /admin/purge {
            # Override to plain text for scripting
            cache_purge_response_type text;
            proxy_cache_purge         main "$arg_key";
        }
    }
}
```

### Large cache — key index and thread pool

```nginx
thread_pool default threads=16;

http {
    proxy_cache_path /var/cache/images levels=1:2 keys_zone=images:512m
                     max_size=500g inactive=30d;
    proxy_cache_path /var/cache/videos levels=1:2 keys_zone=videos:64m
                     max_size=1t inactive=30d;

    cache_purge_background_queue  on;
    cache_purge_queue_size        8192;
    cache_purge_batch_size        8192;
    cache_purge_index             1g videos=64m;
    cache_purge_thread_pool       default;
    cache_purge_index_refresh     1h;
    cache_purge_legacy_status     off;
    cache_purge_response_type     json;

    server {
        location /cdn/ {
            proxy_pass        http://backend;
            proxy_cache       images;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 10.0.0.0/8;
        }
    }
}
```

Size the index at ~170–260 bytes per cached file: 1 GB holds roughly 4–6
million files.

### Vary-aware purge

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

After purging a key, all gzip and Vary variants stored at different filesystem
paths are also removed automatically.

---

## Performance tuning

Good values depend on storage, how many files are cached, and how many
wildcard purges arrive.

- **Few wildcard purges, small cache:** the background queue on its own is
  enough.
- **Many wildcard purges, or more than ~100k cached files:** add
  `cache_purge_index`. A wildcard then costs what it matches instead of a
  full walk, and one that matches nothing is a lock-free lookup.
- **Large caches, or builds and bulk purges that must not slow requests:** add
  `cache_purge_thread_pool default;` (needs `--with-threads`).
- **`cache_purge_batch_size`:** one walk carries out the whole batch for about
  the cost of one purge, so set it as high as `cache_purge_queue_size`.
- **`cache_purge_walk_budget` / `cache_purge_throttle_ms`:** how long a slice
  of inline disk work runs, and how long the event loop gets between slices.
  On spinning disks or network storage, lower the budget and raise the
  throttle. With a thread pool they matter much less.

**Queue memory:** `queue_size × ~1.5 KB`. 2048 slots ≈ 3 MB of shared memory.

**Index memory:** ~170–260 bytes per cached file, depending on key length.

**Measured** on 200 GB / 2.5M real cache files with the thread pool: request
p99 stayed at 7 ms during an index rebuild, 3.6 ms during a 307k-file bulk
purge and 7.6 ms during a 2.1M-file `purge_all`. A rebuild after a crash,
under load, takes ~10 s. Background bulk deletion runs at ~28–30k files/s.
Wildcard purges that match nothing reach ~540k requests/s on 8 cores, about
nginx's own ceiling for the same request.

---

## Monitoring and debugging

Purge answers:

| Status | Meaning |
|---|---|
| `200` | purged |
| `202` | accepted; the rest is carried out in the background |
| `400` | wildcard key too long (512 bytes) |
| `403` | client not in the allowed list |
| `404` / `412` | nothing matched (see `cache_purge_legacy_status`) |
| `429` | background queue full |

The response body uses the format set by `cache_purge_response_type`.

Relevant log messages (all start with `ngx_cache_purge:`):

| Level | Condition |
|---|---|
| `info` | a background walk finished: files checked, deleted, time taken |
| `notice` | index build or refresh started |
| `warn` | queue full; refresh found files the index did not know |
| `error` | cache zone not found; queued purge timed out |
| `crit` | file deletion failed; `opendir()` failed; index full (wildcards fall back to walks); index build failed; out of shared memory |
| `alert` | a process died while updating the queue or an index, which is reset |

```bash
tail -f /var/log/nginx/error.log | grep "cache purge"
```

---

## Troubleshooting

**Purge requests block worker processes**
Enable `cache_purge_background_queue on`. For large caches add
`cache_purge_index` and `cache_purge_thread_pool`.

**Queue full warnings, or `429` on wildcard purges**
Increase `cache_purge_queue_size` and `cache_purge_batch_size`, or add
`cache_purge_index`.

**`crit` "cache_purge_index zone is full"**
Increase `cache_purge_index` (~170–260 bytes per cached file). Until the
index has been rebuilt, wildcards are carried out by walks.

**`warn` "refresh found a cache file the key index ... did not know"**
Files were copied into the cache from outside, or a process was killed at
the wrong moment; the refresh has already added them. If it keeps happening
without either, please open an issue.

**High iowait during purges**
Lower `cache_purge_walk_budget` and raise `cache_purge_throttle_ms`, or use
`cache_purge_thread_pool`.

**Queue drains too slowly**
Raise `cache_purge_batch_size` (up to `cache_purge_queue_size`) and
`cache_purge_walk_budget`, or add `cache_purge_index`.

**`412` responses where `404` is expected**
Set `cache_purge_legacy_status off`.

**Vary / gzip variants remain after purge**
Enable `cache_purge_vary_aware on`.

---

## Testing

The test suite uses [Test::Nginx](https://github.com/openresty/test-nginx).

```bash
prove -r t/
```

Individual suites:

```bash
prove t/basic.t
prove t/background_queue.t
prove t/config.t
prove t/memory.t
prove t/performance.t
prove t/index.t
prove t/index_threads.t   # needs nginx built with --with-threads
```

See [`t/TESTING.md`](t/TESTING.md) for Docker-based testing and the full
testing guide.

### End-to-end and torture suites

[`t/e2e`](t/e2e/README.md) runs a real nginx with the module in front of a
second nginx as origin, over a cache seeded directly onto disk in nginx's
own file format. A verifier checks that no purge leaves stale content and
that a wildcard removes every matching file on disk.

```bash
t/e2e/run.sh quick          # ~80k files, a few minutes
t/e2e/run.sh chaos          # kill -9, reloads, gdb inside an index change, disk tampering
t/e2e/run.sh scale          # 2.5M files, 200 GB (sparse by default)
t/e2e/torture.sh            # targeted fault injection, with and without a thread pool
```

---

## Migration

### From the original FRiCKLE module

The module is backwards compatible. No configuration changes are required.
To opt in to background processing, add:

```nginx
http {
    cache_purge_background_queue on;
}
```

### From nginx-selective-cache-purge-module

Replace the module. Background queue functionality is now built in. Map your
previous queue parameters to `cache_purge_queue_size`, `cache_purge_batch_size`,
and `cache_purge_throttle_ms`.

---

## Security

- Restrict purge endpoints with `allow` / `deny` directives or an upstream
  authentication layer. Never expose them publicly.
- Set `cache_purge_queue_size` to a value appropriate for your traffic to
  limit shared memory consumption under request floods.
- Consider pairing purge locations with `limit_req` to rate-limit purge
  requests independently of normal traffic.

---

## License

```
Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
Copyright (C) 2016-2026, Denis Denisov

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## Technical and historical notes

- The original default branch ends on [23 December 2014](https://github.com/FRiCKLE/ngx_cache_purge/commit/331fe43e8d9a3d1fa5e0c9fec7d3201d431a9177). As of this check, twelve full years have not elapsed.
- **Avoid “fully non-blocking.”** Without `cache_purge_thread_pool`, background walks and index builds do their disk I/O on the event loop, in slices of `cache_purge_walk_budget`. Exact-key purges are always synchronous, and `cache_purge_vary_aware` adds a synchronous directory walk to each one.
- **Do not promise unchanged behavior for every upgrade.** The 2.5 status-code change alone makes that inaccurate. Release 3.0.0 also flags internal breaking changes.
- **Describe CI configuration accurately.** The workflow targets NGINX 1.20.2, 1.26.3, 1.28.2 and 1.29.6. Runs on 1.28.3, 1.30.x and 1.31.6 were done by hand, not in CI.
- **Keep package links factual.** Downstream packaging demonstrates use of the source, not endorsement, an audit, or a support commitment from that distribution to this repository.

---

## See also

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx documentation](https://nginx.org/en/docs/)
