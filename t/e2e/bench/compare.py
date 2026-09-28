#!/usr/bin/env python3
"""compare.py DIR: medians of bench.py runs <mode>-<build>-<rep>.json in
DIR, the builds side by side, and the change from the first to the second.

    compare.py DIR [--a base] [--b head]
"""
import glob, json, os, statistics, sys

# higher is better for these; lower for the rest (times, latencies)
HIGHER = ("rps",)


def flatten(d, prefix=""):
    out = {}
    for k, v in d.items():
        name = prefix + "." + k if prefix else k
        if isinstance(v, dict):
            out.update(flatten(v, name))
        elif isinstance(v, (int, float)) and not k.startswith("status") and k != "non2xx":
            out[name] = v
    return out


def load(d, mode, build):
    runs = {}
    for f in sorted(glob.glob(os.path.join(d, "%s-%s-*.json" % (mode, build)))):
        try:
            res = json.load(open(f))
        except ValueError:
            continue
        for measure, reps in res["runs"].items():
            for r in reps:
                for k, v in flatten(r, measure).items():
                    runs.setdefault(k, []).append(v)
    return runs


def main():
    d = sys.argv[1]
    a = sys.argv[sys.argv.index("--a") + 1] if "--a" in sys.argv else "base"
    b = sys.argv[sys.argv.index("--b") + 1] if "--b" in sys.argv else "1.30.5"
    for mode in ("inline", "threads"):
        ra, rb = load(d, mode, a), load(d, mode, b)
        if not ra or not rb:
            continue
        print("\n== %s (median of %d vs %d runs)" % (mode, len(next(iter(ra.values()))), len(next(iter(rb.values())))))
        print("%-26s %14s %14s %9s   %s" % ("metric", a, b, "change", "spread a / b"))
        for k in sorted(set(ra) & set(rb)):
            ma, mb = statistics.median(ra[k]), statistics.median(rb[k])
            ch = (mb - ma) / ma * 100 if ma else 0.0
            better = ch >= 0 if k.endswith(HIGHER) else ch <= 0
            sp = lambda xs: "%.0f%%" % ((max(xs) - min(xs)) / statistics.median(xs) * 100) if statistics.median(xs) else "-"
            print("%-26s %14.3f %14.3f %+8.1f%% %s  %s / %s"
                  % (k, ma, mb, ch, " " if better or abs(ch) < 3 else "!", sp(ra[k]), sp(rb[k])))


if __name__ == "__main__":
    main()
