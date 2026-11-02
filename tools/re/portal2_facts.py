#!/usr/bin/env python3
"""Gather Portal behaviour numbers from the user's own Portal 2 install (data only: map entity lumps and config files).

usage: python3 tools/re/portal2_facts.py [portal2_dir] [--out FILE] [--maps N]
  portal2_dir  the folder holding pak01_dir.vpk and maps/ (default: the macOS Steam install)
  --out        JSON written here (default ~/Library/Application Support/Skate3PortalMod/portal2_facts.json, outside the repo)
  --maps N     read at most N maps (default all)

What it reads, and why (cleanroom: level-design and config DATA, never scripts or code; .nut vscripts are skipped on purpose):
  - every maps/*.bsp (loose files and any inside the VPK): the entity lump (BSP lump 0, plain "key" "value" text) for
      trigger_catapult  (Aerial Faith Plates: the fling speeds and how a launch is aimed),
      prop_portal       (placed portal sizes),
      prop_tractor_beam (excursion funnel speed), trigger_push / point_push (push speeds),
      info_placement_helper / trigger_portal_cleanser counts (portal placement aids, fizzlers);
  - cfg/*.cfg and *.cfg inside the VPK: lines that set portal / funnel / gravity / speed cvars.
Units: Hammer units (1 unit = 1 inch = 0.0254 m); the summary prints both.
Send back: the "PASTE THIS" block at the end (it has no file contents, only counts and statistics).
"""
import glob, json, os, re, statistics as st, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vpk_list

DEFAULT_DIR = os.path.expanduser('~/Library/Application Support/Steam/steamapps/common/Portal 2/portal2')
DEFAULT_OUT = os.path.expanduser('~/Library/Application Support/Skate3PortalMod/portal2_facts.json')
M = 0.0254
WANT = ('trigger_catapult', 'prop_portal', 'prop_tractor_beam', 'trigger_push', 'point_push', 'info_placement_helper',
        'trigger_portal_cleanser', 'prop_weighted_cube', 'prop_paint_bomb', 'info_paint_sprayer')
CVAR_RE = re.compile(r'^\s*(sv_gravity|sv_maxvelocity|sv_player_funnel\w*|sv_props_funnel\w*|sv_portal\w*|portal\w*|cl_portal\w*|'
                     r'sv_airaccelerate|sv_accelerate|sv_friction|sv_maxspeed)\s+("?)([^"\s]+)\2', re.I)


def bsp_entities(blob, name):
    if blob[:4] != b'VBSP':
        return None, 'not a VBSP file'
    version = struct.unpack_from('<i', blob, 4)[0]
    ofs, ln, lver, fourcc = struct.unpack_from('<iiii', blob, 8)  # lump 0 = entities
    lump = blob[ofs:ofs + ln]
    if lump[:4] == b'LZMA':
        try:
            import lzma
            # Source LZMA lump: 'LZMA' u32 actualSize u32 lzmaSize 5 bytes props, then the stream
            actual, lsize = struct.unpack_from('<II', lump, 4)
            props = lump[12:17]
            data = lump[17:17 + lsize]
            dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[lzma._decode_filter_properties(lzma.FILTER_LZMA1, props)])
            lump = dec.decompress(data)[:actual]
        except Exception as e:  # noqa: BLE001
            return None, f'compressed entity lump ({e})'
    text = lump.decode('latin1', 'replace')
    ents = []
    for block in re.findall(r'\{([^{}]*)\}', text):
        kv = {}
        for k, v in re.findall(r'"([^"]*)"\s+"([^"]*)"', block):
            kv.setdefault(k.lower(), v)
        if kv.get('classname') in WANT:
            ents.append(kv)
    return ents, f'v{version}'


def num(v):
    try:
        return float(str(v).split()[0])
    except (ValueError, IndexError):
        return None


def describe(values, unit='m'):
    """unit: 'm' (lengths), 'm/s' (speeds) or None (plain numbers)."""
    vals = [x for x in values if x is not None]
    if not vals:
        return 'none'
    s = f'n {len(vals)}, median {st.median(vals):.0f}, min {min(vals):.0f}, max {max(vals):.0f}'
    if unit:
        s += f' units ({st.median(vals) * M:.2f} {unit} median, {min(vals) * M:.2f}..{max(vals) * M:.2f} {unit})'
    return s


def half(v, default):
    """prop_portal HalfWidth/HalfHeight: 0 or missing means the default size."""
    x = num(v)
    return default if not x else x


def main():
    args = sys.argv[1:]
    out = DEFAULT_OUT
    max_maps = None
    if '--out' in args:
        i = args.index('--out'); out = args[i + 1]; del args[i:i + 2]
    if '--maps' in args:
        i = args.index('--maps'); max_maps = int(args[i + 1]); del args[i:i + 2]
    root = args[0] if args else DEFAULT_DIR
    if not os.path.isdir(root):
        sys.exit(f'portal2_facts: no folder {root!r}; pass the portal2 folder (the one with pak01_dir.vpk)')

    sources = []  # (name, loader)
    for path in sorted(glob.glob(os.path.join(root, 'maps', '*.bsp'))):
        sources.append((os.path.basename(path), lambda p=path: open(p, 'rb').read()))
    vpk_path = os.path.join(root, 'pak01_dir.vpk')
    vpk = None
    cfg_texts = {}
    if os.path.exists(vpk_path):
        try:
            from portalgun_extract import Vpk
            vpk = Vpk(root)
            for name in vpk.index:
                if name.endswith('.bsp') and name.startswith('maps/'):
                    sources.append((name, lambda n=name: vpk.read(n)))
                elif name.endswith('.cfg'):
                    try:
                        cfg_texts[name] = vpk.read(name).decode('latin1', 'replace')
                    except KeyError:
                        pass
        except Exception as e:  # noqa: BLE001
            print(f'portal2_facts: VPK not usable ({e}); loose files only')
    for path in glob.glob(os.path.join(root, 'cfg', '*.cfg')):
        cfg_texts['cfg/' + os.path.basename(path)] = open(path, 'r', errors='replace').read()

    if max_maps:
        sources = sources[:max_maps]
    facts = {'maps': 0, 'maps_failed': {}, 'by_class': {}, 'catapults': [], 'portals': [], 'tractor': [], 'push': [], 'cvars': []}
    for name, load in sources:
        try:
            ents, info = bsp_entities(load(), name)
        except Exception as e:  # noqa: BLE001
            facts['maps_failed'][name] = str(e)
            continue
        if ents is None:
            facts['maps_failed'][name] = info
            continue
        facts['maps'] += 1
        for e in ents:
            c = e['classname']
            facts['by_class'][c] = facts['by_class'].get(c, 0) + 1
            if c == 'trigger_catapult':
                keep = {k: e.get(k) for k in ('playerspeed', 'physicsspeed', 'usethresholdcheck', 'entryangletolerance', 'useexactvelocity',
                                              'exactvelocitychoicetype', 'lowerthreshold', 'upperthreshold', 'launchdirection', 'launchtarget',
                                              'onlyvelocitycheck', 'applyangularimpulse', 'airctrlsupressiontime') if k in e}
                keep['map'] = name
                facts['catapults'].append(keep)
            elif c == 'prop_portal':
                facts['portals'].append({'map': name, 'halfwidth': e.get('halfwidth'), 'halfheight': e.get('halfheight')})
            elif c == 'prop_tractor_beam':
                facts['tractor'].append({'map': name, 'linearforce': e.get('linearforce')})
            elif c in ('trigger_push', 'point_push'):
                facts['push'].append({'map': name, 'class': c, 'speed': e.get('speed'), 'magnitude': e.get('magnitude'),
                                      'radius': e.get('radius')})
    for name, text in cfg_texts.items():
        for line in text.splitlines():
            m = CVAR_RE.match(line)
            if m:
                facts['cvars'].append({'file': name, 'cvar': m.group(1).lower(), 'value': m.group(3)})

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w') as f:
        json.dump(facts, f, indent=1)

    cat = facts['catapults']
    lines = ['PASTE THIS (portal2_facts.py summary)',
             f'maps read {facts["maps"]}, failed {len(facts["maps_failed"])}' +
             (f' (first: {next(iter(facts["maps_failed"].items()))})' if facts['maps_failed'] else ''),
             'entity counts: ' + ', '.join(f'{k} {v}' for k, v in sorted(facts['by_class'].items())),
             'catapult playerSpeed: ' + describe([num(c.get('playerspeed')) for c in cat], 'm/s'),
             'catapult physicsSpeed: ' + describe([num(c.get('physicsspeed')) for c in cat], 'm/s'),
             'catapult useExactVelocity=1: %d of %d; useThresholdCheck=1: %d; aimed at a target: %d' % (
                 sum(c.get('useexactvelocity') == '1' for c in cat), len(cat), sum(c.get('usethresholdcheck') == '1' for c in cat),
                 sum(bool(c.get('launchtarget')) for c in cat)),
             'catapult entryAngleTolerance: ' + describe([num(c.get('entryangletolerance')) for c in cat], None),
             'catapult lower/upper threshold: ' + describe([num(c.get('lowerthreshold')) for c in cat], None) + ' / ' +
             describe([num(c.get('upperthreshold')) for c in cat], None),
             'catapult AirCtrlSupressionTime: ' + describe([num(c.get('airctrlsupressiontime')) for c in cat], None),
             'prop_portal halfwidth / halfheight: ' + describe([half(p.get('halfwidth'), 32) for p in facts['portals']]) + ' / ' +
             describe([half(p.get('halfheight'), 54) for p in facts['portals']]) + ' (0 = default 32 / 54)',
             'tractor beam linearForce: ' + describe([num(t.get('linearforce')) for t in facts['tractor']], 'm/s'),
             'push speed: ' + describe([num(p.get('speed')) or num(p.get('magnitude')) for p in facts['push']], 'm/s'),
             'config cvars: ' + ('; '.join(sorted({f'{c["cvar"]}={c["value"]} ({c["file"]})' for c in facts['cvars']})[:30]) or 'none set')]
    print('\n'.join(lines))
    print(f'\nfull data: {out}')


if __name__ == '__main__':
    main()
