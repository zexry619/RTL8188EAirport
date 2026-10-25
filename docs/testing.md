# Setup Pengujian & Debugging

---

## 1. Konflik dengan driver vendor (WAJIB dibaca)

`RtWlanU.kext` (vendor Realtek) match pada **`IOUSBInterface`** (stack USB legacy) untuk
USB ID `9047:268`. Driver kita akan match pada **`IOUSBHostInterface`** (stack modern).

Keduanya akan mencoba mengendalikan chip yang sama. Gejala kalau keduanya aktif:
* reset USB berulang, device tidak bisa di-enumerate;
* Wi-Fi hilang total (bahkan ke vendor);
* kernel panic sporadik.

**Aturan:**
1. Saat menguji driver kita, **nonaktifkan `RtWlanU`** (dan `RtWlanU1827`).
2. Simpan installer vendor + USB Wi-Fi cadangan sebagai jaring pengaman.
3. Jangan pernah membuang `RtWlanU` sebelum M5 terbukti stabil.

Cara menonaktifkan (pilih salah satu):

```bash
# Opsi A — pindahkan folder kext (perlu reboot)
sudo mv /Library/Extensions/RtWlanU.kext /Library/Extensions/RtWlanU.kext.disabled
sudo mv /Library/Extensions/RtWlanU1827.kext /Library/Extensions/RtWlanU1827.kext.disabled

# Opsi B — unload saat runtime (butuh SIP dimatikan atau kext di-/Library/Extensions)
sudo kextunload -b com.realtek.driver.RtWlanU

# Opsi C — nonaktifkan permanen lewat OpenCore
#   config.plist → NVRAM/Add → 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102 → "RtWlanU"
#   (Tool "Disable kext" di OpenCore/OcTool akan menulis entri di atas)
```

Setelah berubah, reboot dan pastikan `ioreg` tidak lagi menampilkan `RtWlanU`.

---

## 2. Memastikan device USB terdeteksi

### Data device terverifikasi di mesin ini (21 Okt 2026)

```
802.11n NIC:
  Product ID: 0x010c      → 268 desimal
  Vendor ID : 0x2357      → 9047 desimal
  Manufacturer: Realtek
  Serial Number: 00E04C0001
  Version (bcdDevice): 0x00
  Speed: Up to 480 Mb/s   ← USB 2.0 high-speed (bukan USB 3)
  Location ID: 0x14800000
  Current Available/Required: 500 mA
```

Konsekuensi penting:
* **High-speed (480 Mb/s)** → batas teoretis USB 2.0. Setelah overhead 802.11
  2,4 GHz, realistisnya **30–60 Mbps**, bukan 150 Mbps.
* `bcdDevice = 0x00` → jangan simpulkan tingkat firmware dari sini; chip ID asli
  harus dibaca dari register (`rtwn_read_chipid()`), bukan dari descriptor USB.
* Ada juga USB adapter Realtek lain di mesin ini (VID `0x0bda`, 480 Mb/s) —
  kemungkinan RTL8153 USB-Ethernet. Jangan tertukar dengan Wi-Fi.

### Perintah cepat

```bash
# device tree USB
ioreg -p IOUSB -w0 -l | grep -A5 -i '8188\|2357\|010c'

# USB ID decimal (9047 = 0x2357, 268 = 0x010C)
system_profiler SPUSBDataType | grep -A12 'Wi-Fi\|802.11'

# vendor/product dari ioreg
ioreg -rd1 -c IOUSBHostDevice | grep -E '"idVendor"|"idProduct"|"USB Product Name"'
```

Cukup: `scripts/check-device.sh` melakukan semua ini + mendeteksi apakah `RtWlanU` masih
dimuat.

---

## 3. Build & validasi

```bash
./scripts/fetch-upstream.sh      # sekali saja
./scripts/build-m0.sh            # compile + link + validasi + codesign
```

Yang harus terlihat:

```
[build] compile  src/m0-hello/RTL8188EUHello.cpp
[build] link     build/RTL8188EUHello.kext/Contents/MacOS/RTL8188EUHello
[build] validate (plutil -lint + file + codesign --verify)
[build] sign     codesign -f -s -  → OK
[build] simbol undefined (dicepat di kernel): 298
[build] OK  → …/RTL8188EUHello.kext
```

Kalau `file` tidak menghasilkan `Mach-O 64-bit kext bundle x86_64`, berarti flag
link `-kext` hilang. Kalau `codesign --verify` gagal, ulangi signing.

Validasi penuh (personality benar-benar match ke device) baru terjadi saat
`kmutil load` — lihat §4.

---

## 4. Load & log

```bash
# load (butuh hak akses)
sudo kmutil load --bundle-id io.github.zekri.RTL8188EUHello
# atau
sudo kextutil -v build/RTL8188EUHello.kext

# pastikan bind ke device
ioreg -r -c RTL8188EUHello -l

# log kernel
./scripts/log-m0.sh
# atau manual:
log stream --level debug --predicate 'eventMessage CONTAINS "RTL8188EUHello"'
log show --last 5m --predicate 'eventMessage CONTAINS "RTL8188EUHello"'
```

Unload:

```bash
sudo kmutil unload --bundle-id io.github.zekri.RTL8188EUHello
# atau
sudo kextunload -b io.github.zekri.RTL8188EUHello
```

### Output yang diharapkan (M0)

```
RTL8188EUHello: start() — provider: 802.11n NIC
RTL8188EUHello: idVendor                 = 9047 (0x2357)
RTL8188EUHello: idProduct                = 268 (0x010c)
RTL8188EUHello: bcdDevice                = 0 (0x0000)
RTL8188EUHello: USB Product Name         = "802.11n NIC"
RTL8188EUHello: USB Serial Number        = "00E04C0001"
RTL8188EUHello: device descriptor: bLength=18 … bcdUSB=0200 … bMaxPacketSize0=64
RTL8188EUHello: idVendor=0x2357 idProduct=0x010c bcdDevice=0x0000
RTL8188EUHello: speed: high (480 Mb/s)
RTL8188EUHello: configuration: bConfigurationValue=1 bNumInterfaces=1 bmAttributes=0x80 MaxPower=250 mA
RTL8188EUHello: interface 0: class=0 subclass=0 protocol=0 endpoints=4 altSetting=0
RTL8188EUHello:   endpoint addr=0x81 attrs=0x02 maxPacket=64 interval=0
RTL8188EUHello:   endpoint addr=0x02 attrs=0x02 maxPacket=64 interval=0
…
RTL8188EUHello:   ⚠️  driver vendor 'RtWlanU' SUDAH bind di interface ini …
RTL8188EUHello: start() selesai. Data di atas dipakai untuk Layer 1/2.
```

Data ini yang dipakai untuk menulis Layer 1 & 2 (nomor endpoint, ukuran paket,
jumlah interface). Salin output ini ke `docs/porting-notes.md` sebagai referensi.

---

## 5. Debug kernel serius

Opsi:| Debug level |
|---|---|
| **A. Log saja** (paling aman) | `IOLog`/`OSReport` + `log show`. Cukup untuk 90% masalah |
| **B. Panic log** | `/Library/Logs/DiagnosticReports/kernel-*.panic` → `panic` symbolized by `atos` |
| **C. KDP remote** | Butuh 2 mesin (target + host debugger) via Ethernet |
| **D. Xcode lldb** | `kdp` via USB/Ethernet, symbol dari kext yang di-build lokal |

KDP ringkas:

1. Di mesin target, set `nvram boot-args="kdp_match_name=en0"` (atau `kdp` untuk semua).
2. Di mesin host: `lldb`, lalu `target create <kernel>`, `gdb-remote <ip>`.
3. `image list`, `thread backtrace all`, `breakpoint set -n <fungsi>`.

Symbol: build kext dengan `-g` (debug info) dan simpan `.dSYM`; saat panic,
`atos -o RTL8188EUHello.kext/Contents/MacOS/RTL8188EUHello <address>`.

> Ethernet (`en0`) di mesin ini **tidak aktif**; pakai ethernet USB adapter, atau
> pindahkan debug lewat jaringan Wi-Fi **sebelum** driver kita diuji (berisiko).

---

## 6. Virtual/VM untuk iterasi cepat

macOS VM tidak bisa passthrough USB dengan baik (Virtualization.framework tidak mendukung
USB passthrough umum). Alternatif:

* **QEMU/VMware** dengan USB passthrough ke VM macOS — bisa, tapi macOS guest di VM
 Sometimes bermasalah untuk kext USB.
* **Spare SSD + boot target** — paling realistis. Pasang SSD kedua, boot macOS target
  dari situ, uji kext, ssh/log dari mesin utama.
* **Mesin kedua** — ideal untuk KDP.

Rekomendasi: **spare SSD internal/USB + 1 mesin kedua** (bisaichi pinjam) untuk debugging.

---

## 7. Rescue plan

| Situasi | Tindakan |
|---|---|
| Wi-Fi mati total & tidak ada jaringan | Boot dari USB rescue OpenCore; nonaktifkan kext baru; boot ke sistem utama |
| Kernel panic loop | Boot `-x` (safe mode) dari OpenCore; hapus kext dari ESP (`EFI/OC/Kexts`) |
| Kext baru rusak | Hapus dari `EFI/OC/Kexts`, rebuild `config.plist` dari backup |
| Semua gagal | Restore ESP dari backup: `backup/EFI-BIG-<timestamp>.tar.gz` (di repo proyek) |

Selalu: **backup ESP** sebelum perubahan:

```bash
# ESP sekarang: /dev/disk0s3 (EFI-BIG, 512 MB), ter-mount di /Volumes/EFI-BIG
diskutil list
TS=$(date +%Y%m%d-%H%M%S)
sudo tar -cf - -C /Volumes/EFI-BIG EFI | gzip -1 > "backup/EFI-BIG-$TS.tar.gz"
shasum -a 256 "backup/EFI-BIG-$TS.tar.gz"
```

---

## 8. Checklist sebelum setiap uji

- [ ] `RtWlanU` nonaktif (atau driver kita dalam mode "observe only")
- [ ] Backup ESP sudah dibuat
- [ ] `log stream` sudah berjalan di terminal terpisah
- [ ] Device USB ter-enumerate (`scripts/check-device.sh` hijau)
- [ ] Kext lolos validasi lokal (`plutil -lint`, `file`, `codesign --verify`)
