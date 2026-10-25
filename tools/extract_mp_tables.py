#!/usr/bin/env python3
"""
extract_mp_tables.py — ekstrak tabel parameter ("MP") 8188E dari binary vendor.

Vendor macOS (RtWlanU) memakai pola:
    odm_read_and_config_mp_8188e_xxx()  →  (file MP kalau ada)  atau
                                          odm_config_xxx_8188e()  ← tabel built-in

File MP gaya Windows (rtl8188E\\MAC_REG.txt) tidak ada di macOS, jadi yang
dipakai adalah tabel built-in yang TERTANAM di binary. Tabel ini bisa kita
ekstrak dan port ke C.

Struktur entri (dari decompilation Ghidra):
    OddOtom  = uint32  — reg atau "descriptor" bersyarat
    EvenByte = uint8   — nilai
    padding  = 3 byte
    → 8 byte per entri

Makna descriptor (dari loop vendor):
    OddOtom <  0x40000000        → tulis biasa:  reg = OddOtom,  val = EvenByte
    OddOtom >= 0x80000000        → entri kontrol/mask (bukan tulis biasa)
    OddOtom >= 0x40000000 < 0x80000000 → tulis bersyarat (mask)

Output: file C dengan array konstanta, siap di-include driver.
"""

import struct
import subprocess
import sys
import os

BINARY = "/Library/Extensions.disabled-m1/RtWlanU.kext/Contents/MacOS/RtWlanU"
OUT_C = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "src", "urtwn", "rtwn_mp_8188e.c")

# tabel yang dipakai driver (urutan pemakaian vendor)
TABLES = [
    # (nama_s-simbol, nama_C, tipe entri)
    ("_array_mp_8188e_mac_reg",     "rtwn_mp_8188e_mac_reg",     "u32"),
    ("_array_mp_8188e_phy_reg",     "rtwn_mp_8188e_phy_reg",     "u32"),
    # CATATAN: _array_mp_8188e_phy_reg_pg (144 B) TIDAK ikut diekstrak
    # sebagai tabel tulis-MP. Isinya reg=0x000 + nilai + entri 0xffffffff
    # → itu tabel "Power Group" dengan format & konsumen sendiri.
    ("_array_mp_8188e_agc_tab",     "rtwn_mp_8188e_agc_tab",     "u32"),
    ("_array_mp_8188e_radioa",      "rtwn_mp_8188e_radioa",      "u32"),
    # CATATAN: _array_mp_8188e_txpwr_lmt (31584 B) TIDAK ikut diekstrak
    # sebagai tabel tulis-MP. Isinya uint16 (0x8edd, 0x0024, ...) — itu
    # tabel "TX power limit" dengan format & konsumen sendiri
    # (PHY_InitTxPowerLimit), BUKAN daftar (reg, val).
    # Kalau ikut diapplier → menulis ke register 0x248edd = sampah.
]


def load_binary(path):
    data = open(path, 'rb').read()
    ncmds = struct.unpack('<I', data[16:20])[0]
    off, segs = 32, []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack('<II', data[off:off + 8])
        if cmd == 0x19:  # LC_SEGMENT_64
            vmaddr, vmsize, fileoff, filesize = struct.unpack(
                '<QQQQ', data[off + 24:off + 56])
            segs.append((vmaddr, vmsize, fileoff))
        off += cmdsize
    return data, segs


def load_symbols(path):
    out = subprocess.run(['nm', '-a', path], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and len(p[0]) == 16 and p[1] in 'DdBb':
            try:
                syms[p[2]] = int(p[0], 16)
            except ValueError:
                pass
    return syms


def vaddr_to_fileoff(segs, vmaddr):
    for va, vs, fo in segs:
        if va <= vmaddr < va + vs:
            return fo + (vmaddr - va)
    return None


def extract(syms, segs, data, name):
    if name not in syms:
        return None
    addr = syms[name]
    nxt = [a for _, a in sorted(syms.items(), key=lambda kv: kv[1]) if a > addr]
    size = (min(nxt) - addr) if nxt else 0
    fo = vaddr_to_fileoff(segs, addr)
    if fo is None or size == 0:
        return None
    return data[fo:fo + size]


def decode_entries(blob):
    """Entri 8 byte: uint32 odd + uint8 val + 3 byte pad."""
    out = []
    for i in range(0, len(blob) - 7, 8):
        odd = struct.unpack('<I', blob[i:i + 4])[0]
        val = blob[i + 4]
        out.append((odd, val))
    return out


def main():
    if not os.path.exists(BINARY):
        print(f"❌ binary vendor tidak ada: {BINARY}")
        return 1

    data, segs = load_binary(BINARY)
    syms = load_symbols(BINARY)

    print(f"  binary : {BINARY}")
    print(f"  simbol : {len(syms)}\n")

    lines = [
        '/*',
        ' * rtwn_mp_8188e.c — tabel parameter HAL 8188E, DIEKSTRAK dari',
        ' * binary vendor macOS RtWlanU (Ghidra 12.1.4, 22 Okt 2026).',
        ' *',
        ' * Tabel "MP" ini adalah fallback built-in yang dipakai driver vendor',
        ' * ketika file parameter gaya Windows (rtl8188E\\\\MAC_REG.txt dst.) tidak',
        ' * tersedia — persis kondisi di macOS.',
        ' *',
        ' * Struktur entri (dari decompilation loop vendor):',
        ' *     uint32 odd   (reg atau descriptor bersyarat)',
        ' *     uint8  val   (nilai)',
        ' *     3 byte padding',
        ' *',
        ' * JANGAN diedit manual — regenerate dengan:',
        ' *     python3 tools/extract_mp_tables.py',
        ' */',
        '',
        '#include "rtwn_mp_8188e.h"',
        '',
    ]

    stats = []
    for sym, cname, _ in TABLES:
        blob = extract(syms, segs, data, sym)
        if blob is None:
            print(f"  ⚠️  {sym}: tidak ditemukan")
            continue
        ents = decode_entries(blob)
        plain = sum(1 for o, _ in ents if o < 0x40000000)
        cond = len(ents) - plain
        print(f"  ✅ {sym:34s} {len(blob):6d} B → {len(ents):4d} entri "
              f"({plain} tulis biasa, {cond} bersyarat/kontrol)")

        lines.append(f'/* {sym} — {len(ents)} entri */')
        lines.append(f'const rtwn_mp_entry_t {cname}[{len(ents)}] = {{')
        for o, v in ents:
            lines.append(f'\t{{ 0x{o:08x}u, 0x{v:02x}u }},')
        lines.append('};')
        lines.append('')
        stats.append((cname, len(ents)))

    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    with open(OUT_C, 'w') as f:
        f.write('\n'.join(lines) + '\n')

    hdr = os.path.join(os.path.dirname(OUT_C), 'rtwn_mp_8188e.h')
    h = [
        '/*',
        ' * rtwn_mp_8188e.h — tipe entri tabel parameter HAL 8188E.',
        ' */',
        '#ifndef RTL8188EU_RTWN_MP_8188E_H',
        '#define RTL8188EU_RTWN_MP_8188E_H',
        '',
        '#include <stdint.h>',
        '',
        'typedef struct {',
        '\tuint32_t odd;\t/* reg, atau descriptor bersyarat */',
        '\tuint8_t  val;\t/* nilai yang ditulis */',
        '} rtwn_mp_entry_t;',
        '',
        '/* Arti descriptor (dari loop vendor):',
        ' *   odd <  0x40000000 → tulis biasa: reg = odd, nilai = val',
        ' *   odd >= 0x40000000 → entri kontrol/bersyarat (lihat driver)',
        ' */',
        '#define RTWN_MP_PLAIN_MAX 0x40000000u',
        '',
    ]
    for cname, n in stats:
        h.append(f'extern const rtwn_mp_entry_t {cname}[{n}];')
    h += ['', '#endif /* RTL8188EU_RTWN_MP_8188E_H */', '']
    with open(hdr, 'w') as f:
        f.write('\n'.join(h))

    print(f"\n  → {OUT_C}")
    print(f"  → {hdr}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
