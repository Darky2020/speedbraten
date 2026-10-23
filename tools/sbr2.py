#!/usr/bin/env python3
"""Reference reader for SBR2 speedbraten replay files (the container format: chunks, CRCs, zstd,
BLAKE3 hash chain). Independent of the game's C++ code, for checking files and for outside tools.

Usage:
  sbr2.py check <file.sbr2>    verify CRCs, chain and content hash; print segment heads
  sbr2.py dump <file.sbr2>     also list every chunk, with key-value, tIME, SPLC and raw input contents
  sbr2.py rawcheck <file.sbr2> check that every sim event tagged as coming from a key or mouse
                               button has a raw key/button/wheel event in the same tick
  sbr2.py verify <file.sbr2> <keys.txt>
                               also check the witness signatures (sIGN) against trusted Ed25519
                               public keys (hex, one per line)
  sbr2.py history <export.txt> [file.sbr2...]
                               recompute an attempt history exported with statsexporthistory,
                               and check where each replay sits in it

Needs Python 3.14+ (compression.zstd) and the blake3 module (pip install blake3).

Format (see the design doc for the chunk contents):
  file  = "SBR2" var(version) chunk* END-chunk sIGN-chunk*
  chunk = type[4] var(flags) var(stored length) payload u32le crc32(type..payload)
  An uppercase first letter of the type marks a required chunk. flags bit 0: the payload is
  var(raw length) + the next bytes of the file's zstd stream, flushed at the chunk's end. From
  version 2 on the stream restarts after every SEGE, so decoding can start at any segment.
  hashed(c) = type || var(raw length) || raw payload
  head = BLAKE3("SBR2-head" || var(version)); every chunk except sIGN and END is hashed into a
  running digest; at SEGE: head = BLAKE3("SBR2-seg" || head || digest), digest restarts;
  END = var(segments) || BLAKE3("SBR2-end" || head || digest); only sIGN may follow END.
"""
import hashlib
import struct
import sys
import zlib

try:
    from compression import zstd
except ImportError:
    sys.exit("sbr2.py needs Python 3.14 or later (compression.zstd)")
try:
    from blake3 import blake3
except ImportError:
    blake3 = None


# Pure-Python BLAKE3 (hash mode only), used when the blake3 module is missing. Follows the
# reference implementation in the BLAKE3 specification.
_IV = [0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19]
_PERM = [2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8]
_CHUNK_START, _CHUNK_END, _PARENT, _ROOT = 1, 2, 4, 8
_M32 = 0xFFFFFFFF


def _g(s, a, b, c, d, x, y):
    s[a] = (s[a] + s[b] + x) & _M32
    s[d] ^= s[a]; s[d] = ((s[d] >> 16) | (s[d] << 16)) & _M32
    s[c] = (s[c] + s[d]) & _M32
    s[b] ^= s[c]; s[b] = ((s[b] >> 12) | (s[b] << 20)) & _M32
    s[a] = (s[a] + s[b] + y) & _M32
    s[d] ^= s[a]; s[d] = ((s[d] >> 8) | (s[d] << 24)) & _M32
    s[c] = (s[c] + s[d]) & _M32
    s[b] ^= s[c]; s[b] = ((s[b] >> 7) | (s[b] << 25)) & _M32


def _compress(cv, block, counter, blen, flags):
    m = list(int.from_bytes(block[i*4:i*4+4], "little") for i in range(16))
    s = list(cv) + _IV[:4] + [counter & _M32, counter >> 32, blen, flags]
    for r in range(7):
        _g(s, 0, 4, 8, 12, m[0], m[1]); _g(s, 1, 5, 9, 13, m[2], m[3])
        _g(s, 2, 6, 10, 14, m[4], m[5]); _g(s, 3, 7, 11, 15, m[6], m[7])
        _g(s, 0, 5, 10, 15, m[8], m[9]); _g(s, 1, 6, 11, 12, m[10], m[11])
        _g(s, 2, 7, 8, 13, m[12], m[13]); _g(s, 3, 4, 9, 14, m[14], m[15])
        if r < 6:
            m = [m[i] for i in _PERM]
    return [s[i] ^ s[i+8] for i in range(8)]


class _PyBlake3:
    def __init__(self):
        self.buf = bytearray()

    def update(self, data):
        self.buf += data

    def _chunk_output(self, data, counter):
        cv, flags = list(_IV), _CHUNK_START
        blocks = [data[i:i+64] for i in range(0, max(len(data), 1), 64)] or [b""]
        for i, block in enumerate(blocks):
            if i == len(blocks) - 1:
                return cv, block.ljust(64, b"\0"), counter, len(block), flags | _CHUNK_END
            cv = _compress(cv, block.ljust(64, b"\0"), counter, 64, flags)
            flags = 0

    def digest(self):
        data = bytes(self.buf)
        chunks = [data[i:i+1024] for i in range(0, max(len(data), 1), 1024)] or [b""]
        stack, out = [], None
        for n, chunk in enumerate(chunks):
            out = self._chunk_output(chunk, n)
            if n == len(chunks) - 1:
                break
            cv = _compress(*out[:4], out[4])
            total = n + 1
            while total & 1 == 0:
                left = stack.pop()
                cv = _compress(_IV, b"".join(x.to_bytes(4, "little") for x in left + cv), 0, 64, _PARENT)
                total >>= 1
            stack.append(cv)
        while stack:
            cv = _compress(*out[:4], out[4])
            left = stack.pop()
            out = (_IV, b"".join(x.to_bytes(4, "little") for x in left + cv), 0, 64, _PARENT)
        words = _compress(out[0], out[1], out[2], out[3], out[4] | _ROOT)
        return b"".join(x.to_bytes(4, "little") for x in words)


if blake3 is None:
    blake3 = _PyBlake3

VERSION, MINVERSION = 2, 1
KNOWN_REQUIRED = {b"META", b"SETS", b"DATA", b"ATTM", b"INPS", b"HASH", b"SPLC", b"RSLT", b"SEGE", b"END "}


class FormatError(Exception):
    pass


def var(n):
    out = bytearray()
    while True:
        c = n & 0x7F
        n >>= 7
        if n:
            out.append(c | 0x80)
        else:
            out.append(c)
            return bytes(out)


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def left(self):
        return len(self.data) - self.pos

    def take(self, n):
        if n < 0 or n > self.left():
            raise FormatError("cut off at byte %d" % self.pos)
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def var(self):
        v, shift = 0, 0
        while shift < 64:
            c = self.take(1)[0]
            v |= (c & 0x7F) << shift
            if not c & 0x80:
                if c == 0 and shift:
                    raise FormatError("non-minimal varint at byte %d" % (self.pos - 1))
                return v
            shift += 7
        raise FormatError("varint too long")


def h(*parts):
    hasher = blake3()
    for p in parts:
        hasher.update(p)
    return hasher.digest()


def parse(data):
    r = Reader(data)
    if r.take(4) != b"SBR2":
        raise FormatError("not an SBR2 file")
    version = r.var()
    if not MINVERSION <= version <= VERSION:
        raise FormatError("unsupported container version %d" % version)
    head = h(b"SBR2-head", var(version))
    run = blake3()
    chunks, heads, content, ended, segment = [], [], None, False, 0
    stream = zstd.ZstdDecompressor()
    while r.left():
        start = r.pos
        ctype = r.take(4)
        flags = r.var()
        stored = r.take(r.var())
        crc = int.from_bytes(r.take(4), "little")
        name = ctype.decode("latin1")
        if crc != zlib.crc32(data[start:r.pos - 4]):
            raise FormatError("chunk %d (%s): CRC mismatch" % (len(chunks), name))
        if flags & ~1:
            raise FormatError("chunk %d (%s): unknown flags %d" % (len(chunks), name, flags))
        if flags & 1:
            pr = Reader(stored)
            size = pr.var()
            raw = stream.decompress(stored[pr.pos:])
            if len(raw) != size:
                raise FormatError("chunk %d (%s): bad compressed payload" % (len(chunks), name))
        else:
            raw = stored
        sign, end = ctype == b"sIGN", ctype == b"END "
        if ended and not sign:
            raise FormatError("chunk %d (%s) after END" % (len(chunks), name))
        if 0x41 <= ctype[0] <= 0x5A and ctype not in KNOWN_REQUIRED:
            raise FormatError("unknown required chunk %s" % name)
        if not sign and not end:
            run.update(ctype + var(len(raw)) + raw)
        if ctype == b"SEGE":
            s = Reader(raw)
            if s.var() != segment:
                raise FormatError("segment %d: bad SEGE" % segment)
            s.var()
            if s.left():
                raise FormatError("segment %d: bad SEGE" % segment)
            head = h(b"SBR2-seg", head, run.digest())
            run = blake3()
            heads.append(head)
            segment += 1
            if version >= 2:
                stream = zstd.ZstdDecompressor()
        if end:
            content = h(b"SBR2-end", head, run.digest())
            s = Reader(raw)
            if s.var() != segment:
                raise FormatError("END: wrong segment count")
            if s.take(32) != content or s.left():
                raise FormatError("content hash mismatch")
            ended = True
        chunks.append((name, flags, len(stored), len(raw), segment - (1 if ctype == b"SEGE" else 0), raw))
    if not ended:
        raise FormatError("no END chunk")
    return chunks, heads, content


# Ed25519 signature check (RFC 8032, section 6 reference code)
_P = 2**255 - 19
_Q = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, -1, _P) % _P
_SQRTM1 = pow(2, (_P - 1) // 4, _P)


def _add(a, b):
    A, B = (a[1] - a[0]) * (b[1] - b[0]) % _P, (a[1] + a[0]) * (b[1] + b[0]) % _P
    C, D = 2 * a[3] * b[3] * _D % _P, 2 * a[2] * b[2] % _P
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F % _P, G * H % _P, F * G % _P, E * H % _P)


def _mul(s, pt):
    acc = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            acc = _add(acc, pt)
        pt = _add(pt, pt)
        s >>= 1
    return acc


def _equal(a, b):
    return (a[0] * b[2] - b[0] * a[2]) % _P == 0 and (a[1] * b[2] - b[1] * a[2]) % _P == 0


def _recover_x(y, sign):
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, -1, _P)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P != 0:
        x = x * _SQRTM1 % _P
    if (x * x - x2) % _P != 0:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


_GY = 4 * pow(5, -1, _P) % _P
_GX = _recover_x(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % _P)


def _decompress(b):
    y = int.from_bytes(b, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % _P)


def ed25519_verify(public, msg, sig):
    if len(public) != 32 or len(sig) != 64:
        return False
    A, R = _decompress(public), _decompress(sig[:32])
    if A is None or R is None:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= _Q:
        return False
    h = int.from_bytes(hashlib.sha512(sig[:32] + public + msg).digest(), "little") % _Q
    return _equal(_mul(s, _G), _add(R, _mul(h, A)))


def decode_sign(raw):
    """sIGN payload -> dict with the signed bytes."""
    r = Reader(raw)
    d = {"version": r.take(1)[0], "algorithm": r.take(1)[0], "scope": r.take(1)[0]}
    r.take(1)
    d["keyid"], d["session"], d["nonce"] = r.take(8), r.take(16), r.take(16)
    d["installkey"] = r.take(16) if d["version"] >= 2 else None
    d["segment"], d["counter"] = r.var(), r.var()
    d["recvus"] = int.from_bytes(r.take(8), "little")
    d["hash"] = r.take(32)
    d["extras"] = r.take(r.var())
    end = r.pos
    d["sig"] = r.take(64)
    if r.left():
        raise FormatError("sIGN: trailing bytes")
    d["signed"] = b"SBR2-sig" + raw[:end]
    return d


def installkey_id(public):
    """The id a version 2 signature uses for a client's install (Noise static) public key."""
    return h(b"SBR2-install", public)[:16]


def verify_signatures(chunks, heads, content, keys):
    """-> list of (status, description) per sIGN chunk. Scopes: 0 segment head, 1 content hash,
    2 content hash with a verdict in the extras."""
    byid = {}
    for k in keys:
        h = blake3()
        h.update(k)
        byid[h.digest()[:8]] = k
    out = []
    for name, flags, stored, size, seg, data in chunks:
        if name != "sIGN":
            continue
        d = decode_sign(data)
        if d["version"] not in (1, 2) or d["scope"] > (2 if d["version"] >= 2 else 1):
            out.append(("BAD", "unknown signature version %d scope %d" % (d["version"], d["scope"])))
            continue
        what = {1: "content", 2: "verdict"}.get(d["scope"], "segment %d" % d["segment"])
        if d["scope"] == 2:
            verdict = dict(decode_kv(d["extras"])).get("verdict")
            what += " %s" % verdict
        if d["installkey"] is not None:
            what += ", player %s" % d["installkey"].hex()
        expected = content if d["scope"] in (1, 2) else (heads[d["segment"]] if d["segment"] < len(heads) else None)
        key = byid.get(d["keyid"])
        if key is None:
            out.append(("unknown key", "%s, key id %s" % (what, d["keyid"].hex())))
        elif d["algorithm"] != 1 or expected != d["hash"] or not ed25519_verify(key, d["signed"], d["sig"]):
            out.append(("BAD", what))
        else:
            out.append(("ok", "%s by %s" % (what, key.hex()[:16])))
    return out


def history_summary_content(summary):
    return summary[-32:]


def check_history(export, replays):
    """Recompute an exported history chain; place each replay in it. -> list of problems."""
    lines = [l.split() for l in open(export) if l.strip()]
    if lines[0] != ["SBR2-history", "1"]:
        raise FormatError("not a history export")
    count = int(lines[1][0])
    entries = [(bytes.fromhex(a), bytes.fromhex(b)) for a, b in lines[2:]]
    problems, head, heads = [], bytes(32), [bytes(32)]
    if len(entries) != count:
        problems.append("export lists %d attempts, header says %d" % (len(entries), count))
    for i, (summary, stated) in enumerate(entries):
        h = blake3()
        h.update(head + summary)
        head = h.digest()
        heads.append(head)
        if head != stated:
            problems.append("attempt %d: head doesn't follow from the summary" % i)
    print("history: %d attempts, head %s" % (len(entries), head.hex()))
    for f in replays:
        chunks, _, content = parse(open(f, "rb").read())
        hist = dict(decode_kv(next(c[5] for c in chunks if c[0] == "hIST")))
        n, h = hist.get("historycount"), bytes.fromhex(hist.get("historyhead", ""))
        if n is not None and n == len(entries) and heads[n] == h:
            problems.append("%s: recorded after this export (attempt %d)" % (f, n))
        elif n is None or n >= len(heads) or heads[n] != h:
            problems.append("%s: its history head isn't in the chain" % f)
        elif history_summary_content(entries[n][0]) != content:
            problems.append("%s: attempt %d after its head isn't this replay" % (f, n))
        else:
            print("%s: attempt %d in the chain" % (f, n))
    return problems


KV_CHUNKS = {"META", "cLNT", "sESS", "hIST", "pROV", "fEAT", "SETS", "wIND", "rSTA"}
RAW_TIME_UNIT = 100  # raw event times are stored in units of 100 us
RAW_KINDS = ["keydown", "keyup", "buttondown", "buttonup", "motion", "wheel", "focus"]
SOURCES = ["other", "key", "mouse", "console", "menu", "harness", "recorder", "map"]


def svar_of(r):
    v = r.var()
    return (v >> 1) ^ -(v & 1)


def decode_binds(raw):
    r = Reader(raw)
    return [r.take(r.var()).decode("utf-8", "replace") for _ in range(r.var())]


def decode_raw(raw, lo):
    """rAWI payload (columns) -> list of dicts (tick, us, ms, kind, ...); us has 100 us resolution."""
    r = Reader(raw)
    base, basems, n = r.var(), r.var(), r.var()
    evs = [{} for _ in range(n)]
    t = base
    for e in evs:
        t += svar_of(r); e["us"] = t * RAW_TIME_UNIT
    for e in evs:
        e["ms"] = basems + svar_of(r) + (e["us"] // RAW_TIME_UNIT - base) // (1000 // RAW_TIME_UNIT)
    tick = lo
    for e in evs:
        tick += svar_of(r); e["tick"] = tick
    for e in evs:
        k = r.take(1)[0]
        e["kind"] = RAW_KINDS[k] if k < len(RAW_KINDS) else k
    motion = [e for e in evs if e["kind"] == "motion"]
    for e in motion: e["dx"] = svar_of(r)
    for e in motion: e["dy"] = svar_of(r)
    for e in motion: e["flags"] = r.take(1)[0]
    for e in evs:
        k = e["kind"]
        if k in ("keydown", "keyup"):
            e.update(scancode=r.var(), keycode=r.var(), flags=r.take(1)[0], bind=r.var())
        elif k in ("buttondown", "buttonup"):
            e.update(button=r.take(1)[0], bind=r.var())
        elif k == "wheel":
            e.update(dx=svar_of(r), dy=svar_of(r), bind=r.var())
        elif k == "focus":
            e.update(flags=r.take(1)[0])
        elif k != "motion":
            raise FormatError("rAWI: unknown kind %s" % k)
    return evs


def decode_events(raw):
    """INPS payload (columns) -> (lo, list of (tick, type, source))."""
    r = Reader(raw)
    lo = r.var()
    r.var()
    n = r.var()
    ticks, tick = [], lo
    for _ in range(n):
        tick += r.var()
        ticks.append(tick)
    types = list(r.take(n))
    sources = list(r.take(n))
    controls = types.count(0)
    for _ in range(2 * controls):
        r.var()
    r.take(controls)
    for etype in types:
        if etype == 0:
            continue
        elif etype in (1, 2, 5):
            r.var()
        elif etype == 3:
            r.var()
            r.take(r.var())
        elif etype == 4:
            r.take(8)
            r.take(r.var())
        elif etype == 6:
            r.var()
            r.take(r.var())
        elif etype == 7:
            r.take(r.var())
        else:
            raise FormatError("INPS: unknown event type %d" % etype)
    if r.left():
        raise FormatError("INPS: trailing bytes")
    return lo, list(zip(ticks, types, sources))

def rawcheck(chunks):
    """Sim events from a key or mouse need a raw key/button/wheel event in the same tick."""
    attempt, lo, problems, checked = -1, 0, [], 0
    sim, raw = {}, {}
    for name, flags, stored, size, seg, data in chunks:
        if name == "ATTM":
            attempt += 1
        elif name == "INPS":
            lo, evs = decode_events(data)
            for tick, etype, source in evs:
                if source in (1, 2):
                    sim.setdefault((attempt, tick), []).append((etype, SOURCES[source]))
        elif name == "rAWI":
            for e in decode_raw(data, lo):
                if e["kind"] in ("keydown", "keyup", "buttondown", "buttonup", "wheel"):
                    raw[(attempt, e["tick"])] = True
    for (a, tick), evs in sorted(sim.items()):
        checked += len(evs)
        if (a, tick) not in raw:
            problems.append("attempt %d tick %d: %s without a raw key/button event" % (a, tick, ", ".join("type %d from %s" % ev for ev in evs)))
    return checked, problems


def decode_kv(raw):
    r, out = Reader(raw), []
    for _ in range(r.var()):
        key = r.take(r.var()).decode()
        t = r.take(1)[0]
        if t == 0:
            v = r.var()
            v = (v >> 1) ^ -(v & 1)
        elif t == 1:
            v = r.var()
        elif t == 2:
            v = r.take(r.var()).decode("utf-8", "replace")
        elif t == 3:
            v = r.take(r.var()).hex()
        elif t == 4:
            import struct
            v = round(struct.unpack("<f", r.take(4))[0], 6)
        elif t == 5:
            v = bool(r.take(1)[0])
        else:
            raise FormatError("kv: unknown value type %d" % t)
        out.append((key, v))
    return out


def decode_time(raw):
    r = Reader(raw)
    names = ["wallus", "pausedus", "frames", "frames0", "frames2"] + ["h%d" % i for i in range(8)] + ["speedmin", "speedmax", "fixedframe"]
    out = [(n, r.var()) for n in names]
    if r.left():
        out.append(("drawn", r.var()))
    return out


def decode_splc(raw):
    r, out = Reader(raw), []
    for _ in range(r.var()):
        tick, discarded, kind = r.var(), r.var(), r.take(1)[0]
        w = r.var()
        out.append("tick %d discarded %d %s wallms %d" % (tick, discarded, "auto" if kind else "saved", (w >> 1) ^ -(w & 1)))
    return out


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "history":
        problems = check_history(sys.argv[2], sys.argv[3:])
        for p in problems:
            print("FAIL:", p)
        sys.exit(1 if problems else 0)
    if len(sys.argv) != (4 if sys.argv[1:2] == ["verify"] else 3) or sys.argv[1] not in ("check", "dump", "rawcheck", "verify"):
        sys.exit(__doc__)
    data = open(sys.argv[2], "rb").read()
    try:
        chunks, heads, content = parse(data)
    except FormatError as e:
        print("FAIL:", e)
        sys.exit(1)
    if sys.argv[1] == "dump":
        binds = 0
        for i, (name, flags, stored, raw, seg, data) in enumerate(chunks):
            print("chunk %d %s segment %d flags %d %d bytes (stored %d)" % (i, name, seg, flags, raw, stored))
            if name in KV_CHUNKS:
                print("    " + ", ".join("%s=%s" % kv for kv in decode_kv(data)))
            elif name == "tIME":
                print("    " + " ".join("%s=%d" % kv for kv in decode_time(data)))
            elif name == "SPLC":
                for line in decode_splc(data):
                    print("    " + line)
            elif name == "dEVS":
                r = Reader(data)
                for _ in range(r.var()):
                    kind = ["other", "keyboard", "mouse"][min(r.take(1)[0], 2)]
                    vendor, product = r.var(), r.var()
                    transport = r.take(r.var()).decode("utf-8", "replace")
                    devname = r.take(r.var()).decode("utf-8", "replace")
                    print("    %s %04x:%04x %s %s" % (kind, vendor, product, transport, devname))
            elif name == "bIND":
                for b in decode_binds(data):
                    binds += 1
                    print("    %d: %s" % (binds, b))
            elif name == "INPS":
                lo = Reader(data).var()
            elif name == "rAWI":
                for e in decode_raw(data, lo):
                    print("    " + " ".join("%s=%s" % kv for kv in e.items()))
    if sys.argv[1] == "verify":
        keys = [bytes.fromhex(l.strip()) for l in open(sys.argv[3]) if len(l.strip()) == 64]
        results = verify_signatures(chunks, heads, content, keys)
        for status, what in results:
            print("signature %s: %s" % (status, what))
        if not results:
            print("no signatures")
        players = {w.split("player ")[1] for s, w in results if s == "ok" and "player " in w}
        if len(players) > 1:
            print("FAIL: signed for different players")
            sys.exit(1)
        if any(s == "ok" and "verdict" in w and "verdict valid" not in w for s, w in results):
            print("FAIL: rejected by the verdict")
            sys.exit(1)
        if any(s == "BAD" for s, _ in results) or not any(s == "ok" for s, _ in results):
            print("FAIL: no valid signature" if not any(s == "BAD" for s, _ in results) else "FAIL: bad signature")
            sys.exit(1)
    if sys.argv[1] == "rawcheck":
        checked, problems = rawcheck(chunks)
        for p in problems:
            print("FAIL:", p)
        print("%d key/mouse sim events, %d without raw input" % (checked, len(problems)))
        if problems:
            sys.exit(1)
    for i, head in enumerate(heads):
        print("head %d %s" % (i, head.hex()))
    print("ok, content %s" % content.hex())


if __name__ == "__main__":
    main()
