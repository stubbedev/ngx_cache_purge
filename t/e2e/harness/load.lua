-- wrk script: GETs over the seeded key set (hits) plus unseeded keys
-- (misses: fills from the origin, i.e. cache writes).
--   wrk ... -s load.lua URL -- <files> <variants> <tenants> <video_permille>
--                              <miss_permille>
-- The key formula mirrors tools/cachegen.c and harness/lib.py.

local files, variants, tenants, permille, miss
local counter = 0
local threads = {}

function setup(thread)
    counter = counter + 1
    thread:set("tid", counter)
    table.insert(threads, thread)
end

function init(args)
    files    = tonumber(args[1])
    variants = tonumber(args[2])
    tenants  = tonumber(args[3])
    permille = tonumber(args[4])
    miss     = tonumber(args[5])
    math.randomseed(os.time() * 1000 + (tid or 0))
end

local function video_asset(a)
    return (a * 7919) % 1000 < permille
end

local function key(i)
    local a = math.floor(i / variants)
    local v = i % variants
    local t = a % tenants
    if video_asset(a) then
        return string.format("/video/t%d/a%d/s%d.mp4?r=%d", t, a, 6 + v % 4, v)
    end
    return string.format("/cdn/t%d/a%d/s%d.jpg?w=%d", t, a, v % 6,
                         160 * (v + 1))
end

function request()
    local i
    if math.random(1000) <= miss then
        -- an unseeded image: a miss that is filled and stored
        repeat
            i = files + math.random(0, files * 4)
        until not video_asset(math.floor(i / variants))
    else
        i = math.random(0, files - 1)
    end
    return wrk.format("GET", key(i))
end

function done(summary, latency, requests)
    local e = summary.errors
    io.write(string.format(
        'E2E_JSON {"requests": %d, "duration_us": %d, "bytes": %d, ' ..
        '"errors": {"connect": %d, "read": %d, "write": %d, ' ..
        '"status": %d, "timeout": %d}, ' ..
        '"latency_us": {"p50": %d, "p90": %d, "p99": %d, "p999": %d, ' ..
        '"max": %d, "mean": %.1f}}\n',
        summary.requests, summary.duration, summary.bytes,
        e.connect, e.read, e.write, e.status, e.timeout,
        latency:percentile(50), latency:percentile(90),
        latency:percentile(99), latency:percentile(99.9),
        latency.max, latency.mean))
end
