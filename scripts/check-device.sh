#!/bin/bash
# Periksa kondisi device USB Wi-Fi dan potensi konflik dengan driver vendor.
set -uo pipefail

echo "=============================================================="
echo " 1. Device USB (TP-Link RTL8188EUS, 2357:010c)"
echo "=============================================================="
system_profiler SPUSBDataType 2>/dev/null \
	| grep -A12 -i '802.11n NIC\|Realtek' | head -30 \
	|| true
if ! system_profiler SPUSBDataType 2>/dev/null | grep -qi '802.11n NIC'; then
	echo "  (tidak menemukan device 802.11n NIC)"
fi

echo
echo "=============================================================="
echo " 2. Properti device di IORegistry"
echo "=============================================================="
ioreg -rd1 -c IOUSBHostDevice 2>/dev/null \
	| awk '/"USB Product Name" = "802.11n NIC"/{f=1} f&&/"idVendor"|"idProduct"|"bcdDevice"|"USB Serial Number"|"locationID"/{print "   "$0} f&&/^  }/{f=0}' \
	| head -12

echo
echo "=============================================================="
echo " 3. Driver vendor Realtek"
echo "=============================================================="
if kextstat 2>/dev/null | grep -qi 'RtWlanU'; then
	echo "  ⚠️  RtWlanU SEDANG AKTIF:"
	kextstat 2>/dev/null | grep -i 'RtWlanU' | sed 's/^/     /'
	echo "  → Nonaktifkan sebelum menguji driver kita (lihat docs/testing.md)."
else
	echo "  ✅ RtWlanU tidak aktif."
fi

echo
echo "=============================================================="
echo " 4. Interface jaringan"
echo "=============================================================="
networksetup -listallhardwareports 2>/dev/null | head -30

echo
echo "=============================================================="
echo " 5. Kext kita"
echo "=============================================================="
if kextstat 2>/dev/null | grep -qi 'RTL8188EUHello'; then
	kextstat 2>/dev/null | grep -i 'RTL8188EUHello' | sed 's/^/  /'
else
	echo "  (RTL8188EUHello belum dimuat)"
fi

echo
echo "=============================================================="
echo " 6. Kesiapan"
echo "=============================================================="
if kextstat 2>/dev/null | grep -qi 'RtWlanU'; then
	echo "  Status: PERLU TINDAKAN — matikan RtWlanU dulu (M0 aman, M1+ wajib)."
else
	echo "  Status: SIAP untuk M0."
fi
