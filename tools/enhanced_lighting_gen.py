#!/usr/bin/env python3
"""Generate enhanced_lighting_table.bin: the reference client's per-zone light keyframes,
re-keyed to 3.3.5's Light.dbc (docs/re/enhanced-lighting.md section 2b).

The output is Blizzard's data, converted: it is written to the local client
data, never committed. Wowee reads it from
<data>/override/wowee/enhanced_lighting_table.bin (format in
include/rendering/enhanced_lighting_table.hpp); without it the enhanced daylight row
uses the averaged trend everywhere.

Inputs:
  --dbc DIR       3.3.5 DBFilesClient (light.dbc, lightparams.dbc,
                  lightintband.dbc, lightfloatband.dbc)
  --reference DIR reference client 1.60.1 Light.csv, LightData.csv, ZoneLight.csv and
                  ZoneLightPoint.csv (wago.tools exports)
  --out FILE      the table to write (omit to only print the report)

Example:
  tools/enhanced_lighting_gen.py --dbc <data>/dbfilesclient --reference <csv dir> \\
      --out <data>/override/wowee/enhanced_lighting_table.bin

Which reference light a 3.3.5 light gets: the one that lights the same ground
in the reference client. Each 3.3.5 light on maps 0 and 1 is sampled on a 32 x 32 grid over
its outer radius, each point weighted by the 3.3.5 light's own weight there;
at each point the reference client's lights are painted as the stock client paints them
(the map default first - or, inside one of the reference client's ZoneLight polygons,
that zone's light - then the rest farthest first, each at its linear falloff
weight, the nearest on top), and each reference light's share of the final mix
is summed. The reference light with the largest share is the match. A 3.3.5
map default takes the reference client's map default; any other 3.3.5 light takes only a
reference light that holds at least half of its area - the reference client's map default
only if the 3.3.5 light is a 1.12 one (its id is in the reference client too) that the reference client
shrank or moved, leaving that ground to the default. Otherwise it has no
match and Wowee keeps the averaged trend there.

What is copied, per matched light and slot (clear and storm):
  LightData's colours, in 3.3.5's LightIntBand channel order (established by
  matching Classic Era 1.15's LightData to 3.3.5's bands for the same
  LightParams: 99% of keys equal):
    0 DirectColor  1 AmbientColor  2 SkyTop  3 SkyMiddle  4 SkyBand1
    5 SkyBand2  6 SkySmog  7 SkyFog  8 ShadowOpacity  9 Sun  10 CloudSun
    11 CloudLayer1Ambient  12 CloudEmissive  13 CloudLayer2Ambient
    14 OceanClose  15 OceanFar  16 RiverClose  17 RiverFar
  and the floats in LightFloatBand order: 0 FogEnd, 1 FogScaler,
  2 SunFogAngle (celestial glow through), 3 CloudDensity. Same units as
  3.3.5 (FogEnd 18000 is Light 1's noon in both), times in half-minutes.
A column a reference profile leaves at zero on every key is one the reference client does
not use (its water colours, the shadow grey, the second cloud layer); it is
marked absent and Wowee keeps 3.3.5's band for it. A FogEnd of zero on a
single key is a key without a fog end (the reference client sets it on 277 of 1455 keys);
those keys are left out of the fog end band.
"""

import argparse
import csv
import math
import struct
import sys

COLOR_COLUMNS = [
    "DirectColor", "AmbientColor", "SkyTopColor", "SkyMiddleColor",
    "SkyBand1Color", "SkyBand2Color", "SkySmogColor", "SkyFogColor",
    "ShadowOpacity", "SunColor", "CloudSunColor", "CloudLayer1AmbientColor",
    "CloudEmissiveColor", "CloudLayer2AmbientColor", "OceanCloseColor",
    "OceanFarColor", "RiverCloseColor", "RiverFarColor",
]
FLOAT_COLUMNS = ["FogEnd", "FogScaler", "SunFogAngle", "CloudDensity"]
MAPS = (0, 1)
ZEROPOINT = 32.0 * 533.33333
SLOTS = {0: "LightParamsID_0", 2: "LightParamsID_2"}  # clear, storm
GRID = 32


def load_wdbc(path):
    data = open(path, "rb").read()
    magic, n, fields, size, _ = struct.unpack_from("<4sIIII", data)
    if magic != b"WDBC":
        sys.exit(f"{path}: not a WDBC file")
    out = []
    for i in range(n):
        off = 20 + i * size
        out.append((struct.unpack_from(f"<{fields}I", data, off),
                    struct.unpack_from(f"<{fields}f", data, off)))
    return out


def wotlk_lights(dbc_dir):
    lights = []
    for u, f in load_wdbc(f"{dbc_dir}/light.dbc"):
        if u[1] not in MAPS:
            continue
        x, z, y = f[2], f[3], f[4]
        default = x == 0 and y == 0 and z == 0
        lights.append(dict(id=u[0], map=u[1], default=default,
                           x=ZEROPOINT - y / 36.0, y=ZEROPOINT - x / 36.0,
                           inner=f[5] / 36.0, outer=f[6] / 36.0,
                           params=list(u[7:12])))
    return lights


def reference_lights(fdir):
    lights = []
    for r in csv.DictReader(open(f"{fdir}/Light.csv")):
        m = int(r["ContinentID"])
        if m not in MAPS:
            continue
        x, y, z = (float(r[f"GameCoords_{i}"]) for i in range(3))
        lights.append(dict(id=int(r["ID"]), map=m,
                           default=x == 0 and y == 0 and z == 0,
                           x=x, y=y, inner=float(r["GameFalloffStart"]),
                           outer=float(r["GameFalloffEnd"]),
                           slots={s: int(r[c]) for s, c in SLOTS.items()}))
    return lights


def zone_lights(fdir, flights):
    """The reference client's ZoneLight polygons on maps 0 and 1: inside one, its Light is
    the canvas instead of the map default. Those limited to a height band
    (only "EK High Light", z >= 50, whose light has the default's colours)
    are left out: the vote is in two dimensions."""
    points = {}
    for r in csv.DictReader(open(f"{fdir}/ZoneLightPoint.csv")):
        points.setdefault(r["ZoneLightID"], []).append(
            (int(r["PointOrder"]), float(r["Pos_0"]), float(r["Pos_1"])))
    by_id = {l["id"]: l for l in flights}
    zones = []
    for r in csv.DictReader(open(f"{fdir}/ZoneLight.csv")):
        m = int(r["MapID"])
        if m not in MAPS or float(r["Zmin"]) > -1000 or int(r["LightID"]) not in by_id:
            continue
        poly = [(x, y) for _, x, y in sorted(points.get(r["ID"], []))]
        if len(poly) >= 3:
            zones.append(dict(map=m, name=r["Name"], light=by_id[int(r["LightID"])], poly=poly))
    return zones


def inside(px, py, poly):
    hit = False
    j = len(poly) - 1
    for i in range(len(poly)):
        (xi, yi), (xj, yj) = poly[i], poly[j]
        if (yi > py) != (yj > py) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
            hit = not hit
        j = i
    return hit


def weight(d, inner, outer):
    if d <= inner:
        return 1.0
    if d >= outer or outer <= inner:
        return 0.0
    return 1.0 - (d - inner) / (outer - inner)


def painted_shares(px, py, lights, default, zones):
    """Each light's share of the stock painter's mix at one point."""
    for z in zones:
        if inside(px, py, z["poly"]):
            default = z["light"]
            break
    hits = []
    for l in lights:
        if l is default or l["outer"] < 3.0:
            continue
        d = math.hypot(px - l["x"], py - l["y"])
        w = weight(d, l["inner"], l["outer"])
        if w > 0.0:
            hits.append((d, l, w))
    hits.sort(key=lambda h: -h[0])  # farthest first, nearest on top
    shares = {default["id"]: 1.0}
    for _, l, w in hits:
        for k in shares:
            shares[k] *= 1.0 - w
        shares[l["id"]] = shares.get(l["id"], 0.0) + w
    return shares


def match(w, flights, zones):
    same_map = [l for l in flights if l["map"] == w["map"]]
    zones = [z for z in zones if z["map"] == w["map"]]
    fdef = next(l for l in same_map if l["default"])
    if w["default"]:
        return fdef, 1.0
    total, acc = 0.0, {}
    r = w["outer"]
    for i in range(GRID):
        for j in range(GRID):
            dx = (i + 0.5) / GRID * 2 - 1
            dy = (j + 0.5) / GRID * 2 - 1
            ww = weight(math.hypot(dx, dy) * r, w["inner"], w["outer"])
            if ww <= 0.0:
                continue
            total += ww
            for k, s in painted_shares(w["x"] + dx * r, w["y"] + dy * r, same_map, fdef, zones).items():
                acc[k] = acc.get(k, 0.0) + s * ww
    if total == 0.0:
        return None, 0.0
    best = max(acc, key=acc.get)
    share = acc[best] / total
    if share < 0.5:
        return None, share
    # The reference client's map default only for a 1.12 light the reference client shrank or moved
    # away, leaving its ground to the default; a light 3.3.5 added has no
    # reference counterpart.
    if best == fdef["id"] and not any(l["id"] == w["id"] for l in same_map):
        return None, share
    return next(l for l in same_map if l["id"] == best), share


# The reference client's global fog layer (docs/re/enhanced-lighting.md section 5b): the
# density per yard in LightParams field 031 where field 035 turns the layer
# on. It scatters the light's fog colour (SkyFogColor, already channel 7).
AIR_ON = "Field_1_60_1_69876_035"
AIR_DENSITY = "Field_12_0_1_65617_031"


def air_densities(fdir):
    out = {}
    for r in csv.DictReader(open(f"{fdir}/LightParams.csv")):
        on = float(r.get(AIR_ON) or 0) != 0.0
        out[int(r["ID"])] = float(r.get(AIR_DENSITY) or 0) if on else 0.0
    return out


def reference_profiles(fdir, wanted):
    density = air_densities(fdir)
    keys = {}
    for r in csv.DictReader(open(f"{fdir}/LightData.csv")):
        p = int(r["LightParamID"])
        if p in wanted:
            keys.setdefault(p, []).append(r)
    profiles = {}
    for p, rows in keys.items():
        rows.sort(key=lambda r: int(r["Time"] or 0))
        rows = rows[:16]
        color_mask = 0
        for ch, col in enumerate(COLOR_COLUMNS):
            if any(int(float(r[col] or 0)) != 0 for r in rows):
                color_mask |= 1 << ch
        float_mask = 0
        for ch, col in enumerate(FLOAT_COLUMNS):
            if any(float(r[col] or 0) != 0.0 for r in rows):
                float_mask |= 1 << ch
        profiles[p] = dict(
            color_mask=color_mask, float_mask=float_mask, air_density=density.get(p, 0.0),
            keys=[dict(time=int(r["Time"] or 0) % 2880,
                       colors=[int(float(r[c] or 0)) & 0xFFFFFF for c in COLOR_COLUMNS],
                       floats=[float(r[c] or 0) for c in FLOAT_COLUMNS]) for r in rows])
    return profiles


LUMA = (0.2126, 0.7152, 0.0722)
AMBIENT = 1          # colour channel
BOUND_PERCENTILE = 0.9
BOUND_STEP = 60      # half-minutes: the bound is taken every half hour


def split(c):
    return [(c >> 16) & 255, (c >> 8) & 255, c & 255]


def ambient_at(keys, t):
    """A profile's ambient at time t, keys interpolated as the client does."""
    times = [k["time"] for k in keys]
    nxt = next((i for i, kt in enumerate(times) if kt > t), 0)
    a, b = keys[nxt - 1], keys[nxt]
    span = (b["time"] - a["time"]) % 2880 or 2880
    f = ((t - a["time"]) % 2880) / span
    ca, cb = split(a["colors"][AMBIENT]), split(b["colors"][AMBIENT])
    return [x + (y - x) * f for x, y in zip(ca, cb)]


def luma_chroma(rgb):
    y = sum(w * v for w, v in zip(LUMA, rgb))
    return y, math.sqrt(sum((v - y) ** 2 for v in rgb))


def percentile(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))]


def bound_ambient(profiles):
    """Keep every key's ambient within what the reference client's zones use at that hour.

    The reference client runs a linear-light pipeline and scales its colours by
    intensities this data does not explain; a few profiles carry ambients far
    outside the rest (Mulgore's LightParams 7567: #bae6ff by day, #cb84ff at
    19:30, #1d9eff at night) that on 3.3.5's clamped 8-bit lighting read as a
    strong cast. Until that scaling is known (an approximation): at each half
    hour, the 90th percentile of the ambient's luminance and of its chroma
    (distance from its grey) over all the profiles; a key above either is
    brought down to it - luminance by scaling the colour, chroma by pulling it
    toward its grey - keeping its hue."""
    lum_max, chroma_max = {}, {}
    for t in range(0, 2880, BOUND_STEP):
        samples = [luma_chroma(ambient_at(p["keys"], t))
                   for p in profiles.values() if p["color_mask"] & (1 << AMBIENT)]
        lum_max[t] = percentile([s[0] for s in samples], BOUND_PERCENTILE)
        chroma_max[t] = percentile([s[1] for s in samples], BOUND_PERCENTILE)
    changed = []
    for pid, p in profiles.items():
        for k in p["keys"]:
            slot = round(k["time"] / BOUND_STEP) * BOUND_STEP % 2880
            rgb = split(k["colors"][AMBIENT])
            y, c = luma_chroma(rgb)
            if y > lum_max[slot] and y > 0:
                rgb = [v * lum_max[slot] / y for v in rgb]
                y, c = luma_chroma(rgb)
            if c > chroma_max[slot] and c > 0:
                rgb = [y + (v - y) * chroma_max[slot] / c for v in rgb]
            r, g, b = (max(0, min(255, int(round(v)))) for v in rgb)
            packed = (r << 16) | (g << 8) | b
            if packed != k["colors"][AMBIENT]:
                changed.append((pid, k["time"], k["colors"][AMBIENT], packed))
                k["colors"][AMBIENT] = packed
    return changed


# The file: little-endian 32-bit words (see include/rendering/enhanced_lighting_table.hpp).
MAGIC = b"WFL2"


def write_table(path, order, profiles, links):
    index = {p: i for i, p in enumerate(order)}
    keys, profs = [], []
    for p in order:
        pr = profiles[p]
        profs.append((p, len(keys), len(pr["keys"]), pr["color_mask"], pr["float_mask"],
                      pr["air_density"]))
        keys.extend(pr["keys"])
    out = bytearray(MAGIC)
    out += struct.pack("<III", len(keys), len(profs), len(links))
    for k in keys:
        out += struct.pack("<I18I4f", k["time"], *k["colors"], *k["floats"])
    for pr in profs:
        out += struct.pack("<5If", *pr)
    for lid, slot, p in links:
        out += struct.pack("<3I", lid, slot, index[p])
    with open(path, "wb") as fh:
        fh.write(out)
    return len(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbc", required=True)
    ap.add_argument("--reference", required=True)
    ap.add_argument("--out")
    a = ap.parse_args()

    wl = wotlk_lights(a.dbc)
    fl = reference_lights(a.reference)
    zones = zone_lights(a.reference, fl)
    links, report = [], []
    for w in sorted(wl, key=lambda l: l["id"]):
        f, share = match(w, fl, zones)
        if not f:
            report.append(f"  {w['id']:5d} map {w['map']}: no match (best share {share:.2f})")
            continue
        for slot, fparams in f["slots"].items():
            if fparams:
                links.append((w["id"], slot, fparams))
        tag = "" if f["id"] == w["id"] else f"  <- reference light {f['id']}"
        report.append(f"  {w['id']:5d} map {w['map']}: the reference client {f['id']} share {share:.2f}{tag}")

    profiles = reference_profiles(a.reference, {p for _, _, p in links})
    links = [l for l in links if l[2] in profiles and profiles[l[2]]["keys"]]
    order = sorted(profiles)
    bounded = bound_ambient(profiles)

    matched = len({l[0] for l in links})
    print(f"{matched} of {len(wl)} 3.3.5 lights on maps 0 and 1 matched; "
          f"{len(links)} slots, {len(order)} reference profiles, "
          f"{sum(len(profiles[p]['keys']) for p in order)} keys; "
          f"{len(bounded)} ambient keys bounded")
    print("\n".join(report))
    for pid, t, old, new in bounded:
        print(f"  ambient bound: LightParams {pid} {t / 120:5.2f}h #{old:06x} -> #{new:06x}")
    if a.out:
        size = write_table(a.out, order, profiles, links)
        print(f"wrote {a.out}: {size} bytes")


if __name__ == "__main__":
    main()
