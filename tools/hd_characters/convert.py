#!/usr/bin/env python3
"""Convert the reference client's HD player models into files Wowee loads.

Enhanced (product wowf) draws the classic races with the HD models Blizzard
made in Warlords of Draenor: chunked M2s (MD21 with SFID/AFID/TXID), external
skins and .anim files, 2048x1024 body atlases, and appearance chosen through
the ChrCustomization tables instead of CharSections. This turns each of the
twenty into:

  <out>/character/<race>/<sex>/<race><sex>.m2      the model, plain MD20 v264
  <out>/character/<race>/<sex>/<race><sex>00.skin  its skin
  <out>/character/<race>/<sex>/<race><sex>NNNN-VV.anim
  <out>/character/<race>/<sex>/<race><sex>.hdlook  appearance tables (below)
  <out>/<Enhanced path of every texture they use>

Wowee reads these from its HD characters folder when the "HD characters"
setting is on (see src/pipeline/hd_character_look.cpp); with it off they are
never read. Nothing converted here belongs in git.

The .hdlook file maps the 3.3.5 appearance indices (the five bytes the server
stores: skin, face, hair style, hair colour, facial features) onto Enhanced's
customization choices, with Enhanced's own table for that,
ChrCustomizationConversion (legacy option and index -> choice, a face's
depending on the skin). Enhanced has none for blood elves and draenei, which it
does not ship as playable; there index N is the choice with OrderIndex N, or,
given --wowee-data, the skin and hair colour nearest the 3.3.5 one. Lines:

  order T T T ...          body layers by ChrModelTextureTarget, bottom first
  hairtex T                the target that is the hair texture (type 6)
  choice L I C O [DL DI]   3.3.5 option L (1 skin, 2 face, 3 hair style,
                           4 hair colour, 5 facial) at index I picks choice C
                           of Enhanced option O; only when option DL is at DI
  default C O              choice C stands for option O when no 3.3.5 one
                           picked anything in O (eye colour, ears, brows)
  tex C R T PATH           choice C gives target T; when R is not 0, only
                           together with choice R
  geo C ID                 choice C shows geoset ID; choice 0 is always shown
  bone C PATH              choice C (a face) moves bones by the .bone file

usage: convert.py --out DIR [--cache DIR] [--db2 DIR] [--listfile CSV]
                  [--share URL] [models...]     (default: all twenty)
"""
import argparse
import csv
import os
import struct
import sys
import urllib.request
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from enhanced_cdn import Enhanced, DEFAULT_SHARE  # noqa: E402

BUILD = '1.60.1.70862'
DB2_TABLES = ['ChrModel', 'ChrRaceXChrModel', 'CreatureDisplayInfo', 'CreatureModelData',
              'ChrCustomizationOption', 'ChrCustomizationChoice', 'ChrCustomizationElement',
              'ChrCustomizationGeoset', 'ChrCustomizationMaterial', 'ChrCustomizationReq',
              'ChrModelTextureLayer', 'TextureFileData', 'ChrCustomizationConversion',
              'ChrCustomizationBoneSet']

# 3.3.5 race id -> model folder (Undead art lives under Scourge).
RACES = {1: 'Human', 2: 'Orc', 3: 'Dwarf', 4: 'NightElf', 5: 'Scourge', 6: 'Tauren',
         7: 'Gnome', 8: 'Troll', 10: 'BloodElf', 11: 'Draenei'}
SEXES = {0: 'Male', 1: 'Female'}

# Enhanced's option for each of the five 3.3.5 appearance bytes, by name: the
# first of these names the model has. The fifth byte is the race's "facial
# features" (CharacterFacialHairStyles): a beard, piercings, tusks, markings...
# Where Enhanced split one into two (a dwarf's Beard and Mustache, an undead's
# Face and Jaw Features) the second is left at its first choice.
CLASSIC_OPTIONS = [
    ('skin', ['Skin Color']),
    ('face', ['Face']),
    ('hairstyle', ['Hair Style', 'Horn Style']),
    ('haircolor', ['Hair Color', 'Horn Color']),
    ('facial', ['Facial Hair', 'Piercings', 'Beard', 'Markings', 'Face Features', 'Earrings',
                'Tusks', 'Horns', 'Hair']),
]

# Geoset groups that equipment or the 3.3.5 appearance already decides.
DECIDED_GROUPS = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 15, 17, 18, 20}

# Enhanced texture types Wowee's character renderer has no slot for.
TEX_EYES = 19


# ---------------------------------------------------------------- data tables
class Tables:
    def __init__(self, db2_dir):
        self.dir = db2_dir
        os.makedirs(db2_dir, exist_ok=True)
        self.t = {n: self._load(n) for n in DB2_TABLES}

    def _load(self, name):
        fn = os.path.join(self.dir, name + '.csv')
        if not os.path.exists(fn):
            url = 'https://wago.tools/db2/%s/csv?build=%s' % (name, BUILD)
            data = urllib.request.urlopen(url, timeout=300).read()
            with open(fn + '.part', 'wb') as f:
                f.write(data)
            os.replace(fn + '.part', fn)
        with open(fn, newline='') as f:
            return list(csv.DictReader(f))

    def __getitem__(self, name):
        return self.t[name]


def load_listfile(path):
    out = {}
    with open(path, errors='replace') as f:
        for line in f:
            a, _, b = line.strip().partition(';')
            if a.isdigit():
                out[int(a)] = b
    return out


# ---------------------------------------------------------------- M2 handling
def chunks(d):
    out, p = {}, 0
    while p + 8 <= len(d):
        tag = d[p:p + 4]
        n = struct.unpack_from('<I', d, p + 4)[0]
        out[tag] = d[p + 8:p + 8 + n]
        p += 8 + n
    return out


# MD20 header: offset of the M2Array for each field we touch.
OFS_SEQUENCES = 0x1C
OFS_VERTICES = 0x3C
OFS_TEXTURES = 0x50


def skin_vertex_count(skin):
    """How many of the model's vertices the first skin uses. The HD models
    carry every LOD's vertices (226,301 for HumanMale) and the first skin
    indexes the first 37,813; a 3.3.5 skin indexes in 16 bits anyway."""
    n, o = struct.unpack_from('<II', skin, 4)
    return max(struct.unpack_from('<%dH' % n, skin, o)) + 1 if n else 0


def convert_m2(md20, texture_names, eye_texture, vertex_count=0):
    """MD21 payload -> MD20 v264 with texture file names filled in.

    texture_names: index -> path for the hard-coded (type 0) textures, whose
    names Enhanced moved to the TXID chunk. Type 19 (eyes) becomes a hard-coded
    texture of the default eye colour, as 3.3.5 has no eye option."""
    m = bytearray(md20)
    struct.pack_into('<I', m, 4, 264)
    if vertex_count and vertex_count < struct.unpack_from('<I', m, OFS_VERTICES)[0]:
        struct.pack_into('<I', m, OFS_VERTICES, vertex_count)
    n, o = struct.unpack_from('<II', m, OFS_TEXTURES)
    for i in range(n):
        typ = struct.unpack_from('<I', m, o + i * 16)[0]
        name = None
        if typ == 0:
            name = texture_names.get(i)
        elif typ == TEX_EYES and eye_texture:
            struct.pack_into('<I', m, o + i * 16, 0)
            name = eye_texture
        if not name:
            continue
        raw = name.encode() + b'\0'
        while len(m) % 16:
            m.append(0)
        struct.pack_into('<II', m, o + i * 16 + 8, len(raw), len(m))
        m += raw
    convert_cameras(m)
    # Neither the HD player models nor 3.3.5 ones carry these; a model that
    # did would need its emitters rewritten (Cata changed both layouts).
    for ofs, what in ((OFS_RIBBONS, 'ribbon emitters'), (OFS_PARTICLES, 'particle emitters')):
        if struct.unpack_from('<I', m, ofs)[0]:
            print('  dropping %s (layout differs from 3.3.5)' % what)
            struct.pack_into('<II', m, ofs, 0, 0)
    return bytes(m)


OFS_CAMERAS = 0x110
OFS_RIBBONS = 0x120
OFS_PARTICLES = 0x128
CAMERA_CATA = 116   # type, far, near, 3 tracks + 2 bases, then a FoV track
CAMERA_WOTLK = 100  # type, fov, far, near, 3 tracks + 2 bases


def convert_cameras(m):
    """Cataclysm made the camera's field of view a track at the end of the
    record; 3.3.5 has a float after the type. Rewrite the array at the end
    of the file in the 3.3.5 layout, with the FoV track's first key."""
    n, o = struct.unpack_from('<II', m, OFS_CAMERAS)
    if not n:
        return
    out = bytearray()
    for i in range(n):
        c = bytes(m[o + i * CAMERA_CATA:o + (i + 1) * CAMERA_CATA])
        fov = 0.7853982
        nvals, ovals = struct.unpack_from('<II', c, 96 + 12)
        if nvals:
            k, ko = struct.unpack_from('<II', m, ovals)
            if k:
                fov = struct.unpack_from('<f', m, ko)[0]
        out += c[0:4] + struct.pack('<f', fov) + c[4:96]
    while len(m) % 16:
        m.append(0)
    struct.pack_into('<II', m, OFS_CAMERAS, n, len(m))
    m += out


def anim_payload(d):
    """A Enhanced .anim is either the raw track data 3.3.5 used, or the same
    in chunks: AFM2 for a model's own bones, AFSB for a .skel's (whose bone
    tracks point into it, relative to its start, as merge_skeleton leaves
    them). Wowee reads bone tracks only."""
    if d[:4] in (b'AFM2', b'AFSA', b'AFSB'):
        c = chunks(d)
        return c.get(b'AFSB') or c[b'AFM2']
    return d


# MD20 header fields a .skel provides.
OFS_GLOBAL_LOOPS = 0x14
OFS_SEQ_LOOKUP = 0x24
OFS_BONES = 0x2C
OFS_KEY_BONE_LOOKUP = 0x34
OFS_ATTACHMENTS = 0xF0
OFS_ATTACH_LOOKUP = 0xF8
BONE_SIZE = 88
BONE_TRACKS = (16, 36, 56)  # translation, rotation, scale
ATTACHMENT_SIZE = 40
ATTACHMENT_TRACKS = (20,)


def relocate_track(m, at, base, inline):
    """An M2Track at m[at]: move its two outer arrays, and the per-sequence
    inner arrays of the inline sequences, by base. The inner arrays of the
    other sequences point into their .anim files and stay."""
    for arr in (at + 4, at + 12):
        n, o = struct.unpack_from('<II', m, arr)
        if not n:
            continue
        struct.pack_into('<I', m, arr + 4, o + base)
        for i in range(n):
            q = o + base + i * 8
            cnt, ofs = struct.unpack_from('<II', m, q)
            if cnt and (i >= len(inline) or inline[i]):
                struct.pack_into('<I', m, q + 4, ofs + base)


def merge_skeleton(md20, skel):
    """Put a .skel's chunks into the model so it reads as one 3.3.5 M2:
    each chunk is appended whole and its offsets (relative to the chunk)
    moved to where it now lies."""
    m = bytearray(md20)

    def append(payload):
        while len(m) % 16:
            m.append(0)
        base = len(m)
        m.extend(payload)
        return base

    sks = skel[b'SKS1']
    b = append(sks)
    for field, at in ((OFS_GLOBAL_LOOPS, 0), (OFS_SEQUENCES, 8), (OFS_SEQ_LOOKUP, 16)):
        n, o = struct.unpack_from('<II', sks, at)
        struct.pack_into('<II', m, field, n, o + b if n else 0)
    n, o = struct.unpack_from('<II', m, OFS_SEQUENCES)
    inline = [bool(struct.unpack_from('<I', m, o + i * 64 + 12)[0] & 0x20) for i in range(n)]

    for tag, fields, size, tracks in (
            (b'SKB1', (OFS_BONES, OFS_KEY_BONE_LOOKUP), BONE_SIZE, BONE_TRACKS),
            (b'SKA1', (OFS_ATTACHMENTS, OFS_ATTACH_LOOKUP), ATTACHMENT_SIZE, ATTACHMENT_TRACKS)):
        if tag not in skel:
            continue
        payload = skel[tag]
        b = append(payload)
        for k, field in enumerate(fields):
            n, o = struct.unpack_from('<II', payload, k * 8)
            struct.pack_into('<II', m, field, n, o + b if n else 0)
        n, o = struct.unpack_from('<II', m, fields[0])
        for i in range(n):
            for t in tracks:
                relocate_track(m, o + i * size + t, b, inline)
    return bytes(m)


def skin_geosets(skin):
    nsub, osub = struct.unpack_from('<II', skin, 28)
    return {struct.unpack_from('<H', skin, osub + i * 48)[0] for i in range(nsub)}


def default_geosets(skin, look):
    """Every group 3.3.5 and equipment do not decide draws its first variant
    unless a chosen choice names another, as Enhanced does (the eyes, 3301;
    the torso, 2201). Written as choice 0; Wowee lets a chosen geoset of the
    same group replace it."""
    have = skin_geosets(skin)
    out = []
    for grp in sorted({g // 100 for g in have} - DECIDED_GROUPS):
        # Group 32 is the face. Its first variant is a stub of some sixty
        # triangles; the face is 3202 (the human's face shapes add 3203-3204,
        # chosen through Face Shape). Races without that option need 3202.
        if grp == 32 and 3202 in have:
            out.append('geo 0 3202')
            continue
        if grp * 100 + 1 in have:
            out.append('geo 0 %d' % (grp * 100 + 1))
    return out


def check_blp(d, name):
    if d[:4] != b'BLP2':
        raise ValueError('%s: not BLP2' % name)
    enc, alpha_type = d[8], d[10]
    if enc not in (1, 2, 3) or (enc == 2 and alpha_type not in (0, 1, 7)):
        raise ValueError('%s: BLP encoding %d/%d Wowee does not decode' % (name, enc, alpha_type))


# ---------------------------------------------------------------- conversion
class Converter:
    def __init__(self, args):
        self.args = args
        self.db = Tables(args.db2)
        self.lf = load_listfile(args.listfile)
        self.src = Enhanced(args.share, args.cache)
        self.out = args.out
        self.written_tex = set()
        db = self.db
        self.choices = defaultdict(list)
        for c in db['ChrCustomizationChoice']:
            self.choices[c['ChrCustomizationOptionID']].append(c)
        self.elements = defaultdict(list)
        for e in db['ChrCustomizationElement']:
            self.elements[e['ChrCustomizationChoiceID']].append(e)
        self.geo = {g['ID']: g for g in db['ChrCustomizationGeoset']}
        self.boneset = {b['ID']: b for b in db['ChrCustomizationBoneSet']}
        self.mat = {m['ID']: m for m in db['ChrCustomizationMaterial']}
        self.tfd = defaultdict(list)
        for r in db['TextureFileData']:
            self.tfd[r['MaterialResourcesID']].append(int(r['FileDataID']))
        self.req = {r['ID']: r for r in db['ChrCustomizationReq']}

    def read(self, fdid):
        d = self.src.read(fdid)
        if d is None:
            raise KeyError('file %d is not in this build' % fdid)
        return d

    def path_of(self, fdid):
        p = self.lf.get(fdid)
        return p if p else 'hd_unnamed/%d.blp' % fdid

    def put(self, rel, data):
        fn = os.path.join(self.out, rel.replace('\\', '/').lower())
        os.makedirs(os.path.dirname(fn), exist_ok=True)
        with open(fn + '.part', 'wb') as f:
            f.write(data)
        os.replace(fn + '.part', fn)

    def texture(self, fdid):
        """Write a Enhanced texture into the HD tree; return Wowee's name for it."""
        rel = self.path_of(fdid)
        if fdid not in self.written_tex:
            d = self.read(fdid)
            check_blp(d, rel)
            self.put(rel, d)
            self.written_tex.add(fdid)
        return 'HD\\' + rel.replace('/', '\\')

    def bone_file(self, fdid):
        """A face's bone adjustments (.bone: BIDA bone ids, BOMT matrices),
        copied as they are."""
        rel = self.path_of(fdid)
        if not rel.endswith('.bone'):
            rel = 'hd_unnamed/%d.bone' % fdid
        if fdid not in self.written_tex:
            d = self.read(fdid)
            if d[:4] != b'\x01\x00\x00\x00':
                raise ValueError('%s: .bone version %r' % (rel, d[:4]))
            self.put(rel, d)
            self.written_tex.add(fdid)
        return 'HD\\' + rel.replace('/', '\\')

    def chr_model(self, race, sex):
        for r in self.db['ChrRaceXChrModel']:
            if int(r['ChrRacesID']) == race and int(r['Sex']) == sex:
                return r['ChrModelID']
        raise KeyError('no ChrModel for race %d sex %d' % (race, sex))

    def model_fdid(self, chr_model_id):
        cm = [r for r in self.db['ChrModel'] if r['ID'] == chr_model_id][0]
        cdi = [r for r in self.db['CreatureDisplayInfo'] if r['ID'] == cm['DisplayID']][0]
        cmd = [r for r in self.db['CreatureModelData'] if r['ID'] == cdi['ModelID']][0]
        return int(cmd['FileDataID']), int(cm['CharComponentTextureLayoutID'])

    def convert(self, race, sex):
        folder, sexn = RACES[race], SEXES[sex]
        base = '%s%s' % (folder, sexn)
        rel_dir = 'character/%s/%s/' % (folder.lower(), sexn.lower())
        cmid = self.chr_model(race, sex)
        fdid, layout = self.model_fdid(cmid)
        print('%s: ChrModel %s, model %d (%s), layout %d' % (base, cmid, fdid, self.path_of(fdid), layout))

        c = chunks(self.read(fdid))
        if b'MD21' not in c:
            raise ValueError('%s: not a chunked M2' % base)
        md20 = c[b'MD21']
        afid = c.get(b'AFID', b'')
        if b'SKID' in c:
            # Legion's remakes (blood elves, draenei) keep the skeleton, the
            # sequences and the attachments in a .skel beside the model.
            skel = chunks(self.read(struct.unpack('<I', c[b'SKID'])[0]))
            if b'SKPD' in skel and struct.unpack_from('<I', skel[b'SKPD'], 8)[0]:
                raise ValueError('%s: .skel with a parent skeleton, not handled' % base)
            md20 = merge_skeleton(md20, skel)
            afid = skel.get(b'AFID', b'')
        txid = struct.unpack_from('<%dI' % (len(c.get(b'TXID', b'')) // 4), c.get(b'TXID', b''))
        sfid = struct.unpack_from('<%dI' % (len(c[b'SFID']) // 4), c[b'SFID'])
        anim_files = {}
        for i in range(0, len(afid), 8):
            aid, sub, fd = struct.unpack_from('<HHI', afid, i)
            if fd:
                anim_files[(aid, sub)] = fd

        look, eye = self.look(race, sex, cmid, layout)
        tex_names = {i: self.texture(fd) for i, fd in enumerate(txid) if fd}
        eye_name = self.texture(eye) if eye else None
        skin = self.read(sfid[0])
        self.put(rel_dir + base + '.m2', convert_m2(md20, tex_names, eye_name, skin_vertex_count(skin)))
        self.put(rel_dir + base + '00.skin', skin)
        look += default_geosets(skin, look)

        n, o = struct.unpack_from('<II', md20, OFS_SEQUENCES)
        wrote = 0
        for i in range(n):
            aid, var = struct.unpack_from('<HH', md20, o + i * 64)
            flags = struct.unpack_from('<I', md20, o + i * 64 + 12)[0]
            if flags & 0x20 or flags & 0x40:
                continue
            fd = anim_files.get((aid, var))
            if not fd:
                continue
            self.put(rel_dir + '%s%04d-%02d.anim' % (base, aid, var), anim_payload(self.read(fd)))
            wrote += 1

        lines = ['# %s from Enhanced %s, ChrModel %s' % (base, BUILD, cmid)] + look
        self.put(rel_dir + base + '.hdlook', ('\n'.join(lines) + '\n').encode())
        print('  %d sequences, %d .anim files, %d textures so far' % (n, wrote, len(self.written_tex)))

    def look(self, race, sex, cmid, layout):
        db = self.db
        opts = sorted([o for o in db['ChrCustomizationOption'] if o['ChrModelID'] == cmid],
                      key=lambda o: int(o['OrderIndex']))
        option_of = {}  # choice id -> option id
        for o in opts:
            for c in self.choices[o['ID']]:
                option_of[c['ID']] = o['ID']

        out = []
        layers = sorted([r for r in db['ChrModelTextureLayer']
                         if int(r['CharComponentTextureLayoutsID']) == layout],
                        key=lambda r: int(r['Layer']))
        out.append('order ' + ' '.join(r['ChrModelTextureTargetID_0'] for r in layers if r['TextureType'] == '1'))
        hair = [r['ChrModelTextureTargetID_0'] for r in layers if r['TextureType'] == '6']
        if hair:
            out.append('hairtex ' + hair[0])
        eye_targets = {r['ChrModelTextureTargetID_0'] for r in layers if r['TextureType'] == str(TEX_EYES)}

        # 3.3.5 index -> choice: Enhanced's conversion table, else OrderIndex.
        used = set()
        conv = [r for r in db['ChrCustomizationConversion']
                if r['ChrRacesID'] == str(race) and r['Sex'] == str(sex)
                and r['ChrCustomizationChoiceID'] in option_of]
        touched = {}  # legacy option -> Enhanced options its conversion rows reach
        for r in conv:
            c = r['ChrCustomizationChoiceID']
            dep = '' if r['DependentOptionID'] == '0' else ' %s %s' % (r['DependentOptionID'], r['DependentData'])
            out.append('choice %s %s %s %s%s' % (r['OptionID'], r['Data'], c, option_of[c], dep))
            used.add(c)
            touched.setdefault(int(r['OptionID']), set()).add(option_of[c])
        # Where the table does not reach the option a 3.3.5 byte is named
        # for - blood elves and draenei, which Enhanced does not ship playable,
        # or an orc's facial hair, which it converts to Grime alone - that
        # option's choices by OrderIndex (or nearest colour).
        fallback = []
        for legacy, (on, names) in enumerate(CLASSIC_OPTIONS, start=1):
            found = [o for n in names for o in opts if o['Name_lang'] == n]
            if not found:
                if legacy not in touched:
                    raise ValueError('ChrModel %s has no %s option' % (cmid, on))
                continue
            if found[0]['ID'] in touched.get(legacy, set()):
                continue
            ch = sorted(self.choices[found[0]['ID']], key=lambda c: int(c['OrderIndex']))
            for idx, c in self.colour_order(race, sex, legacy, ch):
                out.append('choice %d %d %s %s' % (legacy, idx, c['ID'], found[0]['ID']))
                used.add(c['ID'])
            fallback.append(on)
        print('  appearance: %d ChrCustomizationConversion rows%s' %
              (len(conv), '; by OrderIndex: ' + ', '.join(fallback) if fallback else ''))

        # The first choice of every other option, as a new Enhanced character
        # has it. Options behind a requirement (an NPC-only eye style) stay off.
        for o in opts:
            if o['Requirement'] != '0':
                continue
            ch = sorted(self.choices[o['ID']], key=lambda c: int(c['OrderIndex']))
            if ch:
                out.append('default %s %s' % (ch[0]['ID'], o['ID']))
                used.add(ch[0]['ID'])

        eye = None
        for cid in sorted(used, key=int):
            for e in self.elements[cid]:
                g = self.geo.get(e['ChrCustomizationGeosetID'])
                if g:
                    out.append('geo %s %d' % (cid, int(g['GeosetType']) * 100 + int(g['GeosetID'])))
                b = self.boneset.get(e['ChrCustomizationBoneSetID'])
                if b and b['BoneFileDataID'] != '0':
                    try:
                        out.append('bone %s %s' % (cid, self.bone_file(int(b['BoneFileDataID']))))
                    except KeyError as err:  # a file the build does not carry
                        print('  skipped: %s' % err)
                mt = self.mat.get(e['ChrCustomizationMaterialID'])
                fds = self.tfd.get(mt['MaterialResourcesID'], []) if mt else []
                if not fds:
                    continue
                tgt = mt['ChrModelTextureTargetID']
                rel = e['RelatedChrCustomizationChoiceID']
                if tgt in eye_targets:
                    # Eyes are drawn by their own texture slot; 3.3.5 has no
                    # eye colour, so the default one is baked into the model.
                    if eye is None and rel == '0' and option_of.get(cid) and \
                            any(l == 'default %s %s' % (cid, option_of[cid]) for l in out):
                        eye = fds[0]
                    continue
                if rel != '0' and rel not in used:
                    continue
                try:
                    out.append('tex %s %s %s %s' % (cid, rel, tgt, self.texture(fds[0])))
                except KeyError as err:  # a file the build does not carry
                    print('  skipped: %s' % err)
        return out, eye

    def colour_order(self, race, sex, legacy, choices):
        """(3.3.5 index, choice) pairs for a race Enhanced has no conversion
        rows for: OrderIndex, or for skin and hair colour (given the 3.3.5
        data) the choice whose texture is nearest in colour to CharSections'."""
        plain = [(int(c['OrderIndex']), c) for c in choices]
        if not self.args.wowee_data or legacy not in (1, 4):
            return plain
        section, target = (0, '1') if legacy == 1 else (3, '10')
        classic = classic_colours(self.args.wowee_data, race, sex, section)
        hd = []
        for c in choices:
            for e in self.elements[c['ID']]:
                mt = self.mat.get(e['ChrCustomizationMaterialID'])
                fds = self.tfd.get(mt['MaterialResourcesID'], []) if mt else []
                if fds and mt['ChrModelTextureTargetID'] == target:
                    hd.append((c, blp_mean(self.read(fds[0]))))
                    break
        if not classic or not hd:
            return plain
        out = []
        for idx, col in sorted(classic.items()):
            best = min(hd, key=lambda h: sum((a - b) ** 2 for a, b in zip(h[1], col)))
            out.append((idx, best[0]))
        return out


def blp_mean(d):
    """Mean colour of a BLP's top mip (palette, DXT endpoints or BGRA), opaque
    pixels only. Enough to tell skin and hair colours apart."""
    enc, alpha_depth, alpha_type = d[8], d[9], d[10]
    w, h = struct.unpack_from('<II', d, 12)
    off, size = struct.unpack_from('<I', d, 20)[0], struct.unpack_from('<I', d, 84)[0]
    m = d[off:off + size]
    tot, n = [0, 0, 0], 0
    if enc == 1:
        pal = d[148:148 + 1024]
        for i in range(0, w * h, 7):
            if alpha_depth == 8 and m[w * h + i] < 128:
                continue
            k = m[i] * 4
            tot[0] += pal[k + 2]; tot[1] += pal[k + 1]; tot[2] += pal[k]; n += 1
    elif enc == 2:
        bs = 8 if alpha_type == 0 else 16
        for p in range(0, len(m) - bs + 1, bs):
            if bs == 16 and alpha_type == 7 and m[p] < 128 and m[p + 1] < 128:
                continue
            for c in struct.unpack_from('<HH', m, p + bs - 8):
                tot[0] += ((c >> 11) & 31) * 255 // 31
                tot[1] += ((c >> 5) & 63) * 255 // 63
                tot[2] += (c & 31) * 255 // 31
                n += 1
    else:
        for i in range(0, min(len(m), w * h * 4), 28):
            if m[i + 3] < 128:
                continue
            tot[0] += m[i + 2]; tot[1] += m[i + 1]; tot[2] += m[i]; n += 1
    return tuple(t / max(n, 1) for t in tot)


def classic_colours(data_dir, race, sex, section):
    """3.3.5 CharSections: colour index -> mean colour of its first texture,
    for the skin (section 0) or hair (section 3) rows of a race and sex."""
    fn = os.path.join(data_dir, 'dbfilesclient', 'charsections.dbc')
    if not os.path.exists(fn):
        return {}
    d = open(fn, 'rb').read()
    n, f, rs, _ = struct.unpack_from('<4I', d, 4)
    strs = d[20 + n * rs:]
    out = {}
    for i in range(n):
        r = struct.unpack_from('<%dI' % f, d, 20 + i * rs)
        if r[1] != race or r[2] != sex or r[3] != section or r[9] in out:
            continue
        name = strs[r[4]:strs.index(b'\0', r[4])].decode(errors='replace')
        path = os.path.join(data_dir, name.replace('\\', '/').lower())
        if name and os.path.exists(path):
            out[r[9]] = blp_mean(open(path, 'rb').read())
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True, help='Wowee HD characters folder to write')
    ap.add_argument('--cache', default=os.path.join(here, '.cache', 'enhanced'))
    ap.add_argument('--db2', default=os.path.join(here, '.cache', 'db2'))
    ap.add_argument('--listfile', required=True, help='community-listfile.csv (FileDataID;path)')
    ap.add_argument('--share', default=DEFAULT_SHARE)
    ap.add_argument('--wowee-data', help="Wowee's 3.3.5 data (Data/expansions/wotlk), to match "
                                         'colours where Enhanced has no conversion table')
    ap.add_argument('models', nargs='*', help='e.g. HumanMale OrcFemale (default: all)')
    args = ap.parse_args()
    conv = Converter(args)
    want = {m.lower() for m in args.models}
    failed = []
    for race, folder in RACES.items():
        for sex, sexn in SEXES.items():
            name = folder + sexn
            if want and name.lower() not in want:
                continue
            try:
                conv.convert(race, sex)
            except Exception as e:  # report and go on with the next model
                print('%s: FAILED: %s' % (name, e))
                failed.append(name)
    if failed:
        print('failed:', ' '.join(failed))
        sys.exit(1)


if __name__ == '__main__':
    main()
