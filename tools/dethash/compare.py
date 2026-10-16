#!/usr/bin/env python3
"""Compare dethash logs: report the first diverging frame and which state groups differ.

Usage: compare.py [--attempt N] <reference.log> <other.log> [...]

--attempt N compares only attempt N of the reference (attempts are split at each map start,
where lastmillis goes back; -1 = last) against the start of each other log.
"""
import sys

GROUPS = ['player', 'monsters', 'projectiles', 'movables', 'entities', 'rng']


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            parts = line.split()
            rows.append((int(parts[0]), int(parts[1]), parts[2:]))
    return rows


def split_attempts(rows):
    starts = [0] + [i for i in range(1, len(rows)) if rows[i][1] < rows[i - 1][1]]
    return [rows[a:b] for a, b in zip(starts, starts[1:] + [len(rows)])]


def compare(ref, other):
    first = None
    counts = dict.fromkeys(GROUPS, 0)
    for a, b in zip(ref, other):
        diff = [g for g, x, y in zip(GROUPS, a[2], b[2]) if x != y]
        for g in diff:
            counts[g] += 1
        if (diff or a[1] != b[1]) and first is None:
            first = (a[0], a[1], b[1], diff)
    return first, counts


def main():
    args = sys.argv[1:]
    attempt = None
    if args[0] == '--attempt':
        attempt = int(args[1])
        args = args[2:]
    ref = load(args[0])
    if attempt is not None:
        ref = split_attempts(ref)[attempt]
    print(f'reference: {args[0]} ({len(ref)} frames{"" if attempt is None else f", attempt {attempt}"})')
    ok = True
    for path in args[1:]:
        other = load(path)
        if attempt is not None:
            other = other[:len(ref)]
        first, counts = compare(ref, other)
        lens = '' if len(other) == len(ref) else f', length differs: {len(other)} vs {len(ref)}'
        if first is None and not lens:
            print(f'IDENTICAL  {path}')
            continue
        ok = False
        print(f'DIVERGES   {path}{lens}')
        if first:
            frame, ma, mb, groups = first
            print(f'  first diverging frame {frame} (lastmillis {ma} vs {mb}), groups: {", ".join(groups) or "clock only"}')
            print('  diverging frames per group: ' + ', '.join(f'{g}={n}' for g, n in counts.items() if n))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
