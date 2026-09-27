/*
 * cachegen -- seed and inspect nginx proxy cache directories for the e2e
 * suite.  Built against the nginx source tree the module is built with, so
 * the on-disk header (ngx_http_file_cache_header_t, NGX_HTTP_CACHE_VERSION)
 * is exactly what that nginx reads.
 *
 *   cachegen seed  --out DIR --files N --bytes B [--sparse] [--seed S]
 *                  [--threads T] [--variants V] [--tenants T]
 *                  [--video-permille P] [--video-avg BYTES]
 *       Writes N cache files for the deterministic key set below into
 *       DIR/cache/images and DIR/cache/videos (levels=1:2) and the parameters
 *       to DIR/seed.json.
 *
 *   cachegen scan  --dir D [--prefix P]... [--keys] [--threads T]
 *       Counts cache files (32 hex digit names) under D and, per prefix, the
 *       ones whose KEY starts with it (case-insensitive, like the module).
 *       Prints JSON.  --keys also prints every matching key, one per line,
 *       to stderr.
 *
 *   cachegen key   --i N [--variants V] [--tenants T] [--video-permille P]
 *       Prints the key of file N (for cross-checking the harness).
 *
 * The key set (mirrored in harness/keys.py and harness/load.lua):
 *   file i: asset a = i / V, variant v = i % V, tenant t = a % T
 *   video asset when (a * 7919) % 1000 < P:
 *       /video/t<t>/a<a>/s<6 + v % 4>.mp4?r=<v>          (zone videos)
 *   else
 *       /cdn/t<t>/a<a>/s<v % 6>.jpg?w=<160 * (v + 1)>    (zone images)
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <pthread.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <zlib.h>
#include <openssl/evp.h>


typedef struct {
    uint64_t   files;
    uint64_t   bytes;
    uint64_t   seed;
    unsigned   variants;
    unsigned   tenants;
    unsigned   video_permille;
    uint64_t   video_avg;
    uint64_t   image_avg;
    int        sparse;
    int        threads;
    const char *out;
} seed_conf_t;

static seed_conf_t  sc = {
    .files = 100000, .bytes = 0, .seed = 1, .variants = 8, .tenants = 64,
    .video_permille = 1001, .video_avg = 6u << 20, .threads = 8,
};

static atomic_uint_fast64_t  seed_next;
static atomic_uint_fast64_t  seed_done_bytes;
static atomic_uint_fast64_t  seed_done_files;
static atomic_uint_fast64_t  seed_errors;


static uint64_t
splitmix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}


static int
is_video_asset(uint64_t a, unsigned permille)
{
    return (a * 7919) % 1000 < permille;
}


/* key of file i into buf; returns its length; *video set for the videos zone */
static size_t
make_key(uint64_t i, unsigned variants, unsigned tenants, unsigned permille,
    char *buf, size_t size, int *video, unsigned *bucket)
{
    uint64_t  a = i / variants;
    unsigned  v = (unsigned) (i % variants);
    unsigned  t = (unsigned) (a % tenants);
    int       n;

    if (is_video_asset(a, permille)) {
        *video = 1;
        *bucket = 6 + v % 4;
        n = snprintf(buf, size, "/video/t%u/a%llu/s%u.mp4?r=%u", t,
                     (unsigned long long) a, *bucket, v);
    } else {
        *video = 0;
        *bucket = v % 6;
        n = snprintf(buf, size, "/cdn/t%u/a%llu/s%u.jpg?w=%u", t,
                     (unsigned long long) a, *bucket, 160 * (v + 1));
    }

    return (size_t) n;
}


static void
md5_hex(const char *data, size_t len, char *hex)
{
    static const char  digits[] = "0123456789abcdef";
    unsigned char      md[16];
    unsigned int       mdlen;
    EVP_MD_CTX        *ctx;
    int                k;

    ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, md, &mdlen);
    EVP_MD_CTX_free(ctx);

    for (k = 0; k < 16; k++) {
        hex[2 * k] = digits[md[k] >> 4];
        hex[2 * k + 1] = digits[md[k] & 0xf];
    }
    hex[32] = '\0';
}


static int
mkdir_p(const char *path)
{
    char   tmp[4096];
    char  *p;

    snprintf(tmp, sizeof(tmp), "%s", path);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) == -1 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) == -1 && errno != EEXIST) {
        return -1;
    }

    return 0;
}


static uint64_t
file_size(uint64_t i, int video)
{
    uint64_t  r = splitmix64(sc.seed ^ (i * 0x2545f4914f6cdd1dULL));
    uint64_t  avg = video ? sc.video_avg : sc.image_avg;

    /* uniform in [avg / 2, avg * 3 / 2) */
    return avg / 2 + r % (avg ? avg : 1);
}


static void *
seed_thread(void *arg)
{
    static const size_t  chunk = 1024;
    char                 key[1024], hex[33], path[4096], hdrs[512];
    u_char              *head, *body;
    size_t               klen, hlen, off, bodylen;
    uint64_t             i, end, size;
    unsigned             bucket;
    int                  video, fd;
    ngx_http_file_cache_header_t  *h;
    time_t               now = time(NULL);
    ssize_t              w;

    (void) arg;

    head = calloc(1, 8192);
    body = malloc(1 << 20);
    for (off = 0; off < (1 << 20); off++) {
        body[off] = (u_char) ('a' + off % 26);
    }

    for ( ;; ) {
        i = atomic_fetch_add(&seed_next, chunk);
        if (i >= sc.files) {
            break;
        }
        end = i + chunk < sc.files ? i + chunk : sc.files;

        for ( ; i < end; i++) {
            klen = make_key(i, sc.variants, sc.tenants, sc.video_permille,
                            key, sizeof(key), &video, &bucket);
            md5_hex(key, klen, hex);
            size = file_size(i, video);

            hlen = (size_t) snprintf(hdrs, sizeof(hdrs),
                       "HTTP/1.1 200 OK\r\n"
                       "Server: cachegen\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %llu\r\n"
                       "Cache-Control: max-age=31536000\r\n"
                       "X-Seed: %llu\r\n"
                       "\r\n",
                       video ? "video/mp4" : "image/jpeg",
                       (unsigned long long) size, (unsigned long long) i);

            memset(head, 0, sizeof(ngx_http_file_cache_header_t));
            h = (ngx_http_file_cache_header_t *) head;
            h->version = NGX_HTTP_CACHE_VERSION;
            h->valid_sec = now + 365 * 86400;
            h->updating_sec = 0;
            h->error_sec = 0;
            h->last_modified = now - 86400;
            h->date = now;
            h->crc32 = (uint32_t) crc32(0L, (const Bytef *) key, klen);
            h->valid_msec = 0;
            h->header_start = (u_short) (sizeof(ngx_http_file_cache_header_t)
                                         + sizeof("\nKEY: ") - 1 + klen + 1);
            h->body_start = (u_short) (h->header_start + hlen);

            off = sizeof(ngx_http_file_cache_header_t);
            memcpy(head + off, "\nKEY: ", 6);
            off += 6;
            memcpy(head + off, key, klen);
            off += klen;
            head[off++] = '\n';
            memcpy(head + off, hdrs, hlen);
            off += hlen;

            /* the first body bytes name the file: a HIT is recognisable */
            off += (size_t) snprintf((char *) head + off, 64, "SEED %llu\n",
                                     (unsigned long long) i);

            snprintf(path, sizeof(path), "%s/cache/%s/%c/%c%c/%s", sc.out,
                     video ? "videos" : "images", hex[31], hex[29], hex[30],
                     hex);

            fd = open(path, O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0644);
            if (fd == -1) {
                atomic_fetch_add(&seed_errors, 1);
                continue;
            }

            w = write(fd, head, off);
            bodylen = off - h->body_start;      /* bytes of body written */

            if (w != (ssize_t) off) {
                atomic_fetch_add(&seed_errors, 1);
            } else if (sc.sparse) {
                if (ftruncate(fd, (off_t) (h->body_start + size)) == -1) {
                    atomic_fetch_add(&seed_errors, 1);
                }
            } else {
                while (bodylen < size) {
                    size_t n = size - bodylen;
                    if (n > (1 << 20)) {
                        n = 1 << 20;
                    }
                    w = write(fd, body, n);
                    if (w <= 0) {
                        atomic_fetch_add(&seed_errors, 1);
                        break;
                    }
                    bodylen += (size_t) w;
                }
            }

            close(fd);
            atomic_fetch_add(&seed_done_bytes, size);
            atomic_fetch_add(&seed_done_files, 1);
        }
    }

    free(head);
    free(body);
    return NULL;
}


static int
cmd_seed(void)
{
    char        path[4096];
    const char *zones[] = { "images", "videos" };
    pthread_t  *tids;
    uint64_t    avg, last;
    unsigned    z, l1, l2;
    int         t;
    FILE       *f;
    time_t      start = time(NULL);

    if (sc.out == NULL) {
        fprintf(stderr, "seed: --out required\n");
        return 2;
    }

    if (sc.bytes == 0) {
        sc.bytes = sc.files * 40960;
    }

    avg = sc.bytes / (sc.files ? sc.files : 1);

    if (sc.video_permille > 1000) {
        /* auto: images average ~24 KB, the rest of the bytes are video */
        if (avg > 24576) {
            sc.video_permille = (unsigned) ((avg - 24576) * 1000
                                            / (sc.video_avg - 24576));
        } else {
            sc.video_permille = 0;
        }
        if (sc.video_permille > 200) {
            sc.video_permille = 200;
        }
    }

    sc.image_avg = (avg * 1000 > sc.video_permille * sc.video_avg)
                   ? (avg * 1000 - sc.video_permille * sc.video_avg)
                     / (1000 - sc.video_permille)
                   : 4096;
    if (sc.image_avg < 4096) {
        sc.image_avg = 4096;
    }

    for (z = 0; z < 2; z++) {
        for (l1 = 0; l1 < 16; l1++) {
            for (l2 = 0; l2 < 256; l2++) {
                snprintf(path, sizeof(path), "%s/cache/%s/%x/%02x", sc.out,
                         zones[z], l1, l2);
                if (mkdir_p(path) == -1) {
                    perror(path);
                    return 1;
                }
            }
        }
    }

    tids = calloc((size_t) sc.threads, sizeof(pthread_t));
    for (t = 0; t < sc.threads; t++) {
        pthread_create(&tids[t], NULL, seed_thread, NULL);
    }

    last = 0;
    for ( ;; ) {
        uint64_t done = atomic_load(&seed_done_files);
        if (done >= sc.files) {
            break;
        }
        if (done - last >= sc.files / 20 + 1) {
            fprintf(stderr, "seed: %llu/%llu files, %.1f GB\n",
                    (unsigned long long) done, (unsigned long long) sc.files,
                    atomic_load(&seed_done_bytes) / 1e9);
            last = done;
        }
        /* threads that hit errors still count down via seed_next */
        if (atomic_load(&seed_next) >= sc.files) {
            break;
        }
        usleep(200000);
    }

    for (t = 0; t < sc.threads; t++) {
        pthread_join(tids[t], NULL);
    }

    snprintf(path, sizeof(path), "%s/seed.json", sc.out);
    f = fopen(path, "w");
    if (f == NULL) {
        perror(path);
        return 1;
    }
    fprintf(f, "{\"files\": %llu, \"bytes_target\": %llu, \"bytes\": %llu, "
               "\"seed\": %llu, \"variants\": %u, \"tenants\": %u, "
               "\"video_permille\": %u, \"video_avg\": %llu, "
               "\"image_avg\": %llu, \"sparse\": %d, \"errors\": %llu, "
               "\"header_size\": %zu, \"cache_version\": %d, "
               "\"seconds\": %ld}\n",
            (unsigned long long) sc.files, (unsigned long long) sc.bytes,
            (unsigned long long) atomic_load(&seed_done_bytes),
            (unsigned long long) sc.seed, sc.variants, sc.tenants,
            sc.video_permille, (unsigned long long) sc.video_avg,
            (unsigned long long) sc.image_avg, sc.sparse,
            (unsigned long long) atomic_load(&seed_errors),
            sizeof(ngx_http_file_cache_header_t), NGX_HTTP_CACHE_VERSION,
            (long) (time(NULL) - start));
    fclose(f);

    fprintf(stderr, "seed: done, %llu files, %.2f GB, %llu errors, %lds\n",
            (unsigned long long) atomic_load(&seed_done_files),
            atomic_load(&seed_done_bytes) / 1e9,
            (unsigned long long) atomic_load(&seed_errors),
            (long) (time(NULL) - start));

    return atomic_load(&seed_errors) ? 1 : 0;
}


/* -- scan ----------------------------------------------------------------- */

#define SCAN_MAX_PREFIXES  256

static char        *scan_prefixes[SCAN_MAX_PREFIXES];
static size_t       scan_prefix_len[SCAN_MAX_PREFIXES];
static int          scan_nprefixes;
static int          scan_keys;
static int          scan_threads = 8;
static char       **scan_dirs;
static size_t       scan_ndirs, scan_cap;
static atomic_size_t        scan_next;
static atomic_uint_fast64_t scan_files;
static atomic_uint_fast64_t scan_bytes;
static atomic_uint_fast64_t scan_unreadable;
static atomic_uint_fast64_t scan_matches[SCAN_MAX_PREFIXES];
static pthread_mutex_t      scan_out_lock = PTHREAD_MUTEX_INITIALIZER;


static void
scan_add_dir(const char *path)
{
    if (scan_ndirs == scan_cap) {
        scan_cap = scan_cap ? scan_cap * 2 : 8192;
        scan_dirs = realloc(scan_dirs, scan_cap * sizeof(char *));
    }
    scan_dirs[scan_ndirs++] = strdup(path);
}


/* every directory to depth 3 below root, root included */
static void
scan_collect(const char *path, int depth)
{
    DIR            *d;
    struct dirent  *de;
    char            sub[4096];

    scan_add_dir(path);

    if (depth == 3) {
        return;
    }

    d = opendir(path);
    if (d == NULL) {
        return;
    }

    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        if (de->d_type == DT_DIR) {
            snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
            scan_collect(sub, depth + 1);
        }
    }

    closedir(d);
}


static int
is_hex32(const char *s)
{
    int  k;

    for (k = 0; k < 32; k++) {
        if (!((s[k] >= '0' && s[k] <= '9') || (s[k] >= 'a' && s[k] <= 'f'))) {
            return 0;
        }
    }

    return s[32] == '\0';
}


static void *
scan_thread(void *arg)
{
    DIR            *d;
    struct dirent  *de;
    struct stat     st;
    char            buf[1024];
    size_t          k, len;
    ssize_t         n;
    int             fd, p;
    char           *nl;
    uint64_t        files, bytes;

    (void) arg;

    for ( ;; ) {
        k = atomic_fetch_add(&scan_next, 1);
        if (k >= scan_ndirs) {
            break;
        }

        d = opendir(scan_dirs[k]);
        if (d == NULL) {
            continue;
        }

        files = 0;
        bytes = 0;

        while ((de = readdir(d)) != NULL) {
            if (de->d_type != DT_REG || !is_hex32(de->d_name)) {
                continue;
            }

            fd = openat(dirfd(d), de->d_name, O_RDONLY|O_CLOEXEC);
            if (fd == -1) {
                continue;               /* deleted meanwhile */
            }

            files++;
            if (fstat(fd, &st) == 0) {
                bytes += (uint64_t) st.st_size;
            }

            if (scan_nprefixes == 0 && !scan_keys) {
                close(fd);
                continue;
            }

            n = pread(fd, buf, sizeof(buf) - 1,
                      sizeof(ngx_http_file_cache_header_t) + 6);
            close(fd);

            if (n <= 0) {
                atomic_fetch_add(&scan_unreadable, 1);
                continue;
            }

            buf[n] = '\0';
            nl = memchr(buf, '\n', (size_t) n);
            len = nl ? (size_t) (nl - buf) : (size_t) n;

            for (p = 0; p < scan_nprefixes; p++) {
                if (len >= scan_prefix_len[p]
                    && strncasecmp(buf, scan_prefixes[p], scan_prefix_len[p])
                       == 0)
                {
                    atomic_fetch_add(&scan_matches[p], 1);

                    if (scan_keys) {
                        pthread_mutex_lock(&scan_out_lock);
                        fprintf(stderr, "%.*s\n", (int) len, buf);
                        pthread_mutex_unlock(&scan_out_lock);
                    }
                }
            }

            if (scan_keys && scan_nprefixes == 0) {
                pthread_mutex_lock(&scan_out_lock);
                fprintf(stderr, "%.*s\n", (int) len, buf);
                pthread_mutex_unlock(&scan_out_lock);
            }
        }

        closedir(d);
        atomic_fetch_add(&scan_files, files);
        atomic_fetch_add(&scan_bytes, bytes);
    }

    return NULL;
}


static void
json_str(const char *s)
{
    putchar('"');
    for ( ; *s; s++) {
        if (*s == '"' || *s == '\\') {
            putchar('\\');
        }
        putchar(*s);
    }
    putchar('"');
}


static int
cmd_scan(const char **dirs, int ndirs)
{
    pthread_t  *tids;
    int         t, p;

    for (t = 0; t < ndirs; t++) {
        scan_collect(dirs[t], 0);
    }

    tids = calloc((size_t) scan_threads, sizeof(pthread_t));
    for (t = 0; t < scan_threads; t++) {
        pthread_create(&tids[t], NULL, scan_thread, NULL);
    }
    for (t = 0; t < scan_threads; t++) {
        pthread_join(tids[t], NULL);
    }

    printf("{\"files\": %llu, \"bytes\": %llu, \"unreadable\": %llu, "
           "\"matches\": {",
           (unsigned long long) atomic_load(&scan_files),
           (unsigned long long) atomic_load(&scan_bytes),
           (unsigned long long) atomic_load(&scan_unreadable));

    for (p = 0; p < scan_nprefixes; p++) {
        if (p) {
            printf(", ");
        }
        json_str(scan_prefixes[p]);
        printf(": %llu", (unsigned long long) atomic_load(&scan_matches[p]));
    }

    printf("}}\n");

    return 0;
}


static void
usage(void)
{
    fprintf(stderr,
        "usage: cachegen seed --out DIR --files N [--bytes B] [--sparse]\n"
        "                     [--seed S] [--threads T] [--variants V]\n"
        "                     [--tenants T] [--video-permille P]\n"
        "                     [--video-avg BYTES]\n"
        "       cachegen scan --dir D [--dir D2] [--prefix P]... [--keys]\n"
        "                     [--threads T]\n"
        "       cachegen key --i N [--variants V] [--tenants T]\n"
        "                    [--video-permille P]\n"
        "       cachegen info\n");
}


int
main(int argc, char **argv)
{
    const char  *dirs[16];
    int          ndirs = 0, a;
    uint64_t     idx = 0;
    char         key[1024];
    int          video;
    unsigned     bucket;

    if (argc < 2) {
        usage();
        return 2;
    }

    for (a = 2; a < argc; a++) {
        const char *o = argv[a];
        const char *v = (a + 1 < argc) ? argv[a + 1] : NULL;

#define NEXT()  do { if (v == NULL) { usage(); return 2; } a++; } while (0)

        if (strcmp(o, "--out") == 0) { NEXT(); sc.out = v; }
        else if (strcmp(o, "--files") == 0) { NEXT(); sc.files = strtoull(v, NULL, 10); }
        else if (strcmp(o, "--bytes") == 0) { NEXT(); sc.bytes = strtoull(v, NULL, 10); }
        else if (strcmp(o, "--seed") == 0) { NEXT(); sc.seed = strtoull(v, NULL, 10); }
        else if (strcmp(o, "--threads") == 0) { NEXT(); sc.threads = scan_threads = atoi(v); }
        else if (strcmp(o, "--variants") == 0) { NEXT(); sc.variants = (unsigned) atoi(v); }
        else if (strcmp(o, "--tenants") == 0) { NEXT(); sc.tenants = (unsigned) atoi(v); }
        else if (strcmp(o, "--video-permille") == 0) { NEXT(); sc.video_permille = (unsigned) atoi(v); }
        else if (strcmp(o, "--video-avg") == 0) { NEXT(); sc.video_avg = strtoull(v, NULL, 10); }
        else if (strcmp(o, "--sparse") == 0) { sc.sparse = 1; }
        else if (strcmp(o, "--dir") == 0) { NEXT(); if (ndirs < 16) dirs[ndirs++] = v; }
        else if (strcmp(o, "--prefix") == 0) {
            NEXT();
            if (scan_nprefixes < SCAN_MAX_PREFIXES) {
                scan_prefixes[scan_nprefixes] = (char *) v;
                scan_prefix_len[scan_nprefixes++] = strlen(v);
            }
        }
        else if (strcmp(o, "--keys") == 0) { scan_keys = 1; }
        else if (strcmp(o, "--i") == 0) { NEXT(); idx = strtoull(v, NULL, 10); }
        else { usage(); return 2; }
    }

    if (sc.threads < 1) {
        sc.threads = scan_threads = 1;
    }

    if (strcmp(argv[1], "seed") == 0) {
        return cmd_seed();
    }

    if (strcmp(argv[1], "scan") == 0) {
        if (ndirs == 0) {
            usage();
            return 2;
        }
        return cmd_scan(dirs, ndirs);
    }

    if (strcmp(argv[1], "key") == 0) {
        if (sc.video_permille > 1000) {
            sc.video_permille = 0;
        }
        make_key(idx, sc.variants, sc.tenants, sc.video_permille, key,
                 sizeof(key), &video, &bucket);
        printf("%s %s %u\n", key, video ? "videos" : "images", bucket);
        return 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        printf("{\"header_size\": %zu, \"cache_version\": %d, "
               "\"key_offset\": %zu}\n",
               sizeof(ngx_http_file_cache_header_t), NGX_HTTP_CACHE_VERSION,
               sizeof(ngx_http_file_cache_header_t) + 6);
        return 0;
    }

    usage();
    return 2;
}
