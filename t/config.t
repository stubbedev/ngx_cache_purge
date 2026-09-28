# t/config.t - Configuration validation tests
use Test::Nginx::Socket 'no_plan';
use Cwd qw(cwd);

my $pwd = cwd();
$ENV{TEST_NGINX_SERVROOT} = server_root();
no_long_string();
run_tests();

__DATA__

=== TEST 1: valid background queue configuration
--- http_config
    cache_purge_background_queue on;
    cache_purge_queue_size  2048;
    cache_purge_batch_size  20;
    cache_purge_throttle_ms 25ms;
--- config
    location /health {
        return 200 "ok";
    }
--- request
GET /health
--- error_code: 200
--- no_error_log
[error]

=== TEST 2: invalid queue size — negative value causes config error
--- http_config
    cache_purge_background_queue on;
    cache_purge_queue_size -1;
--- config
    location /health {
        return 200 "ok";
    }
--- must_die
--- error_log eval
qr/invalid number/

=== TEST 3: invalid response_type value causes config error
--- http_config
--- config
    cache_purge_response_type invalid;
    location /health {
        return 200 "ok";
    }
--- must_die
--- error_log eval
qr/invalid parameter.*expected.*html/

=== TEST 4a: Content-Type text/html for html response type
# Separate-location syntax (proxy_cache_purge zone key) - no proxy_pass needed.
# Background queue ON + wildcard URI -> 202, exercising send_response + Content-Type.
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=ct_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type html;
    location /purge { proxy_cache_purge ct_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: text/html

=== TEST 4b: Content-Type application/json for json response type
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=ct_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type json;
    location /purge { proxy_cache_purge ct_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: application/json

=== TEST 4c: Content-Type text/xml for xml response type
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=ct_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type xml;
    location /purge { proxy_cache_purge ct_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: text/xml

=== TEST 4d: Content-Type text/plain for text response type
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=ct_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type text;
    location /purge { proxy_cache_purge ct_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: text/plain

=== TEST 5a: throttle_ms with explicit ms suffix — accepted
--- http_config
    cache_purge_background_queue on;
    cache_purge_throttle_ms 10ms;
--- config
    location /health { return 200 "ok"; }
--- request
GET /health
--- error_code: 200

=== TEST 5b: throttle_ms with s suffix — 1s accepted
--- http_config
    cache_purge_background_queue on;
    cache_purge_throttle_ms 1s;
--- config
    location /health { return 200 "ok"; }
--- request
GET /health
--- error_code: 200

=== TEST 6a: response_type set in http{} context — accepted and inherited (Bug 1 + Bug 2)
# The C type includes NGX_HTTP_MAIN_CONF, so http{} is valid.
# The defunct NGX_HTTP_MODULE guard would have rejected this if it had ever fired.
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=rt_zone:1m;
    cache_purge_background_queue on;
    cache_purge_response_type json;
--- config
    location /purge { proxy_cache_purge rt_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: application/json

=== TEST 6b: duplicate response_type in server{} context causes config error (Bug 3)
# Before the fix, duplicates in server{} were silently swallowed.
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=rt2_zone:1m;
--- config eval
"cache_purge_response_type html;\n    cache_purge_response_type json;"
--- must_die
--- error_log eval
qr/is duplicate/

=== TEST 6c: response_type set in server{} context inherits into location (Bug 1)
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=rt3_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type xml;
    location /purge { proxy_cache_purge rt3_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: text/xml

=== TEST 7: "purge_all from" with no address list causes config error
# Used to read past the argument list and fall through to an open purge.
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=pf_zone:1m;
--- config
    location /cache {
        proxy_pass http://127.0.0.1:$TEST_NGINX_SERVER_PORT/origin;
        proxy_cache pf_zone;
        proxy_cache_key $uri;
        proxy_cache_purge PURGE purge_all from;
    }
--- must_die
--- error_log eval
qr/"from" must be followed by "all" or addresses/

=== TEST 8: key is HTML-escaped in the html response
# A $1 capture is the decoded URI: markup in a purge link must not reflect.
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=esc_zone:1m;
    cache_purge_background_queue on;
--- config
    location ~ ^/purge(/.*) { proxy_cache_purge esc_zone "$1"; }
--- request
GET /purge/%3Cscript%3Ealert(%22x%22)%3C/script%3E*
--- error_code: 202
--- response_body_like: <p>Key: /&lt;script&gt;alert\(&quot;x&quot;\)&lt;/script&gt;\*</p>

=== TEST 9: key is JSON-escaped in the json response
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=escj_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type json;
    location ~ ^/purge(/.*) { proxy_cache_purge escj_zone "$1"; }
--- request
GET /purge/a%22b%5Cc%01*
--- error_code: 202
--- response_body_like: ^\{"Key": "/a\\"b\\\\c\\u0001\*", "Status": "[^"]+"\}$

=== TEST 10: key is escaped in the xml response
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=escx_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type xml;
    location ~ ^/purge(/.*) { proxy_cache_purge escx_zone "$1"; }
--- request
GET /purge/x%5D%5D%3E%3Cy%3E*
--- error_code: 202
--- response_body_like: <Key>/x\]\]&gt;&lt;y&gt;\*</Key>
