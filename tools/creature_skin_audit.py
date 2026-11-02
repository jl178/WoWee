#!/usr/bin/env python3
"""Every creature display's texture slots, as the stock client fills them.

    tools/creature_skin_audit.py [DATA] [-v] [--used FILE]

DATA is a Data directory with an extracted expansions/wotlk (manifest.json and
dbfilesclient/); it defaults to $WOW_DATA_PATH, then ./Data. Without one there
is nothing to audit and it says so and exits 0.

WHY

A creature's M2 does not name its skin. It declares slots by type - 0 names a
file, 11/12/13 are CreatureDisplayInfo.TextureVariation[0..2], the rest belong
to the character composite - and the display row fills them. Wowee had four
copies of that rule, each with its own fallbacks, and a creature drawn in the
wrong skin was found one screenshot at a time. This walks all of them.

THE RULE (core/creature_skin_rules.hpp, docs/re/characters.md section 5)

  type 0      the model's own file
  type 11-13  model directory + variation, extension made .blp
              - file there:      that file                       "skin"
              - file not there:  the stock default, solid white  "missing"
              - no variation:    nothing bound, reads as black   "unbound"
  other       the composite's (Extra displays); not counted here

An Extra display's 11-13 slots are counted with "(Extra)": Wowee's composite
puts the bake there, which is its own question and not this rule's.

It counts slots and the batches that draw them (stage 0 of each batch's
texture combo, from the model's 00.skin), and lists the displays whose drawn
batches get no real file. With --used FILE (lines "displayId<TAB>...", e.g. a
creature_template_model dump) it marks the displays creatures actually use.
"""

import collections
import json
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m2_textures  # noqa: E402


def find_data(arg):
    for c in [arg, os.environ.get("WOW_DATA_PATH"), "Data"]:
        if c and (Path(c) / "expansions/wotlk/manifest.json").is_file():
            return Path(c) / "expansions/wotlk"
    return None


class Files:
    def __init__(self, root):
        self.root = root
        self.entries = json.loads((root / "manifest.json").read_text())["entries"]

    @staticmethod
    def key(p):
        return p.replace("/", "\\").lower()

    def exists(self, p):
        return self.key(p) in self.entries

    def read(self, p):
        e = self.entries.get(self.key(p))
        return (self.root / e["p"]).read_bytes() if e else None


def load_dbc(data):
    _, n, fields, size, _ = struct.unpack_from("<4s4I", data, 0)
    strings = data[20 + n * size:]
    rows = [struct.unpack_from("<%dI" % fields, data, 20 + i * size) for i in range(n)]

    def string(o):
        return strings[o:strings.index(b"\0", o)].decode("latin1")
    return rows, string


def model_dir(path):
    i = max(path.rfind("\\"), path.rfind("/"))
    return path[:i + 1] if i >= 0 else ""


def skin_file(directory, variation):
    """core::creatureSkinFile."""
    if not variation:
        return ""
    path = directory + variation
    slash = max(path.rfind("\\"), path.rfind("/"))
    dot = path.rfind(".")
    if dot >= 0 and dot > slash:
        path = path[:dot]
    return path + ".blp"


def model_batches(files, m2path, cache={}):
    """(texture entries, texture lookup, [(submesh id, combo index)])."""
    if m2path in cache:
        return cache[m2path]
    out = None
    data = files.read(m2path)
    if data and data[:4] == b"MD20":
        version = struct.unpack_from("<I", data, 4)[0]
        tex = m2_textures.texture_entries(data)
        n, o = m2_textures.m2_array(data, version, "TextureLookup")
        lookup = list(struct.unpack_from("<%dH" % n, data, o)) if 0 < n < 10000 else []
        batches = []
        skin = files.read(m2path[:-3] + "00.skin")
        if skin and skin[:4] == b"SKIN":
            _, _, _, _, _, _, ns, os_, nb, ob = struct.unpack_from("<10I", skin, 4)
            subs = [struct.unpack_from("<H", skin, os_ + 48 * i)[0] for i in range(ns)]
            for i in range(nb):
                f = struct.unpack_from("<BbHHHHHHHH", skin, ob + 24 * i)
                section, combo = f[3], f[9]
                batches.append((subs[section] if section < len(subs) else 0, combo))
        out = (tex, lookup, batches)
    cache[m2path] = out
    return out


def main(argv):
    verbose = "-v" in argv
    used = {}
    if "--used" in argv:
        for line in open(argv[argv.index("--used") + 1]):
            parts = line.rstrip("\n").split("\t")
            if parts and parts[0].isdigit():
                used[int(parts[0])] = parts[-1]
    positional = [a for i, a in enumerate(argv)
                  if not a.startswith("-") and (i == 0 or argv[i - 1] != "--used")]
    root = find_data(positional[0] if positional else None)
    if not root:
        print("creature_skin_audit: no extracted Data/expansions/wotlk here - nothing to audit")
        return 0
    files = Files(root)
    cmd_raw = files.read("DBFilesClient\\CreatureModelData.dbc")
    cdi_raw = files.read("DBFilesClient\\CreatureDisplayInfo.dbc")
    if not cmd_raw or not cdi_raw:
        print("creature_skin_audit: CreatureDisplayInfo/CreatureModelData not extracted")
        return 0
    cmd, cmd_s = load_dbc(cmd_raw)
    cdi, cdi_s = load_dbc(cdi_raw)
    paths = {r[0]: cmd_s(r[2]) for r in cmd}

    slots = collections.Counter()
    batches = collections.Counter()
    no_model = []
    flagged = collections.defaultdict(lambda: collections.Counter())
    for r in cdi:
        display, model_id, extra = r[0], r[1], r[3]
        variations = [cdi_s(r[6]), cdi_s(r[7]), cdi_s(r[8])]
        p = paths.get(model_id, "")
        if not p:
            continue
        m2 = p[:-4] + ".m2" if p.lower().endswith((".mdx", ".mdl")) else p
        model = model_batches(files, m2)
        if not model:
            no_model.append((display, m2))
            continue
        tex, lookup, bl = model
        directory = model_dir(m2)
        kinds = []
        for e in tex:
            t = e["type"]
            if t == 0:
                kinds.append("model file" if files.exists(e["filename"]) else "model file missing")
            elif 11 <= t <= 13:
                f = skin_file(directory, variations[t - 11])
                k = "unbound" if not f else ("skin" if files.exists(f) else "missing")
                # An Extra display's model is a character model; Wowee lays the
                # NPC's bake over its 11-13 slots, which stock does not (its
                # component binds types 1, 2, 6 and 8 only). Counted apart.
                kinds.append(k + " (Extra)" if extra else k)
            else:
                kinds.append("composite" if extra else "composite slot, no Extra")
        for k in kinds:
            slots[k] += 1
        for _, combo in bl:
            if combo >= len(lookup) or lookup[combo] >= len(kinds):
                continue
            k = kinds[lookup[combo]]
            batches[k] += 1
            if display in used:
                batches[k + " [used]"] += 1
            if k in ("unbound", "missing", "model file missing"):
                flagged[(display, m2)][k] += 1

    print(f"{len(cdi)} CreatureDisplayInfo rows, {len(no_model)} whose model file is absent")
    print("\nslots:")
    for k, v in sorted(slots.items()):
        print(f"  {v:7d}  {k}")
    print("\nbatches (stage 0 of the texture combo):")
    for k, v in sorted(batches.items()):
        print(f"  {v:7d}  {k}")
    shown = sorted(flagged.items(), key=lambda kv: (kv[0][0] not in used, kv[0][0]))
    print(f"\n{len(flagged)} displays draw a batch with no real file"
          f" ({sum(1 for (d, _) in flagged if d in used)} used by a creature):")
    for (display, m2), kinds in (shown if verbose else shown[:25]):
        who = f"  {used[display]}" if display in used else ""
        what = ", ".join(f"{n} {k}" for k, n in sorted(kinds.items()))
        print(f"  {display:6d} {m2.split(chr(92))[-1]:32s} {what}{who}")
    if not verbose and len(shown) > 25:
        print(f"  ... {len(shown) - 25} more (-v)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
