/*
 * Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
 * Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
 * Copyright (C) 2016-2026 Denis Denisov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ngx_config.h>
#include <nginx.h>
#include <ngx_core.h>
#include <ngx_http.h>


#ifndef nginx_version
# error This module cannot be built against an unknown nginx version.
#endif

#define NGX_CACHE_PURGE_RESPONSE_TYPE_HTML  1
#define NGX_CACHE_PURGE_RESPONSE_TYPE_XML   2
#define NGX_CACHE_PURGE_RESPONSE_TYPE_JSON  3
#define NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT  4

#define NGX_CACHE_PURGE_QUEUE_SIZE_DEFAULT   1024
#define NGX_CACHE_PURGE_BATCH_SIZE_DEFAULT   10
/*
 * This constant is assigned directly to an ngx_msec_t field via
 * ngx_conf_init_msec_value() -- it bypasses ngx_parse_time() and is
 * therefore in raw milliseconds.  The corresponding directive,
 * cache_purge_throttle_ms, is parsed by ngx_conf_set_msec_slot which
 * calls ngx_parse_time(value, 0): bare integers are treated as seconds
 * per the nginx time-value contract, so operators must write an explicit
 * suffix ("10ms", "1s", ...) to get the intended unit.
 */
#define NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT  10  /* milliseconds */
#define NGX_CACHE_PURGE_KEY_MAX_LEN          512
/*
 * Default age after which a queued purge is dropped.  0 = never: a purge that
 * is silently discarded leaves stale content in the cache, and with passes
 * that coalesce many purges into one walk an item routinely waits for the
 * pass in front of it to finish.  cache_purge_queue_timeout overrides it.
 */
#define NGX_CACHE_PURGE_QUEUE_TIMEOUT        0       /* ms, 0 = never */
/*
 * Time a background walk may run before yielding to the event loop; the walk
 * resumes from where it stopped on the next tick.  0 = run each walk to
 * completion in a single tick (the pre-slicing behaviour).
 */
#define NGX_CACHE_PURGE_WALK_BUDGET_DEFAULT  20      /* ms */
/* Cache root plus the at most three levels= subdirectory levels */
#define NGX_CACHE_PURGE_WALK_DEPTH           4
/* Files visited between clock reads while a walk has a time budget */
#define NGX_CACHE_PURGE_WALK_CLOCK_EVERY     256

/* How a queued purge is carried out */
#define NGX_CACHE_PURGE_MODE_WALK            0
#define NGX_CACHE_PURGE_MODE_INDEX           1

/* cache_purge_index */
#define NGX_CACHE_PURGE_INDEX_PENDING        0   /* not built yet: walk */
#define NGX_CACHE_PURGE_INDEX_BUILDING       1   /* usable; build walk runs */
#define NGX_CACHE_PURGE_INDEX_READY          2
#define NGX_CACHE_PURGE_INDEX_REBUILD        3   /* lost an entry: walk */
#define NGX_CACHE_PURGE_INDEX_FAILED         4   /* zone too small: walk */

#define NGX_CACHE_PURGE_INDEX_PATHS          32     /* cache zones */
#define NGX_CACHE_PURGE_INDEX_BATCH          1024   /* build: reads at once */
#define NGX_CACHE_PURGE_INDEX_CHUNK          256    /* build: files per lock */

/* build units (see build_init) */
#define NGX_CACHE_PURGE_UNIT_TODO            0
#define NGX_CACHE_PURGE_UNIT_DONE            (-1)
#define NGX_CACHE_PURGE_UNIT_NONE            ((ngx_uint_t) -1)
#define NGX_CACHE_PURGE_INDEX_SLICE          1024   /* reconcile: per lock */
#define NGX_CACHE_PURGE_INDEX_SYNC_LIMIT     1024
#define NGX_CACHE_PURGE_INDEX_TAKE           64     /* entries per lock */
#define NGX_CACHE_PURGE_INDEX_RECONCILE      600000 /* ms */
/*
 * Byte offset from the start of a cache file to the first character of the
 * cached key string.  The nginx cache file layout is:
 *
 *   [ ngx_http_file_cache_header_t ][ "\nKEY: " ][ <key> ][ "\n" ]...
 *
 * sizeof(ngx_http_file_cache_header_t) skips the binary header.
 * NGX_CACHE_PURGE_KEY_HDR_OFFSET (6) accounts for the literal prefix
 * "\nKEY: " (newline + 'K' + 'E' + 'Y' + ':' + ' ' = 6 bytes).
 *
 * This layout has been stable since nginx 0.7.x.  If it ever changes, only
 * this constant and its comment need updating.
 */
#define NGX_CACHE_PURGE_KEY_HDR_OFFSET       6

/*
 * Minimum shared-memory size for the background queue, expressed in pages.
 * The slab allocator consumes an amount of metadata (pool header, slot
 * descriptors, stat entries, page descriptors, and an alignment gap) that
 * varies with nginx version, build flags, and architecture.  Rather than
 * attempting to compute that overhead from internal slab structs -- which
 * differ between nginx 1.8/1.9+, are affected by NGX_HAVE_POSIX_SEM /
 * --with-threads, and scale with the OS page size (4 KB on x86 Linux,
 * 8-64 KB on some *BSD / ARM / POWER platforms) -- we simply enforce a
 * floor of 8 pages.  ngx_pagesize is the runtime value, so the minimum
 * scales automatically on big-page architectures.  8 pages is generous
 * enough to accommodate all slab metadata overhead while leaving several
 * full pages of usable heap even for queue_size=1.
 */
#define NGX_CACHE_PURGE_SHM_MIN_PAGES        8


static const char ngx_http_cache_purge_content_type_json[] = "application/json";
static const char ngx_http_cache_purge_content_type_html[] = "text/html";
static const char ngx_http_cache_purge_content_type_xml[]  = "text/xml";
static const char ngx_http_cache_purge_content_type_text[] = "text/plain";

static const size_t ngx_http_cache_purge_content_type_json_size =
    sizeof(ngx_http_cache_purge_content_type_json);
static const size_t ngx_http_cache_purge_content_type_html_size =
    sizeof(ngx_http_cache_purge_content_type_html);
static const size_t ngx_http_cache_purge_content_type_xml_size =
    sizeof(ngx_http_cache_purge_content_type_xml);
static const size_t ngx_http_cache_purge_content_type_text_size =
    sizeof(ngx_http_cache_purge_content_type_text);

static const char ngx_http_cache_purge_body_templ_json[] =
    "{\"Key\": \"%s\", \"Status\": \"%s\"}";
static const char ngx_http_cache_purge_body_templ_html[] =
    "<html><head><title>Cache Purge</title></head>"
    "<body bgcolor=\"white\"><center><h1>Cache Purge</h1>"
    "<p>Key: %s</p><p>Status: %s</p></center></body></html>";
static const char ngx_http_cache_purge_body_templ_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<status><Key><![CDATA[%s]]></Key><Status>%s</Status></status>";
static const char ngx_http_cache_purge_body_templ_text[] =
    "Key: %s\nStatus: %s\n";

static const size_t ngx_http_cache_purge_body_templ_json_size =
    sizeof(ngx_http_cache_purge_body_templ_json);
static const size_t ngx_http_cache_purge_body_templ_html_size =
    sizeof(ngx_http_cache_purge_body_templ_html);
static const size_t ngx_http_cache_purge_body_templ_xml_size =
    sizeof(ngx_http_cache_purge_body_templ_xml);
static const size_t ngx_http_cache_purge_body_templ_text_size =
    sizeof(ngx_http_cache_purge_body_templ_text);


#if (NGX_HTTP_CACHE)

/* -- forward declarations ----------------------------------------------- */

typedef struct ngx_http_cache_purge_queue_item_s ngx_http_cache_purge_queue_item_t;
typedef struct ngx_http_cache_purge_queue_s      ngx_http_cache_purge_queue_t;
typedef struct ngx_http_cache_purge_main_conf_s  ngx_http_cache_purge_main_conf_t;


/* -- data structures ---------------------------------------------------- */

struct ngx_http_cache_purge_queue_item_s {
    ngx_str_t                          cache_path;
    ngx_str_t                          key_partial;
    ngx_uint_t                         hash;
    ngx_flag_t                         purge_all;
    ngx_uint_t                         mode;        /* NGX_CACHE_PURGE_MODE_* */
    ngx_msec_t                         enqueued_at;
    ngx_http_cache_purge_queue_item_t *next;
    ngx_http_cache_purge_queue_item_t *hnext;       /* duplicate buckets */
};

struct ngx_http_cache_purge_queue_s {
    ngx_http_cache_purge_queue_item_t *head;
    ngx_http_cache_purge_queue_item_t *tail;
    /* size is always read and written while the queue is locked */
    ngx_uint_t                         size;
    ngx_slab_pool_t                   *shpool;
    /* items by hash, so a duplicate enqueue is found in O(1) */
    ngx_http_cache_purge_queue_item_t **buckets;
    ngx_uint_t                         nbuckets;    /* power of two */
    ngx_uint_t                         busy;        /* see queue_lock */
    ngx_uint_t                         generation;
    ngx_uint_t                         max_size;
    ngx_uint_t                         batch_size;
    ngx_msec_t                         throttle_ms;
};

struct ngx_http_cache_purge_main_conf_s {
    ngx_slab_pool_t                   *queue_shpool;   /* data: the queue */
    ngx_uint_t                         queue_buckets;
    ngx_shm_zone_t                    *shm_zone;
    ngx_uint_t                         queue_size;
    ngx_uint_t                         batch_size;
    ngx_msec_t                         throttle_ms;
    ngx_flag_t                         background_purge;
    ngx_flag_t                         legacy_status_codes;
    /*
     * vary_aware: when on, an exact-key purge walks the cache directory after
     * deleting the primary file and removes any remaining files that carry the
     * same KEY: string (i.e. Vary / gzip_vary variants at different paths).
     * Disabled by default because it adds a full cache walk per purge request.
     */
    ngx_flag_t                         vary_aware;
    ngx_msec_t                         walk_budget;
    ngx_msec_t                         queue_timeout;
    size_t                             index_size;
    ngx_uint_t                         index_sync_limit;
    ngx_msec_t                         index_reconcile;
    ngx_shm_zone_t                    *index_zone;
    ngx_slab_pool_t                   *index_shpool;   /* data: index */
};

typedef struct {
    ngx_flag_t    enable;
    ngx_str_t     method;
    ngx_flag_t    purge_all;
    ngx_array_t  *access;    /* ngx_in_cidr_t  */
    ngx_array_t  *access6;   /* ngx_in6_cidr_t */
} ngx_http_cache_purge_conf_t;

typedef struct {
# if (NGX_HTTP_FASTCGI)
    ngx_http_cache_purge_conf_t  fastcgi;
# endif
# if (NGX_HTTP_PROXY)
    ngx_http_cache_purge_conf_t  proxy;
# endif
# if (NGX_HTTP_SCGI)
    ngx_http_cache_purge_conf_t  scgi;
# endif
# if (NGX_HTTP_UWSGI)
    ngx_http_cache_purge_conf_t  uwsgi;
# endif

    ngx_http_cache_purge_conf_t *conf;
    ngx_http_handler_pt          handler;
    ngx_http_handler_pt          original_handler;
    ngx_uint_t                   response_type;

# if (NGX_HTTP_PROXY)
    /*
     * Separate-location syntax stores the cache zone and purge key here
     * instead of in plcf->upstream, which avoids triggering nginx's internal
     * proxy_cache merge path and the resulting duplicate location "/" error
     * introduced in nginx >= 1.27.x.
     */
    ngx_shm_zone_t              *proxy_separate_zone;   /* static zone name  */
    ngx_http_complex_value_t    *proxy_separate_value;  /* dynamic zone expr */
    ngx_http_complex_value_t     proxy_separate_key;    /* purge key template*/
# endif
} ngx_http_cache_purge_loc_conf_t;

typedef struct {
    u_char                 *key_partial;
    ngx_uint_t              key_len;
    u_char                  key_buffer[NGX_CACHE_PURGE_KEY_MAX_LEN];
    ngx_uint_t              files_deleted;
    ngx_uint_t              files_checked;
    /*
     * cache is set by ngx_http_cache_purge_delete_variants() so that
     * delete_exact_file can update shm metadata (sh->size, node->exists,
     * node->fs_size) for each variant it deletes.  NULL in all other walk
     * contexts where metadata updates are not needed.
     */
    ngx_http_file_cache_t  *cache;
} ngx_http_cache_purge_walk_ctx_t;

/*
 * Cache directory scanner used by wildcard and purge_all purges.
 *
 * Compared with ngx_walk_tree() + a file handler it
 *   - trusts readdir()'s d_type instead of stat()ing every entry,
 *   - opens, reads and unlinks relative to the directory fd (openat /
 *     unlinkat), so the kernel resolves one path component per call
 *     instead of the full cache path,
 *   - only considers 32-character names, so the in-flight temp files that
 *     use_temp_path=off leaves next to the cache files ("<md5>.<n>") are
 *     never read or deleted mid-write,
 *   - matches any number of key prefixes at once: patterns are lowercased,
 *     sorted and made prefix-free, so the one candidate that can be a prefix
 *     of a key is the greatest pattern <= key (binary search),
 *   - keeps its position in the tree between calls, so a background walk can
 *     yield to the event loop and resume.
 */
typedef struct {
    ngx_str_t              *elts;
    ngx_uint_t              nelts;
    size_t                  read_len;       /* longest pattern */
    ngx_flag_t              match_all;      /* purge_all or a bare "*" */
} ngx_http_cache_purge_patterns_t;

typedef struct {
    ngx_http_cache_purge_patterns_t  pats;
    ngx_http_file_cache_t  *cache;          /* NULL: no shm accounting */
    ngx_log_t              *log;
    DIR                    *dirs[NGX_CACHE_PURGE_WALK_DEPTH];
    ngx_uint_t              depth;
    ngx_uint_t              files_checked;
    ngx_uint_t              files_deleted;
    ngx_uint_t              errors;         /* subdirectories not read */
    u_char                  buf[NGX_CACHE_PURGE_KEY_MAX_LEN];
} ngx_http_cache_purge_scan_t;

/* An index entry: one cache file.  Ordered by (cache id, key, md5). */
typedef struct {
    ngx_rbtree_node_t       node;           /* node.key: cache id */
    u_char                  md5[NGX_HTTP_CACHE_KEY_LEN];
    u_short                 len;
    u_char                  key[1];         /* lowercased, no NUL */
} ngx_http_cache_purge_index_node_t;

/* A cache zone's share of a build: units first .. first + count - 1 */
typedef struct {
    ngx_rbtree_key_t        id;
    ngx_uint_t              first;
    ngx_uint_t              count;
    ngx_uint_t              l0;             /* levels= lengths */
    ngx_uint_t              l1;
} ngx_http_cache_purge_build_cache_t;

typedef struct ngx_http_cache_purge_index_sh_s {
    ngx_rbtree_t            rbtree;
    ngx_rbtree_node_t       sentinel;
    ngx_uint_t              state;          /* NGX_CACHE_PURGE_INDEX_* */
    ngx_uint_t              entries;
    ngx_uint_t              npaths;
    ngx_str_t               paths[NGX_CACHE_PURGE_INDEX_PATHS];  /* ids */
    /*
     * Wildcards received while a build walk runs, for the files it has not
     * reached yet: (cache id, prefix) index nodes with a zero md5, kept
     * prefix-free, so "does one cover this key" is one tree lookup.
     */
    ngx_rbtree_t            build;
    ngx_rbtree_node_t       build_sentinel;
    ngx_uint_t              nbuild;

    /* the running build: its units and who walks them (see build_init) */
    ngx_http_cache_purge_build_cache_t  build_caches[NGX_CACHE_PURGE_INDEX_PATHS];
    ngx_uint_t              build_ncaches;
    ngx_pid_t              *build_claims;   /* per unit: TODO, DONE or pid */
    ngx_uint_t              build_units;
    ngx_uint_t              build_next;
    ngx_uint_t              build_done;
    ngx_uint_t              build_files;
    ngx_uint_t              build_deleted;
    ngx_msec_t              build_started;
    ngx_uint_t              build_seq;
    ngx_uint_t              busy;           /* see queue_lock */
    ngx_uint_t              generation;     /* bumped by every reset */
    ngx_msec_t              failed_at;
} ngx_http_cache_purge_index_sh_t;

/* An entry taken out of the index for deletion */
typedef struct {
    u_char                  md5[NGX_HTTP_CACHE_KEY_LEN];
    ngx_str_t               key;            /* to put it back if needed */
} ngx_http_cache_purge_index_hit_t;

/* A worker's part in a build: the unit it walks */
typedef struct {
    ngx_uint_t                        unit;       /* or UNIT_NONE */
    ngx_uint_t                        generation; /* of the index */
    ngx_uint_t                        seq;        /* of the build */
    ngx_http_file_cache_t            *cache;
    ngx_rbtree_key_t                  id;
    ngx_http_cache_purge_scan_t       scan;
} ngx_http_cache_purge_build_t;

typedef struct {
    ngx_flag_t              active;
    ngx_uint_t              generation;     /* of the index */
    ngx_flag_t              has_cursor;
    ngx_rbtree_key_t        id;
    size_t                  len;
    u_char                  key[NGX_CACHE_PURGE_KEY_MAX_LEN];
    u_char                  md5[NGX_HTTP_CACHE_KEY_LEN];
    ngx_http_file_cache_t  *caches[NGX_CACHE_PURGE_INDEX_PATHS];
    ngx_uint_t              npaths;
    ngx_uint_t              checked;
    ngx_uint_t              dropped;
    ngx_msec_t              started;
    ngx_msec_t              last;           /* end of the last pass */
} ngx_http_cache_purge_reconcile_t;

typedef struct {
    ngx_str_t     key;
    ngx_flag_t    purge_all;
} ngx_http_cache_purge_pass_item_t;

/*
 * One background pass: every queued purge for one cache path, merged into a
 * single walk.  Worker-local; pool is NULL when no pass is running.
 */
typedef struct {
    ngx_pool_t                   *pool;
    ngx_str_t                     cache_path;
    ngx_array_t                   items;    /* pass_item_t, for re-enqueue */
    ngx_uint_t                    mode;     /* NGX_CACHE_PURGE_MODE_* */
    ngx_uint_t                    next_pattern;          /* index mode */
    ngx_http_cache_purge_index_hit_t  *hits;             /* index mode */
    ngx_http_cache_purge_index_hit_t   cursor;           /* index mode */
    u_char                        cursor_key[NGX_CACHE_PURGE_KEY_MAX_LEN];
    ngx_flag_t                    has_cursor;
    ngx_msec_t                    started;
    ngx_http_cache_purge_scan_t   scan;
} ngx_http_cache_purge_pass_t;


/* -- function prototypes ------------------------------------------------ */

static void *ngx_http_cache_purge_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf);
static ngx_int_t ngx_http_cache_purge_init_shm_zone(ngx_shm_zone_t *shm_zone,
    void *data);
static ngx_int_t ngx_http_cache_purge_init_worker(ngx_cycle_t *cycle);
static void ngx_http_cache_purge_exit_worker(ngx_cycle_t *cycle);
static void ngx_http_cache_purge_background_handler(ngx_event_t *ev);
static ngx_int_t ngx_http_cache_purge_enqueue(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all);
static ngx_int_t ngx_http_cache_purge_process_queue(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cache_purge_enqueue_raw(
    ngx_http_cache_purge_main_conf_t *cmcf, ngx_log_t *log,
    ngx_str_t *cache_path, ngx_str_t *key, ngx_flag_t purge_all,
    ngx_uint_t mode);
static ngx_int_t ngx_http_cache_purge_try_enqueue(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all);
static void ngx_http_cache_purge_pass_end(ngx_http_cache_purge_pass_t *pass);
static ngx_http_file_cache_t *ngx_http_cache_purge_find_cache(
    ngx_cycle_t *cycle, ngx_str_t *cache_path);
static ngx_int_t ngx_http_cache_purge_patterns_init(
    ngx_http_cache_purge_patterns_t *p, ngx_str_t *patterns, ngx_uint_t n,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_purge_scan_open(
    ngx_http_cache_purge_scan_t *s, ngx_str_t *root);
static void ngx_http_cache_purge_scan_close(ngx_http_cache_purge_scan_t *s);
static ngx_int_t ngx_http_cache_purge_scan(ngx_http_cache_purge_scan_t *s,
    ngx_msec_t budget);
static ngx_uint_t ngx_http_cache_purge_scan_sync(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *pattern);
static ngx_int_t ngx_http_cache_purge_index_purge(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all);
static ngx_int_t ngx_http_cache_purge_index_pass(
    ngx_http_cache_purge_pass_t *pass,
    ngx_http_cache_purge_main_conf_t *cmcf, ngx_msec_t budget);
static ngx_int_t ngx_http_cache_purge_index_work(ngx_cycle_t *cycle,
    ngx_http_cache_purge_main_conf_t *cmcf);
static void ngx_http_cache_purge_build_end(ngx_http_cache_purge_build_t *b);
static void ngx_http_cache_purge_build_release(
    ngx_http_cache_purge_main_conf_t *cmcf, ngx_http_cache_purge_build_t *b);
static void ngx_http_cache_purge_build_tick(ngx_event_t *ev);
static ngx_http_file_cache_node_t *ngx_http_cache_purge_lookup_node(
    ngx_http_file_cache_t *cache, u_char *key16);
static ngx_int_t ngx_http_cache_purge_filter_init(ngx_conf_t *cf);
static void ngx_http_cache_purge_index_rbtree_insert(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static ngx_http_cache_purge_index_sh_t *ngx_http_cache_purge_index_peek(
    ngx_http_cache_purge_main_conf_t *cmcf);
static ngx_http_cache_purge_index_sh_t *ngx_http_cache_purge_index_lock(
    ngx_http_cache_purge_main_conf_t *cmcf);
static void ngx_http_cache_purge_index_unlock(
    ngx_http_cache_purge_main_conf_t *cmcf);
static ngx_int_t ngx_http_cache_purge_unhex(u_char *p, u_char *md5);
static ngx_int_t ngx_http_cache_purge_index_init_zone(ngx_shm_zone_t *shm_zone,
    void *data);
static ngx_uint_t ngx_http_cache_purge_hash_key(ngx_str_t *cache_path,
    ngx_str_t *key);
static ngx_http_cache_purge_queue_item_t *ngx_http_cache_purge_find_duplicate(
    ngx_http_cache_purge_queue_t *queue, ngx_uint_t mode, ngx_uint_t hash,
    ngx_str_t *cache_path, ngx_str_t *key);

# if (NGX_HTTP_FASTCGI)
char      *ngx_http_fastcgi_cache_purge_conf(ngx_conf_t *cf,
               ngx_command_t *cmd, void *conf);
ngx_int_t  ngx_http_fastcgi_cache_purge_handler(ngx_http_request_t *r);
# endif

# if (NGX_HTTP_PROXY)
char      *ngx_http_proxy_cache_purge_conf(ngx_conf_t *cf,
               ngx_command_t *cmd, void *conf);
ngx_int_t  ngx_http_proxy_cache_purge_handler(ngx_http_request_t *r);
# endif

# if (NGX_HTTP_SCGI)
char      *ngx_http_scgi_cache_purge_conf(ngx_conf_t *cf,
               ngx_command_t *cmd, void *conf);
ngx_int_t  ngx_http_scgi_cache_purge_handler(ngx_http_request_t *r);
# endif

# if (NGX_HTTP_UWSGI)
char      *ngx_http_uwsgi_cache_purge_conf(ngx_conf_t *cf,
               ngx_command_t *cmd, void *conf);
ngx_int_t  ngx_http_uwsgi_cache_purge_handler(ngx_http_request_t *r);
# endif

char *ngx_http_cache_purge_response_type_conf(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
char *ngx_http_cache_purge_queue_conf(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
char *ngx_http_cache_purge_legacy_status_conf(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
char *ngx_http_cache_purge_vary_aware_conf(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_purge_file_cache_noop(ngx_tree_ctx_t *ctx,
    ngx_str_t *path);
/*
 * ngx_http_purge_file_cache_delete_exact_file:
 *
 * Vary-aware exact-match handler.  Reads (key_len + 1) bytes from the
 * cache file at the KEY: offset.  Deletes the file only when:
 *   - the first key_len bytes match key_partial exactly (case-insensitive), AND
 *   - the byte immediately following the key is '\n'
 *
 * The '\n' check confirms the stored key is exactly key_len characters, so
 * keys that are longer but share a common prefix are not matched.  Because all
 * Vary variants of an entry store the same KEY: string, this walk removes every
 * variant file regardless of the filesystem path each one occupies.
 */
static ngx_int_t ngx_http_purge_file_cache_delete_exact_file(
    ngx_tree_ctx_t *ctx, ngx_str_t *path);
static void ngx_http_cache_purge_invalidate_node(ngx_http_file_cache_t *cache,
    ngx_str_t *path);

static void ngx_http_cache_purge_delete_variants(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache);

ngx_int_t  ngx_http_cache_purge_access_handler(ngx_http_request_t *r);
ngx_int_t  ngx_http_cache_purge_access(ngx_array_t *a, ngx_array_t *a6,
               struct sockaddr *s);
ngx_int_t  ngx_http_cache_purge_send_response(ngx_http_request_t *r,
               ngx_str_t *status);
# if (nginx_version >= 1007009)
ngx_int_t  ngx_http_cache_purge_cache_get(ngx_http_request_t *r,
               ngx_http_upstream_t *u, ngx_http_file_cache_t **cache);
# endif
ngx_int_t  ngx_http_cache_purge_init(ngx_http_request_t *r,
               ngx_http_file_cache_t *cache,
               ngx_http_complex_value_t *cache_key);
void       ngx_http_cache_purge_handler(ngx_http_request_t *r);
ngx_int_t  ngx_http_file_cache_purge(ngx_http_request_t *r);
void       ngx_http_cache_purge_all(ngx_http_request_t *r,
               ngx_http_file_cache_t *cache);
ngx_uint_t ngx_http_cache_purge_partial(ngx_http_request_t *r,
               ngx_http_file_cache_t *cache);
ngx_int_t  ngx_http_cache_purge_is_partial(ngx_http_request_t *r);
char      *ngx_http_cache_purge_conf(ngx_conf_t *cf,
               ngx_http_cache_purge_conf_t *cpcf);
void      *ngx_http_cache_purge_create_loc_conf(ngx_conf_t *cf);
char      *ngx_http_cache_purge_merge_loc_conf(ngx_conf_t *cf,
               void *parent, void *child);


/* -- module commands ---------------------------------------------------- */

static ngx_command_t  ngx_http_cache_purge_module_commands[] = {

# if (NGX_HTTP_FASTCGI)
    { ngx_string("fastcgi_cache_purge"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_fastcgi_cache_purge_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
# endif

# if (NGX_HTTP_PROXY)
    { ngx_string("proxy_cache_purge"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_proxy_cache_purge_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
# endif

# if (NGX_HTTP_SCGI)
    { ngx_string("scgi_cache_purge"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_scgi_cache_purge_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
# endif

# if (NGX_HTTP_UWSGI)
    { ngx_string("uwsgi_cache_purge"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_uwsgi_cache_purge_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
# endif

    { ngx_string("cache_purge_response_type"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_response_type_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

    { ngx_string("cache_purge_background_queue"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_queue_conf,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, background_purge), NULL },

    { ngx_string("cache_purge_queue_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, queue_size), NULL },

    { ngx_string("cache_purge_batch_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, batch_size), NULL },

    /* Accepts standard nginx time values.  A bare integer means seconds per
     * the nginx time-value contract; use an explicit suffix for milliseconds:
     *   cache_purge_throttle_ms 10ms;   -- 10 ms  (correct)
     *   cache_purge_throttle_ms 10;     -- 10 s   (almost certainly wrong)
     * Default when directive is absent: 10 ms (set via ngx_conf_init_msec_value,
     * which bypasses the parser and assigns the raw integer directly). */
    { ngx_string("cache_purge_throttle_ms"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, throttle_ms), NULL },

    /* Time a background walk runs before yielding; 0 = whole walk per tick */
    { ngx_string("cache_purge_walk_budget"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, walk_budget), NULL },

    /* Drop queued purges older than this; 0 (default) = never */
    { ngx_string("cache_purge_queue_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, queue_timeout), NULL },

    /* Key index for wildcard purges (shared memory size; 0 = off) */
    { ngx_string("cache_purge_index"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, index_size), NULL },

    /* Matching files an indexed wildcard deletes before answering */
    { ngx_string("cache_purge_index_sync_limit"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, index_sync_limit), NULL },

    /* How often expired entries are dropped from the index */
    { ngx_string("cache_purge_index_reconcile"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, index_reconcile), NULL },

    { ngx_string("cache_purge_legacy_status"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_legacy_status_conf,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, legacy_status_codes), NULL },

    { ngx_string("cache_purge_vary_aware"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_vary_aware_conf,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, vary_aware), NULL },

    ngx_null_command
};


/* -- module context & descriptor ---------------------------------------- */

static ngx_http_module_t  ngx_http_cache_purge_module_ctx = {
    NULL,                                   /* preconfiguration  */
    ngx_http_cache_purge_filter_init,       /* postconfiguration */
    ngx_http_cache_purge_create_main_conf,  /* create main conf  */
    ngx_http_cache_purge_init_main_conf,    /* init main conf    */
    NULL,                                   /* create srv conf   */
    NULL,                                   /* merge srv conf    */
    ngx_http_cache_purge_create_loc_conf,   /* create loc conf   */
    ngx_http_cache_purge_merge_loc_conf     /* merge loc conf    */
};

ngx_module_t  ngx_http_cache_purge_module = {
    NGX_MODULE_V1,
    &ngx_http_cache_purge_module_ctx,
    ngx_http_cache_purge_module_commands,
    NGX_HTTP_MODULE,
    NULL,                                   /* init master  */
    NULL,                                   /* init module  */
    ngx_http_cache_purge_init_worker,       /* init process */
    NULL,                                   /* init thread  */
    NULL,                                   /* exit thread  */
    ngx_http_cache_purge_exit_worker,       /* exit process */
    NULL,                                   /* exit master  */
    NGX_MODULE_V1_PADDING
};

/* Per-worker globals -- safe because nginx forks one process per worker */
static ngx_event_t                        ngx_cache_purge_event;
static ngx_http_cache_purge_main_conf_t  *ngx_cache_purge_main_conf;
static ngx_http_cache_purge_pass_t        ngx_cache_purge_pass;
static ngx_http_cache_purge_build_t       ngx_cache_purge_build;
static ngx_event_t                        ngx_cache_purge_build_event;
static ngx_http_cache_purge_main_conf_t  *ngx_cache_purge_build_conf;
static ngx_http_cache_purge_reconcile_t   ngx_cache_purge_reconcile;


/* -- main configuration ------------------------------------------------- */

static void *
ngx_http_cache_purge_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_cache_purge_main_conf_t *cmcf;

    cmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cache_purge_main_conf_t));
    if (cmcf == NULL) {
        return NULL;
    }

    cmcf->background_purge    = NGX_CONF_UNSET;
    cmcf->queue_size          = NGX_CONF_UNSET_UINT;
    cmcf->batch_size          = NGX_CONF_UNSET_UINT;
    cmcf->throttle_ms         = NGX_CONF_UNSET_MSEC;
    cmcf->legacy_status_codes = NGX_CONF_UNSET;
    cmcf->vary_aware          = NGX_CONF_UNSET;
    cmcf->walk_budget         = NGX_CONF_UNSET_MSEC;
    cmcf->queue_timeout       = NGX_CONF_UNSET_MSEC;
    cmcf->index_size          = NGX_CONF_UNSET_SIZE;
    cmcf->index_sync_limit    = NGX_CONF_UNSET_UINT;
    cmcf->index_reconcile     = NGX_CONF_UNSET_MSEC;

    return cmcf;
}

static char *
ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_cache_purge_main_conf_t *cmcf = conf;
    ngx_str_t                         name = ngx_string("cache_purge_queue");
    ngx_str_t                         iname = ngx_string("cache_purge_index");
    size_t                            shm_size;
    size_t                            stride;   /* bytes per queue slot (item + 2 keys) */

    ngx_conf_init_value(cmcf->background_purge,    0);
    ngx_conf_init_uint_value(cmcf->queue_size,     NGX_CACHE_PURGE_QUEUE_SIZE_DEFAULT);
    ngx_conf_init_uint_value(cmcf->batch_size,     NGX_CACHE_PURGE_BATCH_SIZE_DEFAULT);
    ngx_conf_init_msec_value(cmcf->throttle_ms,    NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT);
    /* Default on: return 412 for missing entries (backwards compatibility) */
    ngx_conf_init_value(cmcf->legacy_status_codes, 1);
    /* Default off: vary-aware walk adds cost; opt in explicitly */
    ngx_conf_init_value(cmcf->vary_aware,          0);
    ngx_conf_init_msec_value(cmcf->walk_budget,
                             NGX_CACHE_PURGE_WALK_BUDGET_DEFAULT);
    ngx_conf_init_msec_value(cmcf->queue_timeout,
                             NGX_CACHE_PURGE_QUEUE_TIMEOUT);
    ngx_conf_init_size_value(cmcf->index_size, 0);
    ngx_conf_init_uint_value(cmcf->index_sync_limit,
                             NGX_CACHE_PURGE_INDEX_SYNC_LIMIT);
    ngx_conf_init_msec_value(cmcf->index_reconcile,
                             NGX_CACHE_PURGE_INDEX_RECONCILE);

    if (cmcf->index_size != 0 && !cmcf->background_purge) {
        /* the build walk and reconcile run on the background drainer */
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "cache_purge_index requires "
                           "cache_purge_background_queue on");
        return NGX_CONF_ERROR;
    }

    if (cmcf->index_sync_limit == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "cache_purge_index_sync_limit must be greater "
                           "than 0");
        return NGX_CONF_ERROR;
    }

    /*
     * Reject zero values: queue_size=0 makes the queue permanently "full"
     * (every enqueue hits the size >= max_size guard); batch_size=0 makes
     * process_queue a no-op loop that never processes any item.
     */
    if (cmcf->queue_size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "cache_purge_queue_size must be greater than 0");
        return NGX_CONF_ERROR;
    }

    if (cmcf->batch_size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "cache_purge_batch_size must be greater than 0");
        return NGX_CONF_ERROR;
    }

    if (!cmcf->background_purge) {
        return NGX_CONF_OK;
    }

    /*
     * Guard against unsigned integer overflow in the shm_size calculation.
     *
     *   shm_size = sizeof(queue_t)
     *            + queue_size * sizeof(item_t)
     *            + queue_size * 2 * KEY_MAX_LEN
     *            = sizeof(queue_t) + queue_size * stride
     *
     * Overflow condition (unsigned arithmetic):
     *   queue_size > (SIZE_MAX - sizeof(queue_t)) / stride
     *
     * where SIZE_MAX is represented as (size_t) -1, valid in C89/C90.
     */
    stride = sizeof(ngx_http_cache_purge_queue_item_t)
             + 2 * NGX_CACHE_PURGE_KEY_MAX_LEN;

    if (cmcf->queue_size > ((size_t) -1
                            - sizeof(ngx_http_cache_purge_queue_t)) / stride)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "cache_purge_queue_size %ui overflows shared "
                           "memory size calculation", cmcf->queue_size);
        return NGX_CONF_ERROR;
    }

    for (cmcf->queue_buckets = 1;
         cmcf->queue_buckets < cmcf->queue_size;
         cmcf->queue_buckets <<= 1)
    { /* void */ }

    shm_size = sizeof(ngx_http_cache_purge_queue_t)
             + cmcf->queue_buckets
               * sizeof(ngx_http_cache_purge_queue_item_t *)
             + cmcf->queue_size * sizeof(ngx_http_cache_purge_queue_item_t)
             + cmcf->queue_size * 2 * NGX_CACHE_PURGE_KEY_MAX_LEN;

    /*
     * The slab allocator imposes metadata overhead that is NOT reflected in
     * the payload calculation above:
     *
     *   ngx_slab_pool_t header  -- size varies by nginx version, build flags
     *                             (e.g. NGX_HAVE_POSIX_SEM, --with-threads),
     *                             and platform ABI.
     *   Slot descriptors        -- (pagesize_shift - min_shift) page structs.
     *   Stat entries            -- same count, added in nginx ~1.9.x.
     *   Page descriptors        -- one per allocatable page.
     *   Alignment gap           -- up to one full page between descriptors
     *                             and the first usable byte (pool->start is
     *                             rounded up to the next page boundary).
     *
     * Attempting to compute this precisely requires knowledge of internal
     * nginx structs that differ across versions (1.8 vs 1.9+), build
     * configurations, and architectures (Linux, *BSD, Solaris, macOS --
     * each may have different page sizes: 4 KB, 8 KB, 16 KB, 64 KB).
     *
     * The portable, version-agnostic approach: round up the payload to a
     * page boundary, then enforce a minimum of NGX_CACHE_PURGE_SHM_MIN_PAGES
     * pages.  This minimum is chosen so that, on every supported platform,
     * the slab overhead leaves at least one full page of usable heap even
     * when queue_size=1.  ngx_pagesize is always the runtime system page
     * size, so the minimum scales automatically on big-page architectures.
     */
    shm_size = ngx_align(shm_size, ngx_pagesize);

    if (shm_size < NGX_CACHE_PURGE_SHM_MIN_PAGES * ngx_pagesize) {
        shm_size = NGX_CACHE_PURGE_SHM_MIN_PAGES * ngx_pagesize;
    }

    cmcf->shm_zone = ngx_shared_memory_add(cf, &name, shm_size,
                                           &ngx_http_cache_purge_module);
    if (cmcf->shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    cmcf->shm_zone->init = ngx_http_cache_purge_init_shm_zone;
    cmcf->shm_zone->data = cmcf;

    if (cmcf->index_size != 0) {

        if (cmcf->index_size < NGX_CACHE_PURGE_SHM_MIN_PAGES * ngx_pagesize) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "cache_purge_index %uz is too small",
                               cmcf->index_size);
            return NGX_CONF_ERROR;
        }

        cmcf->index_zone = ngx_shared_memory_add(cf, &iname,
                                                 cmcf->index_size,
                                                 &ngx_http_cache_purge_module);
        if (cmcf->index_zone == NULL) {
            return NGX_CONF_ERROR;
        }

        cmcf->index_zone->init = ngx_http_cache_purge_index_init_zone;
        cmcf->index_zone->data = cmcf;
    }

    return NGX_CONF_OK;
}

/*
 * Crash safety of the shared zones (queue and key index).
 *
 * Both are locked with their slab pool's mutex -- the one lock nginx's master
 * force-unlocks (ngx_unlock_mutexes) when a worker dies holding it -- so a
 * killed or crashed worker can never leave every other process spinning on a
 * lock.  The holder also raises a busy flag while it has the lock; finding
 * the flag already up on locking means the previous holder died in the middle
 * of an update, and whatever it was changing may be half done.  The zone is
 * then wiped and started over (ngx_slab_init) rather than trusted: for the
 * index that means a rebuild from the cache directory, for the queue losing
 * what was queued (logged).
 *
 * Because a reset re-creates the zone's root struct, processes never keep a
 * pointer to it: they fetch it from shpool->data under the lock, and compare
 * a generation number before acting on anything they took out of the zone
 * earlier.
 */

static ngx_http_cache_purge_queue_t *
ngx_http_cache_purge_queue_create(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_slab_pool_t *shpool, ngx_uint_t generation)
{
    ngx_http_cache_purge_queue_t  *queue;

    /* a fresh pool sized for these two cannot fail them */
    queue = ngx_slab_calloc_locked(shpool,
                                   sizeof(ngx_http_cache_purge_queue_t));
    if (queue == NULL) {
        return NULL;
    }

    queue->buckets = ngx_slab_calloc_locked(shpool, cmcf->queue_buckets
                         * sizeof(ngx_http_cache_purge_queue_item_t *));
    if (queue->buckets == NULL) {
        return NULL;
    }

    queue->nbuckets    = cmcf->queue_buckets;
    queue->shpool      = shpool;
    queue->generation  = generation;
    queue->max_size    = cmcf->queue_size;
    queue->batch_size  = cmcf->batch_size;
    queue->throttle_ms = cmcf->throttle_ms;

    shpool->data = queue;

    return queue;
}

/* Lock the queue zone; returns the queue (NULL only if it cannot exist) */
static ngx_http_cache_purge_queue_t *
ngx_http_cache_purge_queue_lock(ngx_http_cache_purge_main_conf_t *cmcf)
{
    ngx_slab_pool_t               *shpool = cmcf->queue_shpool;
    ngx_http_cache_purge_queue_t  *queue;
    ngx_uint_t                     generation;

    ngx_shmtx_lock(&shpool->mutex);

    queue = shpool->data;

    if (queue != NULL && queue->busy) {
        generation = queue->generation + 1;

        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "ngx_cache_purge: a process died while updating the "
                      "purge queue; resetting it, %ui queued purge(s) lost",
                      queue->size);

        ngx_slab_init(shpool);
        shpool->data = NULL;
        queue = ngx_http_cache_purge_queue_create(cmcf, shpool, generation);
    }

    if (queue != NULL) {
        queue->busy = 1;
    }

    return queue;
}

static void
ngx_http_cache_purge_queue_unlock(ngx_http_cache_purge_main_conf_t *cmcf)
{
    ngx_http_cache_purge_queue_t  *queue = cmcf->queue_shpool->data;

    if (queue != NULL) {
        queue->busy = 0;
    }

    ngx_shmtx_unlock(&cmcf->queue_shpool->mutex);
}

/* Unlocked peek, for hints only */
static ngx_http_cache_purge_queue_t *
ngx_http_cache_purge_queue_peek(ngx_http_cache_purge_main_conf_t *cmcf)
{
    return (cmcf->queue_shpool != NULL) ? cmcf->queue_shpool->data : NULL;
}

/*
 * Shared-memory zone initialiser -- called by the master process once per
 * nginx start or live reload.  On a reload the existing queue is kept, so
 * purges queued before it are not lost, and its tunables are refreshed.
 * max_size is not reduced below the current occupancy.
 */
static ngx_int_t
ngx_http_cache_purge_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_cache_purge_main_conf_t *cmcf   = shm_zone->data;
    ngx_http_cache_purge_main_conf_t *old    = data;
    ngx_http_cache_purge_queue_t     *queue;
    ngx_slab_pool_t                  *shpool;

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    cmcf->queue_shpool = shpool;

    if (old != NULL || shm_zone->shm.exists) {
        queue = ngx_http_cache_purge_queue_lock(cmcf);

        if (queue != NULL) {
            queue->batch_size  = cmcf->batch_size;
            queue->throttle_ms = cmcf->throttle_ms;
            queue->max_size    = (cmcf->queue_size > queue->size)
                                 ? cmcf->queue_size : queue->size;
        }

        ngx_http_cache_purge_queue_unlock(cmcf);

        return NGX_OK;
    }

    ngx_shmtx_lock(&shpool->mutex);
    queue = ngx_http_cache_purge_queue_create(cmcf, shpool, 1);
    ngx_shmtx_unlock(&shpool->mutex);

    if (queue == NULL) {
        ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                      "ngx_cache_purge: could not allocate queue "
                      "in shared memory zone \"%V\"", &shm_zone->shm.name);
        return NGX_ERROR;
    }

    return NGX_OK;
}


/* -- worker lifecycle --------------------------------------------------- */

static ngx_int_t
ngx_http_cache_purge_init_worker(ngx_cycle_t *cycle)
{
    ngx_http_core_main_conf_t        *cmcf_core;
    ngx_http_cache_purge_main_conf_t *cmcf;

    cmcf_core = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_core_module);
    if (cmcf_core == NULL) {
        return NGX_OK;
    }

    cmcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_cache_purge_module);
    if (cmcf == NULL || !cmcf->background_purge) {
        return NGX_OK;
    }

    /* every worker walks a share of an index build (build_tick) */
    if (cmcf->index_zone != NULL
        && (ngx_process == NGX_PROCESS_WORKER
            || ngx_process == NGX_PROCESS_SINGLE))
    {
        ngx_cache_purge_build_conf = cmcf;
        ngx_cache_purge_build.unit = NGX_CACHE_PURGE_UNIT_NONE;

        ngx_memzero(&ngx_cache_purge_build_event, sizeof(ngx_event_t));
        ngx_cache_purge_build_event.handler    = ngx_http_cache_purge_build_tick;
        ngx_cache_purge_build_event.log        = cycle->log;
        ngx_cache_purge_build_event.data       = cycle;
        ngx_cache_purge_build_event.cancelable = 1;

        ngx_add_timer(&ngx_cache_purge_build_event, cmcf->throttle_ms);
    }

    /*
     * One drainer for the whole shared queue: worker 0.  Parallel walks of
     * the same tree only contend on dentry/inode locks and multiply the sys
     * time; one walker that coalesces every pending purge does the same work
     * once.  The check also keeps the cache manager and cache loader out:
     * they run init_process with ngx_worker still 0, and the loader exits
     * once loading is done, taking any dequeued purge with it.
     */
    if (ngx_process != NGX_PROCESS_SINGLE
        && (ngx_process != NGX_PROCESS_WORKER || ngx_worker != 0))
    {
        return NGX_OK;
    }

    ngx_cache_purge_main_conf = cmcf;

    /* first index reconcile one interval from now, not at every reload */
    ngx_cache_purge_reconcile.last = ngx_current_msec;

    ngx_memzero(&ngx_cache_purge_event, sizeof(ngx_event_t));
    ngx_cache_purge_event.handler     = ngx_http_cache_purge_background_handler;
    ngx_cache_purge_event.log         = cycle->log;
    ngx_cache_purge_event.data        = cycle;
    /*
     * Mark the timer as cancelable (nginx >= 1.11.11, June 2017).
     * Without this flag nginx's graceful-shutdown path waits for every
     * pending timer to fire before allowing the worker to exit.  Because
     * this handler re-arms itself on every invocation the worker would
     * never exit cleanly, causing Test::Nginx (and real deployments) to
     * time out and fall back to SIGKILL.  "cancelable" tells the event
     * loop: "discard this timer when the worker is exiting -- do not wait
     * for it."  All nginx versions we support (>= 1.20) have this field.
     */
    ngx_cache_purge_event.cancelable  = 1;

    ngx_add_timer(&ngx_cache_purge_event, cmcf->throttle_ms);

    return NGX_OK;
}

static void
ngx_http_cache_purge_exit_worker(ngx_cycle_t *cycle)
{
    ngx_http_cache_purge_pass_t       *pass = &ngx_cache_purge_pass;
    ngx_http_cache_purge_pass_item_t  *it;
    ngx_uint_t                         i;

    if (ngx_cache_purge_event.timer_set) {
        ngx_del_timer(&ngx_cache_purge_event);
    }

    /* the unit this worker was walking goes to another one */
    if (ngx_cache_purge_build_event.timer_set) {
        ngx_del_timer(&ngx_cache_purge_build_event);
    }

    if (ngx_cache_purge_build_conf != NULL) {
        ngx_http_cache_purge_build_release(ngx_cache_purge_build_conf,
                                           &ngx_cache_purge_build);
    }

    if (pass->pool == NULL) {
        return;
    }

    /*
     * Exiting mid-pass (reload, graceful stop): hand the purges back to the
     * shared queue, which survives reloads, so the next worker 0 walks them
     * again from the start instead of silently dropping them.
     */
    if (ngx_cache_purge_main_conf != NULL
        && ngx_cache_purge_main_conf->queue_shpool != NULL)
    {
        it = pass->items.elts;
        for (i = 0; i < pass->items.nelts; i++) {
            (void) ngx_http_cache_purge_enqueue_raw(
                       ngx_cache_purge_main_conf, cycle->log,
                       &pass->cache_path, &it[i].key, it[i].purge_all,
                       pass->mode);
        }

        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "ngx_cache_purge: re-queued %ui purge(s) of \"%V\" "
                      "from an unfinished walk", pass->items.nelts,
                      &pass->cache_path);
    }

    ngx_http_cache_purge_pass_end(pass);
}

/*
 * Background timer callback -- fires every throttle_ms milliseconds.
 *
 * Each invocation calls process_queue(), which dequeues and walks exactly
 * one item before returning.  This one-item-per-tick design ensures the
 * event loop is never blocked for more than the duration of a single
 * directory walk, regardless of queue depth.
 *
 *   NGX_AGAIN  -- one item was processed; re-arm with throttle_ms so the
 *                next item is handled promptly.
 *
 *   NGX_OK     -- queue is empty; re-arm with throttle_ms * 10 to avoid
 *                busy-polling on an idle queue.
 *
 *   NGX_ERROR  -- module not yet initialised; use the raw constant and
 *                retry next tick.
 *
 * Historical note: the previous implementation called ngx_msleep() inside
 * the callback to throttle I/O.  ngx_msleep() is a literal usleep() that
 * blocks the OS thread -- stalling every connection on that worker for the
 * full sleep duration.  Timer-based yielding is the correct nginx idiom.
 */
static void
ngx_http_cache_purge_background_handler(ngx_event_t *ev)
{
    ngx_cycle_t                      *cycle = ev->data;
    ngx_http_cache_purge_main_conf_t *cmcf  = ngx_cache_purge_main_conf;
    ngx_int_t                         rc;
    ngx_msec_t                        next_delay;

    if (cmcf == NULL || cmcf->queue_shpool == NULL) {
        /* cmcf not yet initialised; use the raw-ms constant directly
         * (not through ngx_parse_time, so no *1000 conversion). */
        ngx_add_timer(ev, NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT);
        return;
    }

    rc = ngx_http_cache_purge_process_queue(cycle);

    /*
     * NGX_AGAIN  -> an item was processed and more remain; come back soon.
     * NGX_OK     -> queue is now empty; back off to avoid busy-polling.
     * NGX_ERROR  -> configuration problem; back off.
     */
    next_delay = (rc == NGX_AGAIN) ? cmcf->throttle_ms
                                   : cmcf->throttle_ms * 10;

    ngx_add_timer(ev, next_delay);
}


/* -- queue operations --------------------------------------------------- */

static ngx_int_t
ngx_http_cache_purge_enqueue(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all)
{
    ngx_http_cache_purge_main_conf_t   *cmcf;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    if (cmcf == NULL || cmcf->queue_shpool == NULL) {
        return NGX_ERROR;
    }

    return ngx_http_cache_purge_enqueue_raw(cmcf, r->connection->log,
                                            &cache->path->name, key,
                                            purge_all,
                                            NGX_CACHE_PURGE_MODE_WALK);
}

/*
 * Returns NGX_OK when queued (or already queued), NGX_BUSY when the queue is
 * full or its shared memory is exhausted, NGX_ERROR otherwise.
 */
static ngx_int_t
ngx_http_cache_purge_enqueue_raw(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_log_t *log, ngx_str_t *cache_path, ngx_str_t *key,
    ngx_flag_t purge_all, ngx_uint_t mode)
{
    ngx_http_cache_purge_queue_t       *queue;
    ngx_http_cache_purge_queue_item_t  *item;
    ngx_uint_t                          hash, size;
    u_char                             *p;

    hash = ngx_http_cache_purge_hash_key(cache_path, key);

    queue = ngx_http_cache_purge_queue_lock(cmcf);

    if (queue == NULL) {
        ngx_http_cache_purge_queue_unlock(cmcf);
        return NGX_BUSY;
    }

    if (queue->size >= queue->max_size) {
        size = queue->size;
        ngx_http_cache_purge_queue_unlock(cmcf);
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "ngx_cache_purge: queue full (%ui items)", size);
        return NGX_BUSY;
    }

    if (ngx_http_cache_purge_find_duplicate(queue, mode, hash, cache_path,
                                            key)
        != NULL)
    {
        ngx_http_cache_purge_queue_unlock(cmcf);
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, log, 0,
                       "ngx_cache_purge: duplicate enqueue for \"%V\" "
                       "key \"%V\", skipping", cache_path, key);
        return NGX_OK;
    }

    item = ngx_slab_calloc_locked(queue->shpool,
                                  sizeof(ngx_http_cache_purge_queue_item_t));
    if (item == NULL) {
        goto nomem;
    }

    /* +1: NUL terminator required by opendir() */
    p = ngx_slab_alloc_locked(queue->shpool, cache_path->len + 1);
    if (p == NULL) {
        ngx_slab_free_locked(queue->shpool, item);
        goto nomem;
    }
    ngx_memcpy(p, cache_path->data, cache_path->len);
    p[cache_path->len] = '\0';
    item->cache_path.data = p;
    item->cache_path.len  = cache_path->len;

    if (key->len > 0) {
        p = ngx_slab_alloc_locked(queue->shpool, key->len + 1);
        if (p == NULL) {
            ngx_slab_free_locked(queue->shpool, item->cache_path.data);
            ngx_slab_free_locked(queue->shpool, item);
            goto nomem;
        }
        ngx_memcpy(p, key->data, key->len);
        p[key->len] = '\0';
        item->key_partial.data = p;
        item->key_partial.len  = key->len;
    }

    item->hash        = hash;
    item->purge_all   = purge_all;
    item->mode        = mode;
    item->enqueued_at = ngx_current_msec;

    if (queue->tail != NULL) {
        queue->tail->next = item;
    } else {
        queue->head = item;
    }
    queue->tail = item;
    queue->size++;

    item->hnext = queue->buckets[hash & (queue->nbuckets - 1)];
    queue->buckets[hash & (queue->nbuckets - 1)] = item;

    ngx_http_cache_purge_queue_unlock(cmcf);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, log, 0,
                   "ngx_cache_purge: enqueued purge of \"%V\" key \"%V\"",
                   cache_path, key);

    return NGX_OK;

nomem:

    ngx_http_cache_purge_queue_unlock(cmcf);
    ngx_log_error(NGX_LOG_CRIT, log, 0,
                  "ngx_cache_purge: shared memory exhausted, "
                  "could not queue a purge");
    return NGX_BUSY;
}

/* Drop an item from its duplicate bucket.  Locked. */
static void
ngx_http_cache_purge_queue_unhash(ngx_http_cache_purge_queue_t *queue,
    ngx_http_cache_purge_queue_item_t *item)
{
    ngx_http_cache_purge_queue_item_t  **pp;

    for (pp = &queue->buckets[item->hash & (queue->nbuckets - 1)];
         *pp != NULL;
         pp = &(*pp)->hnext)
    {
        if (*pp == item) {
            *pp = item->hnext;
            return;
        }
    }
}

/* Free dequeued items -- unless the queue was reset since.  Locks. */
static void
ngx_http_cache_purge_queue_free(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_http_cache_purge_queue_item_t *item, ngx_uint_t generation)
{
    ngx_http_cache_purge_queue_t       *queue;
    ngx_http_cache_purge_queue_item_t  *next;

    queue = ngx_http_cache_purge_queue_lock(cmcf);

    if (queue != NULL && queue->generation == generation) {
        for ( /* void */ ; item != NULL; item = next) {
            next = item->next;
            ngx_slab_free_locked(queue->shpool, item->cache_path.data);
            if (item->key_partial.data) {
                ngx_slab_free_locked(queue->shpool, item->key_partial.data);
            }
            ngx_slab_free_locked(queue->shpool, item);
        }
    }

    ngx_http_cache_purge_queue_unlock(cmcf);
}
/*
 * Request-side wrapper around the queue.  NGX_DONE: the request has been
 * answered (202 queued, or 429 when the queue is full).  NGX_DECLINED: not
 * queued, the caller falls back to a synchronous walk.
 *
 * A full queue answers 429 rather than walking synchronously: a synchronous
 * wildcard walk is exactly what pins request workers, and doing it under
 * overload turns a backlog into an outage.
 */
static ngx_int_t
ngx_http_cache_purge_try_enqueue(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all)
{
    ngx_str_t  status;
    ngx_int_t  rc;

    /* neither the index nor a walk can match a longer prefix: say so */
    if (!purge_all && key->len > NGX_CACHE_PURGE_KEY_MAX_LEN) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "ngx_cache_purge: wildcard key too long (%uz bytes)",
                      key->len);
        r->main->count++;
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return NGX_DONE;
    }

    /* the key index answers without a walk whenever it can */
    if (ngx_http_cache_purge_index_purge(r, cache, key, purge_all) == NGX_DONE)
    {
        return NGX_DONE;
    }

    rc = ngx_http_cache_purge_enqueue(r, cache, key, purge_all);

    if (rc == NGX_OK) {
        r->headers_out.status = NGX_HTTP_ACCEPTED;
        ngx_str_set(&status, "queued");
        r->main->count++;
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (rc == NGX_BUSY) {
        r->main->count++;
        ngx_http_finalize_request(r, NGX_HTTP_TOO_MANY_REQUESTS);
        return NGX_DONE;
    }

    return NGX_DECLINED;
}

/*
 * pass_start -- dequeue the next batch and open a walk for it.
 *
 * The batch is every queued purge for the cache path at the head of the
 * queue, up to batch_size items; they are matched in one walk, so N pending
 * wildcards cost one walk instead of N.  Items are copied into worker memory
 * and freed from shared memory straight away, which keeps the queue free for
 * new purges while the walk runs.
 *
 * Returns NGX_OK (walk open), NGX_DONE (queue empty) or NGX_DECLINED (batch
 * consumed but the walk could not start).
 */
static ngx_int_t
ngx_http_cache_purge_pass_start(ngx_cycle_t *cycle,
    ngx_http_cache_purge_main_conf_t *cmcf, ngx_http_cache_purge_pass_t *pass)
{
    ngx_http_cache_purge_queue_t       *queue;
    ngx_http_cache_purge_queue_item_t  *item, *next, *prev, *taken, *last;
    ngx_http_cache_purge_pass_item_t   *it;
    ngx_str_t                          *path, *pat;
    ngx_uint_t                          n, batch;
    ngx_msec_t                          now, timeout;
    ngx_flag_t                          expired;
    ngx_uint_t                          mode, generation;

    mode    = 0;
    now     = ngx_current_msec;
    timeout = cmcf->queue_timeout;
    taken   = NULL;
    last    = NULL;
    prev    = NULL;
    path    = NULL;
    n       = 0;

    queue = ngx_http_cache_purge_queue_lock(cmcf);

    if (queue == NULL) {
        ngx_http_cache_purge_queue_unlock(cmcf);
        return NGX_DONE;
    }

    batch      = queue->batch_size;
    generation = queue->generation;

    for (item = queue->head; item != NULL && n < batch; item = next) {
        next    = item->next;
        expired = (timeout != 0 && now - item->enqueued_at > timeout);

        if (!expired
            && path != NULL
            && (item->mode != mode
                || item->cache_path.len != path->len
                || ngx_memcmp(item->cache_path.data, path->data, path->len)
                   != 0))
        {
            prev = item;
            continue;
        }

        /* unlink */
        if (prev != NULL) {
            prev->next = next;
        } else {
            queue->head = next;
        }
        if (queue->tail == item) {
            queue->tail = prev;
        }
        queue->size--;
        item->next = NULL;
        ngx_http_cache_purge_queue_unhash(queue, item);

        if (expired) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                          "ngx_cache_purge: purge of \"%V\" key \"%V\" "
                          "timed out after %Mms, discarding",
                          &item->cache_path, &item->key_partial,
                          now - item->enqueued_at);
            ngx_slab_free_locked(queue->shpool, item->cache_path.data);
            if (item->key_partial.data) {
                ngx_slab_free_locked(queue->shpool, item->key_partial.data);
            }
            ngx_slab_free_locked(queue->shpool, item);
            continue;
        }

        if (path == NULL) {
            path = &item->cache_path;
            mode = item->mode;
        }

        if (last != NULL) {
            last->next = item;
        } else {
            taken = item;
        }
        last = item;
        n++;
    }

    ngx_http_cache_purge_queue_unlock(cmcf);

    if (taken == NULL) {
        return NGX_DONE;
    }

    /* Copy the batch into worker memory, then release the shm items */

    ngx_memzero(pass, sizeof(ngx_http_cache_purge_pass_t));

    pass->pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, cycle->log);

    if (pass->pool == NULL
        || ngx_array_init(&pass->items, pass->pool, n,
                          sizeof(ngx_http_cache_purge_pass_item_t)) != NGX_OK)
    {
        goto failed;
    }

    pass->cache_path.len  = taken->cache_path.len;
    pass->cache_path.data = ngx_pnalloc(pass->pool, taken->cache_path.len + 1);
    if (pass->cache_path.data == NULL) {
        goto failed;
    }
    ngx_memcpy(pass->cache_path.data, taken->cache_path.data,
               taken->cache_path.len + 1);
    path = &pass->cache_path;   /* the shm copy is freed below */

    pat = ngx_palloc(pass->pool, n * sizeof(ngx_str_t));
    if (pat == NULL) {
        goto failed;
    }

    for (item = taken, n = 0; item != NULL; item = item->next, n++) {
        it = ngx_array_push(&pass->items);
        if (it == NULL) {
            goto failed;
        }

        it->purge_all = item->purge_all;
        it->key.len   = item->key_partial.len;
        it->key.data  = ngx_pstrdup(pass->pool, &item->key_partial);
        if (it->key.len > 0 && it->key.data == NULL) {
            goto failed;
        }

        /* purge_all matches everything: an empty prefix */
        pat[n].len  = it->purge_all ? 0 : it->key.len;
        pat[n].data = it->key.data;

        if (pat[n].len > 0 && pat[n].data[pat[n].len - 1] == '*') {
            pat[n].len--;
        }
    }

    ngx_http_cache_purge_queue_free(cmcf, taken, generation);
    taken = NULL;

    pass->scan.log   = cycle->log;
    pass->scan.cache = ngx_http_cache_purge_find_cache(cycle,
                                                       &pass->cache_path);
    pass->started    = ngx_current_msec;
    pass->mode       = mode;

    if (ngx_http_cache_purge_patterns_init(&pass->scan.pats, pat, n,
                                           cycle->log) != NGX_OK)
    {
        goto failed;
    }

    if (mode == NGX_CACHE_PURGE_MODE_INDEX) {
        pass->hits = ngx_palloc(pass->pool, cmcf->index_sync_limit
                                   * sizeof(ngx_http_cache_purge_index_hit_t));
        if (pass->hits == NULL) {
            goto failed;
        }

    } else if (ngx_http_cache_purge_scan_open(&pass->scan, &pass->cache_path)
               != NGX_OK)
    {
        goto failed;
    }

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, cycle->log, 0,
                   "ngx_cache_purge: pass of \"%V\" (mode %ui) started for "
                   "%ui purge(s), %ui distinct prefix(es)", &pass->cache_path,
                   mode, pass->items.nelts, pass->scan.pats.nelts);

    return NGX_OK;

failed:

    ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                  "ngx_cache_purge: could not start a walk of \"%V\", "
                  "%ui purge(s) dropped", path, n);

    ngx_http_cache_purge_queue_free(cmcf, taken, generation);

    ngx_http_cache_purge_pass_end(pass);

    return NGX_DECLINED;
}

static void
ngx_http_cache_purge_pass_end(ngx_http_cache_purge_pass_t *pass)
{
    ngx_http_cache_purge_scan_close(&pass->scan);

    if (pass->pool != NULL) {
        ngx_destroy_pool(pass->pool);
    }

    ngx_memzero(pass, sizeof(ngx_http_cache_purge_pass_t));
}

/*
 * process_queue -- advance the background walk by one time slice.
 *
 * A pass (see pass_start) is walked in slices of walk_budget; between slices
 * the timer returns control to the event loop, so the draining worker keeps
 * serving requests throughout a walk of any size.  Purges that arrive while
 * a pass runs wait for the next pass.
 *
 * Return values:
 *   NGX_AGAIN  -- work remains (walk unfinished, or queue maybe non-empty).
 *   NGX_OK     -- queue is empty; caller should apply the backoff delay.
 *   NGX_ERROR  -- module not initialised; caller should apply backoff.
 */
static ngx_int_t
ngx_http_cache_purge_process_queue(ngx_cycle_t *cycle)
{
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_cache_purge_pass_t       *pass;
    ngx_int_t                          rc;
    ngx_flag_t                         purges;
    static ngx_uint_t                  turn;

    cmcf = ngx_cache_purge_main_conf;
    if (cmcf == NULL || cmcf->queue_shpool == NULL) {
        return NGX_ERROR;
    }

    pass = &ngx_cache_purge_pass;

    /*
     * Purges and index upkeep (build / rebuild / reconcile) share the
     * drainer; when both have work they take turns, so neither starves.
     * queue->head is read unlocked: it is only a hint here.
     */
    purges = (pass->pool != NULL
              || (!ngx_exiting
                  && ngx_http_cache_purge_queue_peek(cmcf) != NULL
                  && ngx_http_cache_purge_queue_peek(cmcf)->head != NULL));

    if (!purges || (ngx_http_cache_purge_index_peek(cmcf) != NULL && (turn++ & 1))) {
        rc = ngx_http_cache_purge_index_work(cycle, cmcf);

        if (rc == NGX_AGAIN || !purges) {
            return rc;
        }
    }

    if (pass->pool == NULL) {

        /* a worker shutting down after a reload leaves new passes to the
         * new worker 0; an unfinished one is re-queued by exit_worker */
        if (ngx_exiting) {
            return NGX_OK;
        }

        rc = ngx_http_cache_purge_pass_start(cycle, cmcf, pass);

        if (rc == NGX_DONE) {
            return NGX_OK;
        }

        if (rc != NGX_OK) {
            return NGX_AGAIN;
        }
    }

    if (pass->mode == NGX_CACHE_PURGE_MODE_INDEX) {

        rc = ngx_http_cache_purge_index_pass(pass, cmcf, cmcf->walk_budget);

        if (rc == NGX_AGAIN) {
            return NGX_AGAIN;
        }

        if (rc == NGX_DECLINED) {
            /* the index stopped being complete: walk for the rest */
            pass->mode = NGX_CACHE_PURGE_MODE_WALK;

            if (ngx_http_cache_purge_scan_open(&pass->scan, &pass->cache_path)
                != NGX_OK)
            {
                ngx_http_cache_purge_pass_end(pass);
                return NGX_AGAIN;
            }

            return NGX_AGAIN;
        }

    } else if (ngx_http_cache_purge_scan(&pass->scan, cmcf->walk_budget)
               == NGX_AGAIN)
    {
        return NGX_AGAIN;
    }

    ngx_time_update();

    ngx_log_error(NGX_LOG_INFO, cycle->log, 0,
                  "ngx_cache_purge: %s of \"%V\" for %ui purge(s) checked "
                  "%ui file(s), deleted %ui, in %Mms",
                  pass->mode == NGX_CACHE_PURGE_MODE_INDEX
                  ? "index purge" : "walk",
                  &pass->cache_path, pass->items.nelts,
                  pass->scan.files_checked, pass->scan.files_deleted,
                  ngx_current_msec - pass->started);

    ngx_http_cache_purge_pass_end(pass);

    return NGX_AGAIN;
}

static ngx_uint_t
ngx_http_cache_purge_hash_key(ngx_str_t *cache_path, ngx_str_t *key)
{
    ngx_uint_t  hash = 0;
    ngx_uint_t  i;

    for (i = 0; i < cache_path->len; i++) {
        hash = hash * 31 + cache_path->data[i];
    }
    for (i = 0; i < key->len; i++) {
        hash = hash * 31 + key->data[i];
    }

    return hash;
}

static ngx_http_cache_purge_queue_item_t *
ngx_http_cache_purge_find_duplicate(ngx_http_cache_purge_queue_t *queue,
    ngx_uint_t mode, ngx_uint_t hash, ngx_str_t *cache_path, ngx_str_t *key)
{
    ngx_http_cache_purge_queue_item_t *item;

    for (item = queue->buckets[hash & (queue->nbuckets - 1)];
         item != NULL;
         item = item->hnext)
    {
        /*
         * Two-step check: hash first (fast), then full string comparison.
         * Comparing only the hash is insufficient because a 32-bit
         * multiplier hash will collide for distinct keys in large caches,
         * causing legitimate purge requests to be silently discarded.
         */
        if (item->hash != hash || item->mode != mode) {
            continue;
        }

        if (item->cache_path.len != cache_path->len
            || ngx_memcmp(item->cache_path.data, cache_path->data,
                          cache_path->len) != 0)
        {
            continue;
        }

        if (item->key_partial.len != key->len) {
            continue;
        }

        if (key->len > 0
            && ngx_memcmp(item->key_partial.data, key->data, key->len) != 0)
        {
            continue;
        }

        return item;
    }

    return NULL;
}


/* -- directive callbacks ------------------------------------------------ */

char *
ngx_http_cache_purge_queue_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_main_conf_t *cmcf  = conf;
    ngx_str_t                        *value = cf->args->elts;

    if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0) {
        cmcf->background_purge = 1;
    } else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0) {
        cmcf->background_purge = 0;
    } else {
        return "invalid value, use 'on' or 'off'";
    }

    return NGX_CONF_OK;
}

char *
ngx_http_cache_purge_legacy_status_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_main_conf_t *cmcf  = conf;
    ngx_str_t                        *value = cf->args->elts;

    if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0) {
        cmcf->legacy_status_codes = 1;  /* 412 Precondition Failed */
    } else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0) {
        cmcf->legacy_status_codes = 0;  /* 404 Not Found           */
    } else {
        return "invalid value, use 'on' or 'off'";
    }

    return NGX_CONF_OK;
}

char *
ngx_http_cache_purge_vary_aware_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_main_conf_t *cmcf  = conf;
    ngx_str_t                        *value = cf->args->elts;

    if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0) {
        cmcf->vary_aware = 1;
    } else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0) {
        cmcf->vary_aware = 0;
    } else {
        return "invalid value, use 'on' or 'off'";
    }

    return NGX_CONF_OK;
}


/* -- file-walk helpers -------------------------------------------------- */

/*
 * ngx_http_cache_purge_invalidate_node -- clear a cache node's shared-memory
 * metadata (exists, fs_size, the zone's size) for the cache file at path.
 *
 * The last 32 characters of a cache file path are the hex-encoded 16-byte
 * cache key, which is what the zone's rbtree is keyed on (see
 * ngx_http_cache_purge_lookup_node) -- for primary entries and Vary variants
 * alike.
 *
 * Locking: the zone's shpool->mutex is held for the lookup and the field
 * updates only; the caller deletes the file after this returns.
 */
static void
ngx_http_cache_purge_invalidate_node(ngx_http_file_cache_t *cache,
    ngx_str_t *path)
{
    ngx_http_file_cache_node_t  *fcn;
    u_char                       key16[NGX_HTTP_CACHE_KEY_LEN];

    if (path->len < 2 * NGX_HTTP_CACHE_KEY_LEN
        || ngx_http_cache_purge_unhex(path->data + path->len
                                      - 2 * NGX_HTTP_CACHE_KEY_LEN, key16)
           != NGX_OK)
    {
        return;
    }

    ngx_shmtx_lock(&cache->shpool->mutex);

    fcn = ngx_http_cache_purge_lookup_node(cache, key16);

    if (fcn != NULL && fcn->exists) {
#if (nginx_version >= 1000001)
        cache->sh->size -= fcn->fs_size;
        fcn->fs_size     = 0;
#else
        cache->sh->size -= (fcn->length + cache->bsize - 1) / cache->bsize;
        fcn->length       = 0;
#endif
        fcn->exists = 0;
    }

    ngx_shmtx_unlock(&cache->shpool->mutex);
}

/*
 * Exact-match walk handler (vary-aware).
 *
 * Reads (key_len + 1) bytes from the KEY: region of each cache file.
 * The file is deleted only when:
 *   - the first key_len bytes match key_partial exactly (case-insensitive), AND
 *   - the byte at position key_len is '\n' (exact-length confirmation)
 *
 * The '\n' check prevents false matches against keys that share a common prefix.
 * Because all Vary variants of a cached response store the same KEY: string,
 * this walk removes every variant file regardless of its filesystem path.
 *
 * The primary file was already deleted by ngx_http_file_cache_purge() which
 * correctly updated its rbtree node.  For each VARIANT file this handler finds,
 * it calls ngx_http_cache_purge_invalidate_node() to clear the variant's own
 * rbtree node metadata BEFORE calling ngx_delete_file(), so that:
 *   - cache->sh->size remains accurate (no phantom disk-space accounting)
 *   - the node's exists flag is cleared (no stale HIT responses)
 *
 * If the primary file appears again during the walk (its node was already
 * cleared), ngx_delete_file() returns ENOENT which is silently ignored.
 * invalidate_node() on an already-cleared node is a no-op (fcn->exists == 0).
 */
static ngx_int_t
ngx_http_purge_file_cache_delete_exact_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path)
{
    ngx_http_cache_purge_walk_ctx_t *wctx;
    ngx_file_t                       file;
    ngx_int_t                        n;

    wctx = ctx->data;
    wctx->files_checked++;

    /* key_len == 0 or buffer too small to hold key + '\n' terminator: skip */
    if (wctx->key_len == 0
        || wctx->key_len + 1 >= NGX_CACHE_PURGE_KEY_MAX_LEN)
    {
        return NGX_OK;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (file.fd == NGX_INVALID_FILE) {
        return NGX_OK;
    }
    file.log = ctx->log;

    /* Read key_len + 1 bytes: the key string followed by its '\n' terminator */
    n = ngx_read_file(&file, wctx->key_buffer, wctx->key_len + 1,
                      sizeof(ngx_http_file_cache_header_t)
                      + NGX_CACHE_PURGE_KEY_HDR_OFFSET);
    ngx_close_file(file.fd);

    if (n != (ngx_int_t)(wctx->key_len + 1)) {
        return NGX_OK;
    }

    /* Exact-length check: the next byte must be '\n' */
    if (wctx->key_buffer[wctx->key_len] != '\n') {
        return NGX_OK;
    }

    if (ngx_strncasecmp(wctx->key_buffer, wctx->key_partial,
                        wctx->key_len) != 0)
    {
        return NGX_OK;
    }

    /*
     * Key confirmed.  Update the rbtree node's shared-memory metadata
     * BEFORE deleting the file.  This keeps cache->sh->size accurate and
     * prevents subsequent requests from getting a stale HIT on a missing file.
     */
    if (wctx->cache != NULL) {
        ngx_http_cache_purge_invalidate_node(wctx->cache, path);
    }

    if (ngx_delete_file(path->data) == NGX_FILE_ERROR) {
        /* ENOENT: primary file was already deleted -- not an error */
        if (ngx_errno != NGX_ENOENT) {
            ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                          "ngx_cache_purge: could not delete \"%V\"", path);
        }
    } else {
        wctx->files_deleted++;
    }

    return NGX_OK;
}

/*
 * Walk the cache directory and delete all files whose KEY: string matches
 * the purged key exactly.  Called after a successful ngx_http_file_cache_purge()
 * when cache_purge_vary_aware is on.
 */
static void
ngx_http_cache_purge_delete_variants(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache)
{
    ngx_http_cache_purge_walk_ctx_t  ctx;
    ngx_tree_ctx_t                   tree;
    ngx_str_t                       *key;

    key = r->cache->keys.elts;

    if (key[0].len == 0) {
        return;
    }

    ngx_memzero(&ctx,  sizeof(ngx_http_cache_purge_walk_ctx_t));
    ngx_memzero(&tree, sizeof(ngx_tree_ctx_t));

    ctx.key_partial = key[0].data;
    ctx.key_len     = key[0].len;
    ctx.cache       = cache;   /* enables shm metadata updates in the walk */

    tree.file_handler      = ngx_http_purge_file_cache_delete_exact_file;
    tree.pre_tree_handler  = ngx_http_purge_file_cache_noop;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler      = ngx_http_purge_file_cache_noop;
    tree.data              = &ctx;
    tree.log               = ngx_cycle->log;

    ngx_walk_tree(&tree, &cache->path->name);

    if (ctx.files_deleted > 0) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "ngx_cache_purge: vary-aware walk deleted %ui variant(s) "
                       "for key \"%V\"", ctx.files_deleted, &key[0]);
    }
}

static ngx_int_t
ngx_http_purge_file_cache_noop(ngx_tree_ctx_t *ctx, ngx_str_t *path)
{
    (void) ctx;
    (void) path;
    return NGX_OK;
}

/* -- cache directory scanner -------------------------------------------- */

# if (NGX_HTTP_FASTCGI)
extern ngx_module_t  ngx_http_fastcgi_module;
# endif
# if (NGX_HTTP_PROXY)
extern ngx_module_t  ngx_http_proxy_module;
# endif
# if (NGX_HTTP_SCGI)
extern ngx_module_t  ngx_http_scgi_module;
# endif
# if (NGX_HTTP_UWSGI)
extern ngx_module_t  ngx_http_uwsgi_module;
# endif

/* Is zone a proxy/fastcgi/scgi/uwsgi file cache?  Returns it, or NULL. */
static ngx_http_file_cache_t *
ngx_http_cache_purge_zone_cache(ngx_shm_zone_t *zone)
{
    void  *tag;

    tag = zone->tag;

    if (tag == NULL || zone->data == NULL) {
        return NULL;
    }

    if (0
# if (NGX_HTTP_FASTCGI)
        || tag == &ngx_http_fastcgi_module
# endif
# if (NGX_HTTP_PROXY)
        || tag == &ngx_http_proxy_module
# endif
# if (NGX_HTTP_SCGI)
        || tag == &ngx_http_scgi_module
# endif
# if (NGX_HTTP_UWSGI)
        || tag == &ngx_http_uwsgi_module
# endif
       )
    {
        return zone->data;
    }

    return NULL;
}

/*
 * Find the file cache that owns cache_path, so a background walk can keep the
 * zone's shared-memory accounting right.  The queue stores only the path:
 * it survives reloads, and a pointer into the old cycle would not.
 */
static ngx_http_file_cache_t *
ngx_http_cache_purge_find_cache(ngx_cycle_t *cycle, ngx_str_t *cache_path)
{
    ngx_list_part_t        *part;
    ngx_shm_zone_t         *zone;
    ngx_http_file_cache_t  *cache;
    ngx_uint_t              i;

    part = &cycle->shared_memory.part;
    zone = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            zone = part->elts;
            i = 0;
        }

        cache = ngx_http_cache_purge_zone_cache(&zone[i]);

        if (cache != NULL
            && cache->path != NULL
            && cache->path->name.len == cache_path->len
            && ngx_strncmp(cache->path->name.data, cache_path->data,
                           cache_path->len) == 0)
        {
            return cache;
        }
    }

    return NULL;
}

static int ngx_libc_cdecl
ngx_http_cache_purge_pattern_cmp(const void *one, const void *two)
{
    const ngx_str_t  *a = one;
    const ngx_str_t  *b = two;
    ngx_int_t         rc;

    rc = ngx_memn2cmp(a->data, b->data, a->len, b->len);

    return (rc < 0) ? -1 : (rc > 0);
}

/*
 * Load the prefixes to match.  Lowercases in place (matching has always been
 * case-insensitive), sorts, and drops every pattern that another pattern is a
 * prefix of: "/cdn/a/" already covers "/cdn/a/b/".  In a prefix-free sorted
 * set the only pattern that can be a prefix of a key is the greatest one
 * that is <= the key, so matching is one binary search per file however many
 * purges were coalesced.  An empty pattern (bare "*", purge_all) matches
 * every key and makes reading the keys unnecessary.
 */
static ngx_int_t
ngx_http_cache_purge_patterns_init(ngx_http_cache_purge_patterns_t *p,
    ngx_str_t *patterns, ngx_uint_t n, ngx_log_t *log)
{
    ngx_uint_t  i, j;

    p->match_all = 0;
    p->read_len  = 0;

    for (i = 0, j = 0; i < n; i++) {

        if (patterns[i].len == 0) {
            p->match_all = 1;
            continue;
        }

        if (patterns[i].len >= NGX_CACHE_PURGE_KEY_MAX_LEN) {
            ngx_log_error(NGX_LOG_WARN, log, 0,
                          "ngx_cache_purge: key too long (%uz bytes), "
                          "skipping \"%V\"", patterns[i].len, &patterns[i]);
            continue;
        }

        ngx_strlow(patterns[i].data, patterns[i].data, patterns[i].len);
        patterns[j++] = patterns[i];
    }

    n = j;

    if (n > 1) {
        ngx_qsort(patterns, n, sizeof(ngx_str_t),
                  ngx_http_cache_purge_pattern_cmp);

        /* sorted: a pattern that covers others comes right before them */
        for (i = 1, j = 0; i < n; i++) {
            if (patterns[i].len >= patterns[j].len
                && ngx_memcmp(patterns[i].data, patterns[j].data,
                              patterns[j].len) == 0)
            {
                continue;
            }
            patterns[++j] = patterns[i];
        }
        n = j + 1;
    }

    for (i = 0; i < n; i++) {
        if (patterns[i].len > p->read_len) {
            p->read_len = patterns[i].len;
        }
    }

    p->elts  = patterns;
    p->nelts = n;

    return (p->match_all || n > 0) ? NGX_OK : NGX_DECLINED;
}

/* Is one of the patterns a prefix of key (already lowercased)? */
static ngx_flag_t
ngx_http_cache_purge_patterns_match(ngx_http_cache_purge_patterns_t *p,
    u_char *key, size_t len)
{
    ngx_str_t   *pat;
    ngx_uint_t   lo, hi, mid;

    if (p->match_all) {
        return 1;
    }

    /* greatest pattern <= key */
    lo = 0;
    hi = p->nelts;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        pat = &p->elts[mid];

        if (ngx_memn2cmp(pat->data, key, pat->len, len) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo == 0) {
        return 0;
    }

    pat = &p->elts[lo - 1];

    return len >= pat->len && ngx_memcmp(pat->data, key, pat->len) == 0;
}

static ngx_int_t
ngx_http_cache_purge_scan_open(ngx_http_cache_purge_scan_t *s,
    ngx_str_t *root)
{
    DIR  *dir;

    /* root->data is NUL-terminated: cache paths from the configuration and
     * the copies made by enqueue both are */
    dir = opendir((const char *) root->data);
    if (dir == NULL) {
        if (ngx_errno == NGX_ENOENT) {
            return NGX_DECLINED;        /* levels= directories come lazily */
        }
        ngx_log_error(NGX_LOG_CRIT, s->log, ngx_errno,
                      "ngx_cache_purge: opendir() \"%V\" failed", root);
        return NGX_ERROR;
    }

    s->dirs[0] = dir;
    s->depth   = 1;

    return NGX_OK;
}

static void
ngx_http_cache_purge_scan_close(ngx_http_cache_purge_scan_t *s)
{
    while (s->depth > 0) {
        (void) closedir(s->dirs[--s->depth]);
    }
}

/*
 * Advance to the next cache file.  NGX_OK: *dir_fd / *name name it (valid
 * until the next call).  NGX_DONE: the tree is exhausted.
 */
static ngx_int_t
ngx_http_cache_purge_scan_next(ngx_http_cache_purge_scan_t *s, int *dir_fd,
    ngx_str_t *name)
{
    DIR            *dir, *sub;
    struct dirent  *de;
    struct stat     st;
    unsigned char   type;
    int             fd;

    while (s->depth > 0) {

        dir     = s->dirs[s->depth - 1];
        *dir_fd = dirfd(dir);

        de = readdir(dir);

        if (de == NULL) {
            (void) closedir(dir);
            s->depth--;
            continue;
        }

        /* ".", ".." and nothing nginx creates */
        if (de->d_name[0] == '.') {
            continue;
        }

        type = de->d_type;

        if (type == DT_UNKNOWN) {
            if (fstatat(*dir_fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == -1)
            {
                continue;
            }
            type = S_ISDIR(st.st_mode) ? DT_DIR
                 : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
        }

        if (type == DT_DIR) {
            if (s->depth == NGX_CACHE_PURGE_WALK_DEPTH) {
                continue;
            }

            fd = openat(*dir_fd, de->d_name,
                        O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if (fd == -1) {
                if (ngx_errno != NGX_ENOENT) {
                    s->errors++;
                    ngx_log_error(NGX_LOG_ERR, s->log, ngx_errno,
                                  "ngx_cache_purge: openat() \"%s\" failed",
                                  de->d_name);
                }
                continue;
            }

            sub = fdopendir(fd);
            if (sub == NULL) {
                s->errors++;
                (void) close(fd);
                continue;
            }

            s->dirs[s->depth++] = sub;
            continue;
        }

        name->data = (u_char *) de->d_name;
        name->len  = ngx_strlen(de->d_name);

        /* cache files are named by their 32 hex digit md5 key; anything
         * else, notably "<md5>.<n>" temp files being written, is left be */
        if (type != DT_REG || name->len != 2 * NGX_HTTP_CACHE_KEY_LEN) {
            continue;
        }

        return NGX_OK;
    }

    return NGX_DONE;
}

/*
 * Read up to size bytes of the KEY: line of dir_fd/name into buf.  Returns
 * the byte count read (the key may be followed by "\n" and headers), or -1.
 */
static int
ngx_http_cache_purge_open_file(int dir_fd, const char *name)
{
    static int   open_flags = O_RDONLY|O_CLOEXEC
#  ifdef O_NOATIME
                              |O_NOATIME
#  endif
                              ;
    int          fd;

    fd = openat(dir_fd, name, open_flags);

#  ifdef O_NOATIME
    /* O_NOATIME needs the file's owner; fall back once and remember */
    if (fd == -1 && ngx_errno == NGX_EPERM && (open_flags & O_NOATIME)) {
        open_flags &= ~O_NOATIME;
        fd = openat(dir_fd, name, open_flags);
    }
#  endif

    return fd;
}

#define NGX_CACHE_PURGE_KEY_OFFSET                                            \
    (sizeof(ngx_http_file_cache_header_t) + NGX_CACHE_PURGE_KEY_HDR_OFFSET)

static ssize_t
ngx_http_cache_purge_read_key(int dir_fd, const char *name, u_char *buf,
    size_t size)
{
    ssize_t  n;
    int      fd;

    fd = ngx_http_cache_purge_open_file(dir_fd, name);
    if (fd == -1) {
        return -1;
    }

    n = pread(fd, buf, size, NGX_CACHE_PURGE_KEY_OFFSET);
    (void) close(fd);

    return n;
}

/*
 * Walk (or continue walking) the tree, deleting the files whose key matches.
 * Returns NGX_OK when the walk is complete and NGX_AGAIN when budget (ms,
 * 0 = none) ran out first.
 */
static ngx_int_t
ngx_http_cache_purge_scan(ngx_http_cache_purge_scan_t *s, ngx_msec_t budget)
{
    ngx_str_t   name;
    ngx_uint_t  visited;
    ngx_msec_t  start;
    ngx_flag_t  match;
    ssize_t     n;
    int         dir_fd;

    start   = ngx_current_msec;
    visited = 0;

    while (ngx_http_cache_purge_scan_next(s, &dir_fd, &name) == NGX_OK) {

        s->files_checked++;

        if (s->pats.match_all) {
            match = 1;

        } else {
            n = ngx_http_cache_purge_read_key(dir_fd, (char *) name.data,
                                              s->buf, s->pats.read_len);
            if (n > 0) {
                ngx_strlow(s->buf, s->buf, (size_t) n);
                match = ngx_http_cache_purge_patterns_match(&s->pats, s->buf,
                                                            (size_t) n);
            } else {
                match = 0;
            }
        }

        if (match) {

            /* clear the shm node first: no HIT on a vanished file, and the
             * zone's size stays right for the cache manager */
            if (s->cache != NULL) {
                ngx_http_cache_purge_invalidate_node(s->cache, &name);
            }

            if (unlinkat(dir_fd, (char *) name.data, 0) == -1) {
                if (ngx_errno != NGX_ENOENT) {
                    ngx_log_error(NGX_LOG_CRIT, s->log, ngx_errno,
                                  "ngx_cache_purge: could not delete "
                                  "\"%V\"", &name);
                }
            } else {
                s->files_deleted++;
            }
        }

        if (budget != 0
            && ++visited % NGX_CACHE_PURGE_WALK_CLOCK_EVERY == 0)
        {
            ngx_time_update();

            if (ngx_current_msec - start >= budget) {
                return NGX_AGAIN;
            }
        }
    }

    return NGX_OK;
}

/* A whole walk, synchronously, for one wildcard (or purge_all when pattern
 * is empty).  Returns the number of files deleted. */
static ngx_uint_t
ngx_http_cache_purge_scan_sync(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *pattern)
{
    ngx_http_cache_purge_scan_t  s;
    ngx_str_t                    pat;

    ngx_memzero(&s, sizeof(ngx_http_cache_purge_scan_t));
    s.log   = r->connection->log;
    s.cache = cache;

    /* lowercased in place by patterns_init: work on a copy of the key */
    pat.len  = pattern->len;
    pat.data = ngx_pnalloc(r->pool, pattern->len + 1);
    if (pat.data == NULL) {
        return 0;
    }
    ngx_memcpy(pat.data, pattern->data, pattern->len);

    if (ngx_http_cache_purge_patterns_init(&s.pats, &pat, 1, s.log) != NGX_OK
        || ngx_http_cache_purge_scan_open(&s, &cache->path->name) != NGX_OK)
    {
        return 0;
    }

    (void) ngx_http_cache_purge_scan(&s, 0);
    ngx_http_cache_purge_scan_close(&s);

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "ngx_cache_purge: walk of \"%V\" checked %ui file(s), "
                   "deleted %ui", &cache->path->name, s.files_checked,
                   s.files_deleted);

    return s.files_deleted;
}


/* -- key index ------------------------------------------------------------ */

/*
 * cache_purge_index keeps every cache key in a shared-memory rbtree ordered
 * by (cache, lowercased key, md5), so a wildcard purge is a range lookup
 * plus exact deletes: its cost follows the files that match, not the size of
 * the cache.
 *
 * Entries are recorded by a header filter when an upstream response goes to
 * a cache (before the file is stored, so a stored file is never unindexed).
 * With Vary the file ends up under either c->key or c->main
 * (ngx_http_file_cache_update_variant), so both are recorded.  The filter
 * sees subrequests too: slices and background updates.
 *
 * Files already on disk are added by a build walk after a (re)start.  While
 * it runs, wildcards are applied to the entries indexed so far and kept in a
 * list the walk checks each file against, so nothing needs a second walk.
 * If the zone runs full the index is no longer complete: wildcards fall back
 * to walks until a reconcile + rebuild has made it whole again.
 *
 * Entries outlive their files when the cache manager expires them; a
 * periodic reconcile drops entries whose cache node is gone.
 */

static u_char  ngx_http_cache_purge_zero_md5[NGX_HTTP_CACHE_KEY_LEN];

/* The index of cmcf; under the lock it is current, unlocked only a hint */
static ngx_http_cache_purge_index_sh_t *
ngx_http_cache_purge_index_peek(ngx_http_cache_purge_main_conf_t *cmcf)
{
    return (cmcf->index_shpool != NULL) ? cmcf->index_shpool->data : NULL;
}

/* Locked (fresh or reset pool) */
static ngx_http_cache_purge_index_sh_t *
ngx_http_cache_purge_index_create(ngx_slab_pool_t *shpool,
    ngx_uint_t generation)
{
    ngx_http_cache_purge_index_sh_t  *sh;

    sh = ngx_slab_calloc_locked(shpool,
                                sizeof(ngx_http_cache_purge_index_sh_t));
    if (sh == NULL) {
        return NULL;
    }

    ngx_rbtree_init(&sh->rbtree, &sh->sentinel,
                    ngx_http_cache_purge_index_rbtree_insert);
    ngx_rbtree_init(&sh->build, &sh->build_sentinel,
                    ngx_http_cache_purge_index_rbtree_insert);

    sh->state      = NGX_CACHE_PURGE_INDEX_PENDING;
    sh->generation = generation;

    shpool->data = sh;

    /* running full is reported by the module, once */
    shpool->log_nomem = 0;

    return sh;
}

/*
 * Lock the index.  Its busy flag, unlike the queue's, is raised only while
 * the tree or the slab pool is being changed (index_busy): a process that
 * dies during one of the many read-only lookups -- most purges match
 * nothing, most fills are already indexed -- leaves nothing to repair, and
 * a reset at production size means minutes of rebuild.  A flag found up
 * means the holder died mid-change; see queue_lock.  A reset leaves an
 * empty index in PENDING: wildcards walk until the drainer has rebuilt it.
 * Never returns NULL in practice: a wiped pool always has room for the root.
 */
static ngx_http_cache_purge_index_sh_t *
ngx_http_cache_purge_index_lock(ngx_http_cache_purge_main_conf_t *cmcf)
{
    ngx_slab_pool_t                  *shpool = cmcf->index_shpool;
    ngx_http_cache_purge_index_sh_t  *sh;
    ngx_uint_t                        generation;

    ngx_shmtx_lock(&shpool->mutex);

    sh = shpool->data;

    if (sh == NULL || sh->busy) {
        generation = (sh != NULL) ? sh->generation + 1 : 1;

        if (sh != NULL) {
            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                          "ngx_cache_purge: a process died while updating the "
                          "key index; resetting it (it is rebuilt from the "
                          "cache directories)");
        }

        ngx_slab_init(shpool);
        shpool->data = NULL;
        sh = ngx_http_cache_purge_index_create(shpool, generation);
    }

    return sh;
}

/*
 * Around every change of the index tree or slab pool.  Locked.  Kept out of
 * line so that a debugger can stop a process exactly inside a change: the
 * crash-recovery tests kill it there.
 */
#if (NGX_HAVE_GCC_ATTRIBUTE_ALIGNED) || defined(__GNUC__)
__attribute__((noinline))
#endif
static void
ngx_http_cache_purge_index_busy(ngx_http_cache_purge_index_sh_t *sh,
    ngx_uint_t busy)
{
    ngx_memory_barrier();
    sh->busy = busy;
    ngx_memory_barrier();
}

static void
ngx_http_cache_purge_index_unlock(ngx_http_cache_purge_main_conf_t *cmcf)
{
    ngx_http_cache_purge_index_sh_t  *sh = cmcf->index_shpool->data;

    if (sh != NULL) {
        sh->busy = 0;
    }

    ngx_shmtx_unlock(&cmcf->index_shpool->mutex);
}

static ngx_int_t
ngx_http_cache_purge_index_cmp(ngx_rbtree_key_t id, u_char *key, size_t len,
    u_char *md5, ngx_http_cache_purge_index_node_t *n)
{
    ngx_int_t  rc;

    if (id != n->node.key) {
        return (id < n->node.key) ? -1 : 1;
    }

    rc = ngx_memn2cmp(key, n->key, len, n->len);
    if (rc != 0) {
        return rc;
    }

    return ngx_memcmp(md5, n->md5, NGX_HTTP_CACHE_KEY_LEN);
}

static void
ngx_http_cache_purge_index_rbtree_insert(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_http_cache_purge_index_node_t  *n;
    ngx_rbtree_node_t                 **p;

    n = (ngx_http_cache_purge_index_node_t *) node;

    for ( ;; ) {

        p = (ngx_http_cache_purge_index_cmp(node->key, n->key, n->len, n->md5,
                 (ngx_http_cache_purge_index_node_t *) temp) < 0)
            ? &temp->left : &temp->right;

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}

/* First entry >= (id, key, md5), or > it when after is set.  Locked. */
static ngx_http_cache_purge_index_node_t *
ngx_http_cache_purge_index_bound(ngx_rbtree_t *tree,
    ngx_rbtree_key_t id, u_char *key, size_t len, u_char *md5,
    ngx_flag_t after)
{
    ngx_rbtree_node_t  *node, *sentinel, *best;
    ngx_int_t           rc;

    node     = tree->root;
    sentinel = tree->sentinel;
    best     = NULL;

    while (node != sentinel) {

        rc = ngx_http_cache_purge_index_cmp(id, key, len, md5,
                 (ngx_http_cache_purge_index_node_t *) node);

        if (rc < 0 || (rc == 0 && !after)) {
            best = node;
            node = node->left;

        } else {
            node = node->right;
        }
    }

    return (ngx_http_cache_purge_index_node_t *) best;
}

static ngx_flag_t
ngx_http_cache_purge_index_in_range(ngx_http_cache_purge_index_node_t *n,
    ngx_rbtree_key_t id, ngx_str_t *prefix)
{
    return n != NULL
           && n->node.key == id
           && n->len >= prefix->len
           && ngx_memcmp(n->key, prefix->data, prefix->len) == 0;
}

/*
 * The index can no longer vouch for every file (an entry could not be
 * stored): stop using it for wildcards.  Locked.
 */
static void
ngx_http_cache_purge_index_lost(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_log_t *log, const char *why)
{
    ngx_http_cache_purge_index_sh_t  *sh = ngx_http_cache_purge_index_peek(cmcf);

    if (sh->state == NGX_CACHE_PURGE_INDEX_READY) {
        sh->state = NGX_CACHE_PURGE_INDEX_REBUILD;
        ngx_log_error(NGX_LOG_CRIT, log, 0,
                      "ngx_cache_purge: key index incomplete: %s "
                      "(%ui entries); wildcard purges walk the cache until "
                      "it has been reconciled and rebuilt", why,
                      sh->entries);

    } else if (sh->state == NGX_CACHE_PURGE_INDEX_BUILDING) {
        sh->state     = NGX_CACHE_PURGE_INDEX_FAILED;
        sh->failed_at = ngx_current_msec;
        ngx_log_error(NGX_LOG_CRIT, log, 0,
                      "ngx_cache_purge: key index build failed: %s "
                      "(%ui entries); wildcard purges walk the cache, and "
                      "the build is retried every cache_purge_index_reconcile",
                      why, sh->entries);
    }

    /* PENDING and REBUILD walk anyway, and the coming build adds the file */
}

/* Id of a cache path in the index (created if asked).  Locked. */
static ngx_int_t
ngx_http_cache_purge_index_cache_id(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_str_t *path, ngx_flag_t create)
{
    ngx_http_cache_purge_index_sh_t  *sh = ngx_http_cache_purge_index_peek(cmcf);
    ngx_uint_t                        i;
    u_char                           *p;

    for (i = 0; i < sh->npaths; i++) {
        if (sh->paths[i].len == path->len
            && ngx_memcmp(sh->paths[i].data, path->data, path->len) == 0)
        {
            return (ngx_int_t) i;
        }
    }

    if (!create || sh->npaths == NGX_CACHE_PURGE_INDEX_PATHS) {
        return NGX_ERROR;
    }

    ngx_http_cache_purge_index_busy(sh, 1);

    p = ngx_slab_alloc_locked(cmcf->index_shpool, path->len);
    if (p == NULL) {
        ngx_http_cache_purge_index_busy(sh, 0);
        return NGX_ERROR;
    }

    ngx_memcpy(p, path->data, path->len);
    sh->paths[sh->npaths].data = p;
    sh->paths[sh->npaths].len  = path->len;
    sh->npaths++;

    ngx_http_cache_purge_index_busy(sh, 0);

    return (ngx_int_t) sh->npaths - 1;
}

/* NGX_OK added, NGX_DECLINED already there, NGX_ERROR no memory.  Locked. */
static ngx_int_t
ngx_http_cache_purge_index_insert(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_rbtree_key_t id, u_char *key, size_t len, u_char *md5)
{
    ngx_http_cache_purge_index_sh_t    *sh = ngx_http_cache_purge_index_peek(cmcf);
    ngx_http_cache_purge_index_node_t  *n;

    n = ngx_http_cache_purge_index_bound(&sh->rbtree, id, key, len, md5, 0);

    if (n != NULL && ngx_http_cache_purge_index_cmp(id, key, len, md5, n) == 0)
    {
        return NGX_DECLINED;
    }

    ngx_http_cache_purge_index_busy(sh, 1);

    n = ngx_slab_alloc_locked(cmcf->index_shpool,
                    offsetof(ngx_http_cache_purge_index_node_t, key) + len);
    if (n == NULL) {
        ngx_http_cache_purge_index_busy(sh, 0);
        return NGX_ERROR;
    }

    n->node.key = id;
    ngx_memcpy(n->md5, md5, NGX_HTTP_CACHE_KEY_LEN);
    n->len = (u_short) len;
    ngx_memcpy(n->key, key, len);

    ngx_rbtree_insert(&sh->rbtree, &n->node);
    sh->entries++;

    ngx_http_cache_purge_index_busy(sh, 0);

    return NGX_OK;
}

static void
ngx_http_cache_purge_index_remove(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_http_cache_purge_index_node_t *n)
{
    ngx_http_cache_purge_index_sh_t  *sh = ngx_http_cache_purge_index_peek(cmcf);

    ngx_http_cache_purge_index_busy(sh, 1);
    ngx_rbtree_delete(&sh->rbtree, &n->node);
    ngx_slab_free_locked(cmcf->index_shpool, n);
    sh->entries--;
    ngx_http_cache_purge_index_busy(sh, 0);
}

/*
 * An entry this request added, for pending_cleanup.  The header filter
 * records every upstream response that may be stored -- whether it will be
 * is only decided afterwards -- so entries of responses that were not stored
 * are taken out again when the request ends.  Otherwise a flood of uncachable
 * misses (errors, no_cache, random URLs) would fill the index with keys that
 * have no file.
 */
typedef struct {
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_file_cache_t             *cache;
    ngx_uint_t                         generation;
    ngx_rbtree_key_t                   id;
    ngx_uint_t                         n;           /* md5s: 1, or 2 (Vary) */
    u_char                             created[2];  /* added by this request */
    ngx_flag_t                         failed;      /* could not be added */
    u_char                             md5[2][NGX_HTTP_CACHE_KEY_LEN];
    size_t                             len;
    u_char                             key[1];
} ngx_http_cache_purge_index_pending_t;

/*
 * Request end, when the response is stored (or not) for sure.
 *
 * Entries this request added are dropped unless a file backs them or may
 * still come: the zone's node exists, is in use or is being updated -- the
 * test reconcile applies.  Another request storing the same key holds its
 * node, so an entry it relies on is never dropped here.
 *
 * And the other way round: if the index was reset while the request ran, or
 * its entry could not be added, a rebuild walk may already have passed the
 * directory before the file was renamed into it.  A stored file is added
 * again here, so it is never left unindexed.
 */
static void
ngx_http_cache_purge_index_pending_cleanup(void *data)
{
    ngx_http_cache_purge_index_pending_t  *p = data;
    ngx_http_cache_purge_index_sh_t       *sh;
    ngx_http_cache_purge_index_node_t     *n;
    ngx_http_file_cache_node_t            *fcn;
    ngx_flag_t                             stored, backed, same;
    ngx_int_t                              id;
    ngx_uint_t                             i;

    sh = ngx_http_cache_purge_index_lock(p->cmcf);

    if (sh == NULL) {
        ngx_http_cache_purge_index_unlock(p->cmcf);
        return;
    }

    same = (sh->generation == p->generation);

    for (i = 0; i < p->n; i++) {

        if (same && !p->created[i] && !p->failed) {
            continue;
        }

        ngx_shmtx_lock(&p->cache->shpool->mutex);

        fcn    = ngx_http_cache_purge_lookup_node(p->cache, p->md5[i]);
        stored = (fcn != NULL && fcn->exists);
        backed = (fcn != NULL
                  && (fcn->exists || fcn->count != 0 || fcn->updating));

        ngx_shmtx_unlock(&p->cache->shpool->mutex);

        if (same && p->created[i] && !backed) {

            n = ngx_http_cache_purge_index_bound(&sh->rbtree, p->id, p->key, p->len,
                                                 p->md5[i], 0);
            if (n != NULL
                && ngx_http_cache_purge_index_cmp(p->id, p->key, p->len,
                                                  p->md5[i], n) == 0)
            {
                ngx_http_cache_purge_index_remove(p->cmcf, n);
            }

            continue;
        }

        if ((!same || p->failed) && stored) {

            id = ngx_http_cache_purge_index_cache_id(p->cmcf,
                                                     &p->cache->path->name, 1);

            if (id == NGX_ERROR
                || ngx_http_cache_purge_index_insert(p->cmcf,
                                                     (ngx_rbtree_key_t) id,
                                                     p->key, p->len,
                                                     p->md5[i])
                   == NGX_ERROR)
            {
                ngx_http_cache_purge_index_lost(p->cmcf, ngx_cycle->log,
                    "cache_purge_index zone is full, increase "
                    "cache_purge_index");
            }
        }
    }

    ngx_http_cache_purge_index_unlock(p->cmcf);
}

/* Header filter: record the key of a response that is going to a cache */
static void
ngx_http_cache_purge_index_record(ngx_http_request_t *r,
    ngx_http_cache_purge_main_conf_t *cmcf)
{
    ngx_http_cache_purge_index_pending_t  *p;
    ngx_http_cache_purge_index_sh_t       *sh;
    ngx_http_cache_t                      *c;
    ngx_pool_cleanup_t                    *cln;
    ngx_str_t                             *key;
    ngx_uint_t                             i;
    ngx_int_t                              id, rc;
    size_t                                 len, n;
    u_char                                 buf[NGX_CACHE_PURGE_KEY_MAX_LEN];

    c   = r->cache;
    key = c->keys.elts;
    len = 0;

    /* the KEY: line of the cache file, truncated like the index keeps it:
     * a truncated key still matches every prefix a purge may carry */
    for (i = 0; i < c->keys.nelts; i++) {
        n = ngx_min(key[i].len, NGX_CACHE_PURGE_KEY_MAX_LEN - 1 - len);
        ngx_memcpy(buf + len, key[i].data, n);
        len += n;
    }

    ngx_strlow(buf, buf, len);

    /* r->pool is the main request's pool, for subrequests too */
    p = ngx_pcalloc(r->pool,
                    offsetof(ngx_http_cache_purge_index_pending_t, key) + len);
    cln = ngx_pool_cleanup_add(r->pool, 0);

    if (p == NULL || cln == NULL) {
        return;
    }

    p->cmcf  = cmcf;
    p->cache = c->file_cache;
    p->len   = len;
    ngx_memcpy(p->key, buf, len);

    ngx_memcpy(p->md5[p->n++], c->key, NGX_HTTP_CACHE_KEY_LEN);
    if (ngx_memcmp(c->key, c->main, NGX_HTTP_CACHE_KEY_LEN) != 0) {
        ngx_memcpy(p->md5[p->n++], c->main, NGX_HTTP_CACHE_KEY_LEN);
    }

    sh = ngx_http_cache_purge_index_lock(cmcf);

    if (sh != NULL) {
        p->generation = sh->generation;

        id = ngx_http_cache_purge_index_cache_id(cmcf,
                                                 &c->file_cache->path->name, 1);
        if (id == NGX_ERROR) {
            p->failed = 1;

        } else {
            p->id = (ngx_rbtree_key_t) id;

            for (i = 0; i < p->n; i++) {
                rc = ngx_http_cache_purge_index_insert(cmcf, p->id, buf, len,
                                                       p->md5[i]);
                if (rc == NGX_OK) {
                    p->created[i] = 1;

                } else if (rc == NGX_ERROR) {
                    p->failed = 1;
                    break;
                }
            }
        }

        if (p->failed) {
            ngx_http_cache_purge_index_lost(cmcf, r->connection->log,
                "cache_purge_index zone is full, increase cache_purge_index");
        }
    }

    ngx_http_cache_purge_index_unlock(cmcf);

    cln->handler = ngx_http_cache_purge_index_pending_cleanup;
    cln->data    = p;
}
/*
 * Take (remove) up to max entries under prefix.  Key copies go to pool, so
 * an entry whose file is not there yet can be put back.  Locked.
 */
static ngx_uint_t
ngx_http_cache_purge_index_take(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_rbtree_key_t id, ngx_str_t *prefix,
    ngx_http_cache_purge_index_hit_t *after,
    ngx_http_cache_purge_index_hit_t *hits, ngx_uint_t max, ngx_pool_t *pool,
    ngx_flag_t *more)
{
    ngx_http_cache_purge_index_sh_t    *sh = ngx_http_cache_purge_index_peek(cmcf);
    ngx_http_cache_purge_index_node_t  *n, *next;
    ngx_uint_t                          i;

    *more = 0;

    /* from the start of the range, or strictly after the last one taken:
     * entries put back (see index_delete) are not taken again */
    n = (after != NULL)
        ? ngx_http_cache_purge_index_bound(&sh->rbtree, id, after->key.data,
                                           after->key.len, after->md5, 1)
        : ngx_http_cache_purge_index_bound(&sh->rbtree, id, prefix->data,
                                           prefix->len,
                                           ngx_http_cache_purge_zero_md5, 0);

    for (i = 0; ngx_http_cache_purge_index_in_range(n, id, prefix); i++) {

        if (i == max) {
            *more = 1;
            break;
        }

        next = (ngx_http_cache_purge_index_node_t *)
                   ngx_rbtree_next(&sh->rbtree, &n->node);

        ngx_memcpy(hits[i].md5, n->md5, NGX_HTTP_CACHE_KEY_LEN);

        hits[i].key.data = ngx_pnalloc(pool, n->len);
        hits[i].key.len  = (hits[i].key.data != NULL) ? n->len : 0;

        if (hits[i].key.data != NULL) {
            ngx_memcpy(hits[i].key.data, n->key, n->len);
        }

        ngx_http_cache_purge_index_remove(cmcf, n);

        n = next;
    }

    return i;
}

/*
 * Delete the cache file named by md5.  NGX_OK deleted, NGX_DECLINED not
 * there (expired, or still being stored), NGX_ERROR otherwise.
 */
static ngx_int_t
ngx_http_cache_purge_delete_md5(ngx_http_file_cache_t *cache, u_char *md5,
    ngx_log_t *log)
{
    ngx_str_t  path;
    size_t     len;
    u_char    *p, name[NGX_MAX_PATH];

    len = cache->path->name.len + 1 + cache->path->len
          + 2 * NGX_HTTP_CACHE_KEY_LEN;

    if (len >= NGX_MAX_PATH) {
        return NGX_ERROR;
    }

    /* the layout of ngx_http_file_cache_name() */
    p = ngx_cpymem(name, cache->path->name.data, cache->path->name.len);
    p += 1 + cache->path->len;
    p = ngx_hex_dump(p, md5, NGX_HTTP_CACHE_KEY_LEN);
    *p = '\0';

    ngx_create_hashed_filename(cache->path, name, len);

    path.data = name;
    path.len  = len;

    ngx_http_cache_purge_invalidate_node(cache, &path);

    if (ngx_delete_file(name) == NGX_FILE_ERROR) {

        if (ngx_errno == NGX_ENOENT) {
            return NGX_DECLINED;
        }

        ngx_log_error(NGX_LOG_CRIT, log, ngx_errno,
                      "ngx_cache_purge: could not delete \"%s\"", name);
        return NGX_ERROR;
    }

    return NGX_OK;
}

/* Delete the files of taken entries; returns how many were deleted */
static ngx_uint_t
ngx_http_cache_purge_index_delete(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_http_file_cache_t *cache, ngx_rbtree_key_t id, ngx_uint_t generation,
    ngx_http_cache_purge_index_hit_t *hits, ngx_uint_t n, ngx_log_t *log)
{
    ngx_http_cache_purge_index_sh_t  *sh;
    ngx_http_file_cache_node_t       *fcn;
    ngx_flag_t                        pending, stored;
    ngx_uint_t                        i, deleted;
    ngx_int_t                         rc;

    deleted = 0;

    for (i = 0; i < n; i++) {

        rc = ngx_http_cache_purge_delete_md5(cache, hits[i].md5, log);

        if (rc == NGX_OK) {
            deleted++;
            continue;
        }

        /*
         * Not there.  If a response for it is still being stored (entries
         * are recorded before the file exists; the storing request holds
         * the zone's node), put the entry back so a later purge finds the
         * file.  Otherwise the entry was stale (expired, purged exactly):
         * leave it out.
         */
        if (rc == NGX_DECLINED && hits[i].key.len > 0) {

            ngx_shmtx_lock(&cache->shpool->mutex);
            fcn = ngx_http_cache_purge_lookup_node(cache, hits[i].md5);
            stored  = (fcn != NULL && fcn->exists);
            pending = (fcn != NULL && (fcn->count != 0 || fcn->updating));
            ngx_shmtx_unlock(&cache->shpool->mutex);

            /* the store finished in between: the file is there now */
            if (stored
                && ngx_http_cache_purge_delete_md5(cache, hits[i].md5, log)
                   == NGX_OK)
            {
                deleted++;
                continue;
            }

            if (!stored && !pending) {
                continue;
            }

            sh = ngx_http_cache_purge_index_lock(cmcf);

            /* not into an index that was reset meanwhile: ids differ */
            if (sh != NULL && sh->generation == generation
                && ngx_http_cache_purge_index_insert(cmcf, id,
                                                     hits[i].key.data,
                                                     hits[i].key.len,
                                                     hits[i].md5)
                   == NGX_ERROR)
            {
                ngx_http_cache_purge_index_lost(cmcf, log,
                                            "cache_purge_index zone is full, increase cache_purge_index");
            }

            ngx_http_cache_purge_index_unlock(cmcf);
        }
    }

    return deleted;
}

/* Greatest entry <= (id, key, md5).  Locked. */
static ngx_http_cache_purge_index_node_t *
ngx_http_cache_purge_index_floor(ngx_rbtree_t *tree, ngx_rbtree_key_t id,
    u_char *key, size_t len, u_char *md5)
{
    ngx_rbtree_node_t  *node, *sentinel, *best;

    node     = tree->root;
    sentinel = tree->sentinel;
    best     = NULL;

    while (node != sentinel) {
        if (ngx_http_cache_purge_index_cmp(id, key, len, md5,
                (ngx_http_cache_purge_index_node_t *) node) >= 0)
        {
            best = node;
            node = node->right;

        } else {
            node = node->left;
        }
    }

    return (ngx_http_cache_purge_index_node_t *) best;
}

/*
 * Does a purge received during the build cover key (of cache id)?  The set
 * is prefix-free, so only the greatest entry <= key can.  Locked.
 */
static ngx_flag_t
ngx_http_cache_purge_index_build_match(ngx_http_cache_purge_index_sh_t *sh,
    ngx_rbtree_key_t id, u_char *key, size_t len)
{
    ngx_http_cache_purge_index_node_t  *n;

    if (sh->nbuild == 0) {
        return 0;
    }

    n = ngx_http_cache_purge_index_floor(&sh->build, id, key, len,
                                         ngx_http_cache_purge_zero_md5);

    return n != NULL && n->node.key == id && n->len <= len
           && ngx_memcmp(n->key, key, n->len) == 0;
}

/*
 * Remember a purge for the build walk (the files it has not reached),
 * keeping the set prefix-free: a prefix already covered is not added, one
 * that covers others replaces them.  Locked.
 */
static ngx_int_t
ngx_http_cache_purge_index_build_add(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_rbtree_key_t id, ngx_str_t *prefix)
{
    ngx_http_cache_purge_index_sh_t    *sh = ngx_http_cache_purge_index_peek(cmcf);
    ngx_http_cache_purge_index_node_t  *n, *next;

    if (ngx_http_cache_purge_index_build_match(sh, id, prefix->data,
                                               prefix->len))
    {
        return NGX_OK;
    }

    ngx_http_cache_purge_index_busy(sh, 1);

    n = ngx_http_cache_purge_index_bound(&sh->build, id, prefix->data,
                                         prefix->len,
                                         ngx_http_cache_purge_zero_md5, 0);

    while (ngx_http_cache_purge_index_in_range(n, id, prefix)) {
        next = (ngx_http_cache_purge_index_node_t *)
                   ngx_rbtree_next(&sh->build, &n->node);
        ngx_rbtree_delete(&sh->build, &n->node);
        ngx_slab_free_locked(cmcf->index_shpool, n);
        sh->nbuild--;
        n = next;
    }

    n = ngx_slab_alloc_locked(cmcf->index_shpool,
                    offsetof(ngx_http_cache_purge_index_node_t, key)
                    + prefix->len);
    if (n == NULL) {
        ngx_http_cache_purge_index_busy(sh, 0);
        return NGX_ERROR;
    }

    n->node.key = id;
    ngx_memzero(n->md5, NGX_HTTP_CACHE_KEY_LEN);
    n->len = (u_short) prefix->len;
    ngx_memcpy(n->key, prefix->data, prefix->len);

    ngx_rbtree_insert(&sh->build, &n->node);
    sh->nbuild++;

    ngx_http_cache_purge_index_busy(sh, 0);

    return NGX_OK;
}

/* Build walk finished or given up: forget its purges.  Locked. */
static void
ngx_http_cache_purge_index_build_clear(ngx_http_cache_purge_main_conf_t *cmcf)
{
    ngx_http_cache_purge_index_sh_t  *sh = ngx_http_cache_purge_index_peek(cmcf);
    ngx_rbtree_node_t                *node;

    ngx_http_cache_purge_index_busy(sh, 1);

    while (sh->build.root != sh->build.sentinel) {
        node = ngx_rbtree_min(sh->build.root, sh->build.sentinel);
        ngx_rbtree_delete(&sh->build, node);
        ngx_slab_free_locked(cmcf->index_shpool, node);
    }

    sh->nbuild = 0;

    ngx_http_cache_purge_index_busy(sh, 0);
}

/*
 * A wildcard or purge_all request, answered from the index.  NGX_DONE: the
 * request has been answered.  NGX_DECLINED: the index cannot be used right
 * now (never built, full, ...); the caller walks instead.
 *
 * Up to cache_purge_index_sync_limit matching files are deleted right away
 * and the answer is 200 (or the usual not-found code).  A purge that matches
 * more is continued in the background and answered 202, as is any purge
 * while the build walk still has files to look at.
 */
static ngx_int_t
ngx_http_cache_purge_index_purge(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all)
{
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_cache_purge_index_hit_t   hits[NGX_CACHE_PURGE_INDEX_TAKE];
    ngx_http_cache_purge_index_hit_t   cursor;
    ngx_str_t                          prefix, status;
    ngx_http_cache_purge_index_sh_t   *sh;
    ngx_uint_t                         state, n, deleted, generation, taken;
    ngx_int_t                          id, rc;
    ngx_flag_t                         more;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);

    if (cmcf == NULL || cmcf->index_shpool == NULL
        || cmcf->queue_shpool == NULL)
    {
        return NGX_DECLINED;
    }

    prefix.len = purge_all ? 0 : key->len;

    if (prefix.len > 0 && key->data[prefix.len - 1] == '*') {
        prefix.len--;
    }

    if (prefix.len >= NGX_CACHE_PURGE_KEY_MAX_LEN) {
        return NGX_DECLINED;
    }

    prefix.data = ngx_pnalloc(r->pool, prefix.len + 1);
    if (prefix.data == NULL) {
        return NGX_DECLINED;
    }

    ngx_strlow(prefix.data, key->data, prefix.len);

    sh = ngx_http_cache_purge_index_lock(cmcf);

    state      = (sh != NULL) ? sh->state : NGX_CACHE_PURGE_INDEX_FAILED;
    generation = (sh != NULL) ? sh->generation : 0;

    if (state != NGX_CACHE_PURGE_INDEX_READY
        && state != NGX_CACHE_PURGE_INDEX_BUILDING)
    {
        ngx_http_cache_purge_index_unlock(cmcf);
        return NGX_DECLINED;
    }

    /* while building, the purge must also reach the files not yet walked */
    id = ngx_http_cache_purge_index_cache_id(cmcf, &cache->path->name,
                                     state == NGX_CACHE_PURGE_INDEX_BUILDING);

    if (state == NGX_CACHE_PURGE_INDEX_BUILDING
        && (id == NGX_ERROR
            || ngx_http_cache_purge_index_build_add(cmcf,
                                                    (ngx_rbtree_key_t) id,
                                                    &prefix)
               != NGX_OK))
    {
        ngx_http_cache_purge_index_unlock(cmcf);
        return NGX_DECLINED;
    }

    /*
     * Up to sync_limit entries, taken NGX_CACHE_PURGE_INDEX_TAKE at a time:
     * a small batch on the stack (no allocation for the common purge that
     * matches a few files or none), and the lock is given up between
     * batches.  Later batches continue after the last entry taken.
     */
    taken = 0;
    deleted = 0;
    more = 0;

    n = (id == NGX_ERROR)
        ? 0
        : ngx_http_cache_purge_index_take(cmcf, (ngx_rbtree_key_t) id,
                  &prefix, NULL, hits,
                  ngx_min(NGX_CACHE_PURGE_INDEX_TAKE, cmcf->index_sync_limit),
                  r->pool, &more);

    ngx_http_cache_purge_index_unlock(cmcf);

    for ( ;; ) {
        deleted += ngx_http_cache_purge_index_delete(cmcf, cache,
                                                     (ngx_rbtree_key_t) id,
                                                     generation, hits, n,
                                                     r->connection->log);
        taken += n;

        if (!more || taken >= cmcf->index_sync_limit
            || hits[n - 1].key.len == 0)
        {
            break;
        }

        cursor = hits[n - 1];

        sh = ngx_http_cache_purge_index_lock(cmcf);

        if (sh == NULL || sh->generation != generation) {
            /* reset meanwhile: the queued remainder falls back to a walk */
            ngx_http_cache_purge_index_unlock(cmcf);
            break;
        }

        n = ngx_http_cache_purge_index_take(cmcf, (ngx_rbtree_key_t) id,
                  &prefix, &cursor, hits,
                  ngx_min(NGX_CACHE_PURGE_INDEX_TAKE,
                          cmcf->index_sync_limit - taken),
                  r->pool, &more);

        ngx_http_cache_purge_index_unlock(cmcf);

        if (n == 0) {
            break;
        }
    }

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "ngx_cache_purge: index purge \"%V\" took %ui entries, "
                   "deleted %ui file(s), more: %i", &prefix, taken, deleted,
                   more);

    r->main->count++;

    if (more) {
        rc = ngx_http_cache_purge_enqueue_raw(cmcf, r->connection->log,
                                              &cache->path->name, key,
                                              purge_all,
                                              NGX_CACHE_PURGE_MODE_INDEX);
        if (rc != NGX_OK) {
            /* partly done; a retry carries on from what is left */
            ngx_http_finalize_request(r, NGX_HTTP_TOO_MANY_REQUESTS);
            return NGX_DONE;
        }
    }

    if (more || state == NGX_CACHE_PURGE_INDEX_BUILDING) {
        r->headers_out.status = NGX_HTTP_ACCEPTED;
        ngx_str_set(&status, "queued");
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (deleted == 0) {
        ngx_http_finalize_request(r, cmcf->legacy_status_codes
                                     ? NGX_HTTP_PRECONDITION_FAILED
                                     : NGX_HTTP_NOT_FOUND);
        return NGX_DONE;
    }

    ngx_str_set(&status, "purged");
    ngx_http_finalize_request(r,
        ngx_http_cache_purge_send_response(r, &status));

    return NGX_DONE;
}

/*
 * Background part of an index purge (a queued pass in index mode): take and
 * delete matching entries batch by batch.  NGX_OK done, NGX_AGAIN budget ran
 * out, NGX_DECLINED the index became unusable (the pass walks instead).
 */
static ngx_int_t
ngx_http_cache_purge_index_pass(ngx_http_cache_purge_pass_t *pass,
    ngx_http_cache_purge_main_conf_t *cmcf, ngx_msec_t budget)
{
    ngx_http_cache_purge_patterns_t  *pats = &pass->scan.pats;
    ngx_str_t                        *prefix, all = ngx_null_string;
    ngx_uint_t                        n, npats, state;
    ngx_int_t                         id;
    ngx_flag_t                        more;
    ngx_pool_t                       *tmp;
    ngx_msec_t                        start;
    ngx_http_cache_purge_index_sh_t  *sh;
    ngx_uint_t                        generation;

    if (pass->scan.cache == NULL) {
        return NGX_DECLINED;
    }

    npats = pats->match_all ? 1 : pats->nelts;
    start = ngx_current_msec;

    while (pass->next_pattern < npats) {

        prefix = pats->match_all ? &all : &pats->elts[pass->next_pattern];

        tmp = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, pass->scan.log);
        if (tmp == NULL) {
            return NGX_AGAIN;
        }

        sh = ngx_http_cache_purge_index_lock(cmcf);

        state = (sh != NULL) ? sh->state : NGX_CACHE_PURGE_INDEX_FAILED;

        if (state != NGX_CACHE_PURGE_INDEX_READY
            && state != NGX_CACHE_PURGE_INDEX_BUILDING)
        {
            ngx_http_cache_purge_index_unlock(cmcf);
            ngx_destroy_pool(tmp);
            return NGX_DECLINED;
        }

        generation = sh->generation;

        id = ngx_http_cache_purge_index_cache_id(cmcf,
                                                 &pass->scan.cache->path->name,
                                                 0);
        n = 0;
        more = 0;

        if (id != NGX_ERROR) {
            n = ngx_http_cache_purge_index_take(cmcf, (ngx_rbtree_key_t) id,
                                                prefix,
                                                pass->has_cursor
                                                ? &pass->cursor : NULL,
                                                pass->hits,
                                                cmcf->index_sync_limit, tmp,
                                                &more);
        }

        ngx_http_cache_purge_index_unlock(cmcf);

        pass->scan.files_checked += n;
        pass->scan.files_deleted += ngx_http_cache_purge_index_delete(cmcf,
                                        pass->scan.cache,
                                        (ngx_rbtree_key_t) id, generation,
                                        pass->hits, n, pass->scan.log);

        if (n > 0 && pass->hits[n - 1].key.len > 0) {
            pass->cursor.key.len = pass->hits[n - 1].key.len;
            ngx_memcpy(pass->cursor_key, pass->hits[n - 1].key.data,
                       pass->cursor.key.len);
            ngx_memcpy(pass->cursor.md5, pass->hits[n - 1].md5,
                       NGX_HTTP_CACHE_KEY_LEN);
            pass->cursor.key.data = pass->cursor_key;
            pass->has_cursor = 1;
        }

        ngx_destroy_pool(tmp);

        if (!more) {
            pass->next_pattern++;
            pass->has_cursor = 0;
        }

        if (budget != 0) {
            ngx_time_update();

            if (ngx_current_msec - start >= budget) {
                return (pass->next_pattern < npats) ? NGX_AGAIN : NGX_OK;
            }
        }
    }

    return NGX_OK;
}


/* -- key index: build walk (all workers) and reconcile (worker 0) --------- */

/*
 * The build walk, in parallel.
 *
 * The directories of every cache zone are cut into units -- one per
 * directory at the second level of levels= (4096 per zone for levels=1:2),
 * named after the level lengths, so no directory has to be listed to know
 * them.  Every worker process claims units under the index lock and walks
 * them in time slices (build_tick); the one that finishes the last unit
 * declares the index ready.  A unit claimed by a process that died is
 * handed out again (worker 0 looks for those), and a worker leaving on a
 * reload hands its unit back.  Walking a unit twice is harmless: indexing
 * is idempotent.
 */

static ngx_int_t
ngx_http_cache_purge_unhex(u_char *p, u_char *md5)
{
    ngx_int_t   hi, lo;
    ngx_uint_t  i;

    for (i = 0; i < NGX_HTTP_CACHE_KEY_LEN; i++) {
        hi = ngx_hextoi(p + 2 * i, 1);
        lo = ngx_hextoi(p + 2 * i + 1, 1);

        if (hi == NGX_ERROR || lo == NGX_ERROR) {
            return NGX_ERROR;
        }

        md5[i] = (u_char) (hi << 4 | lo);
    }

    return NGX_OK;
}

static void
ngx_http_cache_purge_build_end(ngx_http_cache_purge_build_t *b)
{
    ngx_http_cache_purge_scan_close(&b->scan);
    b->unit = NGX_CACHE_PURGE_UNIT_NONE;
}

/* Hex digits of v, len of them (the name of a levels= directory) */
static u_char *
ngx_http_cache_purge_hex_level(u_char *p, ngx_uint_t v, size_t len)
{
    static u_char  hex[] = "0123456789abcdef";

    while (len--) {
        p[len] = hex[v & 0xf];
        v >>= 4;
    }

    return p;
}

/*
 * Plan a build over the caches of cycle: register their ids and unit ranges
 * and a claim table, and enter BUILDING.  Locked.
 */
static ngx_int_t
ngx_http_cache_purge_build_init(ngx_cycle_t *cycle,
    ngx_http_cache_purge_main_conf_t *cmcf)
{
    ngx_http_cache_purge_index_sh_t     *sh = ngx_http_cache_purge_index_peek(cmcf);
    ngx_http_cache_purge_build_cache_t  *bc;
    ngx_http_file_cache_t               *cache;
    ngx_list_part_t                     *part;
    ngx_shm_zone_t                      *zone;
    ngx_uint_t                           i, units, n;
    ngx_int_t                            id;

    if (sh->build_claims != NULL) {
        ngx_slab_free_locked(cmcf->index_shpool, sh->build_claims);
        sh->build_claims = NULL;
    }

    sh->build_ncaches = 0;
    units = 0;

    part = &cycle->shared_memory.part;
    zone = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            zone = part->elts;
            i = 0;
        }

        cache = ngx_http_cache_purge_zone_cache(&zone[i]);

        if (cache == NULL || cache->path == NULL
            || sh->build_ncaches == NGX_CACHE_PURGE_INDEX_PATHS)
        {
            continue;
        }

        id = ngx_http_cache_purge_index_cache_id(cmcf, &cache->path->name, 1);
        if (id == NGX_ERROR) {
            return NGX_ERROR;
        }

        bc = &sh->build_caches[sh->build_ncaches++];
        bc->id    = (ngx_rbtree_key_t) id;
        bc->first = units;
        bc->l0    = cache->path->level[0];
        bc->l1    = (bc->l0 != 0) ? cache->path->level[1] : 0;

        n = (ngx_uint_t) 1 << (4 * (bc->l0 + bc->l1));
        bc->count = n;
        units += n;
    }

    if (units > 0) {
        ngx_http_cache_purge_index_busy(sh, 1);
        sh->build_claims = ngx_slab_calloc_locked(cmcf->index_shpool,
                                                  units * sizeof(ngx_pid_t));
        ngx_http_cache_purge_index_busy(sh, 0);

        if (sh->build_claims == NULL) {
            return NGX_ERROR;
        }
    }

    sh->build_units   = units;
    sh->build_next    = 0;
    sh->build_done    = 0;
    sh->build_files   = 0;
    sh->build_deleted = 0;
    sh->build_started = ngx_current_msec;
    sh->build_seq++;
    sh->state = NGX_CACHE_PURGE_INDEX_BUILDING;

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                  "ngx_cache_purge: building the key index of %ui cache(s), "
                  "%ui directories, in all workers", sh->build_ncaches,
                  units);

    return NGX_OK;
}

/* A unit to walk, or UNIT_NONE when all are claimed.  Locked. */
static ngx_uint_t
ngx_http_cache_purge_build_claim(ngx_http_cache_purge_index_sh_t *sh)
{
    ngx_uint_t  u;

    while (sh->build_next < sh->build_units) {
        u = sh->build_next++;
        if (sh->build_claims[u] == NGX_CACHE_PURGE_UNIT_TODO) {
            sh->build_claims[u] = ngx_pid;
            return u;
        }
    }

    /* units handed back by processes that left */
    for (u = 0; u < sh->build_units; u++) {
        if (sh->build_claims[u] == NGX_CACHE_PURGE_UNIT_TODO) {
            sh->build_claims[u] = ngx_pid;
            return u;
        }
    }

    return NGX_CACHE_PURGE_UNIT_NONE;
}

/* Units claimed by processes that are gone go back to TODO.  Locked. */
static void
ngx_http_cache_purge_build_reclaim(ngx_http_cache_purge_index_sh_t *sh,
    ngx_log_t *log)
{
    ngx_uint_t  u, n;
    ngx_pid_t   pid;

    n = 0;

    for (u = 0; u < sh->build_units; u++) {
        pid = sh->build_claims[u];

        if (pid > 0 && kill(pid, 0) == -1 && ngx_errno == NGX_ESRCH) {
            sh->build_claims[u] = NGX_CACHE_PURGE_UNIT_TODO;
            n++;
        }
    }

    if (n > 0) {
        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "ngx_cache_purge: %ui index build unit(s) of exited "
                      "processes handed out again", n);
    }
}

/* Directory of unit u into path; returns its cache's id.  Locked. */
static ngx_int_t
ngx_http_cache_purge_build_unit_path(ngx_http_cache_purge_index_sh_t *sh,
    ngx_uint_t u, u_char *path, size_t size)
{
    ngx_http_cache_purge_build_cache_t  *bc;
    ngx_str_t                           *root;
    ngx_uint_t                           k, r;
    u_char                              *p;

    for (k = 0; k < sh->build_ncaches; k++) {
        bc = &sh->build_caches[k];

        if (u < bc->first || u >= bc->first + bc->count) {
            continue;
        }

        root = &sh->paths[bc->id];

        if (root->len + 1 + bc->l0 + 1 + bc->l1 + 1 > size) {
            return NGX_ERROR;
        }

        r = u - bc->first;
        p = ngx_cpymem(path, root->data, root->len);

        if (bc->l0) {
            *p++ = '/';
            ngx_http_cache_purge_hex_level(p, r >> (4 * bc->l1), bc->l0);
            p += bc->l0;
        }

        if (bc->l1) {
            *p++ = '/';
            ngx_http_cache_purge_hex_level(p, r & ((1 << (4 * bc->l1)) - 1),
                                           bc->l1);
            p += bc->l1;
        }

        *p = '\0';

        return (ngx_int_t) bc->id;
    }

    return NGX_ERROR;
}

/*
 * The last unit is done: the index is complete.  Locked.
 */
static void
ngx_http_cache_purge_build_complete(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_log_t *log)
{
    ngx_http_cache_purge_index_sh_t  *sh = ngx_http_cache_purge_index_peek(cmcf);

    sh->state = NGX_CACHE_PURGE_INDEX_READY;
    ngx_http_cache_purge_index_build_clear(cmcf);

    if (sh->build_claims != NULL) {
        ngx_http_cache_purge_index_busy(sh, 1);
        ngx_slab_free_locked(cmcf->index_shpool, sh->build_claims);
        sh->build_claims = NULL;
        ngx_http_cache_purge_index_busy(sh, 0);
    }

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "ngx_cache_purge: key index build complete: %ui file(s) "
                  "read, %ui purged, %ui entries, in %Mms", sh->build_files,
                  sh->build_deleted, sh->entries,
                  ngx_current_msec - sh->build_started);
}

/*
 * One slice of this worker's part of the build.  Files are read in batches
 * outside the lock -- opened first, their header reads announced
 * (POSIX_FADV_WILLNEED) so the kernel issues them together, then read --
 * and then, in chunks under the lock, either indexed or (when a purge that
 * arrived during the build covers them) deleted.  Checking and indexing
 * under the lock the purges take is what makes "applied to the index so far
 * + remembered for the rest of the walk" exact.
 *
 * NGX_AGAIN: budget ran out, more to do.  NGX_DONE: nothing (left) for this
 * worker -- no build, all units claimed, or the build ended.
 */
static ngx_int_t
ngx_http_cache_purge_build_step(ngx_cycle_t *cycle,
    ngx_http_cache_purge_main_conf_t *cmcf, ngx_http_cache_purge_build_t *b,
    ngx_msec_t budget)
{
    static u_char                     keys[NGX_CACHE_PURGE_INDEX_BATCH]
                                          [NGX_CACHE_PURGE_KEY_MAX_LEN];
    static size_t                     lens[NGX_CACHE_PURGE_INDEX_BATCH];
    static u_char                     md5s[NGX_CACHE_PURGE_INDEX_BATCH]
                                          [NGX_HTTP_CACHE_KEY_LEN];
    static u_char                     del[NGX_CACHE_PURGE_INDEX_BATCH];
    static int                        fds[NGX_CACHE_PURGE_INDEX_BATCH];
    ngx_http_cache_purge_index_sh_t  *sh;
    ngx_str_t                         name, path;
    ngx_uint_t                        i, n, m, c, end, deleted;
    ngx_int_t                         id, rc;
    ngx_msec_t                        start;
    ngx_flag_t                        walked, stop;
    ssize_t                           len;
    u_char                           *nl, dir[NGX_MAX_PATH];
    int                               dir_fd;

    start = ngx_current_msec;

    for ( ;; ) {

        if (b->unit == NGX_CACHE_PURGE_UNIT_NONE) {

            sh = ngx_http_cache_purge_index_peek(cmcf);
            if (sh == NULL || sh->state != NGX_CACHE_PURGE_INDEX_BUILDING) {
                return NGX_DONE;                          /* a hint */
            }

            sh = ngx_http_cache_purge_index_lock(cmcf);

            if (sh == NULL || sh->state != NGX_CACHE_PURGE_INDEX_BUILDING
                || ngx_exiting)
            {
                ngx_http_cache_purge_index_unlock(cmcf);
                return NGX_DONE;
            }

            b->generation = sh->generation;
            b->seq        = sh->build_seq;
            b->unit       = ngx_http_cache_purge_build_claim(sh);

            id = NGX_ERROR;
            if (b->unit != NGX_CACHE_PURGE_UNIT_NONE) {
                id = ngx_http_cache_purge_build_unit_path(sh, b->unit, dir,
                                                          sizeof(dir));
                if (id != NGX_ERROR) {
                    b->id = (ngx_rbtree_key_t) id;
                    path.data = sh->paths[id].data;
                    path.len  = sh->paths[id].len;
                    b->cache = ngx_http_cache_purge_find_cache(cycle, &path);
                }
            }

            ngx_http_cache_purge_index_unlock(cmcf);

            if (b->unit == NGX_CACHE_PURGE_UNIT_NONE) {
                return NGX_DONE;
            }

            b->scan.log = cycle->log;
            b->scan.errors = 0;
            path.data = dir;
            path.len  = ngx_strlen(dir);

            /* a directory that does not exist has no files: done */
            if (id == NGX_ERROR || b->cache == NULL
                || ngx_http_cache_purge_scan_open(&b->scan, &path)
                   == NGX_ERROR)
            {
                b->scan.errors++;
            }
        }

        /* read a batch */

        walked = (b->scan.depth == 0);

        for (m = 0; !walked && m < NGX_CACHE_PURGE_INDEX_BATCH; /* void */ ) {

            /* the slice's budget holds within a batch too */
            if (budget != 0 && m != 0 && m % NGX_CACHE_PURGE_INDEX_CHUNK == 0)
            {
                ngx_time_update();
                if (ngx_current_msec - start >= budget) {
                    break;
                }
            }

            if (ngx_http_cache_purge_scan_next(&b->scan, &dir_fd, &name)
                != NGX_OK)
            {
                walked = 1;
                break;
            }

            if (ngx_http_cache_purge_unhex(name.data, md5s[m]) != NGX_OK) {
                continue;
            }

            fds[m] = ngx_http_cache_purge_open_file(dir_fd,
                                                    (char *) name.data);
            if (fds[m] == -1) {
                continue;
            }

#  if (NGX_HAVE_POSIX_FADVISE)
            (void) posix_fadvise(fds[m], NGX_CACHE_PURGE_KEY_OFFSET,
                                 NGX_CACHE_PURGE_KEY_MAX_LEN,
                                 POSIX_FADV_WILLNEED);
#  endif
            m++;
        }

        for (i = 0, n = 0; i < m; i++) {

            len = pread(fds[i], keys[n], NGX_CACHE_PURGE_KEY_MAX_LEN,
                        NGX_CACHE_PURGE_KEY_OFFSET);
            (void) close(fds[i]);

            if (len <= 0) {
                continue;
            }

            if (n != i) {
                ngx_memcpy(md5s[n], md5s[i], NGX_HTTP_CACHE_KEY_LEN);
            }

            nl = ngx_strlchr(keys[n], keys[n] + len, LF);
            lens[n] = (nl != NULL) ? (size_t) (nl - keys[n])
                                   : ngx_min((size_t) len,
                                             NGX_CACHE_PURGE_KEY_MAX_LEN - 1);

            ngx_strlow(keys[n], keys[n], lens[n]);
            n++;
        }

        /*
         * Index it, or delete what a purge during the build covers, in
         * chunks: other workers' fills and purges wait for this lock.
         */
        stop = 0;

        for (c = 0; c < n || (c == 0 && n == 0); c = end) {

            end = ngx_min(n, c + NGX_CACHE_PURGE_INDEX_CHUNK);

            sh = ngx_http_cache_purge_index_lock(cmcf);

            if (sh == NULL || sh->state != NGX_CACHE_PURGE_INDEX_BUILDING
                || sh->generation != b->generation
                || sh->build_seq != b->seq)
            {
                ngx_http_cache_purge_index_unlock(cmcf);
                ngx_http_cache_purge_build_end(b);
                return NGX_DONE;
            }

            if (b->scan.errors != 0) {
                /* files under a directory it could not read are unknown */
                ngx_http_cache_purge_index_lost(cmcf, cycle->log,
                                    "a cache directory could not be read");
                ngx_http_cache_purge_index_unlock(cmcf);
                ngx_http_cache_purge_build_end(b);
                return NGX_DONE;
            }

            for (i = c; i < end; i++) {

                del[i] = ngx_http_cache_purge_index_build_match(sh, b->id,
                                                        keys[i], lens[i]);
                if (del[i]) {
                    continue;
                }

                rc = ngx_http_cache_purge_index_insert(cmcf, b->id, keys[i],
                                                       lens[i], md5s[i]);
                if (rc == NGX_ERROR) {
                    ngx_http_cache_purge_index_lost(cmcf, cycle->log,
                        "cache_purge_index zone is full, increase "
                        "cache_purge_index");
                    stop = 1;
                    break;
                }
            }

            sh->build_files += end - c;

            ngx_http_cache_purge_index_unlock(cmcf);

            if (stop) {
                ngx_http_cache_purge_build_end(b);
                return NGX_DONE;
            }

            if (n == 0) {
                break;
            }
        }

        deleted = 0;

        for (i = 0; i < n; i++) {
            if (del[i]
                && ngx_http_cache_purge_delete_md5(b->cache, md5s[i],
                                                   cycle->log) == NGX_OK)
            {
                deleted++;
            }
        }

        if (walked || deleted) {
            sh = ngx_http_cache_purge_index_lock(cmcf);

            if (sh != NULL && sh->state == NGX_CACHE_PURGE_INDEX_BUILDING
                && sh->generation == b->generation
                && sh->build_seq == b->seq)
            {
                sh->build_deleted += deleted;

                if (walked
                    && sh->build_claims[b->unit] != NGX_CACHE_PURGE_UNIT_DONE)
                {
                    sh->build_claims[b->unit] = NGX_CACHE_PURGE_UNIT_DONE;

                    if (++sh->build_done == sh->build_units) {
                        ngx_http_cache_purge_build_complete(cmcf, cycle->log);
                    }
                }
            }

            ngx_http_cache_purge_index_unlock(cmcf);

            if (walked) {
                ngx_http_cache_purge_build_end(b);
            }
        }

        if (budget != 0) {
            ngx_time_update();

            if (ngx_current_msec - start >= budget) {
                return NGX_AGAIN;
            }
        }
    }
}

/* This worker leaves (reload, stop): hand its unit back.  */
static void
ngx_http_cache_purge_build_release(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_http_cache_purge_build_t *b)
{
    ngx_http_cache_purge_index_sh_t  *sh;

    if (b->unit != NGX_CACHE_PURGE_UNIT_NONE && cmcf->index_shpool != NULL) {
        sh = ngx_http_cache_purge_index_lock(cmcf);

        if (sh != NULL && sh->generation == b->generation
            && sh->build_seq == b->seq && sh->build_claims != NULL
            && b->unit < sh->build_units
            && sh->build_claims[b->unit] == ngx_pid)
        {
            sh->build_claims[b->unit] = NGX_CACHE_PURGE_UNIT_TODO;
        }

        ngx_http_cache_purge_index_unlock(cmcf);
    }

    ngx_http_cache_purge_build_end(b);
}

/* Every worker's build timer: walk units while a build runs. */
static void
ngx_http_cache_purge_build_tick(ngx_event_t *ev)
{
    ngx_http_cache_purge_main_conf_t  *cmcf = ngx_cache_purge_build_conf;
    ngx_int_t                          rc;

    if (cmcf == NULL || ngx_exiting) {
        return;
    }

    rc = ngx_http_cache_purge_build_step((ngx_cycle_t *) ngx_cycle, cmcf,
                                         &ngx_cache_purge_build,
                                         cmcf->walk_budget);

    /* between slices a short pause; without work a poll for a build */
    ngx_add_timer(ev, (rc == NGX_AGAIN) ? cmcf->throttle_ms : 200);
}

/*
 * Find a cache zone's node by the 16-byte cache key.  Mirrors
 * ngx_http_file_cache_lookup(): node.key is the first sizeof(ngx_rbtree_key_t)
 * bytes of the key copied in native byte order (8 bytes on 64-bit), and
 * fcn->key holds the rest.  Cache zone locked.
 */
static ngx_http_file_cache_node_t *
ngx_http_cache_purge_lookup_node(ngx_http_file_cache_t *cache, u_char *key16)
{
    ngx_rbtree_node_t           *node, *sentinel;
    ngx_http_file_cache_node_t  *fcn;
    ngx_rbtree_key_t             node_key;
    ngx_int_t                    rc;

    ngx_memcpy((u_char *) &node_key, key16, sizeof(ngx_rbtree_key_t));

    node     = cache->sh->rbtree.root;
    sentinel = cache->sh->rbtree.sentinel;

    while (node != sentinel) {

        if (node_key < node->key) {
            node = node->left;
            continue;
        }

        if (node_key > node->key) {
            node = node->right;
            continue;
        }

        fcn = (ngx_http_file_cache_node_t *) node;

        rc = ngx_memcmp(&key16[sizeof(ngx_rbtree_key_t)], fcn->key,
                        NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t));

        if (rc == 0) {
            return fcn;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    return NULL;
}

/*
 * One slice of the reconcile pass: walk the index in key order and drop
 * entries whose cache node is gone (the cache manager expired or evicted the
 * file), or holds no file and nobody is storing one.  Zones whose cache
 * loader has not finished (cold) are left alone: their nodes are not all
 * there yet.  NGX_OK the pass is complete, NGX_AGAIN budget ran out.
 */
static ngx_int_t
ngx_http_cache_purge_reconcile_step(ngx_cycle_t *cycle,
    ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_http_cache_purge_reconcile_t *rc, ngx_msec_t budget)
{
    ngx_http_cache_purge_index_sh_t    *sh;
    ngx_http_cache_purge_index_node_t  *n, *next;
    ngx_http_file_cache_node_t         *fcn;
    ngx_http_file_cache_t              *cache;
    ngx_rbtree_node_t                  *root;
    ngx_uint_t                          i, id;
    ngx_msec_t                          start, last;
    ngx_flag_t                          drop;

    start = ngx_current_msec;

    for ( ;; ) {

        sh = ngx_http_cache_purge_index_lock(cmcf);

        if (sh == NULL || (rc->active && rc->generation != sh->generation)) {
            /* the index was reset: nothing of this pass applies any more */
            ngx_http_cache_purge_index_unlock(cmcf);
            rc->active = 0;
            return NGX_OK;
        }

        if (!rc->active) {
            last = rc->last;
            ngx_memzero(rc, sizeof(ngx_http_cache_purge_reconcile_t));
            rc->last       = last;
            rc->active     = 1;
            rc->started    = ngx_current_msec;
            rc->generation = sh->generation;

            /* ids are indexes into sh->paths; resolve them once per pass */
            for (i = 0; i < sh->npaths; i++) {
                rc->caches[i] = ngx_http_cache_purge_find_cache(cycle,
                                                                &sh->paths[i]);
            }
            rc->npaths = sh->npaths;
        }

        if (rc->has_cursor) {
            n = ngx_http_cache_purge_index_bound(&sh->rbtree, rc->id, rc->key, rc->len,
                                                 rc->md5, 1);
        } else {
            root = sh->rbtree.root;
            n = (root == sh->rbtree.sentinel) ? NULL
                : (ngx_http_cache_purge_index_node_t *)
                      ngx_rbtree_min(root, sh->rbtree.sentinel);
        }

        for (i = 0; n != NULL && i < NGX_CACHE_PURGE_INDEX_SLICE; i++) {

            next = (ngx_http_cache_purge_index_node_t *)
                       ngx_rbtree_next(&sh->rbtree, &n->node);

            rc->id  = n->node.key;
            rc->len = n->len;
            ngx_memcpy(rc->key, n->key, n->len);
            ngx_memcpy(rc->md5, n->md5, NGX_HTTP_CACHE_KEY_LEN);
            rc->has_cursor = 1;

            id    = (ngx_uint_t) n->node.key;
            cache = (id < rc->npaths) ? rc->caches[id] : NULL;
            drop  = 0;

            if (cache == NULL) {
                /* the cache is no longer configured */
                drop = (id < rc->npaths);

            } else if (!cache->sh->cold) {
                ngx_shmtx_lock(&cache->shpool->mutex);

                fcn = ngx_http_cache_purge_lookup_node(cache, n->md5);

                drop = (fcn == NULL
                        || (!fcn->exists && fcn->count == 0
                            && !fcn->updating));

                ngx_shmtx_unlock(&cache->shpool->mutex);
            }

            rc->checked++;

            if (drop) {
                ngx_http_cache_purge_index_remove(cmcf, n);
                rc->dropped++;
            }

            n = next;
        }

        ngx_http_cache_purge_index_unlock(cmcf);

        if (n == NULL) {
            ngx_time_update();
            ngx_log_error(NGX_LOG_INFO, cycle->log, 0,
                          "ngx_cache_purge: index reconciled: checked %ui, "
                          "dropped %ui, %ui entries, in %Mms", rc->checked,
                          rc->dropped, sh->entries,
                          ngx_current_msec - rc->started);
            rc->active = 0;
            return NGX_OK;
        }

        if (budget != 0) {
            ngx_time_update();

            if (ngx_current_msec - start >= budget) {
                return NGX_AGAIN;
            }
        }
    }
}

/*
 * The purges a build had registered, copied out (locked) so they can be
 * handed to the walk queue when the build is abandoned.  Elements are
 * { cache path, key } pairs; an empty key stands for purge_all.
 */
static ngx_array_t *
ngx_http_cache_purge_build_handoff(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_pool_t *pool)
{
    ngx_http_cache_purge_index_sh_t    *sh = ngx_http_cache_purge_index_peek(cmcf);
    ngx_http_cache_purge_index_node_t  *n;
    ngx_rbtree_node_t                  *node;
    ngx_array_t                        *a;
    ngx_str_t                          *e;
    ngx_rbtree_key_t                    id;

    a = ngx_array_create(pool, sh->nbuild * 2 + 2, sizeof(ngx_str_t));
    if (a == NULL || sh->build.root == sh->build.sentinel) {
        return a;
    }

    for (node = ngx_rbtree_min(sh->build.root, sh->build.sentinel);
         node != NULL;
         node = ngx_rbtree_next(&sh->build, node))
    {
        n  = (ngx_http_cache_purge_index_node_t *) node;
        id = n->node.key;

        if (id >= sh->npaths) {
            continue;
        }

        e = ngx_array_push_n(a, 2);
        if (e == NULL) {
            return a;
        }

        e[0].len  = sh->paths[id].len;
        e[0].data = ngx_pnalloc(pool, e[0].len + 1);

        /* the queue expects the wildcard form of the key */
        e[1].len  = n->len + (n->len > 0 ? 1 : 0);
        e[1].data = ngx_pnalloc(pool, e[1].len + 1);

        if (e[0].data == NULL || e[1].data == NULL) {
            a->nelts -= 2;
            return a;
        }

        ngx_memcpy(e[0].data, sh->paths[id].data, e[0].len);
        e[0].data[e[0].len] = '\0';

        ngx_memcpy(e[1].data, n->key, n->len);
        if (e[1].len > 0) {
            e[1].data[e[1].len - 1] = '*';
        }
    }

    return a;
}

static void
ngx_http_cache_purge_build_requeue(ngx_http_cache_purge_main_conf_t *cmcf,
    ngx_log_t *log, ngx_array_t *handoff)
{
    ngx_str_t   *e = handoff->elts;
    ngx_uint_t   i, lost, level;

    lost = 0;

    for (i = 0; i + 1 < handoff->nelts; i += 2) {
        if (ngx_http_cache_purge_enqueue_raw(cmcf, log, &e[i], &e[i + 1],
                                             e[i + 1].len == 0,
                                             NGX_CACHE_PURGE_MODE_WALK)
            != NGX_OK)
        {
            lost++;
        }
    }

    level = lost ? NGX_LOG_CRIT : NGX_LOG_NOTICE;

    ngx_log_error(level, log, 0,
                  "ngx_cache_purge: %ui purge(s) received during the "
                  "abandoned index build handed to walks, %ui lost "
                  "(queue full)", handoff->nelts / 2, lost);
}

static ngx_int_t
ngx_http_cache_purge_index_work(ngx_cycle_t *cycle,
    ngx_http_cache_purge_main_conf_t *cmcf)
{
    ngx_http_cache_purge_reconcile_t  *rc = &ngx_cache_purge_reconcile;
    ngx_http_cache_purge_index_sh_t   *sh;
    ngx_array_t                       *handoff;
    ngx_pool_t                        *pool;
    ngx_uint_t                         state;
    static ngx_msec_t                  reclaimed;

    if (cmcf->index_shpool == NULL || ngx_exiting) {
        return NGX_OK;
    }

    sh = ngx_http_cache_purge_index_lock(cmcf);

    if (sh == NULL) {
        ngx_http_cache_purge_index_unlock(cmcf);
        return NGX_OK;
    }

    state = sh->state;

    switch (state) {

    case NGX_CACHE_PURGE_INDEX_PENDING:

        if (ngx_http_cache_purge_build_init(cycle, cmcf) != NGX_OK) {
            sh->state     = NGX_CACHE_PURGE_INDEX_FAILED;
            sh->failed_at = ngx_current_msec;
            ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                          "ngx_cache_purge: could not plan the key index "
                          "build (cache_purge_index zone too small); "
                          "retried every cache_purge_index_reconcile");
        }

        ngx_http_cache_purge_index_unlock(cmcf);
        return NGX_AGAIN;

    case NGX_CACHE_PURGE_INDEX_BUILDING:

        /* the workers walk; worker 0 hands out units of those that died */
        if (ngx_current_msec - reclaimed >= 1000) {
            reclaimed = ngx_current_msec;
            ngx_http_cache_purge_build_reclaim(sh, cycle->log);
        }

        ngx_http_cache_purge_index_unlock(cmcf);
        return NGX_OK;

    case NGX_CACHE_PURGE_INDEX_FAILED:

        /* purges a failed build had taken on still have to be carried out */
        if (sh->nbuild > 0) {
            pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, cycle->log);
            handoff = (pool != NULL)
                      ? ngx_http_cache_purge_build_handoff(cmcf, pool) : NULL;
            ngx_http_cache_purge_index_build_clear(cmcf);
            ngx_http_cache_purge_index_unlock(cmcf);

            if (handoff != NULL) {
                ngx_http_cache_purge_build_requeue(cmcf, cycle->log, handoff);
            }
            if (pool != NULL) {
                ngx_destroy_pool(pool);
            }
            return NGX_AGAIN;
        }

        /* self-heal: after a while reconcile (frees room) and build again */
        if (ngx_current_msec - sh->failed_at >= cmcf->index_reconcile) {
            sh->state = NGX_CACHE_PURGE_INDEX_REBUILD;
            ngx_http_cache_purge_index_unlock(cmcf);
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                          "ngx_cache_purge: retrying the key index");
            return NGX_AGAIN;
        }

        ngx_http_cache_purge_index_unlock(cmcf);
        return NGX_OK;

    default:
        ngx_http_cache_purge_index_unlock(cmcf);
        break;
    }

    switch (state) {

    case NGX_CACHE_PURGE_INDEX_REBUILD:

        /* make room by dropping stale entries, then walk again */
        if (ngx_http_cache_purge_reconcile_step(cycle, cmcf, rc,
                                                cmcf->walk_budget)
            == NGX_AGAIN)
        {
            return NGX_AGAIN;
        }

        sh = ngx_http_cache_purge_index_lock(cmcf);
        if (sh != NULL && sh->state == NGX_CACHE_PURGE_INDEX_REBUILD
            && ngx_http_cache_purge_build_init(cycle, cmcf) != NGX_OK)
        {
            sh->state     = NGX_CACHE_PURGE_INDEX_FAILED;
            sh->failed_at = ngx_current_msec;
        }
        ngx_http_cache_purge_index_unlock(cmcf);

        return NGX_AGAIN;

    case NGX_CACHE_PURGE_INDEX_READY:

        if (!rc->active
            && ngx_current_msec - rc->last < cmcf->index_reconcile)
        {
            return NGX_OK;
        }

        if (ngx_http_cache_purge_reconcile_step(cycle, cmcf, rc,
                                                cmcf->walk_budget)
            == NGX_OK)
        {
            rc->last = ngx_current_msec;
        }

        return NGX_AGAIN;

    default:
        return NGX_OK;
    }
}

/*
 * Header filter: an upstream response is about to be sent (and possibly
 * stored).  Runs for subrequests too, unlike the log phase.
 */
static ngx_http_output_header_filter_pt  ngx_http_cache_purge_next_header_filter;

static ngx_int_t
ngx_http_cache_purge_header_filter(ngx_http_request_t *r)
{
    ngx_http_cache_purge_main_conf_t  *cmcf;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);

    if (cmcf != NULL
        && ngx_http_cache_purge_index_peek(cmcf) != NULL
        /* a failed index cannot take entries; skip the attempt (a hint) */
        && ngx_http_cache_purge_index_peek(cmcf)->state
           != NGX_CACHE_PURGE_INDEX_FAILED
        && r->cache != NULL
        && r->upstream != NULL
        && !r->cached
        && r->upstream->headers_in.status_n != 0
        && r->cache->file_cache != NULL
        && r->cache->keys.nelts > 0)
    {
        ngx_http_cache_purge_index_record(r, cmcf);
    }

    return ngx_http_cache_purge_next_header_filter(r);
}

static ngx_int_t
ngx_http_cache_purge_filter_init(ngx_conf_t *cf)
{
    ngx_http_cache_purge_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_cache_purge_header_filter;

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_purge_index_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_cache_purge_main_conf_t  *cmcf = shm_zone->data;
    ngx_http_cache_purge_index_sh_t   *sh;

    cmcf->index_shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    /* a reload keeps the index (data / shm.exists); a start creates it */
    sh = ngx_http_cache_purge_index_lock(cmcf);

    if (sh != NULL) {
        /* the insert callback lives in this (reloadable) module */
        sh->rbtree.insert = ngx_http_cache_purge_index_rbtree_insert;
        sh->build.insert  = ngx_http_cache_purge_index_rbtree_insert;
    }

    ngx_http_cache_purge_index_unlock(cmcf);

    return (sh != NULL) ? NGX_OK : NGX_ERROR;
}


/* -- FastCGI ------------------------------------------------------------ */

# if (NGX_HTTP_FASTCGI)
extern ngx_module_t  ngx_http_fastcgi_module;

#  if (nginx_version >= 1007009)
typedef struct {
    ngx_array_t  caches;
} ngx_http_fastcgi_main_conf_t;
#  endif

#  if (nginx_version >= 1007008)
typedef struct {
    ngx_array_t  *flushes;
    ngx_array_t  *lengths;
    ngx_array_t  *values;
    ngx_uint_t    number;
    ngx_hash_t    hash;
} ngx_http_fastcgi_params_t;
#  endif

typedef struct {
    ngx_http_upstream_conf_t   upstream;
    ngx_str_t                  index;

#  if (nginx_version >= 1007008)
    ngx_http_fastcgi_params_t  params;
    ngx_http_fastcgi_params_t  params_cache;
#  else
    ngx_array_t               *flushes;
    ngx_array_t               *params_len;
    ngx_array_t               *params;
#  endif

    ngx_array_t               *params_source;
    ngx_array_t               *catch_stderr;
    ngx_array_t               *fastcgi_lengths;
    ngx_array_t               *fastcgi_values;

#  if (nginx_version >= 8040) && (nginx_version < 1007008)
    ngx_hash_t                 headers_hash;
    ngx_uint_t                 header_params;
#  endif

#  if (nginx_version >= 1001004)
    ngx_flag_t                 keep_conn;
#  endif

    ngx_http_complex_value_t   cache_key;

#  if (NGX_PCRE)
    ngx_regex_t               *split_regex;
    ngx_str_t                  split_name;
#  endif
} ngx_http_fastcgi_loc_conf_t;

char *
ngx_http_fastcgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_compile_complex_value_t  ccv;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_http_fastcgi_loc_conf_t      *flcf;
    ngx_str_t                        *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t          cv;
#  endif

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->fastcgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
        return ngx_http_cache_purge_conf(cf, &cplcf->fastcgi);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        return "(separate location syntax) is not allowed here";
    }

    flcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_fastcgi_module);

#  if (nginx_version >= 1007009)
    if (flcf->upstream.cache > 0)
#  else
    if (flcf->upstream.cache != NGX_CONF_UNSET_PTR
        && flcf->upstream.cache != NULL)
#  endif
    {
        return "is incompatible with \"fastcgi_cache\"";
    }

    if (flcf->upstream.upstream || flcf->fastcgi_lengths) {
        return "is incompatible with \"fastcgi_pass\"";
    }

    if (flcf->upstream.store > 0
#  if (nginx_version < 1007009)
        || flcf->upstream.store_lengths
#  endif
       )
    {
        return "is incompatible with \"fastcgi_store\"";
    }

    value = cf->args->elts;

#  if (nginx_version >= 1007009)
    flcf->upstream.cache = 1;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[1];
    ccv.complex_value = &cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cv.lengths != NULL) {
        flcf->upstream.cache_value = ngx_palloc(cf->pool,
                                        sizeof(ngx_http_complex_value_t));
        if (flcf->upstream.cache_value == NULL) {
            return NGX_CONF_ERROR;
        }
        *flcf->upstream.cache_value = cv;
    } else {
        flcf->upstream.cache_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                        &ngx_http_fastcgi_module);
        if (flcf->upstream.cache_zone == NULL) {
            return NGX_CONF_ERROR;
        }
    }
#  else
    flcf->upstream.cache = ngx_shared_memory_add(cf, &value[1], 0,
                               &ngx_http_fastcgi_module);
    if (flcf->upstream.cache == NULL) {
        return NGX_CONF_ERROR;
    }
#  endif

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[2];
    ccv.complex_value = &flcf->cache_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    cplcf->fastcgi.enable = 0;
    cplcf->conf           = &cplcf->fastcgi;
    clcf->handler         = ngx_http_fastcgi_cache_purge_handler;

    return NGX_CONF_OK;
}

ngx_int_t
ngx_http_fastcgi_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_file_cache_t            *cache;
    ngx_http_fastcgi_loc_conf_t      *flcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t                         status;
    ngx_str_t                        *key;
    ngx_uint_t                        deleted;  /* C89: declared at top of scope */
#  if (nginx_version >= 1007009)
    ngx_http_fastcgi_main_conf_t     *fmcf;
    ngx_int_t                         rc;
#  endif

    ngx_str_set(&status, "purged");

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    flcf = ngx_http_get_module_loc_conf(r, ngx_http_fastcgi_module);
    r->upstream->conf = &flcf->upstream;

#  if (nginx_version >= 1007009)
    fmcf = ngx_http_get_module_main_conf(r, ngx_http_fastcgi_module);
    r->upstream->caches = &fmcf->caches;

    rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
    if (rc != NGX_OK) {
        return rc;
    }
#  else
    cache = flcf->upstream.cache->data;
#  endif

    if (ngx_http_cache_purge_init(r, cache, &flcf->cache_key) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cmcf  = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r,  ngx_http_cache_purge_module);

    if (cmcf != NULL && cmcf->background_purge
        && (cplcf->conf->purge_all || ngx_http_cache_purge_is_partial(r)))
    {
        key = r->cache->keys.elts;
        if (ngx_http_cache_purge_try_enqueue(r, cache, &key[0],
                                             cplcf->conf->purge_all)
            == NGX_DONE)
        {
            return NGX_DONE;
        }
    }

    if (cplcf->conf->purge_all) {
        ngx_http_cache_purge_all(r, cache);
        /* purge_all empties the zone -- always report 200 regardless of
         * how many files existed.  Skip ngx_http_cache_purge_handler()
         * so we never attempt an exact-key lookup on a bulk operation. */
        r->main->count++;
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (ngx_http_cache_purge_is_partial(r)) {
        deleted = ngx_http_cache_purge_partial(r, cache);
        /* Return 200 only when at least one matching file was deleted.
         * On a complete miss return 412/404 per cache_purge_legacy_status. */
        r->main->count++;
        if (deleted > 0) {
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
        } else {
            ngx_http_finalize_request(r,
                (cmcf != NULL && cmcf->legacy_status_codes)
                         ? NGX_HTTP_PRECONDITION_FAILED
                         : NGX_HTTP_NOT_FOUND);
        }
        return NGX_DONE;
    }

    r->main->count++;

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_FASTCGI */


/* -- Proxy -------------------------------------------------------------- */

# if (NGX_HTTP_PROXY)
extern ngx_module_t  ngx_http_proxy_module;

typedef struct {
    ngx_str_t  key_start;
    ngx_str_t  schema;
    ngx_str_t  host_header;
    ngx_str_t  port;
    ngx_str_t  uri;
} ngx_http_proxy_vars_t;

#  if (nginx_version >= 1007009)
typedef struct {
    ngx_array_t  caches;
} ngx_http_proxy_main_conf_t;
#  endif

#  if (nginx_version >= 1007008)
typedef struct {
    ngx_array_t  *flushes;
    ngx_array_t  *lengths;
    ngx_array_t  *values;
    ngx_hash_t    hash;
} ngx_http_proxy_headers_t;
#  endif

typedef struct {
    ngx_http_upstream_conf_t   upstream;

#  if (nginx_version >= 1007008)
    ngx_array_t               *body_flushes;
    ngx_array_t               *body_lengths;
    ngx_array_t               *body_values;
    ngx_str_t                  body_source;
    ngx_http_proxy_headers_t   headers;
    ngx_http_proxy_headers_t   headers_cache;
#  else
    ngx_array_t               *flushes;
    ngx_array_t               *body_set_len;
    ngx_array_t               *body_set;
    ngx_array_t               *headers_set_len;
    ngx_array_t               *headers_set;
    ngx_hash_t                 headers_set_hash;
#  endif

    ngx_array_t               *headers_source;
    /* FIX (#52): nginx 1.29.4 inserted host_set here -- without this guard
     * every subsequent field is at the wrong offset, causing a segfault. */
#  if (nginx_version >= 1029004)
    ngx_uint_t                 host_set;
#  endif
#  if (nginx_version < 8040)
    ngx_array_t               *headers_names;
#  endif

    ngx_array_t               *proxy_lengths;
    ngx_array_t               *proxy_values;
    ngx_array_t               *redirects;

#  if (nginx_version >= 1001015)
    ngx_array_t               *cookie_domains;
    ngx_array_t               *cookie_paths;
#  endif
#  if (nginx_version >= 1019003)
    ngx_array_t               *cookie_flags;
#  endif
#  if (nginx_version < 1007008)
    ngx_str_t                  body_source;
#  endif

#  if (nginx_version >= 1011006)
    ngx_http_complex_value_t  *method;
#  else
    ngx_str_t                  method;
#  endif
    ngx_str_t                  location;
    ngx_str_t                  url;

    ngx_http_complex_value_t   cache_key;
    ngx_http_proxy_vars_t      vars;
    ngx_flag_t                 redirect;

#  if (nginx_version >= 1001004)
    ngx_uint_t                 http_version;
#  endif

    ngx_uint_t                 headers_hash_max_size;
    ngx_uint_t                 headers_hash_bucket_size;

#  if (NGX_HTTP_SSL)
#    if (nginx_version >= 1005006)
    ngx_uint_t                 ssl;
    ngx_uint_t                 ssl_protocols;
    ngx_str_t                  ssl_ciphers;
#    endif
#    if (nginx_version >= 1007000)
    ngx_uint_t                 ssl_verify_depth;
    ngx_str_t                  ssl_trusted_certificate;
    ngx_str_t                  ssl_crl;
#    endif
#    if (nginx_version >= 1007008) && (nginx_version < 1021000)
    ngx_str_t                  ssl_certificate;
    ngx_str_t                  ssl_certificate_key;
    ngx_array_t               *ssl_passwords;
#    endif
#    if (nginx_version >= 1019004)
    ngx_array_t               *ssl_conf_commands;
#    endif
#  endif
} ngx_http_proxy_loc_conf_t;

char *
ngx_http_proxy_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_compile_complex_value_t  ccv;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_http_proxy_loc_conf_t        *plcf;
    ngx_str_t                        *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t          cv;
#  endif

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->proxy.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
        return ngx_http_cache_purge_conf(cf, &cplcf->proxy);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        return "(separate location syntax) is not allowed here";
    }

    plcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_proxy_module);

#  if (nginx_version >= 1007009)
    if (plcf->upstream.cache > 0)
#  else
    if (plcf->upstream.cache != NGX_CONF_UNSET_PTR
        && plcf->upstream.cache != NULL)
#  endif
    {
        return "is incompatible with \"proxy_cache\"";
    }

    if (plcf->upstream.upstream || plcf->proxy_lengths) {
        return "is incompatible with \"proxy_pass\"";
    }

    if (plcf->upstream.store > 0
#  if (nginx_version < 1007009)
        || plcf->upstream.store_lengths
#  endif
       )
    {
        return "is incompatible with \"proxy_store\"";
    }

    value = cf->args->elts;

    /*
     * FIX: do NOT set plcf->upstream.cache here.  In nginx >= 1.27 the proxy
     * module's merge_loc_conf walks every location that has upstream.cache set
     * and synthesises a default "location /" entry, which collides with the
     * explicit "location /" block in the user config and produces the fatal
     * "duplicate location" error at startup.
     *
     * Instead we store the zone reference and the purge-key template directly
     * in our own loc_conf fields (proxy_separate_zone / proxy_separate_value /
     * proxy_separate_key).  The handler below resolves them at request time
     * without going through plcf->upstream at all.
     */
#  if (nginx_version >= 1007009)
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[1];
    ccv.complex_value = &cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cv.lengths != NULL) {
        /* dynamic zone expression -- allocate a persistent copy */
        cplcf->proxy_separate_value = ngx_palloc(cf->pool,
                                          sizeof(ngx_http_complex_value_t));
        if (cplcf->proxy_separate_value == NULL) {
            return NGX_CONF_ERROR;
        }
        *cplcf->proxy_separate_value = cv;
    } else {
        /* static zone name -- look it up in shared memory table */
        cplcf->proxy_separate_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                         &ngx_http_proxy_module);
        if (cplcf->proxy_separate_zone == NULL) {
            return NGX_CONF_ERROR;
        }
    }
#  else
    /* nginx < 1.7.9: cache is just a shm_zone pointer */
    cplcf->proxy_separate_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                      &ngx_http_proxy_module);
    if (cplcf->proxy_separate_zone == NULL) {
        return NGX_CONF_ERROR;
    }
#  endif

    /* compile the purge-key template into our own field */
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[2];
    ccv.complex_value = &cplcf->proxy_separate_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    cplcf->proxy.enable = 0;
    cplcf->conf         = &cplcf->proxy;
    clcf->handler       = ngx_http_proxy_cache_purge_handler;

    return NGX_CONF_OK;
}

ngx_int_t
ngx_http_proxy_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_file_cache_t            *cache;
    ngx_http_proxy_loc_conf_t        *plcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t                         status;
    ngx_str_t                        *key;
    ngx_uint_t                        deleted;  /* C89: declared at top of scope */
#  if (nginx_version >= 1007009)
    ngx_http_proxy_main_conf_t       *pmcf;
    ngx_int_t                         rc;
    ngx_uint_t                        i;        /* C89: declared at top of scope */
    ngx_str_t                        *name;     /* C89: declared at top of scope */
    ngx_http_file_cache_t           **caches;   /* C89: declared at top of scope */
    ngx_str_t                         cv_val;   /* C89: declared at top of scope */
#  endif

    ngx_str_set(&status, "purged");

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    /*
     * Separate-location syntax (proxy_cache_purge zone key): the cache zone
     * and purge-key template are stored in cplcf, not in plcf->upstream.
     * Resolve them here without touching plcf->upstream.cache so that we
     * never trigger the nginx >= 1.27 duplicate-location synthesis.
     */
    if (cplcf->proxy.enable == 0
        && (cplcf->proxy_separate_zone || cplcf->proxy_separate_value))
    {
        if (cplcf->proxy_separate_zone) {
            cache = cplcf->proxy_separate_zone->data;
        } else {
            /* dynamic zone name -- evaluate and walk the proxy caches list */
            if (ngx_http_complex_value(r, cplcf->proxy_separate_value,
                                       &cv_val) != NGX_OK)
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

#  if (nginx_version >= 1007009)
            pmcf   = ngx_http_get_module_main_conf(r, ngx_http_proxy_module);
            caches = pmcf->caches.elts;
            cache  = NULL;

            for (i = 0; i < pmcf->caches.nelts; i++) {
                name = &caches[i]->shm_zone->shm.name;
                if (name->len == cv_val.len
                    && ngx_strncmp(name->data, cv_val.data, cv_val.len) == 0)
                {
                    cache = caches[i];
                    break;
                }
            }

            if (cache == NULL) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "ngx_cache_purge: cache zone \"%V\" not found",
                              &cv_val);
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
#  else
            cache = cplcf->proxy_separate_zone->data;
#  endif
        }

        if (ngx_http_cache_purge_init(r, cache,
                                      &cplcf->proxy_separate_key) != NGX_OK)
        {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

    } else {
        /* Inline syntax (proxy_cache_purge METHOD from ...): use plcf->upstream
         * as before. */
        if (ngx_http_upstream_create(r) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        plcf = ngx_http_get_module_loc_conf(r, ngx_http_proxy_module);
        r->upstream->conf = &plcf->upstream;

#  if (nginx_version >= 1007009)
        pmcf = ngx_http_get_module_main_conf(r, ngx_http_proxy_module);
        r->upstream->caches = &pmcf->caches;

        rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
        if (rc != NGX_OK) {
            return rc;
        }
#  else
        cache = plcf->upstream.cache->data;
#  endif

        if (ngx_http_cache_purge_init(r, cache, &plcf->cache_key) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    cmcf  = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r,  ngx_http_cache_purge_module);

    if (cmcf != NULL && cmcf->background_purge
        && (cplcf->conf->purge_all || ngx_http_cache_purge_is_partial(r)))
    {
        key = r->cache->keys.elts;
        if (ngx_http_cache_purge_try_enqueue(r, cache, &key[0],
                                             cplcf->conf->purge_all)
            == NGX_DONE)
        {
            return NGX_DONE;
        }
    }

    if (cplcf->conf->purge_all) {
        ngx_http_cache_purge_all(r, cache);
        r->main->count++;
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (ngx_http_cache_purge_is_partial(r)) {
        deleted = ngx_http_cache_purge_partial(r, cache);
        r->main->count++;
        if (deleted > 0) {
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
        } else {
            ngx_http_finalize_request(r,
                (cmcf != NULL && cmcf->legacy_status_codes)
                         ? NGX_HTTP_PRECONDITION_FAILED
                         : NGX_HTTP_NOT_FOUND);
        }
        return NGX_DONE;
    }

    r->main->count++;

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_PROXY */


/* -- SCGI --------------------------------------------------------------- */

# if (NGX_HTTP_SCGI)
extern ngx_module_t  ngx_http_scgi_module;

#  if (nginx_version >= 1007009)
typedef struct {
    ngx_array_t  caches;
} ngx_http_scgi_main_conf_t;
#  endif

#  if (nginx_version >= 1007008)
typedef struct {
    ngx_array_t  *flushes;
    ngx_array_t  *lengths;
    ngx_array_t  *values;
    ngx_uint_t    number;
    ngx_hash_t    hash;
} ngx_http_scgi_params_t;
#  endif

typedef struct {
    ngx_http_upstream_conf_t  upstream;

#  if (nginx_version >= 1007008)
    ngx_http_scgi_params_t    params;
    ngx_http_scgi_params_t    params_cache;
    ngx_array_t              *params_source;
#  else
    ngx_array_t              *flushes;
    ngx_array_t              *params_len;
    ngx_array_t              *params;
    ngx_array_t              *params_source;
    ngx_hash_t                headers_hash;
    ngx_uint_t                header_params;
#  endif

    ngx_array_t              *scgi_lengths;
    ngx_array_t              *scgi_values;
    ngx_http_complex_value_t  cache_key;
} ngx_http_scgi_loc_conf_t;

char *
ngx_http_scgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_compile_complex_value_t  ccv;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_http_scgi_loc_conf_t         *slcf;
    ngx_str_t                        *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t          cv;
#  endif

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->scgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
        return ngx_http_cache_purge_conf(cf, &cplcf->scgi);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        return "(separate location syntax) is not allowed here";
    }

    slcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_scgi_module);

#  if (nginx_version >= 1007009)
    if (slcf->upstream.cache > 0)
#  else
    if (slcf->upstream.cache != NGX_CONF_UNSET_PTR
        && slcf->upstream.cache != NULL)
#  endif
    {
        return "is incompatible with \"scgi_cache\"";
    }

    if (slcf->upstream.upstream || slcf->scgi_lengths) {
        return "is incompatible with \"scgi_pass\"";
    }

    value = cf->args->elts;

#  if (nginx_version >= 1007009)
    slcf->upstream.cache = 1;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[1];
    ccv.complex_value = &cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cv.lengths != NULL) {
        slcf->upstream.cache_value = ngx_palloc(cf->pool,
                                        sizeof(ngx_http_complex_value_t));
        if (slcf->upstream.cache_value == NULL) {
            return NGX_CONF_ERROR;
        }
        *slcf->upstream.cache_value = cv;
    } else {
        slcf->upstream.cache_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                        &ngx_http_scgi_module);
        if (slcf->upstream.cache_zone == NULL) {
            return NGX_CONF_ERROR;
        }
    }
#  else
    slcf->upstream.cache = ngx_shared_memory_add(cf, &value[1], 0,
                               &ngx_http_scgi_module);
    if (slcf->upstream.cache == NULL) {
        return NGX_CONF_ERROR;
    }
#  endif

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[2];
    ccv.complex_value = &slcf->cache_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    cplcf->scgi.enable = 0;
    cplcf->conf        = &cplcf->scgi;
    clcf->handler      = ngx_http_scgi_cache_purge_handler;

    return NGX_CONF_OK;
}

ngx_int_t
ngx_http_scgi_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_file_cache_t            *cache;
    ngx_http_scgi_loc_conf_t         *slcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t                         status;
    ngx_str_t                        *key;
    ngx_uint_t                        deleted;  /* C89: declared at top of scope */
#  if (nginx_version >= 1007009)
    ngx_http_scgi_main_conf_t        *smcf;
    ngx_int_t                         rc;
#  endif

    ngx_str_set(&status, "purged");

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_scgi_module);
    r->upstream->conf = &slcf->upstream;

#  if (nginx_version >= 1007009)
    smcf = ngx_http_get_module_main_conf(r, ngx_http_scgi_module);
    r->upstream->caches = &smcf->caches;

    rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
    if (rc != NGX_OK) {
        return rc;
    }
#  else
    cache = slcf->upstream.cache->data;
#  endif

    if (ngx_http_cache_purge_init(r, cache, &slcf->cache_key) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cmcf  = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r,  ngx_http_cache_purge_module);

    if (cmcf != NULL && cmcf->background_purge
        && (cplcf->conf->purge_all || ngx_http_cache_purge_is_partial(r)))
    {
        key = r->cache->keys.elts;
        if (ngx_http_cache_purge_try_enqueue(r, cache, &key[0],
                                             cplcf->conf->purge_all)
            == NGX_DONE)
        {
            return NGX_DONE;
        }
    }

    if (cplcf->conf->purge_all) {
        ngx_http_cache_purge_all(r, cache);
        r->main->count++;
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (ngx_http_cache_purge_is_partial(r)) {
        deleted = ngx_http_cache_purge_partial(r, cache);
        r->main->count++;
        if (deleted > 0) {
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
        } else {
            ngx_http_finalize_request(r,
                (cmcf != NULL && cmcf->legacy_status_codes)
                         ? NGX_HTTP_PRECONDITION_FAILED
                         : NGX_HTTP_NOT_FOUND);
        }
        return NGX_DONE;
    }

    r->main->count++;

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_SCGI */


/* -- uWSGI -------------------------------------------------------------- */

# if (NGX_HTTP_UWSGI)
extern ngx_module_t  ngx_http_uwsgi_module;

#  if (nginx_version >= 1007009)
typedef struct {
    ngx_array_t  caches;
} ngx_http_uwsgi_main_conf_t;
#  endif

#  if (nginx_version >= 1007008)
typedef struct {
    ngx_array_t  *flushes;
    ngx_array_t  *lengths;
    ngx_array_t  *values;
    ngx_uint_t    number;
    ngx_hash_t    hash;
} ngx_http_uwsgi_params_t;
#  endif

typedef struct {
    ngx_http_upstream_conf_t  upstream;

#  if (nginx_version >= 1007008)
    ngx_http_uwsgi_params_t   params;
    ngx_http_uwsgi_params_t   params_cache;
    ngx_array_t              *params_source;
#  else
    ngx_array_t              *flushes;
    ngx_array_t              *params_len;
    ngx_array_t              *params;
    ngx_array_t              *params_source;
    ngx_hash_t                headers_hash;
    ngx_uint_t                header_params;
#  endif

    ngx_array_t              *uwsgi_lengths;
    ngx_array_t              *uwsgi_values;
    ngx_http_complex_value_t  cache_key;
    ngx_str_t                 uwsgi_string;
    ngx_uint_t                modifier1;
    ngx_uint_t                modifier2;

#  if (NGX_HTTP_SSL)
#    if (nginx_version >= 1005008)
    ngx_uint_t                ssl;
    ngx_uint_t                ssl_protocols;
    ngx_str_t                 ssl_ciphers;
#    endif
#    if (nginx_version >= 1007000)
    ngx_uint_t                ssl_verify_depth;
    ngx_str_t                 ssl_trusted_certificate;
    ngx_str_t                 ssl_crl;
#    endif
#    if (nginx_version >= 1007008) && (nginx_version < 1021000)
    ngx_str_t                 ssl_certificate;
    ngx_str_t                 ssl_certificate_key;
    ngx_array_t              *ssl_passwords;
#    endif
#    if (nginx_version >= 1019004)
    ngx_array_t              *ssl_conf_commands;
#    endif
#  endif
} ngx_http_uwsgi_loc_conf_t;

char *
ngx_http_uwsgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_compile_complex_value_t  ccv;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_http_uwsgi_loc_conf_t        *ulcf;
    ngx_str_t                        *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t          cv;
#  endif

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->uwsgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
        return ngx_http_cache_purge_conf(cf, &cplcf->uwsgi);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        return "(separate location syntax) is not allowed here";
    }

    ulcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_uwsgi_module);

#  if (nginx_version >= 1007009)
    if (ulcf->upstream.cache > 0)
#  else
    if (ulcf->upstream.cache != NGX_CONF_UNSET_PTR
        && ulcf->upstream.cache != NULL)
#  endif
    {
        return "is incompatible with \"uwsgi_cache\"";
    }

    if (ulcf->upstream.upstream || ulcf->uwsgi_lengths) {
        return "is incompatible with \"uwsgi_pass\"";
    }

    value = cf->args->elts;

#  if (nginx_version >= 1007009)
    ulcf->upstream.cache = 1;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[1];
    ccv.complex_value = &cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cv.lengths != NULL) {
        ulcf->upstream.cache_value = ngx_palloc(cf->pool,
                                        sizeof(ngx_http_complex_value_t));
        if (ulcf->upstream.cache_value == NULL) {
            return NGX_CONF_ERROR;
        }
        *ulcf->upstream.cache_value = cv;
    } else {
        ulcf->upstream.cache_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                        &ngx_http_uwsgi_module);
        if (ulcf->upstream.cache_zone == NULL) {
            return NGX_CONF_ERROR;
        }
    }
#  else
    ulcf->upstream.cache = ngx_shared_memory_add(cf, &value[1], 0,
                               &ngx_http_uwsgi_module);
    if (ulcf->upstream.cache == NULL) {
        return NGX_CONF_ERROR;
    }
#  endif

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[2];
    ccv.complex_value = &ulcf->cache_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    cplcf->uwsgi.enable = 0;
    cplcf->conf         = &cplcf->uwsgi;
    clcf->handler       = ngx_http_uwsgi_cache_purge_handler;

    return NGX_CONF_OK;
}

ngx_int_t
ngx_http_uwsgi_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_file_cache_t            *cache;
    ngx_http_uwsgi_loc_conf_t        *ulcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t                         status;
    ngx_str_t                        *key;
    ngx_uint_t                        deleted;  /* C89: declared at top of scope */
#  if (nginx_version >= 1007009)
    ngx_http_uwsgi_main_conf_t       *umcf;
    ngx_int_t                         rc;
#  endif

    ngx_str_set(&status, "purged");

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ulcf = ngx_http_get_module_loc_conf(r, ngx_http_uwsgi_module);
    r->upstream->conf = &ulcf->upstream;

#  if (nginx_version >= 1007009)
    umcf = ngx_http_get_module_main_conf(r, ngx_http_uwsgi_module);
    r->upstream->caches = &umcf->caches;

    rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
    if (rc != NGX_OK) {
        return rc;
    }
#  else
    cache = ulcf->upstream.cache->data;
#  endif

    if (ngx_http_cache_purge_init(r, cache, &ulcf->cache_key) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cmcf  = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r,  ngx_http_cache_purge_module);

    if (cmcf != NULL && cmcf->background_purge
        && (cplcf->conf->purge_all || ngx_http_cache_purge_is_partial(r)))
    {
        key = r->cache->keys.elts;
        if (ngx_http_cache_purge_try_enqueue(r, cache, &key[0],
                                             cplcf->conf->purge_all)
            == NGX_DONE)
        {
            return NGX_DONE;
        }
    }

    if (cplcf->conf->purge_all) {
        ngx_http_cache_purge_all(r, cache);
        r->main->count++;
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (ngx_http_cache_purge_is_partial(r)) {
        deleted = ngx_http_cache_purge_partial(r, cache);
        r->main->count++;
        if (deleted > 0) {
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
        } else {
            ngx_http_finalize_request(r,
                (cmcf != NULL && cmcf->legacy_status_codes)
                         ? NGX_HTTP_PRECONDITION_FAILED
                         : NGX_HTTP_NOT_FOUND);
        }
        return NGX_DONE;
    }

    r->main->count++;

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_UWSGI */


/* -- response type directive -------------------------------------------- */

char *
ngx_http_cache_purge_response_type_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_loc_conf_t *cplcf = conf;
    ngx_str_t                       *value;

    if (cplcf->response_type != NGX_CONF_UNSET_UINT) {
        return "is duplicate";
    }

    if (cf->args->nelts != 2) {
        return "requires exactly one argument: html|json|xml|text";
    }

    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "html") == 0) {
        cplcf->response_type = NGX_CACHE_PURGE_RESPONSE_TYPE_HTML;
    } else if (ngx_strcmp(value[1].data, "json") == 0) {
        cplcf->response_type = NGX_CACHE_PURGE_RESPONSE_TYPE_JSON;
    } else if (ngx_strcmp(value[1].data, "xml") == 0) {
        cplcf->response_type = NGX_CACHE_PURGE_RESPONSE_TYPE_XML;
    } else if (ngx_strcmp(value[1].data, "text") == 0) {
        cplcf->response_type = NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT;
    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "invalid parameter \"%V\", expected html|json|xml|text",
            &value[1]);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


/* -- access control ----------------------------------------------------- */

ngx_int_t
ngx_http_cache_purge_access_handler(ngx_http_request_t *r)
{
    ngx_http_cache_purge_loc_conf_t *cplcf;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    /*
     * Belt-and-suspenders: the merge logic only installs this handler when
     * conf->conf is set, so this should never be NULL in production.  Guard
     * anyway to eliminate the crash class entirely.
     */
    if (cplcf->conf == NULL) {
        return NGX_HTTP_NOT_FOUND;
    }

    if (r->method_name.len != cplcf->conf->method.len
        || ngx_strncmp(r->method_name.data, cplcf->conf->method.data,
                       r->method_name.len) != 0)
    {
        /*
         * Not a purge request.  Forward to the original content handler
         * if one exists (e.g. proxy_pass), otherwise return 404.
         * original_handler is NULL when proxy_cache is used without
         * proxy_pass (cache-only / purge-only location).
         */
        if (cplcf->original_handler != NULL) {
            return cplcf->original_handler(r);
        }
        return NGX_HTTP_NOT_FOUND;
    }

    if ((cplcf->conf->access || cplcf->conf->access6)
        && ngx_http_cache_purge_access(cplcf->conf->access,
                                       cplcf->conf->access6,
                                       r->connection->sockaddr) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "ngx_cache_purge: access denied for %V",
                      &r->connection->addr_text);
        return NGX_HTTP_FORBIDDEN;
    }

    if (cplcf->handler == NULL) {
        return NGX_HTTP_NOT_FOUND;
    }

    return cplcf->handler(r);
}

ngx_int_t
ngx_http_cache_purge_access(ngx_array_t *access, ngx_array_t *access6,
    struct sockaddr *s)
{
    in_addr_t        inaddr;
    ngx_in_cidr_t   *a;
    ngx_uint_t       i;
# if (NGX_HAVE_INET6)
    struct in6_addr *inaddr6;
    ngx_in6_cidr_t  *a6;
    u_char          *p;
    ngx_uint_t       n;
# endif

    switch (s->sa_family) {
    case AF_INET:
        if (access == NULL) {
            return NGX_DECLINED;
        }

        inaddr = ((struct sockaddr_in *) s)->sin_addr.s_addr;

# if (NGX_HAVE_INET6)
ipv4:
# endif
        a = access->elts;
        for (i = 0; i < access->nelts; i++) {
            if ((inaddr & a[i].mask) == a[i].addr) {
                return NGX_OK;
            }
        }
        return NGX_DECLINED;

# if (NGX_HAVE_INET6)
    case AF_INET6:
        inaddr6 = &((struct sockaddr_in6 *) s)->sin6_addr;
        p       = inaddr6->s6_addr;

        if (access && IN6_IS_ADDR_V4MAPPED(inaddr6)) {
            inaddr  = p[12] << 24;
            inaddr += p[13] << 16;
            inaddr += p[14] << 8;
            inaddr += p[15];
            inaddr  = htonl(inaddr);
            goto ipv4;
        }

        if (access6 == NULL) {
            return NGX_DECLINED;
        }

        a6 = access6->elts;
        for (i = 0; i < access6->nelts; i++) {
            for (n = 0; n < 16; n++) {
                if ((p[n] & a6[i].mask.s6_addr[n]) != a6[i].addr.s6_addr[n]) {
                    goto next;
                }
            }
            return NGX_OK;
next:
            continue;
        }
        return NGX_DECLINED;
# endif
    }

    return NGX_DECLINED;
}


/* -- response builder --------------------------------------------------- */

ngx_int_t
ngx_http_cache_purge_send_response(ngx_http_request_t *r, ngx_str_t *status)
{
    ngx_http_cache_purge_loc_conf_t *cplcf;
    ngx_chain_t                      out;
    ngx_buf_t                       *b;
    ngx_str_t                       *key;
    ngx_int_t                        rc;
    size_t                           body_len;
    u_char                          *buf, *buf_keydata;
    const char                      *resp_ct,   *resp_body;
    size_t                           resp_ct_size, resp_body_size;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    key   = r->cache->keys.elts;

    buf_keydata = ngx_pcalloc(r->pool, key[0].len + 1);
    if (buf_keydata == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_memcpy(buf_keydata, key[0].data, key[0].len);
    /* buf_keydata[key[0].len] is already '\0' from ngx_pcalloc */

    switch (cplcf->response_type) {
    case NGX_CACHE_PURGE_RESPONSE_TYPE_JSON:
        resp_ct        = ngx_http_cache_purge_content_type_json;
        resp_ct_size   = ngx_http_cache_purge_content_type_json_size;
        resp_body      = ngx_http_cache_purge_body_templ_json;
        resp_body_size = ngx_http_cache_purge_body_templ_json_size;
        break;
    case NGX_CACHE_PURGE_RESPONSE_TYPE_XML:
        resp_ct        = ngx_http_cache_purge_content_type_xml;
        resp_ct_size   = ngx_http_cache_purge_content_type_xml_size;
        resp_body      = ngx_http_cache_purge_body_templ_xml;
        resp_body_size = ngx_http_cache_purge_body_templ_xml_size;
        break;
    case NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT:
        resp_ct        = ngx_http_cache_purge_content_type_text;
        resp_ct_size   = ngx_http_cache_purge_content_type_text_size;
        resp_body      = ngx_http_cache_purge_body_templ_text;
        resp_body_size = ngx_http_cache_purge_body_templ_text_size;
        break;
    default:
    case NGX_CACHE_PURGE_RESPONSE_TYPE_HTML:
        resp_ct        = ngx_http_cache_purge_content_type_html;
        resp_ct_size   = ngx_http_cache_purge_content_type_html_size;
        resp_body      = ngx_http_cache_purge_body_templ_html;
        resp_body_size = ngx_http_cache_purge_body_templ_html_size;
        break;
    }

    /*
     * Compute the rendered output length.
     *
     * resp_body_size = sizeof(template_string) which includes the NUL
     * terminator appended by the compiler to every string literal.  Each
     * body template contains exactly two "%s" format specifiers (2 bytes
     * each) that ngx_snprintf replaces with the cache key and the status
     * word respectively.  The rendered output length is therefore:
     *
     *   body_len = sizeof(template)
     *              - 1           (NUL terminator is not sent on the wire)
     *              - (2 * 2)     (two "%s" markers consumed, not emitted)
     *              + key[0].len  (first  %s expansion)
     *              + status->len (second %s expansion)
     *
     * Simplified: (resp_body_size - 5) + key.len + status.len
     *
     * ngx_snprintf writes exactly body_len bytes without a NUL terminator
     * (it stops at buf + max, exclusive).  buf is ngx_pcalloc'd to
     * body_len + 1 so the trailing zero from calloc is there for any code
     * that treats buf as a C string, but it is never sent over the wire.
     */
    body_len = (resp_body_size - 1 - 4) + key[0].len + status->len;

    r->headers_out.content_type.len  = resp_ct_size - 1;
    r->headers_out.content_type.data = (u_char *) resp_ct;

    buf = ngx_pcalloc(r->pool, body_len + 1);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* ngx_snprintf never returns NULL */
    ngx_snprintf(buf, body_len, resp_body, buf_keydata, status->data);

    r->headers_out.status           = (r->headers_out.status == NGX_HTTP_ACCEPTED)
                                      ? NGX_HTTP_ACCEPTED : NGX_HTTP_OK;
    r->headers_out.content_length_n = (off_t) body_len;

    b = ngx_create_temp_buf(r->pool, body_len);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf  = b;
    out.next = NULL;

    b->last     = ngx_cpymem(b->last, buf, body_len);
    b->last_buf = 1;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}


/* -- cache get helper (nginx >= 1.7.9) --------------------------------- */

# if (nginx_version >= 1007009)
ngx_int_t
ngx_http_cache_purge_cache_get(ngx_http_request_t *r, ngx_http_upstream_t *u,
    ngx_http_file_cache_t **cache)
{
    ngx_str_t              *name;
    ngx_str_t               val;
    ngx_uint_t              i;
    ngx_http_file_cache_t **caches;

    if (u->conf->cache_zone) {
        *cache = u->conf->cache_zone->data;
        return NGX_OK;
    }

    if (u->conf->cache_value == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_cache_purge: no cache configured for this location");
        return NGX_HTTP_NOT_FOUND;
    }

    if (ngx_http_complex_value(r, u->conf->cache_value, &val) != NGX_OK) {
        return NGX_ERROR;
    }

    if (val.len == 0
        || (val.len == 3 && ngx_strncmp(val.data, "off", 3) == 0))
    {
        return NGX_DECLINED;
    }

    caches = u->caches->elts;

    for (i = 0; i < u->caches->nelts; i++) {
        name = &caches[i]->shm_zone->shm.name;
        if (name->len == val.len
            && ngx_strncmp(name->data, val.data, val.len) == 0)
        {
            *cache = caches[i];
            return NGX_OK;
        }
    }

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "ngx_cache_purge: cache zone \"%V\" not found", &val);

    return NGX_ERROR;
}
# endif


/* -- request init ------------------------------------------------------- */

ngx_int_t
ngx_http_cache_purge_init(ngx_http_request_t *r, ngx_http_file_cache_t *cache,
    ngx_http_complex_value_t *cache_key)
{
    ngx_http_cache_t *c;
    ngx_str_t        *key;
    ngx_int_t         rc;

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    c = ngx_pcalloc(r->pool, sizeof(ngx_http_cache_t));
    if (c == NULL) {
        return NGX_ERROR;
    }

    rc = ngx_array_init(&c->keys, r->pool, 1, sizeof(ngx_str_t));
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    key = ngx_array_push(&c->keys);
    if (key == NULL) {
        return NGX_ERROR;
    }

    rc = ngx_http_complex_value(r, cache_key, key);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    r->cache      = c;
    c->body_start = ngx_pagesize;
    c->file_cache = cache;
    c->file.log   = r->connection->log;

    /* the md5/crc32 of the key are for exact purges; a wildcard matches
     * the key string itself */
    if (key->len == 0 || key->data[key->len - 1] != '*') {
        ngx_http_file_cache_create_key(r);
    }

    return NGX_OK;
}


/* -- purge dispatch ----------------------------------------------------- */

void
ngx_http_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t                         status;
    ngx_int_t                         rc;
    ngx_int_t                         not_found_code;

# if (NGX_HAVE_FILE_AIO)
    if (r->aio) {
        return;
    }
# endif

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);

    not_found_code = (cmcf != NULL && cmcf->legacy_status_codes)
                     ? NGX_HTTP_PRECONDITION_FAILED
                     : NGX_HTTP_NOT_FOUND;

    rc = ngx_http_file_cache_purge(r);

    switch (rc) {
    case NGX_OK:
        ngx_str_set(&status, "purged");
        r->write_event_handler = ngx_http_request_empty_handler;

        /*
         * Vary-aware cleanup: after deleting the primary file, walk the
         * cache directory to remove any remaining variant files (e.g. those
         * created by gzip_vary / Vary: Accept-Encoding).  All variant files
         * share the same KEY: string, which is matched exactly by the walk.
         * r->cache->file_cache is set by ngx_http_cache_purge_init().
         */
        if (cmcf != NULL && cmcf->vary_aware) {
            ngx_http_cache_purge_delete_variants(r, r->cache->file_cache);
        }

        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return;

    case NGX_DECLINED:
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "ngx_cache_purge: key \"%V\" not found in cache",
                       (ngx_str_t *) r->cache->keys.elts);
        ngx_http_finalize_request(r, not_found_code);
        return;

# if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
        r->write_event_handler = ngx_http_cache_purge_handler;
        return;
# endif

    default:
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
    }
}


/* -- file cache purge --------------------------------------------------- */

ngx_int_t
ngx_http_file_cache_purge(ngx_http_request_t *r)
{
    ngx_http_file_cache_t *cache;
    ngx_http_cache_t      *c;

    switch (ngx_http_file_cache_open(r)) {
    case NGX_OK:
    case NGX_HTTP_CACHE_STALE:
# if (nginx_version >= 8001) \
     || ((nginx_version < 8000) && (nginx_version >= 7060))
    case NGX_HTTP_CACHE_UPDATING:
# endif
        break;

    case NGX_DECLINED:
        return NGX_DECLINED;

# if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
        return NGX_AGAIN;
# endif

    default:
        return NGX_ERROR;
    }

    c     = r->cache;
    cache = c->file_cache;

    ngx_shmtx_lock(&cache->shpool->mutex);

    if (!c->node->exists) {
        ngx_shmtx_unlock(&cache->shpool->mutex);
        return NGX_DECLINED;
    }

# if (nginx_version >= 1000001)
    cache->sh->size -= c->node->fs_size;
    c->node->fs_size  = 0;
# else
    cache->sh->size -= (c->node->length + cache->bsize - 1) / cache->bsize;
    c->node->length   = 0;
# endif

    c->node->exists = 0;
# if (nginx_version >= 8001) \
     || ((nginx_version < 8000) && (nginx_version >= 7060))
    c->node->updating = 0;
# endif

    ngx_shmtx_unlock(&cache->shpool->mutex);

    if (ngx_delete_file(c->file.name.data) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                      "ngx_cache_purge: could not delete \"%V\"", &c->file.name);
    } else {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "ngx_cache_purge: deleted \"%V\"", &c->file.name);
    }

    return NGX_OK;
}


/* -- bulk walk helpers -------------------------------------------------- */

void
ngx_http_cache_purge_all(ngx_http_request_t *r, ngx_http_file_cache_t *cache)
{
    ngx_str_t  all = ngx_null_string;

    (void) ngx_http_cache_purge_scan_sync(r, cache, &all);
}

ngx_uint_t
ngx_http_cache_purge_partial(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache)
{
    ngx_str_t  *key, pattern;

    key = r->cache->keys.elts;

    pattern = key[0];

    if (pattern.len > 0 && pattern.data[pattern.len - 1] == '*') {
        pattern.len--;
    }

    return ngx_http_cache_purge_scan_sync(r, cache, &pattern);
}

ngx_int_t
ngx_http_cache_purge_is_partial(ngx_http_request_t *r)
{
    ngx_http_cache_t *c   = r->cache;
    ngx_str_t        *key = c->keys.elts;

    return c->keys.nelts > 0
        && key[0].len > 0
        && key[0].data[key[0].len - 1] == '*';
}


/* -- configuration parser ----------------------------------------------- */

char *
ngx_http_cache_purge_conf(ngx_conf_t *cf, ngx_http_cache_purge_conf_t *cpcf)
{
    ngx_cidr_t      cidr;
    ngx_in_cidr_t  *access;
# if (NGX_HAVE_INET6)
    ngx_in6_cidr_t *access6;
# endif
    ngx_str_t      *value;
    ngx_int_t       rc;
    ngx_uint_t      i, from_position;

    from_position = 2;
    value         = cf->args->elts;

    if (ngx_strcmp(value[1].data, "off") == 0) {
        cpcf->enable = 0;
        return NGX_CONF_OK;

    } else if (ngx_strcmp(value[1].data, "on") == 0) {
        ngx_str_set(&cpcf->method, "PURGE");
    } else {
        cpcf->method = value[1];
    }

    if (cf->args->nelts < 4) {
        cpcf->enable = 1;
        return NGX_CONF_OK;
    }

    if (ngx_strcmp(value[from_position].data, "purge_all") == 0) {
        cpcf->purge_all = 1;
        from_position++;
    }

    if (ngx_strcmp(value[from_position].data, "from") != 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "invalid parameter \"%V\", expected \"from\" keyword",
            &value[from_position]);
        return NGX_CONF_ERROR;
    }

    if (ngx_strcmp(value[from_position + 1].data, "all") == 0) {
        cpcf->enable = 1;
        return NGX_CONF_OK;
    }

    for (i = from_position + 1; i < cf->args->nelts; i++) {
        rc = ngx_ptocidr(&value[i], &cidr);

        if (rc == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "invalid parameter \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }

        if (rc == NGX_DONE) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                "low address bits of %V are meaningless", &value[i]);
        }

        switch (cidr.family) {
        case AF_INET:
            if (cpcf->access == NULL) {
                cpcf->access = ngx_array_create(cf->pool,
                    cf->args->nelts - (from_position + 1),
                    sizeof(ngx_in_cidr_t));
                if (cpcf->access == NULL) {
                    return NGX_CONF_ERROR;
                }
            }

            access = ngx_array_push(cpcf->access);
            if (access == NULL) {
                return NGX_CONF_ERROR;
            }

            access->mask = cidr.u.in.mask;
            access->addr = cidr.u.in.addr;
            break;

# if (NGX_HAVE_INET6)
        case AF_INET6:
            if (cpcf->access6 == NULL) {
                cpcf->access6 = ngx_array_create(cf->pool,
                    cf->args->nelts - (from_position + 1),
                    sizeof(ngx_in6_cidr_t));
                if (cpcf->access6 == NULL) {
                    return NGX_CONF_ERROR;
                }
            }

            access6 = ngx_array_push(cpcf->access6);
            if (access6 == NULL) {
                return NGX_CONF_ERROR;
            }

            access6->mask = cidr.u.in6.mask;
            access6->addr = cidr.u.in6.addr;
            break;
# endif
        }
    }

    cpcf->enable = 1;

    return NGX_CONF_OK;
}


/* -- location configuration --------------------------------------------- */

static void
ngx_http_cache_purge_merge_conf(ngx_http_cache_purge_conf_t *conf,
    ngx_http_cache_purge_conf_t *prev)
{
    if (conf->enable == NGX_CONF_UNSET) {
        if (prev->enable == 1) {
            conf->enable    = prev->enable;
            conf->method    = prev->method;
            conf->purge_all = prev->purge_all;
            conf->access    = prev->access;
            conf->access6   = prev->access6;
        } else {
            conf->enable = 0;
        }
    }
}

void *
ngx_http_cache_purge_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_cache_purge_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cache_purge_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *   conf->*.method         = { 0, NULL }
     *   conf->*.access         = NULL
     *   conf->*.access6        = NULL
     *   conf->handler          = NULL
     *   conf->original_handler = NULL
     */

# if (NGX_HTTP_FASTCGI)
    conf->fastcgi.enable = NGX_CONF_UNSET;
# endif
# if (NGX_HTTP_PROXY)
    conf->proxy.enable   = NGX_CONF_UNSET;
# endif
# if (NGX_HTTP_SCGI)
    conf->scgi.enable    = NGX_CONF_UNSET;
# endif
# if (NGX_HTTP_UWSGI)
    conf->uwsgi.enable   = NGX_CONF_UNSET;
# endif

    conf->response_type = NGX_CONF_UNSET_UINT;
    conf->conf     = NGX_CONF_UNSET_PTR;

    return conf;
}

char *
ngx_http_cache_purge_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_cache_purge_loc_conf_t *prev = parent;
    ngx_http_cache_purge_loc_conf_t *conf = child;
    ngx_http_core_loc_conf_t        *clcf;
    /*
     * C89: all variables at top of function.
     *
     * was_set_* captures whether each protocol's purge directive was
     * explicitly present in THIS location block BEFORE merging from the
     * parent.  Together with clcf->noname it distinguishes three cases:
     *
     *   Case A -- explicit (enable == 1 before merge):
     *     proxy_cache_purge is in this location.  clcf->handler is the
     *     real upstream handler (e.g. ngx_http_proxy_handler set by
     *     proxy_pass).  Save it as original_handler and install ours.
     *
     *   Case B -- inherited into a named location (enable ==
     *     NGX_CONF_UNSET, clcf->noname == 0):
     *     proxy_cache_purge is set at the server level or in an enclosing
     *     location.  clcf->handler is this location's own handler, as in
     *     case A.
     *
     *   Case C -- inherited into an anonymous location (enable ==
     *     NGX_CONF_UNSET, clcf->noname == 1):
     *     This is an anonymous if-child location synthesised by nginx when
     *     it encounters an "if" block.  The if-block has no handler
     *     directive, so clcf->handler is NULL.  Saving NULL as
     *     original_handler causes every non-PURGE request that enters the
     *     if-branch to return 404 instead of reaching the upstream.
     *     Inherit original_handler from prev (which holds the real handler
     *     saved during the parent location's merge) instead.
     *
     * In both cases clcf->handler must be set to access_handler so that
     * PURGE requests are intercepted regardless of whether the if condition
     * fires.
     */
# if (NGX_HTTP_FASTCGI)
    ngx_flag_t  was_set_fastcgi;
# endif
# if (NGX_HTTP_PROXY)
    ngx_flag_t  was_set_proxy;
# endif
# if (NGX_HTTP_SCGI)
    ngx_flag_t  was_set_scgi;
# endif
# if (NGX_HTTP_UWSGI)
    ngx_flag_t  was_set_uwsgi;
# endif

# if (NGX_HTTP_FASTCGI)
    was_set_fastcgi = (conf->fastcgi.enable == 1);
# endif
# if (NGX_HTTP_PROXY)
    was_set_proxy   = (conf->proxy.enable   == 1);
# endif
# if (NGX_HTTP_SCGI)
    was_set_scgi    = (conf->scgi.enable    == 1);
# endif
# if (NGX_HTTP_UWSGI)
    was_set_uwsgi   = (conf->uwsgi.enable   == 1);
# endif

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    ngx_conf_merge_uint_value(conf->response_type, prev->response_type,
                              NGX_CACHE_PURGE_RESPONSE_TYPE_HTML);

# if (NGX_HTTP_FASTCGI)
    ngx_http_cache_purge_merge_conf(&conf->fastcgi, &prev->fastcgi);

    if (conf->fastcgi.enable) {
        conf->conf             = &conf->fastcgi;
        conf->handler          = ngx_http_fastcgi_cache_purge_handler;
        conf->original_handler = (was_set_fastcgi || !clcf->noname)
                                 ? clcf->handler
                                 : prev->original_handler;
        clcf->handler          = ngx_http_cache_purge_access_handler;
        return NGX_CONF_OK;
    }
# endif

# if (NGX_HTTP_PROXY)
    if (conf->proxy.enable == NGX_CONF_UNSET
        && conf->proxy_separate_zone == NULL
        && conf->proxy_separate_value == NULL)
    {
        conf->proxy_separate_zone  = prev->proxy_separate_zone;
        conf->proxy_separate_value = prev->proxy_separate_value;
        conf->proxy_separate_key   = prev->proxy_separate_key;
    }

    ngx_http_cache_purge_merge_conf(&conf->proxy, &prev->proxy);

    if (conf->proxy.enable) {
        conf->conf             = &conf->proxy;
        conf->handler          = ngx_http_proxy_cache_purge_handler;
        conf->original_handler = (was_set_proxy || !clcf->noname)
                                 ? clcf->handler
                                 : prev->original_handler;
        clcf->handler          = ngx_http_cache_purge_access_handler;
        return NGX_CONF_OK;
    }
# endif

# if (NGX_HTTP_SCGI)
    ngx_http_cache_purge_merge_conf(&conf->scgi, &prev->scgi);

    if (conf->scgi.enable) {
        conf->conf             = &conf->scgi;
        conf->handler          = ngx_http_scgi_cache_purge_handler;
        conf->original_handler = (was_set_scgi || !clcf->noname)
                                 ? clcf->handler
                                 : prev->original_handler;
        clcf->handler          = ngx_http_cache_purge_access_handler;
        return NGX_CONF_OK;
    }
# endif

# if (NGX_HTTP_UWSGI)
    ngx_http_cache_purge_merge_conf(&conf->uwsgi, &prev->uwsgi);

    if (conf->uwsgi.enable) {
        conf->conf             = &conf->uwsgi;
        conf->handler          = ngx_http_uwsgi_cache_purge_handler;
        conf->original_handler = (was_set_uwsgi || !clcf->noname)
                                 ? clcf->handler
                                 : prev->original_handler;
        clcf->handler          = ngx_http_cache_purge_access_handler;
        return NGX_CONF_OK;
    }
# endif

    ngx_conf_merge_ptr_value(conf->conf, prev->conf, NULL);

    if (conf->handler == NULL) {
        conf->handler = prev->handler;
    }

    if (conf->original_handler == NULL) {
        conf->original_handler = prev->original_handler;
    }

    return NGX_CONF_OK;
}


#else /* !NGX_HTTP_CACHE */

static ngx_http_module_t  ngx_http_cache_purge_module_ctx = {
    NULL, NULL,   /* pre/postconfiguration  */
    NULL, NULL,   /* create/init main conf  */
    NULL, NULL,   /* create/merge srv conf  */
    NULL, NULL    /* create/merge loc conf  */
};

ngx_module_t  ngx_http_cache_purge_module = {
    NGX_MODULE_V1,
    &ngx_http_cache_purge_module_ctx,
    NULL,
    NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};

#endif /* NGX_HTTP_CACHE */
