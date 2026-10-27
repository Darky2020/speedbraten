#!/usr/bin/env python3
"""Legacy flagrun records from the demos of github.com/InkoWinko/Flagrun-Demos.

    tools/legacyflagruns.py --parser ~/Desktop/demoparser_c --demos DIR [--fetch] > legacy.csv

Each demo is <map>/<ctf|ictf|ectf>-<player>-<seek time>.dmo. The record is read from the
flagrun server's messages in the demo (N_SERVMSG), which over the years came as:

    Buck takes the lead with 5.498 seconds! -> NEW MAP RECORD!!
    Buck: 8.519 => 8.513 -> NEW MAP RECORD!!
    1st:   Buck - 7.907 seconds (NEW MAP RECORD)

The fastest time flagged as a new map record is the demo's record; a demo without one falls
back to the fastest run of the player the file is named after. When two demos cover the same
map and mode, the faster record wins. The output is the admin site's legacy CSV:
map,kind,mode,player,time,source,date,link (date only when the demo prints one).

--fetch downloads the .dmo files (not the map images) into --demos first. A report goes to
stderr: demos without a record, and names that differ from the file name.
"""
import argparse
import json
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


if __name__ == '__main__':
    main()
