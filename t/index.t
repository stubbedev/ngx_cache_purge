# t/index.t -- cache_purge_index: wildcard purges answered from the key index
use Test::Nginx::Socket 'no_plan';
use Cwd qw(cwd);

my $pwd  = cwd();
my $port = server_port();

our $HttpConfig = qq{
    proxy_cache_path $pwd/cache_idx levels=1:2 keys_zone=idx_zone:10m
                     inactive=60m use_temp_path=off;
    cache_purge_background_queue  on;
    cache_purge_throttle_ms       10ms;
    cache_purge_index             4m;
    cache_purge_index_sync_limit  2;
    upstream backend {
        server 127.0.0.1:$port;
    }
};

$ENV{TEST_NGINX_SERVROOT} = server_root();
no_long_string();
run_tests();

__DATA__

=== TEST 1: an indexed wildcard is purged synchronously (200), then MISS
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache idx_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1h;
        proxy_cache_purge PURGE from 127.0.0.1;
        add_header X-Cache $upstream_cache_status;
    }
    location /origin {
        return 200 "ok";
    }
--- request eval
["GET /cache/a/1", "GET /cache/a/1", "PURGE /cache/A/*", "GET /cache/a/1"]
--- response_headers_like eval
["X-Cache: MISS", "X-Cache: HIT", "", "X-Cache: MISS"]
--- error_code eval
[200, 200, 200, 200]
--- wait: 0.5

=== TEST 2: an indexed wildcard with nothing cached answers not-found
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache idx_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "ok";
    }
--- request eval
["GET /cache/warm", "PURGE /cache/nothing/*"]
--- error_code eval
[200, 412]
--- wait: 0.5

=== TEST 3: more matches than cache_purge_index_sync_limit continue in the background (202)
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache idx_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1h;
        proxy_cache_purge PURGE from 127.0.0.1;
        add_header X-Cache $upstream_cache_status;
    }
    location /origin {
        return 200 "ok";
    }
--- request eval
["GET /cache/m/1", "GET /cache/m/2", "GET /cache/m/3", "PURGE /cache/m/*"]
--- error_code eval
[200, 200, 200, 202]
--- wait: 0.5

=== TEST 4: cache_purge_index needs the background queue
--- http_config
    cache_purge_index 4m;
--- config
    location /health {
        return 200 "ok";
    }
--- must_die
--- error_log eval
qr/cache_purge_index requires cache_purge_background_queue on/


=== TEST 5: cache_purge_index names an unknown cache zone
--- http_config
    proxy_cache_path /tmp/ncp_t9 keys_zone=known:1m;
    cache_purge_background_queue on;
    cache_purge_index 4m nosuch=1m;
--- config
    location /health {
        return 200 "ok";
    }
--- must_die
--- error_log eval
qr/cache_purge_index: unknown cache zone "nosuch"/

=== TEST 6: cache_purge_thread_pool tasks= out of range
--- http_config
    cache_purge_background_queue on;
    cache_purge_thread_pool default tasks=0;
--- config
    location /health {
        return 200 "ok";
    }
--- must_die
--- error_log eval
qr/"tasks=0" must be between 1 and 64/

=== TEST 7: cache_purge_thread_pool off
--- http_config
    cache_purge_background_queue on;
    cache_purge_thread_pool off;
--- config
    location /health {
        return 200 "ok";
    }
--- request
GET /health
--- error_code: 200

=== TEST 8: an entry whose cache key is empty is purged, not skipped
# An empty key ($arg_v unset) was taken for a key copy that failed: the
# indexed purge_all skipped its file and still answered purged.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache idx_zone;
        proxy_cache_key "$arg_v";
        proxy_cache_valid 200 1h;
        proxy_cache_purge PURGE purge_all from 127.0.0.1;
        add_header X-Cache $upstream_cache_status;
    }
    location /origin {
        return 200 "ok";
    }
--- request eval
["GET /cache/e", "GET /cache/e", "PURGE /cache/e", "GET /cache/e"]
--- response_headers_like eval
["X-Cache: MISS", "X-Cache: HIT", "", "X-Cache: MISS"]
--- error_code eval
[200, 200, 200, 200]
--- wait: 0.5
