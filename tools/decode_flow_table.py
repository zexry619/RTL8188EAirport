#!/usr/bin/env python3
"""
decode_flow_table.py — ekstrak tabel "power flow" driver vendor Realtek
                        dari binary RtWlanU.kext (Mach-O x86_64).

Tabel `_rtl<chip>_<phase>_flow` berisi entri WL_PWR_CFG (8 byte):

    +0  u16  Register
    +2  u8   BitMaskReg      (entri jalan kalau != 0)
    +3  u16  Ext             (unaligned)
           Ext & 0x0f        = unit mask
           (Ext >> 4) & 0x0f = type mask
           Ext >> 12         = command type
    +5  u8   AndMask
    +6  u8   OrMask
    +7  u8   (padding)

Command type:
    1 = WRITE : reg = (reg & ~AndMask) | (OrMask & AndMask)
    2 = POLL  : tunggu sampai (reg & AndMask) == (OrMask & AndMask)
    3 = DELAY : tunggu reg ms (atau reg*1000 ms bila OrMask != 0)
    4 = END

Sumber decompilation: RtWlanU::HalPwrSeqCmdParsing (0x9a6c4).

Pakai:
    python3 tools/decode_flow_table.py [path_binary]
"""

import struct
import subprocess
import sys

DEFAULT_BIN = "/Library/Extensions/RtWlanU.kext/Contents/MacOS/RtWlanU"

CMDS = {1: "WRITE", 2: "POLL ", 3: "DELAY", 4: "END  "}


def find_symbols(binary):
    out = subprocess.run(["nm", binary], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("D", "d", "B", "b", "R", "r"):
            syms[parts[2]] = int(parts[0], 16)
    return syms


def decode(data, base, max_entries=64):
    rows = []
    for i in range(max_entries):
        o = base + i * 8
        if o + 8 > len(data):
            break
        reg, = struct.unpack_from("<H", data, o)
        bmask = data[o + 2]
        ext, = struct.unpack_from("<H", data, o + 3)
        andm = data[o + 5]
        orm = data[o + 6]
        cmd = ext >> 12
        if cmd == 4:
            rows.append((i, reg, bmask, ext, cmd, andm, orm, "END"))
            break
        if cmd == 1:
            act = "reg = (reg & ~0x%02x) | (0x%02x & 0x%02x)" % (andm, orm, andm)
        elif cmd == 2:
            want = orm & andm
            act = "tunggu (reg & 0x%02x) == 0x%02x" % (andm, want)
        elif cmd == 3:
            act = "tunggu %d ms%s" % (reg, " (x1000)" if orm else "")
        else:
            act = "? tipe=%d" % cmd
        rows.append((i, reg, bmask, ext, cmd, andm, orm, act))
    return rows


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    try:
        with open(binary, "rb") as f:
            data = f.read()
    except OSError as e:
        print("gagal buka %s: %s" % (binary, e))
        return 1

    syms = find_symbols(binary)
    flows = sorted((a, n) for n, a in syms.items() if n.startswith("_rtl") and n.endswith("_flow"))
    if not flows:
        print("tidak ada tabel flow di", binary)
        return 1

    for addr, name in flows:
        print("=" * 78)
        print("%s  @0x%x" % (name, addr))
        print("=" * 78)
        print("%-3s %-6s %-6s %-7s %-6s %-6s  %s" %
              ("#", "Reg", "BMask", "Ext", "Type", "And/Or", "aksi"))
        for (i, reg, bmask, ext, cmd, andm, orm, act) in decode(data, addr):
            print("%-3d 0x%04x 0x%02x   0x%04x  %-6s 0x%02x/0x%02x  %s" %
                  (i, reg, bmask, ext, CMDS.get(cmd, "?"), andm, orm, act))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
