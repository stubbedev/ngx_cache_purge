# t/e2e/Dockerfile -- end-to-end test image.  Build context: the repo root.
#
#   docker build -f t/e2e/Dockerfile -t ngx-cache-purge-e2e .
#   docker build -f t/e2e/Dockerfile --build-arg SANITIZE=asan -t ngx-cache-purge-e2e:asan .
#
ARG UBUNTU_VERSION=24.04
FROM ubuntu:${UBUNTU_VERSION}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential libpcre2-dev libssl-dev zlib1g-dev \
        wget ca-certificates python3 wrk gdb procps psmisc curl util-linux \
    && rm -rf /var/lib/apt/lists/*

ARG NGINX_VERSION=1.30.5
RUN wget -q https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz \
    && tar xzf nginx-${NGINX_VERSION}.tar.gz \
    && mv nginx-${NGINX_VERSION} /nginx-src \
    && rm nginx-${NGINX_VERSION}.tar.gz

# only the module: harness edits do not rebuild nginx
COPY config ngx_cache_purge_module.c /module/

# SANITIZE=asan: nginx and the module with AddressSanitizer
ARG SANITIZE=
RUN cd /nginx-src \
    && CC_OPT="-g -O2 -fno-omit-frame-pointer" \
    && LD_OPT="" \
    && if [ "$SANITIZE" = "asan" ]; then \
           CC_OPT="-g -O1 -fno-omit-frame-pointer -fsanitize=address"; \
           LD_OPT="-fsanitize=address"; \
       fi \
    && ./configure \
        --prefix=/usr/local/nginx \
        --with-threads \
        --with-file-aio \
        --with-http_stub_status_module \
        --add-module=/module \
        --with-cc-opt="$CC_OPT" \
        --with-ld-opt="$LD_OPT" \
    && make -j"$(nproc)" \
    && make install \
    && echo "$SANITIZE" > /usr/local/nginx/SANITIZE

# cachegen: compiled against this nginx's headers, so the cache file header
# it writes is the one this nginx reads
COPY t/e2e/tools /e2e/tools
RUN N=/nginx-src && gcc -O2 -g -Wall -pthread \
        -I$N/src/core -I$N/src/event -I$N/src/event/modules \
        -I$N/src/os/unix -I$N/src/http -I$N/src/http/modules -I$N/objs \
        -o /usr/local/bin/cachegen /e2e/tools/cachegen.c -lcrypto -lz

COPY t/e2e/harness /e2e/harness

ENV PATH=/usr/local/nginx/sbin:$PATH \
    ASAN_OPTIONS=detect_leaks=0:log_path=/results/asan:abort_on_error=0:handle_abort=1

WORKDIR /e2e
ENTRYPOINT ["python3", "/e2e/harness/e2e.py"]
CMD ["--profile", "quick"]
