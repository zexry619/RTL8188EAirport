# Lisensi

Proyek ini akan menjadi **GPL-2.0-or-later** karena memakai frontend dari
`itlwm` / `laobamac/itlwm` (GPL-2.0). Komponen individual:

| Komponen | Lisensi | Sumber | Catatan |
|---|---|---|---|
| Kode asli proyek ini (shim USB, frontend adaptasi, tool) | GPL-2.0-or-later | — | Harusributions source saatuasive mendistribusikan |
| `sys/dev/usb/if_urtwn.c`, `sys/dev/ic/rtwn.c`, `rtwnvar.h`, `r92creg.h` | ISC | `openbsd/src` | Kompatibel dengan GPL |
| `itl80211/openbsd/net80211/*` (34 file) | ISC (OpenBSD) | `itlwm` (GPL-2.0 overall) | Diambil dari turunan OpenBSD net80211 |
| `AirportItlwm/*` (frontend IO80211) | GPL-2.0 | `laobamac/itlwm` | Wajib GPL + sumber |
| `RTW88USBDevice.cpp` (pola shim USB) | GPL-2.0 | `AirPort-RTW` | Dipakai sebagai *pola*, bukan disalin mentah |
| `rtl8188eufw.bin` | Lisensi firmware Realtek (`LICENCE.rtlwifi_firmware`) | linux-firmware | Firmware umumnya boleh did=redistribusikan; verifikasi ulang sebelum rilis |
| Header IO80211 (`include/Airport/*`) | Apple APSL 2.0 / privasi | `itlwm` | Header privat Apple, tidak boleh dipublikasikan tanpa izin |
| `MacKernelSDK` | BSD-3-Clause + lisensi Apple | `acidanthera/MacKernelSDK` | Hanya untuk build |

## Aturan praktis

1. **Jangan menyalin kode vendor Realtek (`RtWlanU`, `StatusBarApp`).** Binary tertutup,
   lisensinya tidak mengizinkan distribusi. Binary vendor hanya boleh dipakai sebagai
   *oracle* perilaku (bandingkan hasil scan/EFUSE), bukan disalin.
2. **Kode turunan GPL harus tetap GPL** dan disertai sumber lengkap (termasuk skrip patch
   terhadap sumber upstream).
3. **Header privat Apple** (`IO80211Controller.h`, `apple80211_var.h`, …) tidak boleh
   dipublikasikan. Project open-source umumnya menaruh header turunan di tempat yang
   "hanya untuk referensi pribadi", atau meminta izin ke Apple.
4. Kalau distribusi resmi diperlukan, jalur yang aman adalah **clean-room**:
   satu tim menulis spesifikasi perilaku dari dokumen/observasi, tim lain mengimplementasikan
   tanpa melihat kode yang diports.
