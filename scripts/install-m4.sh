#!/bin/bash
# install-m4.sh — pasang kext native ke AuxKC TANPA KDK.
#
# Kenapa script ini ada:
#   `kmutil create --update-all` gagal (rc=71) di mesin tanpa KDK,
#   tapi kegagalannya tidak terlihat kalau output-nya di-pipe. Akibatnya
#   /Library/Extensions berisi binary baru sementara AuxKC masih memuat
#   salinan lama dari StashedExtensions — beberapa reboot bisa "menguji"
#   kode yang sama tanpa disadari.
#
# Yang dilakukan:
#   1. copy kext hasil build ke /Library/Extensions
#   2. ganti StashedExtensions/<uuid>/RTL8188EAirport.kext dengan yang baru
#   3. perbarui cdHash di instructions.plist dan uakl.plist
#   4. verifikasi hash binary + cdHash cocok
#
# Pakai: sudo ./scripts/install-m4.sh
# Setelah itu: reboot (kext IO80211 tidak bisa hot-load).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEXT_SRC="$ROOT/src/driver/build-m4/RTL8188EAirport.kext"
KEXT_DST="/Library/Extensions/RTL8188EAirport.kext"
AUXKC="/var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC"
BUNDLE_ID="io.github.zekri.RTL8188EAirport"

if [ "$(id -u)" -ne 0 ]; then
	echo "jalankan dengan sudo" >&2
	exit 1
fi
if [ ! -d "$KEXT_SRC" ]; then
	echo "kext belum dibuild: $KEXT_SRC" >&2
	exit 2
fi

echo "[1/4] pasang ke $KEXT_DST"
rm -rf "$KEXT_DST"
cp -R "$KEXT_SRC" "$KEXT_DST"
chown -R root:wheel "$KEXT_DST"
chmod -R go-w "$KEXT_DST"

NEW_CDHASH=$(codesign -dvvv "$KEXT_DST" 2>&1 | sed -n 's/^CDHash=//p' | head -1)
if [ -z "$NEW_CDHASH" ]; then
	echo "gagal membaca cdHash" >&2
	exit 3
fi
NEW_BIN_HASH=$(shasum "$KEXT_DST/Contents/MacOS/RTL8188EAirport" | awk '{print $1}')
echo "      cdHash=$NEW_CDHASH"

echo "[2/4] ganti StashedExtensions"
STASH_DIR=""
for d in "$AUXKC"/StashedExtensions/*/; do
	if [ -d "$d/RTL8188EAirport.kext" ]; then
		STASH_DIR="$d"
		break
	fi
done
if [ -z "$STASH_DIR" ]; then
	NEW_UUID=$(uuidgen)
	STASH_DIR="$AUXKC/StashedExtensions/$NEW_UUID"
	mkdir -p "$STASH_DIR"
	echo "      buat stash baru: $STASH_DIR"
fi
rm -rf "$STASH_DIR/RTL8188EAirport.kext"
cp -R "$KEXT_DST" "$STASH_DIR/"
chown -R root:wheel "$STASH_DIR/RTL8188EAirport.kext"
echo "      $STASH_DIR"

echo "[3/4] perbarui cdHash di instructions + uakl"
python3 - "$AUXKC" "$BUNDLE_ID" "$NEW_CDHASH" <<'PY'
import plistlib, sys

auxkc, bundle_id, new_hash = sys.argv[1], sys.argv[2], sys.argv[3]

p = f"{auxkc}/com.apple.kcgen.instructions.plist"
d = plistlib.load(open(p, 'rb'))
ktb = d.setdefault('kextsToBuild', {})
if bundle_id in ktb:
    ktb[bundle_id]['cdHash'] = new_hash
    print("      instructions: ok (updated)")
else:
    ktb[bundle_id] = {
        'bundlePathMainOS': '/Library/Extensions/RTL8188EAirport.kext',
        'cdHash': new_hash,
        'teamID': ''
    }
    print("      instructions: ok (added)")
plistlib.dump(d, open(p, 'wb'))

p2 = f"{auxkc}/com.apple.kcgen.uakl.plist"
d2 = plistlib.load(open(p2, 'rb'))
u = d2.get('UnsignedKernelExtensions', [])
if new_hash not in u:
    u.append(new_hash)
    d2['UnsignedKernelExtensions'] = u
    plistlib.dump(d2, open(p2, 'wb'))
    print("      uakl: ditambahkan")
else:
    print("      uakl: sudah ada")
PY

echo "[3/4] rebuild AuxKC (tanpa KDK)"
#
# kmutil create --update-all gagal rc=71 di mesin tanpa KDK. Yang bekerja:
#   -n aux                    hanya koleksi auxiliary
#   --allow-missing-kdk       paksa rebuild walau KDK tidak ada
#   -S / -B                   path SystemKC & BootKC eksplisit; tanpa ini
#                             dependency com.apple.kpi.* tidak ketemu
#   -s none                   jangan strip string (log diagnostik hilang
#                             kalau di-strip)
SYSC="/System/Library/KernelCollections/SystemKernelExtensions.kc"
BOOTC="/System/Library/KernelCollections/BootKernelExtensions.kc"
kmutil create -n aux --allow-missing-kdk --volume-root / \
	-S "$SYSC" -B "$BOOTC" -s none
if [ $? -ne 0 ]; then
	echo "GAGAL: rebuild AuxKC" >&2
	exit 7
fi

echo "[4/4] verifikasi"
STASH_BIN_HASH=$(shasum "$STASH_DIR/RTL8188EAirport.kext/Contents/MacOS/RTL8188EAirport" | awk '{print $1}')
STASH_CDHASH=$(codesign -dvvv "$STASH_DIR/RTL8188EAirport.kext" 2>&1 | sed -n 's/^CDHash=//p' | head -1)
STASH_VER=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$STASH_DIR/RTL8188EAirport.kext/Contents/Info.plist" 2>/dev/null)

KC="/System/Volumes/Data/Library/KernelCollections/AuxiliaryKernelExtensions.kc"
KC_VER=$(grep -a -o "$STASH_VER" "$KC" 2>/dev/null | head -1)
KC_HAS_LOG=$(grep -a -c "rtl8188e" "$KC" 2>/dev/null)

echo "      binary   : $NEW_BIN_HASH"
echo "      stash    : $STASH_BIN_HASH"
echo "      cdHash   : $STASH_CDHASH"
echo "      versi    : $STASH_VER"
echo "      kc versi : ${KC_VER:-TIDAK ADA}"
echo "      kc rtl8188e: ${KC_HAS_LOG:-0}"

if [ "$NEW_BIN_HASH" != "$STASH_BIN_HASH" ]; then
	echo "GAGAL: binary stash berbeda" >&2
	exit 5
fi
if [ "$NEW_CDHASH" != "$STASH_CDHASH" ]; then
	echo "GAGAL: cdHash stash berbeda" >&2
	exit 6
fi
if [ "$KC_VER" != "$STASH_VER" ]; then
	echo "GAGAL: AuxKC belum berisi versi $STASH_VER" >&2
	exit 8
fi

echo "OK — reboot untuk memuat build $STASH_VER"
