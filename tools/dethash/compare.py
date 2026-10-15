#!/usr/bin/env python3
"""Compare dethash logs: report the first diverging frame and which state groups differ.

Usage: compare.py <reference.log> <other.log> [...]
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
    ref = load(sys.argv[1])
    print(f'reference: {sys.argv[1]} ({len(ref)} frames)')
    ok = True
    for path in sys.argv[2:]:
        other = load(path)
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
