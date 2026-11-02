#!/usr/bin/env python3
"""Read a model (the Portal gun view model, or a Portal 2 character) from the user's own Portal 2 install and write a small local cache.

usage: portalgun_extract.py [portal2_dir] [out.bin] [--preview prefix] [--character chell|atlas|pbody|all]
  portal2_dir  folder holding pak01_dir.vpk (default: the macOS Steam install)
  out.bin      default ~/Library/Application Support/Skate3PortalMod/portalgun.bin (or <character>.bin with --character)
  --character  a player model with its skeleton and skin weights: chell (models/player/chell/player), atlas (models/player/ballbot/*),
               pbody (models/player/eggbot/*); "all" writes all three. Lists the candidate .mdl files it chose from.
Nothing is bundled: the cache is derived on the user's machine from their own copy.

Cache format (little endian): "PGUN" u32 version (1 or 2), u32 ntex, u32 nmesh;
  per texture: u32 w, u32 h, w*h*4 RGBA bytes;
  per mesh: u32 tex_index (0xFFFFFFFF = none), u32 nverts (multiple of 3), nverts * 8 floats (x y z nx ny nz u v);
    version 2 only: then nverts * 16 bytes of skin: 3 x u8 bone, u8 bone count, 3 x f32 weight;
  version 2 only, after the meshes: u32 nbones, per bone: 32 bytes name (NUL padded), i32 parent, 12 x f32 bone -> model matrix (three
    rows [R | t], the inverse of the model's pose-to-bone matrix: the bind pose the skin weights were made for).
Positions are the model's own space (the player models: y up, inches)."""
import os, struct, sys, zlib, math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vpk_list

DEFAULT_DIR = os.path.expanduser('~/Library/Application Support/Steam/steamapps/common/Portal 2/portal2')


class Vpk:
    def __init__(self, d):
        self.dir = d
        self.index = {}
        self.blob = open(os.path.join(d, 'pak01_dir.vpk'), 'rb').read()
        self.tree_end = (28 if struct.unpack_from('<I', self.blob, 4)[0] == 2 else 12) + struct.unpack_from('<I', self.blob, 8)[0]
        for name, arch, off, ln, pre, pre_pos in vpk_list.entries(os.path.join(d, 'pak01_dir.vpk')):
            self.index[name.lower()] = (arch, off, ln, pre, pre_pos)

    def read(self, name):
        arch, off, ln, pre, _ = self.index[name.lower()]
        # the preload bytes sit in the tree right after the 18-byte record; find them by re-walking is costly, so
        # entries() does not expose them: files read here have none unless ln == 0 (then they are all preload)
        if ln == 0:
            raise KeyError(name + ' is preload-only')
        if arch == 0x7FFF:
            return self.blob[self.tree_end + off:self.tree_end + off + ln]
        with open(os.path.join(self.dir, 'pak01_%03d.vpk' % arch), 'rb') as f:
            f.seek(off)
            return f.read(ln)


def preload_text(vpk, name):
    """VMT files are tiny and stored in the directory tree itself (preload bytes after the record)."""
    arch, off, ln, pre, pre_pos = vpk.index[name.lower()]
    return vpk.blob[pre_pos:pre_pos + pre].decode('latin1')


def u32(d, o): return struct.unpack_from('<I', d, o)[0]
def i32(d, o): return struct.unpack_from('<i', d, o)[0]
def u16(d, o): return struct.unpack_from('<H', d, o)[0]
def cstr(d, o):
    e = d.index(b'\0', o)
    return d[o:e].decode('latin1')


# ---------------------------------------------------------------- VTF
def dxt_color_block(b, o, opaque_only):
    c0, c1 = struct.unpack_from('<HH', b, o)
    def rgb(c): return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
    p0, p1 = rgb(c0), rgb(c1)
    if c0 > c1 or not opaque_only:
        p2 = tuple((2 * a + b_) // 3 for a, b_ in zip(p0, p1)); p3 = tuple((a + 2 * b_) // 3 for a, b_ in zip(p0, p1))
        pal = [p0 + (255,), p1 + (255,), p2 + (255,), p3 + (255,)]
    else:
        p2 = tuple((a + b_) // 2 for a, b_ in zip(p0, p1))
        pal = [p0 + (255,), p1 + (255,), p2 + (255,), (0, 0, 0, 0)]
    bits = struct.unpack_from('<I', b, o + 4)[0]
    return [pal[(bits >> (2 * i)) & 3] for i in range(16)]


def decode_dxt(data, w, h, fmt):
    out = bytearray(w * h * 4)
    bs = 8 if fmt == 'dxt1' else 16
    o = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            alpha = None
            if fmt == 'dxt3':
                a = struct.unpack_from('<Q', data, o)[0]
                alpha = [((a >> (4 * i)) & 15) * 17 for i in range(16)]
            elif fmt == 'dxt5':
                a0, a1 = data[o], data[o + 1]
                bits = int.from_bytes(data[o + 2:o + 8], 'little')
                tab = [a0, a1] + ([(( 6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1 else
                                  [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
                alpha = [tab[(bits >> (3 * i)) & 7] for i in range(16)]
            cols = dxt_color_block(data, o + (0 if fmt == 'dxt1' else 8), fmt == 'dxt1')
            for i in range(16):
                x, y = bx + (i & 3), by + (i >> 2)
                if x < w and y < h:
                    r, g, b_, a_ = cols[i]
                    p = (y * w + x) * 4
                    out[p:p + 4] = bytes((r, g, b_, alpha[i] if alpha else a_))
            o += bs
    return out


def decode_vtf(d, max_dim=512):
    assert d[:4] == b'VTF\0', 'not a VTF'
    w, h = u16(d, 16), u16(d, 18)
    hi = i32(d, 52)
    fmts = {13: 'dxt1', 14: 'dxt3', 15: 'dxt5', 12: 'bgra', 0: 'rgba', 3: 'bgr', 2: 'rgb'}
    fmt = fmts.get(hi)
    if fmt is None:
        raise ValueError('VTF format %d not handled' % hi)
    if fmt.startswith('dxt'):
        size = ((w + 3) // 4) * ((h + 3) // 4) * (8 if fmt == 'dxt1' else 16)
    else:
        size = w * h * {'bgra': 4, 'rgba': 4, 'bgr': 3, 'rgb': 3}[fmt]
    raw = d[len(d) - size:]  # the largest mip is the last block of the file
    if fmt.startswith('dxt'):
        rgba = decode_dxt(raw, w, h, fmt)
    else:
        n = {'bgra': 4, 'rgba': 4, 'bgr': 3, 'rgb': 3}[fmt]
        rgba = bytearray(w * h * 4)
        for i in range(w * h):
            px = raw[i * n:i * n + n]
            if fmt in ('bgra', 'bgr'):
                r, g, b_ = px[2], px[1], px[0]
            else:
                r, g, b_ = px[0], px[1], px[2]
            a_ = px[3] if n == 4 else 255
            rgba[i * 4:i * 4 + 4] = bytes((r, g, b_, a_))
    while max(w, h) > max_dim:  # box downscale by 2
        nw, nh = w // 2, h // 2
        out = bytearray(nw * nh * 4)
        for y in range(nh):
            for x in range(nw):
                for c in range(4):
                    s = (rgba[((2 * y) * w + 2 * x) * 4 + c] + rgba[((2 * y) * w + 2 * x + 1) * 4 + c] +
                         rgba[((2 * y + 1) * w + 2 * x) * 4 + c] + rgba[((2 * y + 1) * w + 2 * x + 1) * 4 + c])
                    out[(y * nw + x) * 4 + c] = s // 4
        rgba, w, h = out, nw, nh
    return w, h, bytes(rgba)


# ---------------------------------------------------------------- model
def parse_model(mdl, vvd, vtx, lod=0):
    assert mdl[:4] == b'IDST' and vvd[:4] == b'IDSV'
    ntex, tex_idx = u32(mdl, 204), u32(mdl, 208)
    tex_names = [cstr(mdl, tex_idx + 64 * i + i32(mdl, tex_idx + 64 * i)).lower() for i in range(ntex)]
    ncd, cd_idx = u32(mdl, 212), u32(mdl, 216)
    cd_dirs = [cstr(mdl, i32(mdl, cd_idx + 4 * k)) for k in range(ncd)]
    nref, nfam, skin_idx = u32(mdl, 220), u32(mdl, 224), u32(mdl, 228)
    skin0 = [u16(mdl, skin_idx + 2 * i) for i in range(nref)]
    nbp, bp_idx = u32(mdl, 232), u32(mdl, 236)

    # vertices (LOD 0, fixups applied)
    nfix, fix_start, vdata = u32(vvd, 48), u32(vvd, 52), u32(vvd, 56)
    nlod0 = u32(vvd, 16 + 4 * lod)
    def vert(i):
        o = vdata + 48 * i
        # position(3) normal(3) uv(2), then the skin: weights(3), bones(3), bone count (studio vertex: weights at +0, bones at +12, count +15)
        w = struct.unpack_from('<3f', vvd, o)
        b = struct.unpack_from('<3B', vvd, o + 12)
        return struct.unpack_from('<8f', vvd, o + 16) + (b, vvd[o + 15], w)
    lod0 = []
    if nfix == 0:
        lod0 = [vert(i) for i in range(nlod0)]
    else:
        for k in range(nfix):
            lod_f, src, n = struct.unpack_from('<iii', vvd, fix_start + 12 * k)
            if lod_f >= lod:
                lod0.extend(vert(src + j) for j in range(n))

    meshes = []  # (material_name_candidates, [vertex tuples])
    # VTX
    nvbp, vbp_off = u32(vtx, 28), u32(vtx, 32)
    for bp in range(min(nbp, nvbp)):
        bp_o = bp_idx + 16 * bp
        nmodels, model_idx = u32(mdl, bp_o + 4), u32(mdl, bp_o + 12)
        vbp = vbp_off + 8 * bp
        vnm, vmo = u32(vtx, vbp), u32(vtx, vbp + 4)
        for m in range(min(nmodels, vnm)):
            mo = bp_o + model_idx + 148 * m
            nmesh, mesh_idx, vert_byte = u32(mdl, mo + 72), u32(mdl, mo + 76), u32(mdl, mo + 84)
            vm = vbp + vmo + 8 * m
            vlod_n, vlod_off = u32(vtx, vm), u32(vtx, vm + 4)
            lod_h = vm + vlod_off + 12 * min(lod, vlod_n - 1)  # the chosen LOD's header
            vnmesh, vmesh_off = u32(vtx, lod_h), u32(vtx, lod_h + 4)
            for k in range(min(nmesh, vnmesh)):
                me = mo + mesh_idx + 116 * k
                material, vert_off = u32(mdl, me), u32(mdl, me + 12)
                vh = lod_h + vmesh_off + 9 * k
                nsg, sg_off = u32(vtx, vh), u32(vtx, vh + 4)
                tris = []
                for sgi in range(nsg):
                    for stride in (25, 33):
                        sg = vh + sg_off + stride * sgi
                        nv, vo, ni, io = struct.unpack_from('<IIII', vtx, sg)
                        ok = 0 < nv < 100000 and ni % 3 == 0 and sg + vo + 9 * nv <= len(vtx) and sg + io + 2 * ni <= len(vtx)
                        if ok:
                            idx = struct.unpack_from('<%dH' % ni, vtx, sg + io)
                            ok = all(i < nv for i in idx)
                        if ok:
                            break
                    else:
                        raise ValueError('strip group layout not recognised')
                    orig = [u16(vtx, sg + vo + 9 * v + 4) for v in range(nv)]
                    for i in idx:
                        gi = vert_byte // 48 + vert_off + orig[i]
                        tris.append(lod0[gi])
                if tris:
                    mat = tex_names[skin0[material]] if material < len(skin0) and skin0[material] < len(tex_names) else None
                    meshes.append((mat, cd_dirs, tris))
    return meshes


def parse_bones(mdl):
    """(name, parent, bone->model 3x4 rows) per bone. mstudiobone_t is 216 bytes: name offset +0, parent +4, poseToBone (3x4) at +96."""
    nbones, bidx = u32(mdl, 156), u32(mdl, 160)
    out = []
    for i in range(nbones):
        o = bidx + 216 * i
        name = cstr(mdl, o + i32(mdl, o))
        parent = i32(mdl, o + 4)
        m = struct.unpack_from('<12f', mdl, o + 96)  # model -> bone
        r = [[m[0], m[1], m[2]], [m[4], m[5], m[6]], [m[8], m[9], m[10]]]
        t = [m[3], m[7], m[11]]
        a, b, c = r[0]; d, e, f = r[1]; g, h, k = r[2]
        A, B, C = e * k - f * h, -(d * k - f * g), d * h - e * g
        det = a * A + b * B + c * C
        if abs(det) < 1e-12:
            inv = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
        else:
            inv = [[A / det, -(b * k - c * h) / det, (b * f - c * e) / det],
                   [B / det, (a * k - c * g) / det, -(a * f - c * d) / det],
                   [C / det, -(a * h - b * g) / det, (a * e - b * d) / det]]
        tt = [-(inv[j][0] * t[0] + inv[j][1] * t[1] + inv[j][2] * t[2]) for j in range(3)]
        out.append((name, parent, [inv[0][0], inv[0][1], inv[0][2], tt[0], inv[1][0], inv[1][1], inv[1][2], tt[1],
                                    inv[2][0], inv[2][1], inv[2][2], tt[2]]))
    return out


CHARACTERS = {'chell': ('models/player/chell/', 'player'), 'atlas': ('models/player/ballbot/', 'ballbot'),
              'pbody': ('models/player/eggbot/', 'eggbot')}


def pick_model(vpk, character):
    folder, prefer = CHARACTERS[character]
    cands = sorted(n[:-4] for n in vpk.index if n.startswith(folder) and n.endswith('.mdl'))
    print(character, 'candidates:', cands)
    for c in cands:
        if c.endswith('/' + prefer):
            return c
    # otherwise the largest model in the folder (the full body, not a gib or an arm)
    return max(cands, key=lambda c: vpk.index[c + '.mdl'][2]) if cands else None


def png_write(path, w, h, rgb):
    raw = b''.join(b'\0' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def preview(prefix, meshes, textures, size=360):
    xs = [v[0] for _, _, t in meshes for v in t]; ys = [v[1] for _, _, t in meshes for v in t]; zs = [v[2] for _, _, t in meshes for v in t]
    print('bounds x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f' % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
    c = [(min(a) + max(a)) / 2 for a in (xs, ys, zs)]
    ext = max(max(a) - min(a) for a in (xs, ys, zs)) or 1
    for name, (ax, ay) in {'side': (0, 2), 'top': (0, 1), 'front': (1, 2)}.items():
        depth_ax = ({0, 1, 2} - {ax, ay}).pop()
        img = bytearray([24, 24, 32] * size * size)
        zb = [-1e9] * (size * size)
        sc = size * 0.9 / ext
        for mat, _, tris in meshes:
            tex = textures.get(mat)
            for t in range(0, len(tris), 3):
                p = [tris[t + i] for i in range(3)]
                q = [((v[ax] - c[ax]) * sc + size / 2, size / 2 - (v[ay] - c[ay]) * sc, v[depth_ax]) for v in p]
                x0, x1 = int(max(0, min(a[0] for a in q))), int(min(size - 1, max(a[0] for a in q) + 1))
                y0, y1 = int(max(0, min(a[1] for a in q))), int(min(size - 1, max(a[1] for a in q) + 1))
                den = (q[1][1] - q[2][1]) * (q[0][0] - q[2][0]) + (q[2][0] - q[1][0]) * (q[0][1] - q[2][1])
                if abs(den) < 1e-9:
                    continue
                for y in range(y0, y1 + 1):
                    for x in range(x0, x1 + 1):
                        l0 = ((q[1][1] - q[2][1]) * (x - q[2][0]) + (q[2][0] - q[1][0]) * (y - q[2][1])) / den
                        l1 = ((q[2][1] - q[0][1]) * (x - q[2][0]) + (q[0][0] - q[2][0]) * (y - q[2][1])) / den
                        l2 = 1 - l0 - l1
                        if l0 < 0 or l1 < 0 or l2 < 0:
                            continue
                        z = l0 * q[0][2] + l1 * q[1][2] + l2 * q[2][2]
                        if z <= zb[y * size + x]:
                            continue
                        zb[y * size + x] = z
                        col = (200, 200, 200)
                        if tex:
                            w, h, px = tex
                            u = (l0 * p[0][6] + l1 * p[1][6] + l2 * p[2][6]) % 1.0
                            v = (l0 * p[0][7] + l1 * p[1][7] + l2 * p[2][7]) % 1.0
                            o = (int(v * (h - 1)) * w + int(u * (w - 1))) * 4
                            col = (px[o], px[o + 1], px[o + 2])
                        img[(y * size + x) * 3:(y * size + x) * 3 + 3] = bytes(col)
        png_write('%s_%s.png' % (prefix, name), size, size, img)
        print('wrote %s_%s.png' % (prefix, name))


def write_cache(out, keep, textures, bones=None):
    names = sorted({m for m, _, _ in keep if m in textures})
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'wb') as f:
        f.write(b'PGUN' + struct.pack('<III', 2 if bones is not None else 1, len(names), len(keep)))
        for n in names:
            w, h, px = textures[n]
            f.write(struct.pack('<II', w, h) + px)
        for m, _, t in keep:
            f.write(struct.pack('<II', names.index(m) if m in names else 0xFFFFFFFF, len(t)))
            for v in t:
                f.write(struct.pack('<8f', *v[:8]))
            if bones is not None:
                for v in t:
                    b, n, w = v[8], v[9], v[10]
                    f.write(struct.pack('<4B3f', b[0], b[1], b[2], max(1, min(3, n)), *w))
        if bones is not None:
            f.write(struct.pack('<I', len(bones)))
            for name, parent, m in bones:
                f.write(name.encode('latin1')[:31].ljust(32, b'\0') + struct.pack('<i12f', parent, *m))
    print('wrote', out, os.path.getsize(out), 'bytes', '' if bones is None else '(%d bones)' % len(bones))


def extract(vpk, base, out, prev, skip_default, lod, with_bones):
    mdl, vvd = vpk.read(base + '.mdl'), vpk.read(base + '.vvd')
    vtx = vpk.read(base + '.dx90.vtx') if (base + '.dx90.vtx') in vpk.index else vpk.read(base + '.vtx')
    meshes = parse_model(mdl, vvd, vtx, lod)
    print('meshes:', [(m, len(t) // 3) for m, _, t in meshes])
    textures, mats = {}, {}
    for mat, cd, tris in meshes:
        if mat is None or mat in mats:
            continue
        found = None
        for dirpath in [''] + list(cd):
            d_ = dirpath.replace('\\', '/').strip('/').lower()
            for p in ('materials/' + (d_ + '/' if d_ else '') + mat + '.vmt', 'materials/' + mat + '.vmt'):
                if p in vpk.index:
                    found = p
                    break
            if found:
                break
        base_tex = None
        if found:
            text = preload_text(vpk, found)
            for line in text.splitlines():
                if '$basetexture' in line.lower():
                    base_tex = line.split()[-1].strip('"').replace('\\', '/').lower()
        print('material', mat, '->', found, 'basetexture', base_tex)
        if base_tex:
            try:
                textures[mat] = decode_vtf(vpk.read('materials/' + base_tex + '.vtf'), int(os.environ.get('MAXTEX', '512')))
            except Exception as e:
                print('  texture failed:', e)
        mats[mat] = base_tex
    skip_env = os.environ.get('SKIP', skip_default)
    skip = tuple(x for x in skip_env.split(',') if x)
    keep = [(m, cd, t) for m, cd, t in meshes if not (m and any(sk in m for sk in skip))]
    print('kept meshes:', [(m, len(t) // 3) for m, _, t in keep])
    if prev:
        preview(prev, keep, textures)
    bones = parse_bones(mdl) if with_bones else None
    if bones:
        print('bones:', len(bones), [b[0] for b in bones[:12]], '...')
        used = {bi for _, _, t in keep for v in t for bi, k in zip(v[8], range(v[9]))}
        print('bones used by the skin:', len(used))
    write_cache(out, keep, textures, bones)


def main():
    args = [a for a in sys.argv[1:]]
    prev = None
    if '--preview' in args:
        i = args.index('--preview'); prev = args[i + 1]; del args[i:i + 2]
    character = None
    if '--character' in args:
        i = args.index('--character'); character = args[i + 1].lower(); del args[i:i + 2]
    d = args[0] if args else DEFAULT_DIR
    vpk = Vpk(d)
    cache_dir = os.path.expanduser('~/Library/Application Support/Skate3PortalMod')
    if character:
        todo = list(CHARACTERS) if character == 'all' else [character]
        for c in todo:
            if c not in CHARACTERS:
                sys.exit('unknown character %r (chell, atlas, pbody, all)' % c)
            base = os.environ.get('MODEL') if (os.environ.get('MODEL') and len(todo) == 1) else pick_model(vpk, c)
            if not base:
                print(c, ': no model found in the VPK, skipped')
                continue
            out = args[1] if (len(args) > 1 and len(todo) == 1) else os.path.join(cache_dir, c + '.bin')
            print('==', c, base, '->', out)
            extract(vpk, base, out, prev and prev + '_' + c, '', int(os.environ.get('GUN_LOD', '2')), True)  # LOD 2: the renderer's per-frame vertex space is small
        return
    out = args[1] if len(args) > 1 else os.path.join(cache_dir, 'portalgun.bin')
    base = os.environ.get('MODEL', 'models/weapons/v_portalgun')
    extract(vpk, base, out, prev, 'hand,arm,sleeve,glove,potato', int(os.environ.get('GUN_LOD', '2')), False)


if __name__ == '__main__':
    main()
