"""Tiny static-analysis helpers for RiftApart.exe (PE32+): sections, strings, RIP-relative xrefs,
function bounds from .pdata, RTTI vtables, and llvm-objdump disassembly."""
import struct, subprocess, os, functools, bisect
import numpy as np

EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'RiftApart.exe')
OBJDUMP = '/opt/llvm-mingw-20250924-ucrt-ubuntu-22.04-x86_64/bin/llvm-objdump'

data = open(EXE, 'rb').read()
_pe = struct.unpack_from('<I', data, 0x3C)[0]
_nsec = struct.unpack_from('<H', data, _pe + 6)[0]
_optsz = struct.unpack_from('<H', data, _pe + 20)[0]
BASE = struct.unpack_from('<Q', data, _pe + 24 + 24)[0]
sections = []
for i in range(_nsec):
    o = _pe + 24 + _optsz + i * 40
    name = data[o:o + 8].rstrip(b'\0').decode()
    vsz, rva, rsz, roff = struct.unpack_from('<IIII', data, o + 8)
    sections.append((name, rva, vsz, roff, rsz))
SEC = {s[0]: s for s in sections}


def off(rva):
    for n, r, vs, ro, rs in sections:
        if r <= rva < r + rs:
            return ro + rva - r
    return None


def rva_of_off(o):
    for n, r, vs, ro, rs in sections:
        if ro <= o < ro + rs:
            return r + o - ro
    return None


def u32(rva): return struct.unpack_from('<I', data, off(rva))[0]
def u64(rva): return struct.unpack_from('<Q', data, off(rva))[0]


def cstr(rva, n=200):
    o = off(rva)
    e = data.index(b'\0', o, o + n) if b'\0' in data[o:o + n] else o + n
    return data[o:e].decode('latin1')


def find_bytes(b, sec=None):
    out = []
    lo, hi = 0, len(data)
    if sec:
        _, r, vs, ro, rs = SEC[sec]
        lo, hi = ro, ro + rs
    i = data.find(b, lo, hi)
    while i >= 0:
        out.append(rva_of_off(i))
        i = data.find(b, i + 1, hi)
    return out


def find_str(s):
    return find_bytes(s.encode() + b'\0')


@functools.lru_cache(None)
def _text():
    _, r, vs, ro, rs = SEC['.text']
    return r, np.frombuffer(data, dtype=np.uint8, count=rs, offset=ro)


@functools.lru_cache(None)
def _disps():
    r, t = _text()
    n = len(t) - 4
    d = (t[0:n].astype(np.int64) | (t[1:n + 1].astype(np.int64) << 8) | (t[2:n + 2].astype(np.int64) << 16)
         | (t[3:n + 3].astype(np.int64) << 24))
    d = np.where(d >= 1 << 31, d - (1 << 32), d)
    return r, d


def xrefs(target_rva, max_hits=200):
    """Instruction sites whose rip-relative disp32 (followed by end of instruction) resolves to target.
    Assumes disp32 is the last 4 bytes of the instruction (true for lea/mov/call/jmp rel32 w/o imm)."""
    r, d = _disps()
    pos = np.arange(len(d), dtype=np.int64) + r + 4
    hits = np.nonzero(pos + d == target_rva)[0]
    return [int(h) + r for h in hits[:max_hits]]  # rva of the disp32 field


@functools.lru_cache(None)
def pdata():
    _, r, vs, ro, rs = SEC['.pdata']
    arr = np.frombuffer(data, dtype=np.uint32, count=rs // 4, offset=ro).reshape(-1, 3)
    arr = arr[arr[:, 0] != 0]
    return arr[np.argsort(arr[:, 0])]


def func_of(rva):
    p = pdata()
    i = bisect.bisect_right(p[:, 0], rva) - 1
    if i >= 0 and p[i, 0] <= rva < p[i, 1]:
        # walk back through chained unwind info: find the primary function start
        return int(p[i, 0]), int(p[i, 1])
    return None


def dis(start, end=None, n=None):
    if end is None:
        end = start + (n or 0x80)
    out = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '-M', 'intel',
                          f'--start-address={BASE + start:#x}', f'--stop-address={BASE + end:#x}', EXE],
                         capture_output=True, text=True).stdout
    lines = [l for l in out.splitlines() if l.strip().startswith('14')]
    return '\n'.join(l.replace('       ', ' ') for l in lines)


def rtti_vtables():
    """name -> vtable rva for complete-object locators (offset 0)."""
    _, rr, vs, ro, rs = SEC['.rdata']
    res = {}
    # COL: sig=1, offset, cdOffset, typeDesc rva, classDesc rva, self rva
    arr = np.frombuffer(data, dtype=np.uint32, count=rs // 4, offset=ro)
    idx = np.nonzero(arr == 1)[0]
    for i in idx:
        if i + 6 > len(arr):
            continue
        col_rva = rr + int(i) * 4
        if arr[i + 5] != col_rva or arr[i + 1] != 0:
            continue
        td = int(arr[i + 3])
        o = off(td + 16)
        if o is None:
            continue
        name = data[o:data.index(b'\0', o, o + 512)].decode('latin1')
        # vtable: a qword in .rdata equal to BASE+col_rva, vtable = that + 8
        res.setdefault(name, []).append(col_rva)
    return res


def vtable_for_col(col_rva):
    hits = find_bytes(struct.pack('<Q', BASE + col_rva), '.rdata')
    return [h + 8 for h in hits]


def primary_func(rva):
    """Function containing rva, following chained unwind info back to the primary entry."""
    p = pdata()
    i = bisect.bisect_right(p[:, 0], rva) - 1
    if i < 0 or not (p[i, 0] <= rva < p[i, 1]):
        return None
    beg, end, unw = int(p[i, 0]), int(p[i, 1]), int(p[i, 2])
    for _ in range(16):
        o = off(unw)
        flags = data[o] >> 3
        if not flags & 4:
            return beg
        cnt = data[o + 2]
        ch = o + 4 + ((cnt + 1) & ~1) * 2
        beg, end, unw = struct.unpack_from('<III', data, ch)
    return beg


def callers(func_rva, max_hits=500):
    """call rel32 / jmp rel32 sites targeting func_rva."""
    out = []
    for x in xrefs(func_rva, max_hits):
        o = off(x - 1)
        if data[o] in (0xE8, 0xE9):
            out.append(x - 1)
    return out
