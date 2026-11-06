#!/usr/bin/env python3
"""Legacy flagrun records from the demos of github.com/InkoWinko/Flagrun-Demos.

    tools/legacyflagruns.py --parser ~/Desktop/demoparser_c --demos DIR [--fetch] [--out DIR] > legacy.csv

Each demo is <map>/<ctf|ictf|ectf>-<player>-<seek time>.dmo. The record is read from the
flagrun server's messages in the demo (N_SERVMSG), which over the years came as:

    Buck takes the lead with 5.498 seconds! -> NEW MAP RECORD!!
    Buck: 8.519 => 8.513 -> NEW MAP RECORD!!
    1st:   Buck - 7.907 seconds (NEW MAP RECORD)

The fastest time flagged as a new map record is the demo's record; a demo without one falls
back to the fastest run of the player the file is named after. When two demos cover the same
map and mode, the faster record wins. The output is the admin site's legacy CSV:
map,kind,mode,player,time,source,date,link (date only when the demo prints one).

--out writes, per record, <map>-<mode>.sbg (the record holder's path as a ghost) and
<map>-<mode>.dmo (the demo cut down to the run, playable with /demo). Run it with the parser's
own Python (its polars and numpy).

--fetch downloads the .dmo files (not the map images) into --demos first. A report goes to
stderr: demos without a record, and names that differ from the file name.
"""
import argparse
import gzip
import json
import math
import struct
import re
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

REPO = 'InkoWinko/Flagrun-Demos'
MODES = {'ctf': 11, 'ictf': 12, 'ectf': 17}
# A player's names over the years (clan tags, aliases) -> the one shown on the boards.
NAMES = {'!s]Raze': 'Raze', '*Ur|Ňєkø': 'Ňєkø', '=DK=Ňєkø': 'Ňєkø', 'TrackpadCat': 'Ňєkø',
         'Master|sp4nk|': 'Master', '[sQ]Master': 'Master', '|AV|Inko': 'Inko'}
NUM = r'(\d+(?:\.\d+)?)'
PATTERNS = [
    ('lead', re.compile(r'^(?P<name>.+?) takes the lead with ' + NUM + r' seconds!(?P<rec> -> NEW MAP RECORD!!)?')),
    ('improve', re.compile(r'^(?P<name>.+?): \d+(?:\.\d+)? => ' + NUM + r'(?P<rec> -> NEW MAP RECORD!!)?$')),
    ('summary', re.compile(r'^\d+(?:st|nd|rd|th):\s+(?P<name>.+?) - ' + NUM + r' seconds(?P<rec> \(NEW MAP RECORD\))?')),
]
DATE = re.compile(r'^\w{3} (\w{3}) +(\d+) [\d:]+ (\d{4}) \| paused')
MONTHS = {m: i for i, m in enumerate('Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec'.split(), 1)}


def plain(s):
    """Server text without the game's colour codes (\\f + a character, or \\f[...])."""
    return re.sub(r'\x0c(\[[^\]]*\]|.)', '', s)


def fetch(dest):
    with urllib.request.urlopen(f'https://api.github.com/repos/{REPO}/git/trees/main?recursive=1') as r:
        tree = json.load(r)['tree']
    demos = [t['path'] for t in tree if t['type'] == 'blob' and t['path'].endswith('.dmo')]
    start = last = time.time()
    for i, path in enumerate(demos, 1):
        out = dest / path
        if not out.exists():
            out.parent.mkdir(parents=True, exist_ok=True)
            urllib.request.urlretrieve(f'https://raw.githubusercontent.com/{REPO}/main/{path}', out)
        if time.time() - last > 10 or i == len(demos):
            el = time.time() - start
            print(f'fetched {i}/{len(demos)} in {el:.0f}s - {i / el:.1f}/s - eta {(len(demos) - i) / (i / el):.0f}s', file=sys.stderr, flush=True)
            last = time.time()


S_FLAGPICKUP, S_FLAGSCORE = 60, 63
FLAG = 30            # the flag entity type
FLAGRADIUS = 16      # our flagrun timer starts when the holder's feet are this far from the flag
EYEHEIGHT = 14       # the parser gives the eye; the flagrun timer measures from the feet
SIMTICK = 5
GHOSTPOSSCALE, GHOSTANGLESCALE = 4, 2
INAIR, GUNSHIFT = 1 << 5, 8
GHOSTVERSION = 1
CUTBEFORE, CUTAFTER, CUTLEAD = 2000, 3000, 500


def var(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def svar(n):
    return var((n << 1) ^ (n >> 63) if n < 0 else n << 1)


def string(s):
    b = s.encode('utf-8')
    return var(len(b)) + b


def find_run(d, ms):
    """(cn, pickup time, score time) of the flag run timed ms by the server, or None.

    Flagrun servers sent no flag messages: a run is the holder's flag pickup sound to their
    score sound, and the server's time is the difference, give or take a frame of its clock.
    """
    import polars as pl
    from sdp import frames
    from sdp.schema import MessageType as T
    snd = frames.of(d.handle, T.N_SOUND, ['cn', 'val']).filter(pl.col('val').is_in([S_FLAGPICKUP, S_FLAGSCORE]))
    best, pick = None, {}
    for t, cn, val in snd.select('time', 'cn', 'val').iter_rows():
        if val == S_FLAGPICKUP:
            pick[cn] = t
        elif cn in pick:
            off = abs(t - pick[cn] - ms)
            if off <= 50 and (best is None or off < best[0]):
                best = (off, cn, pick[cn], t)
    return best[1:] if best else None


def write_ghost(d, mapname, mode, holder, date, ms, run, path):
    """The holder's path from leaving the flag to the score, as a legacy ghost file:

        gzip("SBGH" var(version) str(map) var(mode) var(flag side) var(runtime ms) str(holder)
             str(date) trace)

    side is the taken flag's place among the map's flags, plus 1 (0: unknown). trace is a
    replay's gHST payload without the attempt index; its run ticks count from where our timer
    would have started. Returns a note on what could not be worked out, or ''.
    """
    import polars as pl
    from sdp import frames, maps, movement
    from sdp.schema import MessageType as T
    cn, picked, scored = run
    pos = movement.positions(d).filter((pl.col('cn') == cn) & pl.col('time').is_between(picked, scored))
    keys = movement.movement_input(d).filter((pl.col('cn') == cn) & pl.col('time').is_between(picked, scored))
    pos = pos.with_columns(keys['move'], keys['strafe'])
    spawns = frames.of(d.handle, T.N_SPAWN, ['cn', 'gunselect']).filter((pl.col('cn') == cn) & (pl.col('time') <= picked))
    gun = int(spawns['gunselect'][-1]) if len(spawns) else 0
    rows = pos.select('time', 'x', 'y', 'z', 'yaw', 'pitch', 'airborne', 'move', 'strafe').rows()
    if not rows:
        return None, 'no positions'
    note = ''
    flags = sorted((e for e in (maps.entities(mapname, d.handle.protocol) or {}).values() if e.type == FLAG), key=lambda e: e.entid)
    side, start = 0, picked
    if flags:
        feet = lambda r: (r[1], r[2], r[3] - EYEHEIGHT)
        near = min(range(len(flags)), key=lambda i: math.dist(feet(rows[0]), (flags[i].x, flags[i].y, flags[i].z)))
        side = near + 1
        f = flags[near]
        start = next((r[0] for r in rows if math.dist(feet(r), (f.x, f.y, f.z)) >= FLAGRADIUS), picked)
    else:
        note = 'map not in the parser\'s tables: the ghost starts at the pickup'
    samples, last = [], -1
    for t, x, y, z, yaw, pitch, air, move, strafe in rows:
        tick = round((t - start) / SIMTICK)
        if tick < 0 or tick <= last:
            continue
        last = tick
        f = (move + 1) | (strafe + 1) << 2 | (INAIR if air else 0) | gun << GUNSHIFT
        samples.append((tick, x, y, z, yaw, pitch, f))
    out = bytearray(b'SBGH' + var(GHOSTVERSION) + string(mapname) + var(mode) + var(side) + var(ms) +
                    string(holder) + string(date) + var(len(samples)))
    prev = 0
    for s in samples:
        out += var(s[0] - prev)
        prev = s[0]
    for j in range(1, 6):
        prev, scale = 0, GHOSTPOSSCALE if j < 4 else GHOSTANGLESCALE
        for s in samples:
            q = round(s[j] * scale)
            out += svar(q - prev)
            prev = q
    for s in samples:
        out += var(s[6])
    path.write_bytes(gzip.compress(bytes(out), mtime=0))
    return (start - picked, len(samples)), note


def write_cut(d, run, path):
    """The demo cut down to the run: the opening (map, clients, items) at its first clock reading,
    the holder's last spawn before the pickup, and CUTBEFORE ms before the pickup to CUTAFTER ms
    after the score, its clock moved so the cut begins CUTLEAD ms into playback."""
    import numpy as np
    import polars as pl
    import _sdp
    from sdp import frames
    from sdp.schema import MessageType as T
    cn, picked, scored = run
    ix = frames.index(d.handle, row=True)
    lo, hi = picked - CUTBEFORE, scored + CUTAFTER
    first = int(ix['time'][0])
    keep = set(ix.filter((pl.col('time') == first) | pl.col('time').is_between(lo, hi))['row'].to_list())
    spawns = frames.of(d.handle, T.N_SPAWN, ['cn'], row=True).filter((pl.col('cn') == cn) & (pl.col('time') < lo))
    if len(spawns):
        keep.add(int(spawns['row'][-1]))
    ed = _sdp.Edits(d.handle)
    ed.keep_rows(np.array(sorted(keep), dtype=np.uint32))
    raw = d.handle.render(ed)
    out, p = bytearray(raw[:24]), 24
    while p < len(raw):
        millis, chan, n = struct.unpack_from('<iii', raw, p)
        if millis != first:
            millis = CUTLEAD - 100 if millis < lo else millis - lo + CUTLEAD
        out += struct.pack('<iii', millis, chan, n) + raw[p + 12:p + 12 + n]
        p += 12 + n
    path.write_bytes(gzip.compress(bytes(out), mtime=0))


def record_of(messages, holder):
    """(player, ms, date, how) of a demo's record from its server messages, or None."""
    events, date = [], ''
    for _, m in messages:
        for line in plain(m).split('\n'):
            line = line.strip()
            if not date and (d := DATE.match(line)):
                date = f'{d.group(3)}-{MONTHS[d.group(1)]:02d}-{int(d.group(2)):02d}'
            for _, rx in PATTERNS:
                if mm := rx.match(line):
                    events.append((mm.group('name'), round(float(mm.group(2)) * 1000), bool(mm.group('rec'))))
                    break
    records = [e for e in events if e[2]]
    if records:
        name, ms, _ = min(records, key=lambda e: e[1])
        return name, ms, date, 'new map record'
    own = [e for e in events if e[0].lower().endswith(holder.lower())]
    if own:
        name, ms, _ = min(own, key=lambda e: e[1])
        return name, ms, date, 'no record message: the named player\'s best run'
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--parser', required=True, help='the demoparser_c checkout (its sdp package)')
    ap.add_argument('--demos', required=True, type=Path)
    ap.add_argument('--fetch', action='store_true')
    ap.add_argument('--out', type=Path, help='write each record\'s ghost and cut demo here')
    a = ap.parse_args()
    sys.path.insert(0, str(Path(a.parser).expanduser()))
    import sdp
    from sdp import frames
    from sdp.schema import MessageType as T

    if a.fetch:
        a.demos.mkdir(parents=True, exist_ok=True)
        fetch(a.demos)
    best = {}
    for p in sorted(a.demos.rglob('*.dmo')):
        rel = p.relative_to(a.demos).as_posix()
        mapname, fname = rel.split('/', 1)
        parts = fname.split('-')
        if len(parts) < 3 or parts[0] not in MODES:
            print(f'skipped {rel}: not <mode>-<player>-<time>.dmo', file=sys.stderr)
            continue
        try:
            with sdp.open(str(p)) as d:
                messages = [(r['time'], r['msg']) for r in frames.of(d.handle, T.N_SERVMSG).iter_rows(named=True)]
        except Exception as e:  # the parser refuses a few damaged demos
            print(f'no record: {rel}: {e}', file=sys.stderr)
            continue
        rec = record_of(messages, parts[1])
        if not rec:
            print(f'no record: {rel}: no flagrun times in its server messages', file=sys.stderr)
            continue
        name, ms, date, how = rec
        shown = NAMES.get(name, name)
        if ',' in name:
            print(f'no record: {rel}: a comma in the name {name!r}', file=sys.stderr)
            continue
        if how != 'new map record':
            print(f'{rel}: {how}', file=sys.stderr)
        if name not in NAMES and not name.lower().endswith(parts[1].lower()):
            print(f'{rel}: record set by {name!r}', file=sys.stderr)
        key = (mapname, MODES[parts[0]])
        if key not in best or ms < best[key][1]:
            best[key] = (shown, ms, date, rel)
    for (mapname, mode), (name, ms, date, rel) in sorted(best.items()):
        t = f'{ms // 60000}:{ms // 1000 % 60:02d}.{ms % 1000:03d}'
        print(f'{mapname},flagrun,{mode},{name},{t},Flagrun-Demos,{date},https://github.com/{REPO}/blob/main/{urllib.parse.quote(rel)}')
    print(f'{len(best)} records', file=sys.stderr)
    if a.out:
        a.out.mkdir(parents=True, exist_ok=True)
        written = 0
        for (mapname, mode), (name, ms, date, rel) in sorted(best.items()):
            d = sdp.open(str(a.demos / rel))
            run = find_run(d, ms)
            if not run:
                print(f'no ghost: {rel}: no pickup-to-score of {ms} ms', file=sys.stderr)
                continue
            stem = a.out / f'{mapname}-{mode}'
            got, note = write_ghost(d, mapname, mode, name, date, ms, run, stem.with_suffix('.sbg'))
            if not got:
                print(f'no ghost: {rel}: {note}', file=sys.stderr)
                continue
            write_cut(d, run, stem.with_suffix('.dmo'))
            written += 1
            if note:
                print(f'{rel}: {note}', file=sys.stderr)
            if written % 10 == 0:
                print(f'wrote {written}', file=sys.stderr, flush=True)
        print(f'{written} ghosts and cut demos in {a.out}', file=sys.stderr)


if __name__ == '__main__':
    main()
