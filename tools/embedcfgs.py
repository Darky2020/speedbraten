#!/usr/bin/env python3
"""Regenerates the embedded p1xbraten cfgs: data/p1xbraten/<name>.cfg -> <name>.cfg.gz.xxd, and
patches size and CRC of each embeddedfile in src/p1xbraten/embedded_cfgs.cpp.

Portable replacement for `make embed-cfgs` (which needs GNU sed/stat). Run from the repo root.
"""
import gzip
import io
import re
import sys
import zlib

DATA = 'data/p1xbraten'
TARGET = 'src/p1xbraten/embedded_cfgs.cpp'


def gz(data):
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode='wb', compresslevel=9, mtime=0, filename='') as f:
        f.write(data)
    packed = bytearray(out.getvalue())
    packed[9] = 3  # OS = Unix, as written by `gzip --no-name`
    return bytes(packed)


def xxd(data):
    lines = []
    for i in range(0, len(data), 12):
        lines.append('  ' + ', '.join('0x%02x' % b for b in data[i:i+12]))
    return ',\n'.join(lines) + '\n'


def main():
    src = open(TARGET).read()
    names = re.findall(r'struct embeddedfile<\d+> (\w+)_cfg', src)
    for name in names:
        raw = open(f'{DATA}/{name}.cfg', 'rb').read()
        packed = gz(raw)
        open(f'{DATA}/{name}.cfg.gz.xxd', 'w').write(xxd(packed))
        src, n = re.subn(r'struct embeddedfile<\d+> %s_cfg = \{\n(\s*"[^"]+",\n\s*)0x[0-9a-f]+,' % name,
                         lambda m: 'struct embeddedfile<%d> %s_cfg = {\n%s0x%08x,' % (len(packed), name, m.group(1), zlib.crc32(raw)),
                         src)
        if n != 1:
            sys.exit(f'could not patch {name}_cfg')
        print(f'{name}: {len(raw)} -> {len(packed)} bytes, crc 0x{zlib.crc32(raw):08x}')
    open(TARGET, 'w').write(src)


if __name__ == '__main__':
    main()
