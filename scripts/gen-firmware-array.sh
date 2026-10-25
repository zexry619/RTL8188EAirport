#!/bin/bash
# Ubah firmware biner menjadi array C supaya bisa disematkan di kext.
#   firmware/rtl8188eufw.bin  ->  src/urtwn/firmware_rtl8188eufw.cpp
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/firmware/rtl8188eufw.bin"
DST="$ROOT/src/urtwn/firmware_rtl8188eufw.cpp"

if [[ ! -s "$SRC" ]]; then
	echo "[fw] $SRC belum ada — jalankan ./scripts/fetch-upstream.sh" >&2
	exit 1
fi

python3 - "$SRC" "$DST" <<'PY'
import io, sys
src, dst = sys.argv[1], sys.argv[2]
data = io.open(src, 'rb').read()
lines = []
lines.append('/*\n')
lines.append(' * firmware_rtl8188eufw.c — firmware RTL8188EUS, disematkan sebagai array C.\n')
lines.append(' *\n')
lines.append(' * Sumber   : linux-firmware, rtlwifi/rtl8188eufw.bin\n')
lines.append(' * Lisensi  : lisensi firmware Realtek (LICENCE.rtlwifi_firmware)\n')
lines.append(' * Ukuran   : %d byte\n' % len(data))
lines.append(' *\n')
lines.append(' * Di-generate oleh scripts/gen-firmware-array.sh — jangan diedit manual.\n')
lines.append(' */\n\n')
lines.append('extern const unsigned char rtl8188eufw_bin[];\n')
lines.append('extern const unsigned int rtl8188eufw_bin_len;\n\n')
lines.append('const unsigned char rtl8188eufw_bin[] = {\n')
for i in range(0, len(data), 12):
    chunk = data[i:i+12]
    lines.append('\t' + ', '.join('0x%02x' % b for b in chunk) + ',\n')
lines.append('};\n\n')
lines.append('const unsigned int rtl8188eufw_bin_len = %d;\n' % len(data))
io.open(dst, 'w', encoding='utf-8').write(''.join(lines))
print('[fw] %s -> %s (%d byte)' % (src, dst, len(data)))
PY
