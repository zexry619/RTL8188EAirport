#!/bin/bash
# Rollback injeksi OpenCore untuk kext M1.
# Usage: sudo ./scripts/uninstall-opencore.sh
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
ESP_DEV="${ESP_DEV:-/dev/disk0s2}"
ESP_MNT="${ESP_MNT:-$(find_esp || true)}"
OC_DIR="$ESP_MNT/EFI/OC"
BUNDLE="RTL8188EUM1.kext"

if [[ $EUID -ne 0 ]]; then
	echo "Jalankan dengan sudo" >&2
	exit 1
fi

if ! mount | grep -q " on $ESP_MNT "; then
	sudo diskutil mount "$ESP_DEV" >/dev/null || true
fi

if [[ -f "$OC_DIR/config.plist" ]]; then
	sudo python3 - "$OC_DIR/config.plist" "$BUNDLE" <<'PY'
import plistlib, sys
path, bundle = sys.argv[1], sys.argv[2]
with open(path, 'rb') as f:
    d = plistlib.load(f)
adds = d.get('Kernel', {}).get('Add', [])
before = len(adds)
adds[:] = [a for a in adds if a.get('BundlePath') != bundle]
with open(path, 'wb') as f:
    plistlib.dump(d, f)
print('[uninstall] Kernel->Add:', before, '->', len(adds))
PY
fi

sudo rm -rf "$OC_DIR/Kexts/$BUNDLE"
echo "[uninstall] $BUNDLE dihapus dari $OC_DIR/Kexts"
echo "Reboot untuk menerapkan: sudo shutdown -r now"
