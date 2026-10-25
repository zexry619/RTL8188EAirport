#!/bin/bash
# Script pembersih total driver dan utilitas vendor Realtek (RtWlanU / StatusBarApp).
# Driver baru RTL8188EAirport sudah fully native di macOS (Airport / IO80211).
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "[!] Wajib dijalankan dengan hak akses administrator: sudo $0" >&2
    exit 1
fi

echo "=== [1/5] Menghentikan proses aplikasi vendor ==="
killall -9 StatusBarApp 2>/dev/null && echo "[-] Berhasil menghentikan proses StatusBarApp" || echo "[✓] StatusBarApp tidak sedang berjalan"
killall -9 RtWlanHelper 2>/dev/null && echo "[-] Berhasil menghentikan proses RtWlanHelper" || echo "[✓] RtWlanHelper tidak sedang berjalan"

echo "=== [2/5] Menghapus LaunchDaemons dan LaunchAgents vendor ==="
for plist in \
    "/Library/LaunchAgents/Wlan.Software.plist" \
    "/Library/LaunchAgents/Wlan.Software.plist.disabled" \
    "/Library/LaunchDaemons/Wlan.SoftwareDaemon.plist" \
    "/Library/LaunchDaemons/Wlan.SoftwareDaemon.plist.disabled" \
    "/Library/LaunchDaemons/com.zekri.realtek-autoload.plist" \
    "/Library/LaunchDaemons/com.zekri.realtek-autoload.plist.disabled"; do
    if [[ -e "$plist" ]]; then
        launchctl bootout system "$plist" 2>/dev/null || true
        rm -f "$plist"
        echo "[-] Dihapus: $plist"
    fi
done

echo "=== [3/5] Menghapus utilitas dan aplikasi vendor ==="
if [[ -d "/Library/Application Support/WLAN" ]]; then
    rm -rf "/Library/Application Support/WLAN"
    echo "[-] Dihapus folder: /Library/Application Support/WLAN"
fi

rm -f /usr/local/libexec/realtek-autoload.sh 2>/dev/null && echo "[-] Dihapus: /usr/local/libexec/realtek-autoload.sh" || true
rm -f /var/log/realtek-autoload.log 2>/dev/null && echo "[-] Dihapus: /var/log/realtek-autoload.log" || true
rm -f /private/var/tmp/com.realtek.utility.statusbar.socket 2>/dev/null || true

echo "=== [4/5] Menghapus kext driver lama vendor ==="
rm -rf "/Library/Extensions/RtWlanU.kext" 2>/dev/null || true
rm -rf "/Library/Extensions/RtWlanU1827.kext" 2>/dev/null || true
if [[ -d "/Library/Extensions.disabled-m1" ]]; then
    rm -rf "/Library/Extensions.disabled-m1"
    echo "[-] Dihapus: /Library/Extensions.disabled-m1 (berisi RtWlanU & RtWlanU1827)"
fi

echo "=== [5/5] Membersihkan AuxKC Staging ==="
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/clean-auxkc.py" ]]; then
    python3 "$SCRIPT_DIR/clean-auxkc.py" rtwlanu 2>/dev/null || true
fi

echo "=== SELESAI! Semua file driver dan utilitas vendor Realtek telah dibersihkan secara tuntas. ==="
