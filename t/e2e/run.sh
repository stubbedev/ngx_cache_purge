#!/usr/bin/env bash
# Build the e2e image and run one profile in a container.
#
#   t/e2e/run.sh [quick|scale|chaos] [harness options...]
#
# Environment:
#   NGINX_VERSION   nginx to build (default 1.30.5)
#   E2E_SANITIZE    "asan" for an AddressSanitizer build
#   E2E_DATA_DIR    host directory for cache + origin data
#                   (default t/e2e/.data/<profile>)
#   E2E_*           passed to the harness (E2E_FILES, E2E_GB, E2E_SPARSE,
#                   E2E_WORKERS, E2E_DURATION, E2E_HTTP_EXTRA, ...)
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

profile=${1:-quick}
[ $# -gt 0 ] && shift

version=${NGINX_VERSION:-1.30.5}
sanitize=${E2E_SANITIZE:-}
image=${E2E_IMAGE:-ngx-cache-purge-e2e}:${version}${sanitize:+-$sanitize}
data=${E2E_DATA_DIR:-$root/t/e2e/.data/$profile}
results=$root/t/e2e/results/$(date +%Y%m%d-%H%M%S)-$profile${sanitize:+-$sanitize}
name=ngx-cache-purge-e2e-$$

mkdir -p "$data" "$results"

docker build -f t/e2e/Dockerfile \
    --build-arg NGINX_VERSION="$version" \
    --build-arg SANITIZE="$sanitize" \
    -t "$image" .

envfile=$(mktemp)
trap 'rm -f "$envfile"; docker rm -f "$name" >/dev/null 2>&1 || true' EXIT
env | grep '^E2E_' | grep -v '^E2E_DATA_DIR=' > "$envfile" || true

rc=0
docker run --rm --init --name "$name" \
    --cap-add SYS_PTRACE \
    --ulimit nofile=1048576:1048576 \
    --env-file "$envfile" \
    -v "$data":/data \
    -v "$results":/results \
    -v "$root/t/e2e/harness":/e2e/harness:ro \
    "$image" --profile "$profile" "$@" || rc=$?

# the container ran as root: hand the files back
docker run --rm --entrypoint chown -v "$data":/data -v "$results":/results \
    "$image" -R "$(id -u):$(id -g)" /data /results || true

echo "results: $results (exit $rc)"
exit $rc
