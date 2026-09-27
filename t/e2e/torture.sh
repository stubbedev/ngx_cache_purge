#!/usr/bin/env bash
# Fault-injection torture scenarios and the lock-free read hammer, in the
# e2e image (see t/e2e/README.md).
#
#   t/e2e/torture.sh                   # every scenario, thread pool and inline
#   t/e2e/torture.sh s_gdb_crash       # some of them (function names in tor.py)
#   t/e2e/torture.sh --seq             # the lock-free read hammer
#
# Environment: NGINX_VERSION, E2E_SANITIZE=asan, MODE=threads|inline (one
# mode only), RL (descriptor limit for the fd-exhaustion scenario), DUR
# (seconds, --seq).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

version=${NGINX_VERSION:-1.30.5}
sanitize=${E2E_SANITIZE:-}
image=${E2E_IMAGE:-ngx-cache-purge-e2e}:${version}${sanitize:+-$sanitize}

docker build -q -f t/e2e/Dockerfile \
    --build-arg NGINX_VERSION="$version" \
    --build-arg SANITIZE="$sanitize" \
    -t "$image" . >/dev/null

script=tor.py
if [ "${1:-}" = "--seq" ]; then
    script=seq.py
    shift
fi

exec docker run --rm --init --cap-add SYS_PTRACE \
    --ulimit nofile=65536:65536 \
    -e MODE="${MODE:-}" -e RL="${RL:-60}" -e DUR="${DUR:-60}" \
    -e ASAN_OPTIONS="detect_leaks=0:log_path=/tmp/asan" \
    -v "$root/t/e2e/torture":/torture:ro \
    --entrypoint python3 "$image" -u /torture/$script "$@"
