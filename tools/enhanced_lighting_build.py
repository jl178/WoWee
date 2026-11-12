#!/usr/bin/env python3
"""Build and fit Wowee's own enhanced lighting table from 3.3.5's light data.

The "Enhanced daylight" row reads a per-light table in the WFL2 layout
(include/rendering/enhanced_lighting_table.hpp). The client builds Wowee's own
table at start-up from the 3.3.5 client's DBCs and the parameters in
assets/enhanced_lighting_params.json (src/rendering/enhanced_lighting_emulation.cpp);
this script is the same recipe in Python, to write the table to a file, to
look at it, and to fit the parameters. docs/re/enhanced-lighting.md section 2f.

The recipe, per 3.3.5 light on maps 0 and 1 and per weather slot (clear,
storm):
  - the light's zone class: the smallest WorldMapArea box on its map that
    holds the light's centre (outside every box, the nearest), its AreaTable
    zone looked up in the parameter file's class lists; a map default light,
    or one in no listed zone, is "default";
  - the keys: every time 3.3.5's bands for that LightParams set have a key
    (the channels below), plus the parameter file's band times, at most 16;
  - at each key, 3.3.5's colour there, re-graded by the class's parameters
    at that hour (each parameter eased linearly between the band times):
      out = level * Y + saturation * (rgb - Y) + Y * (warmth * W + tint * G)
    with Y the colour's luminance, W the warm axis (orange against blue) and
    G the green axis (green against magenta), both of zero luminance; the
    sun (DirectColor), the ambient, the sky (SkyTop..SkySmog) and the fog
    colour each have their own per class, the cloud colours one set over all
    classes; the sun sprite moves toward white; the sky channels carry the
    table's sky exposure (x1.3, taken out as the renderer reads them);
  - the fog end times the class's fog scale, the fog start fraction plus its
    offset; the class's air density (the sun haze's fog layer).

Usage:
  enhanced_lighting_build.py build --dbc <data>/dbfilesclient
      [--params assets/enhanced_lighting_params.json] [--out table.bin]
  enhanced_lighting_build.py fit --dbc <data>/dbfilesclient --target <table.bin>
      [--params assets/enhanced_lighting_params.json] [--write]
  enhanced_lighting_build.py compare --dbc <data>/dbfilesclient --target <table.bin>
      [--table <table.bin>] [--params ...]

`fit` reads a WFL2 table the user has locally as a measurement target and
fits only this file's parameters to it (needs numpy): Huber-weighted least
squares of each class's grades against the target's clear-slot colours every
half hour, the map defaults weighted x10. Nothing of the target is written
anywhere but those parameters; the zone lists and band hours are kept. `compare` prints how close a table
(or the one built from the parameters) is to the target, per zone class.
"""

import argparse
import json
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PARAMS = os.path.join(HERE, "..", "assets", "enhanced_lighting_params.json")

MAPS = (0, 1)
ZEROPOINT = 32.0 * 533.33333
DAY = 2880
MAX_KEYS = 16
SKY_EXPOSURE = 1.3  # enhanced_lighting_table::kSkyExposure: the table's sky carries it
SLOTS = (0, 2)      # clear, storm

# 3.3.5's LightIntBand channels re-graded, and which parameter group grades each.
SUN, AMBIENT, SKY, FOG_COLOR, SPRITE, CLOUDS = "sun", "ambient", "sky", "fogColor", "sprite", "clouds"
CLASS_GROUPS = (SUN, AMBIENT, SKY, FOG_COLOR)  # graded per class; the others shared
FOG_RANGE = "fogRange"                          # [fog end scale, fog start offset]
COLOR_GROUP = {0: SUN, 1: AMBIENT, 2: SKY, 3: SKY, 4: SKY, 5: SKY, 6: SKY, 7: FOG_COLOR,
               9: SPRITE, 10: CLOUDS, 11: CLOUDS}
SKY_CHANNELS = {2, 3, 4, 5, 6, 7, 10, 11}  # enhanced_lighting_table::isSkyChannel
FLOAT_CHANNELS = (0, 1)                      # fog end, fog start fraction
COLOR_MASK = sum(1 << c for c in COLOR_GROUP)
FLOAT_MASK = sum(1 << f for f in FLOAT_CHANNELS)

LUMA = (0.2126, 0.7152, 0.0722)
WARM = (1.0, -(LUMA[0] - LUMA[2]) / LUMA[1], -1.0)    # orange vs blue, zero luminance
GREEN = (-1.0, (LUMA[0] + LUMA[2]) / LUMA[1], -1.0)   # green vs magenta, zero luminance
GRADE_IDENTITY = [1.0, 1.0, 0.0, 0.0]                 # level, saturation, warmth, tint


# ---- 3.3.5 data -----------------------------------------------------------

def load_wdbc(path):
    data = open(path, "rb").read()
    magic, n, fields, size, _ = struct.unpack_from("<4sIIII", data)
    if magic != b"WDBC":
        sys.exit(f"{path}: not a WDBC file")
    rows = []
    for i in range(n):
        off = 20 + i * size
        rows.append((struct.unpack_from(f"<{fields}I", data, off),
                     struct.unpack_from(f"<{fields}f", data, off)))
    return rows


def find_dbc(dbc_dir, name):
    for cand in (name, name.lower()):
        p = os.path.join(dbc_dir, cand)
        if os.path.exists(p):
            return p
    sys.exit(f"{name} not found in {dbc_dir}")


def load_lights(dbc_dir):
    lights = []
    for u, f in load_wdbc(find_dbc(dbc_dir, "Light.dbc")):
        if u[1] not in MAPS:
            continue
        x, z, y = f[2], f[3], f[4]
        lights.append(dict(id=u[0], map=u[1], default=(x == 0 and y == 0 and z == 0),
                           wx=ZEROPOINT - y / 36.0, wy=ZEROPOINT - x / 36.0,
                           params=list(u[7:12])))
    return sorted(lights, key=lambda l: l["id"])


def load_bands(dbc_dir):
    """{(LightParams id, channel): (times, values)} for colours and floats."""
    colors, floats = {}, {}
    for u, f in load_wdbc(find_dbc(dbc_dir, "LightIntBand.dbc")):
        p, c = (u[0] - 1) // 18 + 1, (u[0] - 1) % 18
        n = min(u[1], MAX_KEYS)
        colors[(p, c)] = ([u[2 + k] % DAY for k in range(n)], [u[18 + k] & 0xFFFFFF for k in range(n)])
    for u, f in load_wdbc(find_dbc(dbc_dir, "LightFloatBand.dbc")):
        p, c = (u[0] - 1) // 6 + 1, (u[0] - 1) % 6
        n = min(u[1], MAX_KEYS)
        floats[(p, c)] = ([u[2 + k] % DAY for k in range(n)], [f[18 + k] for k in range(n)])
    return colors, floats


def load_zone_boxes(dbc_dir):
    """WorldMapArea's zone boxes on maps 0 and 1: (map, area, left, right, top, bottom).
    left/right bound the world Y (left > right), top/bottom the world X."""
    out = []
    for u, f in load_wdbc(find_dbc(dbc_dir, "WorldMapArea.dbc")):
        if u[1] in MAPS and u[2] != 0:
            out.append((u[1], u[2], f[4], f[5], f[6], f[7]))
    return out


def zone_of(light, boxes):
    """The smallest box holding the light's centre; outside every box, the
    nearest one."""
    best, best_key = 0, None
    for m, area, left, right, top, bottom in boxes:
        if m != light["map"]:
            continue
        dx = max(bottom - light["wx"], 0.0, light["wx"] - top)
        dy = max(right - light["wy"], 0.0, light["wy"] - left)
        key = (math.hypot(dx, dy), (left - right) * (top - bottom))
        if best_key is None or key < best_key:
            best, best_key = area, key
    return best


def class_of(light, boxes, params):
    if light["default"]:
        return "default"
    zone = zone_of(light, boxes)
    for name, c in params["classes"].items():
        if zone in c.get("zones", []):
            return name
    return "default"


# ---- the stock client's band sampling (stock_light_math.hpp) --------------

def band_segment(times, t):
    n = len(times)
    for i in range(n):
        a, j = times[i], (i + 1) % n
        b, now = times[j], t
        if a < b:
            if now < a or b < now:
                continue
        else:
            if b < now < a:
                continue
            b += DAY
            if now < a:
                now += DAY
        return i, j, (now - a) / (b - a) if b != a else 0.0
    return 0, 0, 0.0


def round_even(v):
    return int(round(v))  # Python rounds half to even, as the client's FPU does


def sample_color(band, t):
    times, vals = band
    if not times:
        return (0, 0, 0)
    lo, hi, f = band_segment(times, t)
    a, b = vals[lo], vals[hi]
    return tuple(round_even((((b >> s) & 255) - ((a >> s) & 255)) * f + ((a >> s) & 255)) & 255
                 for s in (16, 8, 0))


def sample_float(band, t):
    times, vals = band
    if not times:
        return 0.0
    lo, hi, f = band_segment(times, t)
    return (vals[hi] - vals[lo]) * f + vals[lo]


# ---- the recipe -----------------------------------------------------------

def band_times(params):
    return [int(round(h * 120)) % DAY for h in params["bandHours"]]


def band_weights(params, t):
    """How much of each band's parameters apply at half-minute t: linear
    between the band times, round midnight."""
    ts = band_times(params)
    n = len(ts)
    w = [0.0] * n
    for i in range(n):
        a, b = ts[i], ts[(i + 1) % n]
        span = (b - a) % DAY or DAY
        d = (t - a) % DAY
        if d < span:
            f = d / span
            w[i] += 1.0 - f
            w[(i + 1) % n] += f
            return w
    w[0] = 1.0
    return w


def key_times(params, colors, floats, pid):
    ts = set(band_times(params))
    anchors = set(ts)
    for c in COLOR_GROUP:
        if (pid, c) in colors:
            ts |= set(colors[(pid, c)][0])
    for f in FLOAT_CHANNELS:
        if (pid, f) in floats:
            ts |= set(floats[(pid, f)][0])
    ts = sorted(ts)
    # Too many: drop the key nearest the one before it (never a band time).
    while len(ts) > MAX_KEYS:
        gaps = [((ts[i] - ts[i - 1]) % DAY, ts[i]) for i in range(len(ts)) if ts[i] not in anchors]
        ts.remove(min(gaps)[1])
    return ts


def grade(rgb, g):
    """level * Y + saturation * (rgb - Y) + Y * (warmth * WARM + tint * GREEN)."""
    level, sat, warm, tint = g
    y = sum(w * v for w, v in zip(LUMA, rgb))
    return [level * y + sat * (v - y) + y * (warm * wv + tint * gv)
            for v, wv, gv in zip(rgb, WARM, GREEN)]


def lerp_params(seq, w):
    return [sum(wi * p[k] for wi, p in zip(w, seq)) for k in range(len(seq[0]))]


def group_params(params, cls, group):
    """One grade per band for a class and group."""
    if group in (SPRITE, CLOUDS):
        return params["shared"][group]
    return params["classes"][cls][group]


def pack(rgb):
    r, g, b = (max(0, min(255, int(round(v)))) for v in rgb)
    return (r << 16) | (g << 8) | b


def build_profile(params, cls, pid, colors, floats):
    ts = key_times(params, colors, floats, pid)
    keys = []
    for t in ts:
        w = band_weights(params, t)
        col = [0] * 18
        for c, group in COLOR_GROUP.items():
            if (pid, c) not in colors:
                continue
            g = lerp_params(group_params(params, cls, group), w)
            if group == SPRITE:  # the sun sprite: toward white by its first parameter
                rgb = [v + g[0] * (255.0 - v) for v in sample_color(colors[(pid, c)], t)]
            else:
                rgb = grade(sample_color(colors[(pid, c)], t), g)
            if c in SKY_CHANNELS:
                rgb = [v * SKY_EXPOSURE for v in rgb]
            col[c] = pack(rgb)
        fog = lerp_params(params["classes"][cls][FOG_RANGE], w)
        val = [0.0] * 4
        if (pid, 0) in floats:
            val[0] = max(10.0 * 36.0, sample_float(floats[(pid, 0)], t) * fog[0])
        if (pid, 1) in floats:
            val[1] = max(-1.0, min(0.99, sample_float(floats[(pid, 1)], t) + fog[1]))
        keys.append((t, col, val))
    cmask = sum(1 << c for c in COLOR_GROUP if (pid, c) in colors)
    fmask = sum(1 << f for f in FLOAT_CHANNELS if (pid, f) in floats)
    return dict(pid=pid, keys=keys, cmask=cmask, fmask=fmask,
                air=float(params["classes"][cls]["airDensity"]))


def build_table(dbc_dir, params):
    lights = load_lights(dbc_dir)
    colors, floats = load_bands(dbc_dir)
    boxes = load_zone_boxes(dbc_dir)
    profiles, index, links, classes = [], {}, [], {}
    for l in lights:
        cls = class_of(l, boxes, params)
        classes[l["id"]] = cls
        for slot in SLOTS:
            pid = l["params"][slot]
            if pid == 0 or not any((pid, c) in colors for c in COLOR_GROUP):
                continue
            k = (pid, cls)
            if k not in index:
                index[k] = len(profiles)
                profiles.append(build_profile(params, cls, pid, colors, floats))
            links.append((l["id"], slot, index[k]))
    return dict(profiles=profiles, links=links, classes=classes)


def write_table(path, table):
    keys, profs = [], []
    for p in table["profiles"]:
        profs.append((p["pid"], len(keys), len(p["keys"]), p["cmask"], p["fmask"], p["air"]))
        keys.extend(p["keys"])
    out = bytearray(b"WFL2")
    out += struct.pack("<III", len(keys), len(profs), len(table["links"]))
    for t, col, val in keys:
        out += struct.pack("<I18I4f", t, *col, *val)
    for p in profs:
        out += struct.pack("<5If", *p)
    for link in table["links"]:
        out += struct.pack("<3I", *link)
    with open(path, "wb") as fh:
        fh.write(out)
    return len(out)


def read_table(path):
    """A WFL2 table as {(light, slot): (times, colours, values, cmask, fmask, air)}."""
    b = open(path, "rb").read()
    if b[:4] != b"WFL2":
        sys.exit(f"{path}: not a WFL2 table")
    nk, npr, nl = struct.unpack_from("<3I", b, 4)
    o, keys = 16, []
    for _ in range(nk):
        w = struct.unpack_from("<I18I4f", b, o)
        o += 92
        keys.append((w[0], list(w[1:19]), list(w[19:23])))
    profs = []
    for _ in range(npr):
        profs.append(struct.unpack_from("<5If", b, o))
        o += 24
    out = {}
    for _ in range(nl):
        lid, slot, p = struct.unpack_from("<3I", b, o)
        o += 12
        _, first, n, cmask, fmask, air = profs[p]
        ks = keys[first:first + n]
        out[(lid, slot)] = ([k[0] for k in ks], [k[1] for k in ks], [k[2] for k in ks], cmask, fmask, air)
    return out


def table_as_links(table):
    out = {}
    for lid, slot, p in table["links"]:
        pr = table["profiles"][p]
        ks = pr["keys"]
        out[(lid, slot)] = ([k[0] for k in ks], [k[1] for k in ks], [k[2] for k in ks],
                            pr["cmask"], pr["fmask"], pr["air"])
    return out


def load_params(path):
    text = open(path).read()
    text = re.sub(r"^\s*//.*$", "", text, flags=re.M)  # the file allows // comment lines
    return json.loads(text)


# ---- comparing and fitting against a local target table -------------------

def sample_link(link, ch, t):
    """A table's colour at half-minute t as the renderer uses it: the sky's
    exposure taken out of each key as it is read (withoutSkyExposure)."""
    times, cols, vals, cmask, fmask, air = link
    keys = [c[ch] for c in cols]
    if ch in SKY_CHANNELS:
        keys = [pack([int(((k >> s) & 255) / SKY_EXPOSURE + 0.5) for s in (16, 8, 0)]) for k in keys]
    return list(sample_color((times, keys), t))


COMPARE_GROUPS = {SUN: [0], AMBIENT: [1], SKY: [2, 3, 4, 5, 6], FOG_COLOR: [7], "sprite+clouds": [9, 10, 11]}


def compare(dbc_dir, params, target_path, table_path=None, slots=(0,)):
    """Per class and group: mean RGB distance (0-255, display units) to the
    target over every half hour, and the distance of the class means; for
    the target's links in `slots` (clear by default). 'stock' is 3.3.5's own
    band, which is what a light without a table gets before the trend."""
    tgt = read_table(target_path)
    built = build_table(dbc_dir, params)
    ours = read_table(table_path) if table_path else table_as_links(built)
    colors, _ = load_bands(dbc_dir)
    lights = {l["id"]: l for l in load_lights(dbc_dir)}
    acc = {}
    for (lid, slot), tl in tgt.items():
        if slot not in slots:
            continue
        cls = built["classes"].get(lid, "default")
        pid = lights[lid]["params"][slot] if lid in lights else 0
        for gname, chs in COMPARE_GROUPS.items():
            for ch in chs:
                if not (tl[3] >> ch & 1):
                    continue
                for t in range(0, DAY, 60):
                    tv = sample_link(tl, ch, t)
                    stock = sample_color(colors[(pid, ch)], t) if (pid, ch) in colors else None
                    if stock is None:
                        continue
                    ov = sample_link(ours[(lid, slot)], ch, t) if (lid, slot) in ours else list(stock)
                    for name, v in (("table", ov), ("stock", stock)):
                        a = acc.setdefault((name, cls, gname), [0.0, 0, {}])
                        a[0] += math.dist(v, tv)
                        a[1] += 1
                        m = a[2].setdefault((ch, t), [[0, 0, 0], [0, 0, 0], 0])
                        for i in range(3):
                            m[0][i] += v[i]
                            m[1][i] += tv[i]
                        m[2] += 1
    out = {}
    for (name, cls, gname), (s, n, means) in acc.items():
        bias = sum(math.dist([x / m[2] for x in m[0]], [x / m[2] for x in m[1]]) for m in means.values()) / len(means)
        out[(name, cls, gname)] = (s / n, bias, n)
    return out


def print_compare(res, classes):
    groups = list(COMPARE_GROUPS)
    print("RGB distance to the target (0-255, display units, every half hour): per light / of the class means")
    print(f"{'':24s} " + " ".join(f"{g:>15s}" for g in groups) + f" {'all':>15s}")
    for name in ("stock", "table"):
        for cls in classes + [None]:
            cells, tot, totb, tn = [], 0.0, 0.0, 0
            for g in groups:
                hits = [res[k] for k in res if k[0] == name and k[2] == g and (cls is None or k[1] == cls)]
                if not hits:
                    cells.append(f"{'-':>15s}")
                    continue
                n = sum(h[2] for h in hits)
                e = sum(h[0] * h[2] for h in hits) / n
                b = sum(h[1] * h[2] for h in hits) / n
                cells.append(f"{e:6.1f} / {b:6.1f}")
                tot += e * n
                totb += b * n
                tn += n
            if tn:
                label = f"{name} {cls or 'ALL'}"
                print(f"{label:24s} " + " ".join(cells) + f" {tot / tn:6.1f} / {totb / tn:6.1f}")


def fit(dbc_dir, params, target_path, huber=12.0, ridge=10.0, iters=8):
    """Least squares (Huber-weighted) of each class's grades against the
    target's colours at our own key times; the fog against the target's
    fog; the air as the target's median. Only `params` changes."""
    import numpy as np

    tgt = read_table(target_path)
    lights = {l["id"]: l for l in load_lights(dbc_dir)}
    colors, floats = load_bands(dbc_dir)
    boxes = load_zone_boxes(dbc_dir)
    nb = len(params["bandHours"])
    L = np.array(LUMA)
    W = np.array(WARM)
    G = np.array(GREEN)
    rows = {}  # (cls or shared, group) -> list of (design 3 x 4nb, target 3, weight)
    frows = {}
    airs = {}
    for (lid, slot), tl in tgt.items():
        l = lights.get(lid)
        # The clear slot only: 3.3.5's storm sets are mostly one flat key
        # that no grade turns into a day; they take the clear slot's grade.
        if l is None or slot != 0:
            continue
        cls = class_of(l, boxes, params)
        pid = l["params"][slot]
        weight = 10.0 if l["default"] else 1.0
        airs.setdefault(cls, []).append(tl[5])
        kt = key_times(params, colors, floats, pid)
        kw = [np.array(band_weights(params, t)) for t in kt]
        # Every half hour, as our table gives it there: the two keys round
        # it, each graded with its own band weights, mixed linearly.
        for t in range(0, DAY, 60):
            lo, hi, fr = band_segment(kt, t)
            for c, group in COLOR_GROUP.items():
                if (pid, c) not in colors or not (tl[3] >> c & 1):
                    continue
                tv = np.array(sample_link(tl, c, t), float)
                D = 0.0
                base = np.zeros(3)
                for k, f_ in ((lo, 1.0 - fr), (hi, fr)):
                    s = np.array(sample_color(colors[(pid, c)], kt[k]), float)
                    if group == SPRITE:
                        D = D + f_ * np.kron(kw[k], (255.0 - s)[:, None])
                        base += f_ * s
                        continue
                    y = s @ L
                    F = np.stack([np.full(3, y), s - y, y * W, y * G], 1)
                    D = D + f_ * np.kron(kw[k], F)
                key = ("shared", group) if group in (CLOUDS, SPRITE) else (cls, group)
                rows.setdefault(key, []).append((D, tv - base, weight))
            for f in FLOAT_CHANNELS:
                if (pid, f) not in floats or not (tl[4] >> f & 1):
                    continue
                # A key without a fog end (0) is not in that band, as the
                # renderer reads it.
                fk = [(kt_, v[f]) for kt_, v in zip(tl[0], tl[2]) if f != 0 or v[f] != 0.0]
                if not fk:
                    continue
                tv = sample_float(([a_ for a_, _ in fk], [b_ for _, b_ in fk]), t)
                sv = sample_float(floats[(pid, f)], t)
                w = np.array(band_weights(params, t))
                frows.setdefault((cls, f), []).append((w, sv, tv, weight))

    def solve(items, prior):
        A = np.concatenate([d for d, _, _ in items])
        y = np.concatenate([t for _, t, _ in items])
        w0 = np.repeat([wt for _, _, wt in items], 3)
        w = w0.copy()
        for _ in range(iters):
            sw = np.sqrt(w)
            # Held toward no change: a parameter 1 off it costs what an RGB
            # error of `ridge` on every sample would.
            lam = ridge * np.sqrt(w.sum() / 3.0)
            P = np.linalg.lstsq(np.vstack([A * sw[:, None], lam * np.eye(A.shape[1])]),
                                np.concatenate([y * sw, lam * prior]), rcond=None)[0]
            d = np.linalg.norm((A @ P - y).reshape(-1, 3), axis=1)
            w = w0 * np.repeat(np.where(d <= huber, 1.0, huber / np.maximum(d, 1e-6)), 3)
        return P

    for (cls, group), items in sorted(rows.items()):
        if group == SPRITE:
            P = solve(items, np.zeros(nb))
            params["shared"][group] = [[round(float(np.clip(P[i], 0, 1)), 3)] for i in range(nb)]
            continue
        P = solve(items, np.tile(GRADE_IDENTITY, nb)).reshape(nb, 4)
        vals = [[round(float(x), 3) for x in p] for p in P]
        if cls == "shared":
            params["shared"][group] = vals
        else:
            params["classes"][cls][group] = vals
    for cls, c in params["classes"].items():
        fog = []
        for f in FLOAT_CHANNELS:
            items = frows.get((cls, f), [])
            if not items:
                fog.append([1.0 if f == 0 else 0.0] * nb)
                continue
            A = np.array([w * (sv if f == 0 else 1.0) for w, sv, _, _ in items])
            y = np.array([tv if f == 0 else tv - sv for _, sv, tv, _ in items])
            sw = np.sqrt(np.array([wt for *_, wt in items]))
            prior = np.full(nb, 1.0 if f == 0 else 0.0)
            # The fog end's rows are in its own units (36ths of a yard).
            unit = 1.0 if f == 1 else float(np.median([abs(sv) for _, sv, _, _ in items]))
            reg = 0.3 * unit * float(np.sqrt(sw @ sw))
            P = np.linalg.lstsq(np.vstack([A * sw[:, None], reg * np.eye(nb)]),
                                np.concatenate([y * sw, reg * prior]), rcond=None)[0]
            P = np.clip(P, 0.5, 2.0) if f == 0 else np.clip(P, -0.5, 0.5)
            fog.append([round(float(x), 3) for x in P])
        c[FOG_RANGE] = [[fog[0][i], fog[1][i]] for i in range(nb)]
        a = sorted(airs.get(cls, [0.001]))
        c["airDensity"] = round(a[len(a) // 2], 4)
    return params


# ---- the parameter file ---------------------------------------------------

def format_params(params):
    """The parameter file, one band per line."""
    def row(v):
        return "[" + ", ".join(f"{x:.3f}".rstrip("0").rstrip(".") if x != 0 else "0" for x in v) + "]"
    lines = ["// Wowee's own enhanced daylight: 3.3.5's Light.dbc re-graded per zone class",
             "// and time of day. Read by src/rendering/enhanced_lighting_emulation.cpp at",
             "// start-up and by tools/enhanced_lighting_build.py (which documents the",
             "// recipe and fits the numbers). docs/re/enhanced-lighting.md section 2f.",
             "//",
             "// bandHours: the time-of-day bands; every parameter list below has one entry",
             "// per band, eased linearly between them round the clock.",
             "// sun / ambient / sky / fogColor / shared.clouds: [level, saturation, warmth, tint] -",
             "//   out = level*Y + saturation*(rgb - Y) + Y*(warmth*W + tint*G), Y the",
             "//   luminance, W orange-vs-blue, G green-vs-magenta (no luminance of their own).",
             "// shared.sprite: [toward white] for the sun sprite.",
             "// fogRange: [fog end scale, fog start offset]; airDensity: the sun haze's air per yard.",
             "// zones: AreaTable zone ids (the WorldMapArea box a light stands in).",
             "{",
             f'  "version": {params["version"]},',
             f'  "bandHours": {row(params["bandHours"])},',
             '  "classes": {']
    names = list(params["classes"])
    for i, name in enumerate(names):
        c = params["classes"][name]
        lines.append(f'    "{name}": {{')
        lines.append(f'      "zones": [{", ".join(str(z) for z in c.get("zones", []))}],')
        for g in CLASS_GROUPS + (FOG_RANGE,):
            lines.append(f'      "{g}": [' + ", ".join(row(v) for v in c[g]) + "],")
        lines.append(f'      "airDensity": {c["airDensity"]}')
        lines.append("    }" + ("," if i + 1 < len(names) else ""))
    lines.append("  },")
    lines.append('  "shared": {')
    lines.append('    "sprite": [' + ", ".join(row(v) for v in params["shared"][SPRITE]) + "],")
    lines.append('    "clouds": [' + ", ".join(row(v) for v in params["shared"][CLOUDS]) + "]")
    lines.append("  }")
    lines.append("}")
    return "\n".join(lines) + "\n"


def count_params(params):
    n = len(params["bandHours"])
    for c in params["classes"].values():
        n += sum(len(v) for g in CLASS_GROUPS + (FOG_RANGE,) for v in c[g]) + 1
    n += sum(len(v) for g in (SPRITE, CLOUDS) for v in params["shared"][g])
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("build", "fit", "compare"))
    ap.add_argument("--dbc", required=True, help="3.3.5 DBFilesClient directory")
    ap.add_argument("--params", default=DEFAULT_PARAMS)
    ap.add_argument("--out", help="build: the table to write")
    ap.add_argument("--target", help="fit/compare: a local WFL2 table to measure against")
    ap.add_argument("--table", help="compare: a table to compare instead of the built one")
    ap.add_argument("--write", action="store_true", help="fit: write the fitted parameters back")
    a = ap.parse_args()
    params = load_params(a.params)
    if a.mode == "build":
        t = build_table(a.dbc, params)
        n = sum(len(p["keys"]) for p in t["profiles"])
        print(f"{len(t['links'])} links, {len(t['profiles'])} profiles, {n} keys")
        if a.out:
            print(f"wrote {a.out}: {write_table(a.out, t)} bytes")
    elif a.mode == "fit":
        if not a.target:
            sys.exit("fit needs --target")
        params = fit(a.dbc, params, a.target)
        text = format_params(params)
        print(f"{count_params(params)} parameters")
        if a.write:
            open(a.params, "w").write(text)
            print(f"wrote {a.params}")
        else:
            print(text)
    else:
        if not a.target:
            sys.exit("compare needs --target")
        print_compare(compare(a.dbc, params, a.target, a.table), list(params["classes"]))


if __name__ == "__main__":
    main()
