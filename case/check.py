"""Check the exported STLs before printing.

    python check.py

Three things, all of which have gone wrong at least once in this design:

  1. nothing covers the display's active area
  2. both parts are one watertight shell with no non-manifold edges
  3. neither part exceeds the 39.80 x 53.00 envelope

Re-run it after changing any dimension - especially the display offsets, which
are assumptions until the board is measured.
"""
import re
import sys
from collections import Counter, defaultdict

CASE_W, CASE_H = 39.80, 53.00
GLASS = 27.60           # 200 px at 0.138 mm pitch
GLASS_BOTTOM = 14.40
APERTURE = 28.80
APERTURE_BOTTOM = 13.80


def load(path):
    tris, cur = [], []
    with open(path) as f:
        for line in f:
            m = re.match(r"\s*vertex\s+(\S+)\s+(\S+)\s+(\S+)", line)
            if m:
                cur.append(tuple(float(v) for v in m.groups()))
                if len(cur) == 3:
                    tris.append(tuple(cur))
                    cur = []
    return tris


def topology(tris):
    key = lambda p: (round(p[0], 4), round(p[1], 4), round(p[2], 4))
    parent, vid, edges, tv = {}, {}, Counter(), []

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for t in tris:
        ids = []
        for p in t:
            k = key(p)
            if k not in vid:
                vid[k] = len(vid)
                parent[vid[k]] = vid[k]
            ids.append(vid[k])
        tv.append(ids)
        union(ids[0], ids[1])
        union(ids[1], ids[2])
        for i in range(3):
            edges[tuple(sorted((ids[i], ids[(i + 1) % 3])))] += 1

    groups = defaultdict(list)
    for i, ids in enumerate(tv):
        groups[find(ids[0])].append(i)
    return len(groups), sum(1 for n in edges.values() if n != 2), len(edges)


def point_in(px, py, t):
    (x1, y1, _), (x2, y2, _), (x3, y3, _) = t
    d = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
    if abs(d) < 1e-12:
        return False
    a = ((y2 - y3) * (px - x3) + (x3 - x2) * (py - y3)) / d
    b = ((y3 - y1) * (px - x3) + (x1 - x3) * (py - y3)) / d
    return a >= -1e-9 and b >= -1e-9 and (1 - a - b) >= -1e-9


def check_screen(tris, samples=90):
    gx0 = (CASE_W - GLASS) / 2
    gy0 = GLASS_BOTTOM
    gx1, gy1 = gx0 + GLASS, gy0 + GLASS
    cand = [t for t in tris
            if max(p[0] for p in t) >= gx0 and min(p[0] for p in t) <= gx1
            and max(p[1] for p in t) >= gy0 and min(p[1] for p in t) <= gy1]
    hits = 0
    for i in range(samples):
        for j in range(samples):
            px = gx0 + (gx1 - gx0) * (i + 0.5) / samples
            py = gy0 + (gy1 - gy0) * (j + 0.5) / samples
            if any(point_in(px, py, t) for t in cand):
                hits += 1
    return len(cand), hits


def main():
    ok = True
    for path, label in (("back.stl", "back tub"), ("front.stl", "front bezel")):
        try:
            tris = load(path)
        except FileNotFoundError:
            print(f"{label}: {path} missing - export it first")
            ok = False
            continue

        shells, bad, total = topology(tris)
        pts = [p for t in tris for p in t]
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        zs = [p[2] for p in pts]

        print(f"\n{label} ({path}) - {len(tris)} triangles")
        print(f"  shells            {shells}          {'ok' if shells == 1 else 'FAIL'}")
        print(f"  non-manifold      {bad}/{total}      {'ok' if bad == 0 else 'FAIL'}")
        print(f"  size              {max(xs)-min(xs):.2f} x {max(ys)-min(ys):.2f} x {max(zs)-min(zs):.2f} mm")
        fits = max(xs) <= CASE_W + 0.01 and min(xs) >= -0.01 \
            and max(ys) <= CASE_H + 0.01 and min(ys) >= -0.01
        print(f"  within envelope   {'ok' if fits else 'FAIL - exceeds 39.80 x 53.00'}")
        ok &= shells == 1 and bad == 0 and fits

        if path == "front.stl":
            cand, hits = check_screen(tris)
            margin = ((CASE_W - GLASS) / 2) - ((CASE_W - APERTURE) / 2)
            print(f"  over the screen   {hits} of 8100 samples, "
                  f"{cand} candidate triangles   {'ok' if hits == 0 else 'FAIL'}")
            print(f"  aperture margin   {margin:.2f} mm per side around the glass")
            ok &= hits == 0

    print("\n" + ("all checks passed" if ok else "CHECKS FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
