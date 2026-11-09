"""Read single files out of a the reference client install, by FileDataID.

The install is reached over plain HTTP (Jered's LAN share of the game folder),
which gives the build and CDN configuration and the archive-group index. That
share cannot serve byte ranges, so the file bytes themselves are fetched by
range from Blizzard's public CDN, which holds the same archives. Everything
fetched is cached under --cache; nothing here is ever executed.

    from enhanced_cdn import Enhanced
    f = Enhanced(share_url, cache_dir)
    data = f.read(fdid)          # bytes or None if the build has no such file
"""
import http.client
import os
import pickle
import struct
import time
import urllib.request
import zlib

DEFAULT_SHARE = 'http://192.168.1.97:8080/WoW/World%20of%20Warcraft/'
DEFAULT_PRODUCT = 'wowf'


def _blte(b):
    if b[:4] != b'BLTE':
        raise ValueError('not BLTE')
    hs = struct.unpack_from('>I', b, 4)[0]
    if hs == 0:
        chunks, p = [(len(b) - 8, 0)], 8
    else:
        cnt = int.from_bytes(b[9:12], 'big')
        chunks = [struct.unpack_from('>II', b, 12 + i * 24) for i in range(cnt)]
        p = hs
    out = []
    for cs, ds in chunks:
        c = b[p:p + cs]
        p += cs
        mode = c[:1]
        if mode == b'N':
            out.append(c[1:])
        elif mode == b'Z':
            out.append(zlib.decompress(c[1:]))
        elif mode == b'F':
            out.append(_blte(c[1:]))
        elif mode == b'E':
            # Encrypted chunk without a key: zero-filled, like the client does.
            out.append(b'\0' * ds)
        else:
            raise ValueError('BLTE mode %r' % mode)
    return b''.join(out)


def _parse_encoding(b):
    if b[:2] != b'EN':
        raise ValueError('not an encoding file')
    cps = struct.unpack_from('>H', b, 5)[0]
    cpc = struct.unpack_from('>I', b, 9)[0]
    espec = struct.unpack_from('>I', b, 18)[0]
    p = 22 + espec + cpc * 32
    m = {}
    for i in range(cpc):
        page = b[p + i * cps * 1024:p + (i + 1) * cps * 1024]
        q = 0
        while q + 22 <= len(page):
            kc = page[q]
            if kc == 0:
                break
            m[page[q + 6:q + 22]] = page[q + 22:q + 38]
            q += 22 + 16 * kc
    return m


def _parse_root(b):
    """FileDataID -> content key, enUS (or locale-neutral) entries preferred."""
    if b[:4] != b'TSFM':
        raise ValueError('not an MFST root')
    hsz, ver = struct.unpack_from('<II', b, 4)
    if hsz > 0x100:
        ver, p = 0, 12
    else:
        p = hsz
    files = {}
    while p < len(b):
        if ver >= 2:
            n, loc, f1, f2 = struct.unpack_from('<IIII', b, p)
            content = f1 | f2 | (b[p + 16] << 17)
            p += 17
        else:
            n, content, loc = struct.unpack_from('<III', b, p)
            p += 12
        deltas = struct.unpack_from('<%di' % n, b, p)
        p += 4 * n
        ckeys = [b[p + i * 16:p + i * 16 + 16] for i in range(n)]
        p += 16 * n
        if not content & 0x10000000:
            p += 8 * n
        fd = -1
        for i in range(n):
            fd += 1 + deltas[i]
            if (loc & 0x2) or fd not in files:
                files[fd] = ckeys[i]
    return files


class Enhanced:
    def __init__(self, share=DEFAULT_SHARE, cache='enhanced-cache', product=DEFAULT_PRODUCT):
        self.share = share if share.endswith('/') else share + '/'
        self.cache = cache
        os.makedirs(cache, exist_ok=True)
        self.product = product
        self._load()

    # -- plumbing -----------------------------------------------------------
    def _cached(self, name, fetch):
        fn = os.path.join(self.cache, name)
        if not os.path.exists(fn):
            data = fetch()
            with open(fn + '.part', 'wb') as f:
                f.write(data)
            os.replace(fn + '.part', fn)
        with open(fn, 'rb') as f:
            return f.read()

    def _share(self, rel):
        return self._cached(rel.replace('/', '_'),
                            lambda: urllib.request.urlopen(self.share + rel, timeout=120).read())

    def _config(self, key):
        txt = self._share('Data/config/%s/%s/%s' % (key[0:2], key[2:4], key)).decode()
        out = {}
        for line in txt.splitlines():
            if '=' in line:
                k, v = line.split('=', 1)
                out[k.strip()] = v.strip()
        return out

    def _cdn_raw(self, ek_hex, rng=None, archive=None):
        name = archive or ek_hex
        url = '%sdata/%s/%s/%s' % (self.cdn, name[0:2], name[2:4], name)
        req = urllib.request.Request(url)
        if rng:
            req.add_header('Range', 'bytes=%d-%d' % (rng[0], rng[0] + rng[1] - 1))
        for attempt in range(4):
            try:
                return urllib.request.urlopen(req, timeout=120).read()
            except (OSError, http.client.HTTPException):
                # The CDN drops the odd connection under many small ranges.
                if attempt == 3:
                    raise
                time.sleep(1 + attempt * 2)

    def _load(self):
        state = os.path.join(self.cache, 'state.pickle')
        if os.path.exists(state):
            with open(state, 'rb') as f:
                self.cdn, self.enc, self.files, self.group = pickle.load(f)
            return
        info = self._share('.build.info').decode().splitlines()
        hdr = [h.split('!')[0] for h in info[0].split('|')]
        rows = [dict(zip(hdr, line.split('|'))) for line in info[1:] if line]
        row = [r for r in rows if r.get('Product') == self.product][0]
        host = row['CDN Hosts'].split()[0]
        self.cdn = 'http://%s/%s/' % (host, row['CDN Path'])
        bc = self._config(row['Build Key'])
        cc = self._config(row['CDN Key'])
        self.group = self._group_index(cc)
        enc_ek = bc['encoding'].split()[1]
        enc_raw = self._cached('encoding', lambda: self._read_ekey(bytes.fromhex(enc_ek), raw=True))
        self.enc = _parse_encoding(_blte(enc_raw))
        root_ck = bytes.fromhex(bc['root'].split()[0])
        self.files = _parse_root(self._read_ekey(self.enc[root_ck]))
        with open(state, 'wb') as f:
            pickle.dump((self.cdn, self.enc, self.files, self.group), f)

    def _group_index(self, cc):
        archives = cc['archives'].split()
        d = self._share('Data/indices/%s.index' % cc['archive-group'])
        ft = d[-28:]
        bkb, offb, szb, keyb = ft[11], ft[12], ft[13], ft[14]
        n = struct.unpack_from('<I', ft, 16)[0]
        es, bs = keyb + szb + offb, bkb * 1024
        m, p, cnt = {}, 0, 0
        while cnt < n and p + bs <= len(d):
            q = 0
            while q + es <= bs and cnt < n:
                k = d[p + q:p + q + keyb]
                if k == b'\0' * keyb:
                    break
                sz = int.from_bytes(d[p + q + keyb:p + q + keyb + szb], 'big')
                ov = d[p + q + keyb + szb:p + q + es]
                m[k[:9]] = (archives[int.from_bytes(ov[:2], 'big')], int.from_bytes(ov[2:], 'big'), sz)
                q += es
                cnt += 1
            p += bs
        return m

    def _read_ekey(self, ek, raw=False):
        e = self.group.get(ek[:9])
        if e is None:
            data = self._cdn_raw(ek.hex())
        else:
            data = self._cdn_raw(None, rng=(e[1], e[2]), archive=e[0])
        return data if raw else _blte(data)

    # -- public -------------------------------------------------------------
    def has(self, fdid):
        ck = self.files.get(fdid)
        return ck is not None and ck in self.enc

    def read(self, fdid):
        """The file's bytes, cached under cache/files/<fdid>; None if absent."""
        if not self.has(fdid):
            return None
        os.makedirs(os.path.join(self.cache, 'files'), exist_ok=True)
        return self._cached('files/%d' % fdid, lambda: self._read_ekey(self.enc[self.files[fdid]]))
