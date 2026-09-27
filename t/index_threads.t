# t/index_threads.t -- cache_purge_index with cache_purge_thread_pool
#
# master_on: nginx cannot take on a thread pool on a reload in single
# process mode (it hangs, with or without this module), and Test::Nginx
# reloads between blocks.
use Test::Nginx::Socket 'no_plan';
use Cwd qw(cwd);

master_on();
workers(2);

my $pwd  = cwd();
my $port = server_port();

# Two cache zones, each with its own index, and the disk work in the
# thread pool.
our $HttpConfigThreads = qq{
    proxy_cache_path $pwd/cache_idx_a levels=1:2 keys_zone=idx_a:10m
                     inactive=60m use_temp_path=off;
    proxy_cache_path $pwd/cache_idx_b levels=1:2 keys_zone=idx_b:10m
                     inactive=60m use_temp_path=off;
    cache_purge_background_queue  on;
    cache_purge_throttle_ms       10ms;
    cache_purge_index             8m idx_b=2m;
    cache_purge_index_sync_limit  2;
    cache_purge_thread_pool       default tasks=2;
    upstream backend {
        server 127.0.0.1:$port;
    }
};

our $LocationsThreads = q{
    location /a {
        proxy_pass http://backend/origin;
        proxy_cache idx_a;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1h;
        proxy_cache_purge PURGE from 127.0.0.1;
        add_header X-Cache $upstream_cache_status;
    }
    location /b {
        proxy_pass http://backend/origin;
        proxy_cache idx_b;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1h;
        proxy_cache_purge PURGE from 127.0.0.1;
        add_header X-Cache $upstream_cache_status;
    }
    location /origin {
        return 200 "ok";
    }
};

# Wait until the indexes of both zones are built: while a build runs a
# wildcard is answered 202 (it reaches the files not walked yet), which is
# right but not what these tests look at.
sub wait_index_ready {
    my $log = server_root() . "/logs/error.log";
    for (1 .. 100) {
        if (open my $fh, "<", $log) {
            local $/;
            my $text = <$fh>;
            close $fh;
            return if $text =~ /build of "idx_a" complete/
                      && $text =~ /build of "idx_b" complete/;
        }
        select undef, undef, undef, 0.1;
    }
}

$ENV{TEST_NGINX_SERVROOT} = server_root();
no_long_string();
run_tests();

__DATA__

=== TEST 1: with a thread pool, an indexed wildcard is purged before the answer (200)
--- http_config eval: $::HttpConfigThreads
--- config eval: $::LocationsThreads
--- init: main::wait_index_ready()
--- request eval
["GET /a/t/1", "GET /a/t/1", "PURGE /a/t/*", "GET /a/t/1"]
--- response_headers_like eval
["X-Cache: MISS", "X-Cache: HIT", "", "X-Cache: MISS"]
--- error_code eval
[200, 200, 200, 200]
--- wait: 0.5
--- no_error_log
[alert]
[crit]

=== TEST 2: each cache zone has its own index: a purge of one leaves the other
--- http_config eval: $::HttpConfigThreads
--- config eval: $::LocationsThreads
--- init: main::wait_index_ready()
--- request eval
["GET /a/z/1", "GET /b/z/1", "PURGE /b/z/*", "GET /a/z/1", "GET /b/z/1"]
--- response_headers_like eval
["X-Cache: (MISS|HIT)", "X-Cache: (MISS|HIT)", "", "X-Cache: HIT", "X-Cache: MISS"]
--- error_code eval
[200, 200, 200, 200, 200]
--- wait: 0.5

=== TEST 3: a wildcard that matches nothing, answered from the index
--- http_config eval: $::HttpConfigThreads
--- config eval: $::LocationsThreads
--- init: main::wait_index_ready()
--- request eval
["GET /a/n/1", "PURGE /a/none/*", "PURGE /a/none/*", "GET /a/n/1"]
--- response_headers_like eval
["X-Cache: (MISS|HIT)", "", "", "X-Cache: HIT"]
--- error_code eval
[200, 412, 412, 200]
--- wait: 0.5

=== TEST 4: more matches than the sync limit, with a thread pool (202, then gone)
--- http_config eval: $::HttpConfigThreads
--- config eval: $::LocationsThreads
--- init: main::wait_index_ready()
--- request eval
["GET /a/m/1", "GET /a/m/2", "GET /a/m/3", "GET /a/m/4", "PURGE /a/m/*"]
--- error_code eval
[200, 200, 200, 200, 202]
--- wait: 0.5
