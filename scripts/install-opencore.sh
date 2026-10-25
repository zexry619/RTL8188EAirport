#!/bin/bash
# Injeksi kext M1 ke OpenCore (EFI/OC/Kexts + config.plist Kernel->Add).
#
# Dipakai kalau jalur AuxiliaryKernelExtensions ditolak (kext ad-hoc tidak
# dievaluasi syspolicyd). OpenCore menyuntik kext langsung ke BootKC saat boot,
# jadi tidak perlu approval/syspolicyd.
#
# Usage: sudo ./scripts/install-opencore.sh
set -euo pipefail

# Deteksi ESP secara otomatis (nama volume bisa berubah: EFI-BIG / Mac OS / ...)
find_esp() {
	local v
	for v in /Volumes/*; do
		if [ -f "$v/EFI/OC/OpenCore.efi" ]; then
			echo "$v"
			return 0
		fi
	done
	return 1
}


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEXT_SRC="$ROOT/src/driver/build/RTL8188EUM1.kext"
ESP_DEV="${ESP_DEV:-/dev/disk0s2}"
ESP_MNT="${ESP_MNT:-$(find_esp || true)}"
OC_DIR="$ESP_MNT/EFI/OC"
BUNDLE="RTL8188EUM1.kext"

if [[ $EUID -ne 0 ]]; then
	echo "Jalankan dengan sudo" >&2
	exit 1
fi
if [[ ! -d "$KEXT_SRC" ]]; then
	echo "Kext belum dibangun: ./scripts/build-m1.sh" >&2
	exit 1
fi

# CATATAN PENTING: proyek sedang DIJEDA. Injeksi ke OpenCore berarti kext
# berjalan saat boot — dua kernel panic pernah terjadi karena itu.
# Hanya lanjut kalau memang sengaja & paham risikonya.
if [[ "${ALLOW_BOOT_INJECTION:-0}" != "1" ]]; then
	cat >&2 <<'EOF'
[install] DITAHAN demi keamanan.

Proyek sedang DIJEDA. Injeksi ke OpenCore = kext jalan saat boot, dan itu
menyebabkan 2 kernel panic sebelumnya (use-after-free buffer transfer USB).

Kalau memang mau lanjut:
    ALLOW_BOOT_INJECTION=1 sudo ./scripts/install-opencore.sh

Alternatif yang lebih aman (tidak menyentuh ESP):
    sudo ./scripts/vendor-off.sh
    sudo kmutil load -z -p src/build-test/RTL8188EUM1.kext
    # lalu cabut-colok dongle USB
EOF
	exit 3
fi

# --- 1. pastikan ESP ter-mount ---
if ! mount | grep -q " on $ESP_MNT "; then
	sudo diskutil mount "$ESP_DEV" >/dev/null || {
		echo "Gagal mount $ESP_DEV" >&2; exit 1; }
fi
[[ -d "$OC_DIR" ]] || { echo "OpenCore tidak ditemukan di $OC_DIR" >&2; exit 1; }

# --- 2. backup ---
TS=$(date +%Y%m%d-%H%M%S)
mkdir -p "$ROOT/backup"
sudo cp "$OC_DIR/config.plist" "$ROOT/backup/config.plist.$TS.bak"
sudo tar -cf - -C "$ESP_MNT" EFI 2>/dev/null | gzip -1 > "$ROOT/backup/EFI-BIG-$TS.tar.gz"
echo "[install] backup: backup/config.plist.$TS.bak + backup/EFI-BIG-$TS.tar.gz"

# --- 3. salin kext ---
sudo rm -rf "$OC_DIR/Kexts/$BUNDLE"
sudo cp -R "$KEXT_SRC" "$OC_DIR/Kexts/$BUNDLE"
sudo chown -R root:wheel "$OC_DIR/Kexts/$BUNDLE"
echo "[install] kext disalin ke $OC_DIR/Kexts/$BUNDLE"

# --- 4. tambah entri Kernel -> Add ---
sudo python3 - "$OC_DIR/config.plist" "$BUNDLE" <<'PY'
import plistlib, sys, copy

path, bundle = sys.argv[1], sys.argv[2]
with open(path, 'rb') as f:
    d = plistlib.load(f)

adds = d.setdefault('Kernel', {}).setdefault('Add', [])
# hapus entri lama kalau ada
adds[:] = [a for a in adds if a.get('BundlePath') != bundle]

# salin format dari entri yang sudah ada
tpl = next((a for a in adds if a.get('BundlePath')), None)
entry = {
    'Arch': 'Any',
    'BundlePath': bundle,
    'Comment': 'RTL8188EUS native Wi-Fi (M1 bring-up)',
    'Enabled': True,
    'ExecutablePath': 'Contents/MacOS/RTL8188EUM1',
    'MaxKernel': '24.99.99',
    'MinKernel': '24.0.0',
    'PlistPath': 'Contents/Info.plist',
}
adds.append(entry)

with open(path, 'wb') as f:
    plistlib.dump(d, f)
print('[install] Kernel->Add:', len(adds), 'entri; +', bundle)
PY

# --- 5. validasi plist ---
plutil -lint "$OC_DIR/config.plist" >/dev/null && echo "[install] plutil -lint: OK"

# --- 6. bersihkan kext dari AuxKC (hindari dobel load) ---
echo "[install] bersihkan staging AuxKC"
sudo kmutil clear-staging >/dev/null 2>&1 || true
sudo python3 - <<'PY'
import plistlib, shutil, os
base = "/var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC"
inst = os.path.join(base, "com.apple.kcgen.instructions.plist")
uakl = os.path.join(base, "com.apple.kcgen.uakl.plist")
for p in (inst, uakl):
    if not os.path.exists(p):
        continue
    with open(p, 'rb') as f:
        d = plistlib.load(f)
    changed = False
    ktb = d.get("kextsToBuild", {})
    for k in list(ktb):
        if "zekri" in k.lower():
            del ktb[k]; changed = True
    ul = d.get("UnsignedKernelExtensions", [])
    n = len(ul)
    ul[:] = [x for x in ul if "zekri" not in str(x).lower() and
             "75e5fed63b9bddbb0cc0357ccb4c6d72031c1154" not in str(x)]
    if len(ul) != n:
        changed = True
    if changed:
        shutil.copy(p, p + ".bak-m1")
        with open(p, 'wb') as f:
            plistlib.dump(d, f)
        print("[install] AuxKC dibersihkan:", os.path.basename(p))
PY
# paksa rebuild AuxKC dari repository saat ini (tanpa kext kita)
sudo kmutil load -z -p /Library/Extensions/EnergyDriver.kext >/dev/null 2>&1 || true
if kmutil inspect -p /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-extension-info 2>/dev/null | grep -qi zekri; then
	echo "[install] PERINGATAN: kext masih ada di AuxKC — cek manual"
else
	echo "[install] AuxKC bersih (kext kita tidak ada di sana)"
fi

cat <<'EOF'

============================================================
 SELESAI — reboot untuk memuat kext lewat OpenCore
============================================================
   sudo shutdown -r now

 Setelah boot:
   cd /Users/zekri/Downloads/hackintosh-skills-id/rtl8188eu-airport
   ./scripts/collect-m1-log.sh

 Rollback (hapus injeksi OpenCore):
   sudo ./scripts/uninstall-opencore.sh
============================================================
EOF
